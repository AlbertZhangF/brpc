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

#include <liburing.h>
#include <poll.h>
#include <sys/utsname.h>
#include <unordered_map>
#include <sched.h>

namespace brpc {
namespace iouring_backend {

static const uint64_t IOURING_INTERNAL_EVENT = 0xFFFFFFFFFFFFFFFEULL;

struct IoUringFdInfo {
    int fd;
    uint32_t events;

    IoUringFdInfo() : fd(-1), events(0) {}
    IoUringFdInfo(int f, uint32_t e) : fd(f), events(e) {}
};

struct IoUringContext {
    struct io_uring ring;
    bool initialized;
    bool sqpoll;
    std::unordered_map<IOEventDataId, IoUringFdInfo> fd_info_map;

    IoUringContext() : initialized(false), sqpoll(false) {
        memset(&ring, 0, sizeof(ring));
    }

    ~IoUringContext() {
        if (initialized) {
            io_uring_queue_exit(&ring);
            initialized = false;
        }
    }

    bool NeedsSubmit() const {
        return !sqpoll;
    }

    int Submit() {
        if (sqpoll) {
            return 0;
        }
        return io_uring_submit(&ring);
    }

    int SubmitAndWait(unsigned wait_nr) {
        if (sqpoll) {
            while (io_uring_cq_ready(&ring) < (int)wait_nr) {
                io_uring_sqring_wait(&ring);
            }
            return 0;
        }
        return io_uring_submit_and_wait(&ring, wait_nr);
    }
};

static IoUringContext* GetCtx(EventDispatcher* disp) {
    return static_cast<IoUringContext*>(disp->_iouring_ctx);
}

static int GetKernelVersion() {
    struct utsname buf;
    if (uname(&buf) != 0) {
        return 0;
    }
    int major = 0, minor = 0;
    sscanf(buf.release, "%d.%d", &major, &minor);
    return major * 1000 + minor;
}

void Init(EventDispatcher* disp) {
    IoUringContext* ctx = new IoUringContext();
    disp->_iouring_ctx = ctx;

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    params.flags |= IORING_SETUP_CQSIZE;
    params.cq_entries = 8192;

    bool use_sqpoll = FLAGS_io_uring_sqpoll;
    int kernel_ver = GetKernelVersion();

    if (use_sqpoll && kernel_ver < 5011) {
        LOG(WARNING) << "io_uring SQPOLL requires kernel 5.11+, current kernel is "
                     << kernel_ver / 1000 << "." << kernel_ver % 1000
                     << ", falling back to default mode";
        use_sqpoll = false;
    }

    if (use_sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = FLAGS_io_uring_sqpoll_idle * 1000;

        if (FLAGS_io_uring_sqpoll_cpu >= 0) {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = FLAGS_io_uring_sqpoll_cpu;
        }
    }

    int ret = io_uring_queue_init_params(1024, &ctx->ring, &params);
    if (ret < 0) {
        if (use_sqpoll && (ret == -EINVAL || ret == -EPERM)) {
            LOG(WARNING) << "io_uring SQPOLL not supported (ret=" << ret
                         << "), retrying without SQPOLL";
            use_sqpoll = false;
            memset(&params, 0, sizeof(params));
            params.flags |= IORING_SETUP_CQSIZE;
            params.cq_entries = 8192;
            ret = io_uring_queue_init_params(1024, &ctx->ring, &params);
        }
        if (ret < 0) {
            PLOG(FATAL) << "Fail to create io_uring: " << strerror(-ret);
            delete ctx;
            disp->_iouring_ctx = NULL;
            return;
        }
    }

    ctx->initialized = true;
    ctx->sqpoll = use_sqpoll;
    disp->_event_dispatcher_fd = ctx->ring.ring_fd;

    LOG(INFO) << "io_uring created: ring_fd=" << disp->_event_dispatcher_fd
              << ", sq_entries=" << params.sq_entries
              << ", cq_entries=" << params.cq_entries
              << ", sqpoll=" << (use_sqpoll ? "enabled" : "disabled")
              << ", sq_thread_cpu=" << (use_sqpoll ? params.sq_thread_cpu : -1)
              << ", sq_thread_idle=" << (use_sqpoll ? params.sq_thread_idle / 1000 : 0) << "ms";

    disp->_wakeup_fds[0] = -1;
    disp->_wakeup_fds[1] = -1;
    if (pipe(disp->_wakeup_fds) != 0) {
        PLOG(FATAL) << "Fail to create pipe";
        return;
    }
    CHECK_EQ(0, butil::make_close_on_exec(disp->_wakeup_fds[0]));
    CHECK_EQ(0, butil::make_close_on_exec(disp->_wakeup_fds[1]));
}

void Destroy(EventDispatcher* disp) {
    IoUringContext* ctx = GetCtx(disp);
    if (ctx) {
        delete ctx;
        disp->_iouring_ctx = NULL;
    }
    if (disp->_wakeup_fds[0] > 0) {
        close(disp->_wakeup_fds[0]);
        close(disp->_wakeup_fds[1]);
        disp->_wakeup_fds[0] = -1;
        disp->_wakeup_fds[1] = -1;
    }
}

int Start(EventDispatcher* disp, const bthread_attr_t* thread_attr) {
    IoUringContext* ctx = GetCtx(disp);
    if (!ctx || !ctx->initialized) {
        LOG(ERROR) << "io_uring was not created";
        return -1;
    }

    if (disp->_tid != 0) {
        LOG(ERROR) << "Already started this dispatcher(" << disp
                   << ") in bthread=" << disp->_tid;
        return -1;
    }

    if (thread_attr) {
        disp->_thread_attr = *thread_attr;
    }

    bthread_attr_t io_uring_thread_attr =
        disp->_thread_attr | BTHREAD_NEVER_QUIT | BTHREAD_GLOBAL_PRIORITY;

    int rc = bthread_start_background(&disp->_tid, &io_uring_thread_attr, EventDispatcher::RunThis, disp);
    if (rc) {
        LOG(ERROR) << "Fail to create io_uring thread: " << berror(rc);
        return -1;
    }
    return 0;
}

void Stop(EventDispatcher* disp) {
    disp->_stop = true;

    if (disp->_event_dispatcher_fd >= 0 && disp->_wakeup_fds[1] >= 0) {
        IoUringContext* ctx = GetCtx(disp);
        if (ctx && ctx->initialized) {
            struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
            if (sqe) {
                io_uring_prep_poll_add(sqe, disp->_wakeup_fds[1], POLLOUT);
                sqe->user_data = IOURING_INTERNAL_EVENT;
                ctx->Submit();
            }
        }
    }
}

int AddConsumer(EventDispatcher* disp, IOEventDataId event_data_id, int fd) {
    IoUringContext* ctx = GetCtx(disp);
    if (!ctx || !ctx->initialized) {
        errno = EINVAL;
        return -1;
    }

    if (fd < 0) {
        return -1;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        ctx->Submit();
        sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {
            LOG(ERROR) << "Failed to get SQE";
            return -1;
        }
    }

    io_uring_prep_poll_add(sqe, fd, POLLIN | EPOLLET);
    sqe->user_data = event_data_id;

    ctx->fd_info_map[event_data_id] = IoUringFdInfo(fd, POLLIN | EPOLLET);

    int ret = ctx->Submit();
    if (ret < 0 && !ctx->sqpoll) {
        LOG(ERROR) << "Failed to submit poll_add: " << strerror(-ret);
        return -1;
    }

    return 0;
}

int RemoveConsumer(EventDispatcher* disp, int fd) {
    if (fd < 0) {
        return -1;
    }

    IoUringContext* ctx = GetCtx(disp);
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    IOEventDataId event_data_id_to_remove = 0;
    bool found = false;

    for (auto it = ctx->fd_info_map.begin(); it != ctx->fd_info_map.end(); ++it) {
        if (it->second.fd == fd) {
            event_data_id_to_remove = it->first;
            ctx->fd_info_map.erase(it);
            found = true;
            break;
        }
    }

    if (!found) {
        return 0;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        ctx->Submit();
        sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {
            LOG(WARNING) << "Failed to get SQE for poll remove";
            return -1;
        }
    }

    io_uring_prep_poll_remove(sqe, (unsigned long long)event_data_id_to_remove);
    sqe->user_data = IOURING_INTERNAL_EVENT;

    int ret = ctx->Submit();
    if (ret < 0 && !ctx->sqpoll) {
        LOG(WARNING) << "Failed to submit poll_remove: " << strerror(-ret);
        return -1;
    }

    return 0;
}

int RegisterEvent(EventDispatcher* disp, IOEventDataId event_data_id, int fd, bool pollin) {
    IoUringContext* ctx = GetCtx(disp);
    if (!ctx || !ctx->initialized) {
        errno = EINVAL;
        return -1;
    }

    if (fd < 0) {
        return -1;
    }

    uint32_t events = POLLOUT | EPOLLET;
    if (pollin) {
        events |= POLLIN;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        ctx->Submit();
        sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {
            LOG(ERROR) << "Failed to get SQE for register event";
            return -1;
        }
    }

    io_uring_prep_poll_add(sqe, fd, events);
    sqe->user_data = event_data_id;

    ctx->fd_info_map[event_data_id] = IoUringFdInfo(fd, events);

    int ret = ctx->Submit();
    if (ret < 0 && !ctx->sqpoll) {
        LOG(ERROR) << "Failed to submit register event: " << strerror(-ret);
        return -1;
    }

    return 0;
}

int UnregisterEvent(EventDispatcher* disp, IOEventDataId event_data_id, int fd, bool pollin) {
    IoUringContext* ctx = GetCtx(disp);
    if (!ctx || !ctx->initialized) {
        errno = EINVAL;
        return -1;
    }

    if (fd < 0) {
        return -1;
    }

    if (pollin) {
        auto it = ctx->fd_info_map.find(event_data_id);
        if (it != ctx->fd_info_map.end()) {
            it->second.events = POLLIN | EPOLLET;
        }
        return 0;
    } else {
        return RemoveConsumer(disp, fd);
    }
}

void Run(EventDispatcher* disp) {
    IoUringContext* ctx = GetCtx(disp);
    if (!ctx || !ctx->initialized) {
        LOG(ERROR) << "io_uring context not initialized";
        return;
    }

    while (!disp->_stop) {
        int ret = ctx->SubmitAndWait(1);

        if (disp->_stop) {
            break;
        }

        if (ret < 0) {
            if (ret == -EINTR) {
                continue;
            }
            PLOG(ERROR) << "io_uring wait failed";
            break;
        }

        unsigned head;
        unsigned count = 0;
        struct io_uring_cqe* cqe;

        io_uring_for_each_cqe(&ctx->ring, head, cqe) {
            count++;

            IOEventDataId event_data_id = cqe->user_data;
            int32_t res = cqe->res;

            if (event_data_id == IOURING_INTERNAL_EVENT) {
                continue;
            }

            if (res < 0) {
                if (res == -EBADF || res == -ENOENT) {
                    ctx->fd_info_map.erase(event_data_id);
                } else if (res != -ECANCELED) {
                    LOG(WARNING) << "io_uring poll event failed: " << strerror(-res);
                }
                continue;
            }

            uint32_t events = static_cast<uint32_t>(res);

            if (events & (POLLIN | POLLERR | POLLHUP)) {
                int64_t start_ns = butil::cpuwide_time_ns();
                EventDispatcher::CallInputEventCallback(event_data_id, events, disp->_thread_attr);
                (*g_edisp_read_lantency) << (butil::cpuwide_time_ns() - start_ns);
            }

            if (events & (POLLOUT | POLLERR | POLLHUP)) {
                int64_t start_ns = butil::cpuwide_time_ns();
                EventDispatcher::CallOutputEventCallback(event_data_id, events, disp->_thread_attr);
                (*g_edisp_write_lantency) << (butil::cpuwide_time_ns() - start_ns);
            }

            auto it = ctx->fd_info_map.find(event_data_id);
            if (it != ctx->fd_info_map.end()) {
                struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
                if (sqe) {
                    io_uring_prep_poll_add(sqe, it->second.fd, it->second.events);
                    sqe->user_data = event_data_id;
                }
            }
        }

        if (count > 0) {
            io_uring_cq_advance(&ctx->ring, count);
            ctx->Submit();
        }
    }
}

} // namespace iouring_backend
} // namespace brpc
