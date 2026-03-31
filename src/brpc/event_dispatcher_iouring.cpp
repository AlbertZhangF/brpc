// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "butil/logging.h"
#include "butil/fd_utility.h"
#include <liburing.h>
#include <sys/utsname.h>
#include <poll.h>
#include <unordered_map>
#include <mutex>

namespace brpc {

extern bvar::LatencyRecorder* g_edisp_read_lantency;
extern bvar::LatencyRecorder* g_edisp_write_lantency;

struct IoUringContext {
    struct io_uring ring;
    bool initialized;
    std::mutex fd_map_mutex;
    std::unordered_map<int, IOEventDataId> fd_to_event_data;
    
    IoUringContext() : initialized(false) {
        memset(&ring, 0, sizeof(ring));
    }
};

static IoUringContext& GetIoUringContext(EventDispatcher* dispatcher) {
    static std::unordered_map<EventDispatcher*, IoUringContext*> contexts;
    static std::mutex contexts_mutex;
    
    std::lock_guard<std::mutex> lock(contexts_mutex);
    auto it = contexts.find(dispatcher);
    if (it == contexts.end()) {
        IoUringContext* ctx = new IoUringContext();
        contexts[dispatcher] = ctx;
        return *ctx;
    }
    return *it->second;
}

EventDispatcher::EventDispatcher()
    : _event_dispatcher_fd(-1)
    , _stop(false)
    , _tid(0)
    , _thread_attr(BTHREAD_ATTR_NORMAL) {
    
    IoUringContext& ctx = GetIoUringContext(this);
    
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    
    params.flags |= IORING_SETUP_CQSIZE;
    params.cq_entries = 512;
    
    int ret = io_uring_queue_init_params(256, &ctx.ring, &params);
    if (ret < 0) {
        PLOG(FATAL) << "Fail to create io_uring: " << strerror(-ret);
        return;
    }
    
    ctx.initialized = true;
    _event_dispatcher_fd = ctx.ring.ring_fd;
    
    LOG(INFO) << "io_uring created: ring_fd=" << _event_dispatcher_fd
              << ", sq_entries=" << params.sq_entries
              << ", cq_entries=" << params.cq_entries;

    _wakeup_fds[0] = -1;
    _wakeup_fds[1] = -1;
    if (pipe(_wakeup_fds) != 0) {
        PLOG(FATAL) << "Fail to create pipe";
        return;
    }
}

EventDispatcher::~EventDispatcher() {
    Stop();
    Join();
    
    IoUringContext& ctx = GetIoUringContext(this);
    if (ctx.initialized) {
        io_uring_queue_exit(&ctx.ring);
        ctx.initialized = false;
    }
    
    if (_wakeup_fds[0] > 0) {
        close(_wakeup_fds[0]);
        close(_wakeup_fds[1]);
    }
}

int EventDispatcher::Start(const bthread_attr_t* thread_attr) {
    IoUringContext& ctx = GetIoUringContext(this);
    if (!ctx.initialized) {
        LOG(FATAL) << "io_uring was not created";
        return -1;
    }
    
    if (_tid != 0) {
        LOG(FATAL) << "Already started this dispatcher(" << this 
                   << ") in bthread=" << _tid;
        return -1;
    }

    if (thread_attr) {
        _thread_attr = *thread_attr;
    }

    bthread_attr_t io_uring_thread_attr =
        _thread_attr | BTHREAD_NEVER_QUIT | BTHREAD_GLOBAL_PRIORITY;

    int rc = bthread_start_background(&_tid, &io_uring_thread_attr, RunThis, this);
    if (rc) {
        LOG(FATAL) << "Fail to create io_uring thread: " << berror(rc);
        return -1;
    }
    return 0;
}

bool EventDispatcher::Running() const {
    return !_stop && _event_dispatcher_fd >= 0 && _tid != 0;
}

void EventDispatcher::Stop() {
    _stop = true;

    if (_event_dispatcher_fd >= 0) {
        IoUringContext& ctx = GetIoUringContext(const_cast<EventDispatcher*>(this));
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx.ring);
        if (sqe) {
            io_uring_prep_poll_add(sqe, _wakeup_fds[1], EPOLLOUT);
            sqe->user_data = 0;
            io_uring_submit(&ctx.ring);
        }
    }
}

void EventDispatcher::Join() {
    if (_tid) {
        bthread_join(_tid, NULL);
        _tid = 0;
    }
}

int EventDispatcher::RegisterEvent(IOEventDataId event_data_id,
                                   int fd, bool pollin) {
    IoUringContext& ctx = GetIoUringContext(this);
    if (!ctx.initialized) {
        errno = EINVAL;
        return -1;
    }

    uint32_t events = POLLOUT | EPOLLET;
    if (pollin) {
        events |= POLLIN;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx.ring);
    if (!sqe) {
        LOG(ERROR) << "Failed to get SQE";
        return -1;
    }

    io_uring_prep_poll_add(sqe, fd, events);
    sqe->user_data = event_data_id;
    
    {
        std::lock_guard<std::mutex> guard(ctx.fd_map_mutex);
        ctx.fd_to_event_data[fd] = event_data_id;
    }

    return 0;
}

int EventDispatcher::UnregisterEvent(IOEventDataId event_data_id,
                                     int fd, bool pollin) {
    IoUringContext& ctx = GetIoUringContext(this);
    if (!ctx.initialized) {
        errno = EINVAL;
        return -1;
    }

    if (pollin) {
        uint32_t events = POLLIN | EPOLLET;
        
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx.ring);
        if (!sqe) {
            return -1;
        }

        io_uring_prep_poll_add(sqe, fd, events);
        sqe->user_data = event_data_id;
        
        return 0;
    } else {
        return RemoveConsumer(fd);
    }
}

int EventDispatcher::AddConsumer(IOEventDataId event_data_id, int fd) {
    IoUringContext& ctx = GetIoUringContext(this);
    if (!ctx.initialized) {
        errno = EINVAL;
        return -1;
    }
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx.ring);
    if (!sqe) {
        LOG(ERROR) << "Failed to get SQE";
        return -1;
    }

    io_uring_prep_poll_add(sqe, fd, POLLIN | EPOLLET);
    sqe->user_data = event_data_id;
    
    {
        std::lock_guard<std::mutex> guard(ctx.fd_map_mutex);
        ctx.fd_to_event_data[fd] = event_data_id;
    }

    return 0;
}

int EventDispatcher::RemoveConsumer(int fd) {
    if (fd < 0) {
        return -1;
    }
    
    IoUringContext& ctx = GetIoUringContext(this);
    if (!ctx.initialized) {
        return -1;
    }
    
    {
        std::lock_guard<std::mutex> guard(ctx.fd_map_mutex);
        ctx.fd_to_event_data.erase(fd);
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx.ring);
    if (!sqe) {
        LOG(WARNING) << "Failed to get SQE for poll remove, fd=" << fd;
        return -1;
    }

    io_uring_prep_poll_remove(sqe, (unsigned long long)fd);
    sqe->user_data = 0;
    
    io_uring_submit(&ctx.ring);

    return 0;
}

void* EventDispatcher::RunThis(void* arg) {
    ((EventDispatcher*)arg)->Run();
    return NULL;
}

void EventDispatcher::Run() {
    IoUringContext& ctx = GetIoUringContext(this);
    if (!ctx.initialized) {
        LOG(ERROR) << "io_uring context not initialized";
        return;
    }
    
    while (!_stop) {
        int ret = io_uring_submit_and_wait(&ctx.ring, 1);
        
        if (_stop) {
            break;
        }
        
        if (ret < 0) {
            if (ret == -EINTR) {
                continue;
            }
            PLOG(ERROR) << "io_uring_submit_and_wait failed";
            break;
        }
        
        unsigned head;
        unsigned count = 0;
        struct io_uring_cqe* cqe;
        
        io_uring_for_each_cqe(&ctx.ring, head, cqe) {
            count++;
            
            IOEventDataId event_data_id = cqe->user_data;
            if (event_data_id == 0) {
                continue;
            }
            
            int32_t res = cqe->res;
            
            if (res < 0) {
                if (res == -ECANCELED) {
                    continue;
                }
                LOG(WARNING) << "io_uring operation failed: " << strerror(-res);
                continue;
            }
            
            uint32_t events = static_cast<uint32_t>(res);
            
            if (events & (POLLIN | POLLERR | POLLHUP)) {
                int64_t start_ns = butil::cpuwide_time_ns();
                CallInputEventCallback(event_data_id, events, _thread_attr);
                (*g_edisp_read_lantency) << (butil::cpuwide_time_ns() - start_ns);
            }
            
            if (events & (POLLOUT | POLLERR | POLLHUP)) {
                int64_t start_ns = butil::cpuwide_time_ns();
                CallOutputEventCallback(event_data_id, events, _thread_attr);
                (*g_edisp_write_lantency) << (butil::cpuwide_time_ns() - start_ns);
            }
        }
        
        io_uring_cq_advance(&ctx.ring, count);
    }
}

} // namespace brpc
