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

#if BRPC_WITH_IOURING

#include <errno.h>
#include <liburing.h>
#include <sys/uio.h>
#include <algorithm>
#include <limits.h>
#include <unordered_set>
#include <string.h>
#include <utility>

#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include "butil/fd_utility.h"
#include "butil/logging.h"
#include "butil/macros.h"
#include "butil/third_party/murmurhash3/murmurhash3.h"
#include "bthread/bthread.h"
#include "brpc/event_dispatcher.h"
#include "brpc/input_messenger.h"
#include "brpc/socket.h"
#include "brpc/reloadable_flags.h"
#include "brpc/iouring_transport.h"
#include "brpc/iouring/iouring_block_pool.h"
#include "brpc/iouring/iouring_helper.h"
#include "brpc/iouring/iouring_endpoint.h"

DECLARE_int32(task_group_ntags);

namespace brpc {
namespace iouring {

// ---------------------------------------------------------------------------
// gflags (endpoint-level tunables)
// ---------------------------------------------------------------------------

// Each bthread_tag has exactly one Poller bthread (and one io_uring ring).
// io_uring's SQ is single-producer; bthread work-stealing means multiple
// Pollers per tag could run on different pthreads and race on the SQ.
// Horizontal scaling is achieved by increasing --task_group_ntags instead.

DEFINE_bool(iouring_poller_yield, false,
            "Yield (bthread_yield / sched_yield) after each poll iteration. "
            "Reduces CPU usage at the cost of higher tail latency.");

DEFINE_int32(iouring_max_cqe_poll_once, 32,
             "Maximum CQEs reaped per io_uring_peek_batch_cqe() call.");

static const int32_t MAX_INFLIGHT_WRITES = 64;

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

IouringEndpoint::IouringEndpoint(Socket* s)
    : _socket(s)
    , _inflight_writes(0)
{
    _read_slot = {};
}

IouringEndpoint::~IouringEndpoint() {
    Reset();
}

void IouringEndpoint::Reset() {
    DeallocateResources();
    _inflight_writes.store(0, butil::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Resource management
// ---------------------------------------------------------------------------

int IouringEndpoint::AllocateResources(int fd) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (!BindPoller()) {
        errno = ENODEV;
        return -1;
    }
    // Register this socket with the Poller via the MPSC op_queue.
    // The Poller thread will dequeue the ADD message on its next iteration
    // and issue the first SubmitRead there – on the Poller thread, without
    // any locking.
    //
    // Registered-buffer mode: acquire a fixed read slot on the Poller thread
    // when processing the ADD message (see main loop).  Nothing to do here.
    PollerAddSid(fd);
    return 0;
}

void IouringEndpoint::DeallocateResources() {
    // Embed the read slot (if any) in the REMOVE message so the Poller thread
    // can Release it without any locking – all slot_pool operations happen on
    // the Poller thread.
    PollerRemoveSid(_read_slot);
    _read_slot = {};
}

// ---------------------------------------------------------------------------
// Ring / Poller access
// ---------------------------------------------------------------------------

IouringEndpoint::Poller* IouringEndpoint::GetPoller() const {
    if (!_poller_bound) { return nullptr; }
    if (_poller_tag < 0 ||
        _poller_tag >= static_cast<int>(_poller_groups.size())) {
        return nullptr;
    }
    auto& pollers = _poller_groups[_poller_tag].pollers;
    if (_poller_index < 0 ||
        _poller_index >= static_cast<int>(pollers.size())) {
        return nullptr;
    }
    if (!pollers[_poller_index].ring_initialized) { return nullptr; }
    return &pollers[_poller_index];
}

// ---------------------------------------------------------------------------
// IouringPollerHandle – implementation
//
// ring() and Submit() are implemented here (not in the header) because they
// need access to IouringEndpoint::Poller and IouringEndpoint::_poller_groups,
// which are private and not yet fully defined at the point where
// IouringPollerHandle is declared in iouring_helper.h.
// ---------------------------------------------------------------------------

int IouringPollerHandle::Submit(
        std::function<int(::io_uring*)> prepare_fn) const {
    if (tag_ < 0 || tag_ >= static_cast<int>(
            IouringEndpoint::_poller_groups.size())) {
        errno = ENODEV;
        return -1;
    }
    auto& pollers = IouringEndpoint::_poller_groups[tag_].pollers;
    if (index_ < 0 || index_ >= static_cast<int>(pollers.size())) {
        errno = ENODEV;
        return -1;
    }
    IouringEndpoint::Poller& poller = pollers[index_];
    if (!poller.ring_initialized) {
        errno = ENODEV;
        return -1;
    }
    // Called on the Poller thread (passive path only); no lock needed.
    int n = prepare_fn(&poller.ring);
    if (n < 0) { errno = EBUSY; return -1; }
    if (n == 0) { return 0; }
    int ret = io_uring_submit(&poller.ring);
    if (ret < 0) { errno = -ret; return -1; }
    return ret;
}

// ---------------------------------------------------------------------------
// SubmitOneSqe
//
// Must be called on the Poller thread; no locking.
// Gets one SQE, calls |prepare_fn(sqe)|, issues io_uring_submit().
// Returns io_uring_submit() result (>= 0) or -1 (errno set).
//   errno=ENOBUFS  → SQ full
//   errno=ENODEV   → ring not yet initialised
// ---------------------------------------------------------------------------

int IouringEndpoint::SubmitOneSqe(
        std::function<void(struct io_uring_sqe*)> prepare_fn) {
    Poller* poller = GetPoller();
    if (!poller) { errno = ENODEV; return -1; }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&poller->ring);
    if (!sqe) { errno = ENOBUFS; return -1; }
    prepare_fn(sqe);
    int ret = io_uring_submit(&poller->ring);
    if (ret < 0) { errno = -ret; return -1; }
    return ret;
}

// ---------------------------------------------------------------------------
// Ring parameters factory
// ---------------------------------------------------------------------------

struct io_uring_params IouringEndpoint::BuildRingParams() {
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));

    const IouringPollingMode mode = GetPollingMode();

    switch (mode) {
    case IouringPollingMode::SQPOLL:
    case IouringPollingMode::HYBRID:
        p.flags |= IORING_SETUP_SQPOLL;
        if (FLAGS_iouring_sqpoll_idle_ms > 0) {
            p.sq_thread_idle = static_cast<unsigned>(FLAGS_iouring_sqpoll_idle_ms);
        }
        if (FLAGS_iouring_sqpoll_cpu >= 0) {
            p.flags |= IORING_SETUP_SQ_AFF;
            p.sq_thread_cpu = static_cast<unsigned>(FLAGS_iouring_sqpoll_cpu);
        }
        break;
    case IouringPollingMode::IOPOLL:
        p.flags |= IORING_SETUP_IOPOLL;
        break;
    case IouringPollingMode::NONE:
    default:
        break;
    }

    if (FLAGS_iouring_cq_size > 0) {
        p.flags |= IORING_SETUP_CQSIZE;
        p.cq_entries = static_cast<unsigned>(FLAGS_iouring_cq_size);
    }

    return p;
}

// ---------------------------------------------------------------------------
// SubmitRead
//
// Registered mode  (--iouring_register_buffers=true):
//   IORING_OP_READ_FIXED into _read_slot.buf / _read_slot.buf_index.
//   _read_slot is always valid here (AllocateResources guarantees it).
//
// Unregistered mode (--iouring_register_buffers=false):
//   IORING_OP_READ into a per-call malloc bounce buffer (64 KiB).
//   The buffer is owned by IOBuf after PollCq and free()'d when consumed.
// ---------------------------------------------------------------------------

int IouringEndpoint::SubmitRead(int fd) {
    IouringReqContext* ctx = new IouringReqContext{};
    ctx->fd        = fd;
    ctx->socket_id = _socket->id();

    if (IsFixedBuffersEnabled()) {
        // Registered path – _read_slot is always populated.
        ctx->op = IOURING_OP_READ_FIXED;
        const IouringReadSlot slot = _read_slot;
        int ret = SubmitOneSqe([&](struct io_uring_sqe* sqe) {
            io_uring_prep_read_fixed(sqe, fd,
                                     slot.buf,
                                     static_cast<unsigned>(slot.size),
                                     /*offset=*/0,
                                     slot.buf_index);
            sqe->user_data = reinterpret_cast<uint64_t>(ctx) | kBrpcCqeTag;
        });
        if (ret < 0) { delete ctx; return -1; }
        return 0;
    }

    // Unregistered path – allocate a temporary bounce buffer.
    constexpr size_t kBounceSize = 65536;
    void* bounce = malloc(kBounceSize);
    if (!bounce) { delete ctx; errno = ENOMEM; return -1; }
    ctx->op = IOURING_OP_READ;

    int ret = SubmitOneSqe([&](struct io_uring_sqe* sqe) {
        io_uring_prep_read(sqe, fd, bounce, static_cast<unsigned>(kBounceSize),
                           /*offset=*/0);
        sqe->user_data = reinterpret_cast<uint64_t>(ctx) | kBrpcCqeTag;
    });
    if (ret < 0) { free(bounce); delete ctx; return -1; }
    ctx->bounce = bounce;  // PollCq takes ownership and wraps it in IOBuf
    return 0;
}


ssize_t IouringEndpoint::CutFromIOBufList(butil::IOBuf** from, size_t ndata) {
    CHECK(from != nullptr);
    CHECK(ndata > 0);

    if (!IsWritable()) { errno = EAGAIN; return -1; }

    IouringReqContext* ctx = new IouringReqContext{};
    ctx->op = IsFixedBuffersEnabled() ? IOURING_OP_WRITE_FIXED
                                      : IOURING_OP_WRITE;
    ctx->fd = _socket->fd();
    ctx->socket_id = _socket->id();

    size_t total_bytes = 0;
    for (size_t i = 0; i < ndata; ++i) {
        if (!from[i] || from[i]->empty()) { continue; }
        const size_t n = from[i]->size();
        from[i]->append_to(&ctx->write_buf, n);
        total_bytes += n;
    }
    if (total_bytes == 0) {
        delete ctx;
        return 0;
    }
    const size_t return_bytes = total_bytes;
    ctx->write_total = return_bytes;

    if (EnqueueWrite(ctx) < 0) {
        const int saved_errno = errno;
        delete ctx;
        errno = saved_errno;
        return -1;
    }

    for (size_t i = 0; i < ndata && total_bytes > 0; ++i) {
        if (!from[i] || from[i]->empty()) { continue; }
        const size_t n = std::min(from[i]->size(), total_bytes);
        from[i]->pop_front(n);
        total_bytes -= n;
    }
    return static_cast<ssize_t>(return_bytes);
}

bool IouringEndpoint::IsWritable() const {
    return _inflight_writes.load(butil::memory_order_relaxed) < MAX_INFLIGHT_WRITES;
}

int IouringEndpoint::BuildWriteIovecs(IouringReqContext* ctx) {
    static const size_t IOURING_IOV_MAX = 256;
    ctx->iov.clear();
    ctx->submitted_len = 0;

    size_t skip = ctx->write_offset;
    const size_t nblocks = ctx->write_buf.backing_block_num();
    for (size_t i = 0; i < nblocks && ctx->iov.size() < IOURING_IOV_MAX; ++i) {
        butil::StringPiece blk = ctx->write_buf.backing_block(i);
        if (skip >= blk.size()) {
            skip -= blk.size();
            continue;
        }
        const char* data = blk.data() + skip;
        size_t len = blk.size() - skip;
        if (len > static_cast<size_t>(UINT_MAX)) {
            len = static_cast<size_t>(UINT_MAX);
        }
        ctx->iov.push_back({const_cast<char*>(data), len});
        ctx->submitted_len += len;
        skip = 0;
    }
    return ctx->iov.empty() ? -1 : 0;
}

int IouringEndpoint::SubmitWriteContext(Poller* poller,
                                        IouringEndpoint* ep,
                                        IouringReqContext* ctx) {
    if (!poller || !poller->ring_initialized || !ep || !ctx) {
        errno = ENODEV;
        return -1;
    }

    if (ctx->op == IOURING_OP_WRITE_FIXED) {
        size_t skip = ctx->write_offset;
        const size_t nblocks = ctx->write_buf.backing_block_num();
        for (size_t i = 0; i < nblocks; ++i) {
            butil::StringPiece blk = ctx->write_buf.backing_block(i);
            if (skip >= blk.size()) {
                skip -= blk.size();
                continue;
            }
            void* data = const_cast<char*>(blk.data() + skip);
            size_t len = blk.size() - skip;
            if (len > static_cast<size_t>(UINT_MAX)) {
                len = static_cast<size_t>(UINT_MAX);
            }
            const int buf_idx =
                IouringMemPool::Instance().GetBufIndex(&poller->ring, data);
            if (buf_idx < 0) {
                errno = EINVAL;
                return -1;
            }
            struct io_uring_sqe* sqe = io_uring_get_sqe(&poller->ring);
            if (!sqe) { errno = ENOBUFS; return -1; }
            ctx->submitted_len = len;
            io_uring_prep_write_fixed(sqe, ctx->fd, data,
                                      static_cast<unsigned>(len),
                                      0, buf_idx);
            sqe->user_data = reinterpret_cast<uint64_t>(ctx) | kBrpcCqeTag;
            const int ret = io_uring_submit(&poller->ring);
            if (ret < 0) { errno = -ret; return -1; }
            return 0;
        }
        errno = EINVAL;
        return -1;
    }

    if (BuildWriteIovecs(ctx) < 0) {
        errno = EINVAL;
        return -1;
    }
    struct io_uring_sqe* sqe = io_uring_get_sqe(&poller->ring);
    if (!sqe) { errno = ENOBUFS; return -1; }
    io_uring_prep_writev(sqe, ctx->fd, ctx->iov.data(),
                         static_cast<unsigned>(ctx->iov.size()), 0);
    sqe->user_data = reinterpret_cast<uint64_t>(ctx) | kBrpcCqeTag;
    const int ret = io_uring_submit(&poller->ring);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

bool IouringEndpoint::PollerDrainWriteQueue(Poller* poller) {
    bool progress = false;
    IouringReqContext* ctx = nullptr;
    while (poller->write_queue.Dequeue(ctx)) {
        progress = true;
        SocketUniquePtr s;
        IouringEndpoint* ep = nullptr;
        if (Socket::Address(ctx->socket_id, &s) == 0) {
            IouringTransport* transport =
                static_cast<IouringTransport*>(s->_transport.get());
            ep = transport ? transport->_iouring_ep : nullptr;
        }
        if (!s || !ep) {
            delete ctx;
            continue;
        }
        if (SubmitWriteContext(poller, ep, ctx) < 0) {
            if (errno == ENOBUFS) {
                poller->write_queue.Enqueue(ctx);
                break;
            }
            const int saved_errno = errno;
            s->SetFailed(saved_errno, "io_uring write submit failed: %s",
                         berror(saved_errno));
            ep->_inflight_writes.fetch_sub(1, butil::memory_order_relaxed);
            ep->_socket->WakeAsEpollOut();
            delete ctx;
        }
    }
    return progress;
}

int IouringEndpoint::EnqueueWrite(IouringReqContext* ctx) {
    Poller* poller = GetPoller();
    if (!poller) { errno = ENODEV; return -1; }
    _inflight_writes.fetch_add(1, butil::memory_order_relaxed);
    poller->write_queue.Enqueue(ctx);
    return 0;
}

// ---------------------------------------------------------------------------
// PollCq – CQE completion handler
//
// READ_FIXED path (--iouring_register_buffers=true)
// -------------------------------------------------
// The kernel has written |res| bytes directly into ep->_read_slot.buf (a
// pre-registered, pinned page).  PollCq copies those bytes into the Socket
// read buffer, returns the slot to the Poller-owned slot_pool immediately,
// and queues the next READ_FIXED.
//
// READ path (--iouring_register_buffers=false)
// --------------------------------------------
// ctx->bounce points to a per-call malloc'd bounce buffer (64 KiB).
// IOBuf takes ownership (free() destructor) when the data is wrapped.
// ---------------------------------------------------------------------------

void IouringEndpoint::PollCq(Poller* poller) {
    if (!poller || !poller->ring_initialized) { return; }
    struct io_uring* ring = &poller->ring;
    struct io_uring_cqe* cqes[FLAGS_iouring_max_cqe_poll_once];

    while (true) {
        const int cnt = io_uring_peek_batch_cqe(
            ring, cqes, static_cast<unsigned>(FLAGS_iouring_max_cqe_poll_once));
        if (cnt <= 0) { return; }

        bool consumed_brpc_cqe = false;
        for (int i = 0; i < cnt; ++i) {
            struct io_uring_cqe* cqe = cqes[i];
            const uint64_t udata = cqe->user_data;

            if (!(udata & kBrpcCqeTag)) {
                continue;
            }

            IouringReqContext* ctx =
                reinterpret_cast<IouringReqContext*>(
                    static_cast<uintptr_t>(udata & ~kBrpcCqeTag));
            const int res = cqe->res;
            io_uring_cqe_seen(ring, cqe);
            consumed_brpc_cqe = true;
            if (!ctx) { continue; }

            SocketUniquePtr s;
            IouringEndpoint* ep = nullptr;
            if (Socket::Address(ctx->socket_id, &s) == 0) {
                IouringTransport* transport =
                    static_cast<IouringTransport*>(s->_transport.get());
                ep = transport ? transport->_iouring_ep : nullptr;
            }

            if (res < 0) {
                if (res != -ECANCELED) {
                    const int saved_errno = -res;
                    LOG(WARNING) << "io_uring CQE error fd=" << ctx->fd
                                 << " socket_id=" << ctx->socket_id
                                 << " op=" << (int)ctx->op
                                 << ": " << berror(saved_errno);
                    if (s) {
                        s->SetFailed(saved_errno, "io_uring op error: %s",
                                     berror(saved_errno));
                    }
                }
                if (ctx->op == IOURING_OP_READ) { free(ctx->bounce); }
                if (ctx->op == IOURING_OP_WRITE ||
                    ctx->op == IOURING_OP_WRITE_FIXED) {
                    if (ep) {
                        ep->_inflight_writes.fetch_sub(
                            1, butil::memory_order_relaxed);
                        ep->_socket->WakeAsEpollOut();
                    }
                }
                delete ctx;
                continue;
            }

            if (ctx->op == IOURING_OP_READ || ctx->op == IOURING_OP_READ_FIXED) {
                if (!s || !ep) {
                    if (ctx->op == IOURING_OP_READ) { free(ctx->bounce); }
                    delete ctx;
                    continue;
                }

                if (res == 0) {
                    s->SetEOF();
                    if (ctx->op == IOURING_OP_READ) { free(ctx->bounce); }
                    delete ctx;
                    continue;
                }

                if (ctx->op == IOURING_OP_READ_FIXED) {
                    IouringReadSlot consumed_slot = ep->_read_slot;
                    ep->_read_slot = {};
                    if (consumed_slot.buf) {
                        s->_read_buf.append(consumed_slot.buf,
                                            static_cast<size_t>(res));
                        if (poller->slot_pool.initialized()) {
                            poller->slot_pool.Release(consumed_slot);
                            if (!poller->slot_pool.Acquire(&ep->_read_slot)) {
                                s->SetFailed(ENOMEM,
                                    "io_uring slot pool exhausted");
                            }
                        } else {
                            s->SetFailed(ENODEV,
                                         "io_uring READ_FIXED slot pool "
                                         "is not initialized");
                        }
                    } else {
                        s->SetFailed(EINVAL,
                                     "io_uring READ_FIXED completed without "
                                     "a read slot");
                    }
                } else {
                    butil::IOBuf tmp;
                    tmp.append_user_data(ctx->bounce,
                                         static_cast<size_t>(res),
                                         free);
                    ctx->bounce = nullptr;
                    s->_read_buf.append(std::move(tmp));
                }

                if (!s->Failed()) {
                    ep->SubmitRead(ctx->fd);
                    const int64_t received_us = butil::cpuwide_time_us();
                    const int64_t base_realtime =
                        butil::gettimeofday_us() - received_us;
                    InputMessageClosure last_msg;
                    InputMessenger* messenger =
                        static_cast<InputMessenger*>(s->user());
                    if (messenger && messenger->ProcessNewMessage(
                                s.get(), res, false, received_us,
                                base_realtime, last_msg) < 0) {
                        delete ctx;
                        continue;
                    }
                }
                delete ctx;
                continue;
            }

            if (!ep) {
                delete ctx;
                continue;
            }

            if (res == 0) {
                s->SetFailed(EPIPE, "io_uring write returned 0");
                ep->_inflight_writes.fetch_sub(1, butil::memory_order_relaxed);
                ep->_socket->WakeAsEpollOut();
                delete ctx;
                continue;
            }

            ctx->write_offset += static_cast<size_t>(res);
            if (ctx->write_offset < ctx->write_total) {
                if (SubmitWriteContext(poller, ep, ctx) < 0) {
                    if (errno == ENOBUFS) {
                        poller->write_queue.Enqueue(ctx);
                    } else {
                        const int saved_errno = errno;
                        s->SetFailed(saved_errno,
                                     "io_uring write resubmit failed: %s",
                                     berror(saved_errno));
                        ep->_inflight_writes.fetch_sub(
                            1, butil::memory_order_relaxed);
                        ep->_socket->WakeAsEpollOut();
                        delete ctx;
                    }
                }
                continue;
            }

            ep->_inflight_writes.fetch_sub(1, butil::memory_order_relaxed);
            ep->_socket->WakeAsEpollOut();
            delete ctx;
        }
        if (!consumed_brpc_cqe) { return; }
    }
}

// ---------------------------------------------------------------------------
// DebugInfo
// ---------------------------------------------------------------------------

void IouringEndpoint::DebugInfo(std::ostream& os,
                                butil::StringPiece connector) const {
    os << "iouring_polling_mode=" << FLAGS_iouring_polling_mode
       << connector
       << "iouring_inflight_writes="
       << _inflight_writes.load(butil::memory_order_relaxed)
       << connector << "iouring_writable=" << IsWritable()
       << connector << "iouring_register_buffers=" << IsFixedBuffersEnabled();
    if (IsFixedBuffersEnabled() && _read_slot.buf != nullptr) {
        os << " buf_index=" << _read_slot.buf_index
           << " slot_size="  << _read_slot.size;
    }
}

// ---------------------------------------------------------------------------
// GlobalInitialize / GlobalRelease
// ---------------------------------------------------------------------------

int IouringEndpoint::GlobalInitialize() {
    _poller_groups = std::vector<PollerGroup>(FLAGS_task_group_ntags);
    return 0;
}

void IouringEndpoint::GlobalRelease() {
    for (int i = 0; i < FLAGS_task_group_ntags; ++i) {
        PollingModeRelease(i);
    }
}

// ---------------------------------------------------------------------------
// PollerDrainOpQueue
//
// Dequeues all pending SidOps from poller->op_queue and applies them:
//   ADD    – track the socket, acquire a read slot (fixed mode), issue first read.
//   REMOVE – release the read slot (fixed mode), stop tracking the socket.
//
// Must run exclusively on the Poller thread so that slot_pool is accessed
// without any locking.
// ---------------------------------------------------------------------------

void IouringEndpoint::PollerDrainOpQueue(
        Poller* poller,
        std::unordered_set<SocketId>& tracked_sids) {
    SidOp op;
    while (poller->op_queue.Dequeue(op)) {
        if (op.type == SidOp::ADD) {
            tracked_sids.emplace(op.sid);
            SocketUniquePtr s_add;
            if (Socket::Address(op.sid, &s_add) == 0) {
                if (s_add->fd() != op.fd) {
                    if (!s_add->Failed()) {
                        poller->op_queue.Enqueue(op);
                        bthread_yield();
                    } else {
                        tracked_sids.erase(op.sid);
                    }
                    continue;
                }
                IouringTransport* transport =
                    static_cast<IouringTransport*>(s_add->_transport.get());
                IouringEndpoint* ep =
                    transport ? transport->_iouring_ep : nullptr;
                if (ep) {
                    // Acquire a fixed read slot for this connection.
                    if (IsFixedBuffersEnabled()) {
                        if (!poller->slot_pool.initialized() ||
                            !poller->slot_pool.Acquire(&ep->_read_slot)) {
                            LOG(ERROR)
                                << "IouringEndpoint: slot pool "
                                   "exhausted for socket "
                                << op.sid << "; dropping connection.";
                            tracked_sids.erase(op.sid);
                            s_add->SetFailed(ENOMEM,
                                "io_uring slot pool exhausted");
                            continue;
                        }
                    }
                    // Issue the first SubmitRead on the Poller thread.
                    if (ep->SubmitRead(op.fd) < 0) {
                        LOG(WARNING)
                            << "IouringEndpoint: first SubmitRead "
                               "failed for socket "
                            << op.sid << ": " << berror();
                    }
                }
            } else {
                tracked_sids.erase(op.sid);
            }
        } else {
            // REMOVE: release the read slot (if any) back to the pool.
            if (IsFixedBuffersEnabled() && op.read_slot.buf != nullptr) {
                if (poller->slot_pool.initialized()) {
                    poller->slot_pool.Release(op.read_slot);
                }
            }
            tracked_sids.erase(op.sid);
        }
    }
}

// ---------------------------------------------------------------------------
// PollingModeInitialize
// ---------------------------------------------------------------------------

std::vector<IouringEndpoint::PollerGroup> IouringEndpoint::_poller_groups;

int IouringEndpoint::PollingModeInitialize(
        bthread_tag_t tag,
        std::function<void(IouringPollerHandle)> callback,
        std::function<void(IouringPollerHandle)> init_fn,
        std::function<void(IouringPollerHandle)> release_fn) {

    if (tag < 0 || tag >= static_cast<int>(_poller_groups.size())) {
        LOG(ERROR) << "io_uring: invalid bthread tag " << tag;
        return -1;
    }

    auto& group   = _poller_groups[tag];
    auto& pollers = group.pollers;
    auto& running = group.running;

    bool expected = false;
    if (!running.compare_exchange_strong(expected, true)) { return 0; }

    // -----------------------------------------------------------------------
    // Poller thread arguments
    // -----------------------------------------------------------------------
    struct FnArgs {
        Poller*            poller;
        std::atomic<bool>* running;
        bthread_tag_t      tag;
        int                index;   // poller index within the group
    };

    // -----------------------------------------------------------------------
    // Poller thread body
    // -----------------------------------------------------------------------
    auto fn = [](void* p) -> void* {
        std::unique_ptr<FnArgs> args(static_cast<FnArgs*>(p));
        Poller*            poller  = args->poller;
        std::atomic<bool>* running = args->running;

        // 1. Create the ring.
        struct io_uring_params params = BuildRingParams();
        const unsigned sq_size = static_cast<unsigned>(GetIouringSqSize());

        int ret = io_uring_queue_init_params(sq_size, &poller->ring, &params);
        if (ret < 0) {
            LOG(ERROR) << "io_uring_queue_init_params failed: " << berror(-ret);
            running->store(false, std::memory_order_relaxed);
            return nullptr;
        }
        poller->ring_initialized = true;

        // 2. Initialise the fixed-buffer infrastructure.
        if (IsFixedBuffersEnabled()) {
            // 2a. Register this ring with IouringMemPool.
            // The callback is called for each existing region immediately
            // (to bring the ring up to date) and for each future region
            // (when the pool grows).  It issues register_buffers_update /
            // full re-registration to pin the new pages in this ring.
            IouringMemPool::Instance().AddRingRegistrar(
                &poller->ring,
                [poller](void* base, size_t size, size_t block_size,
                         int buf_index_base) {
                    // Build one iovec per block in this new region.
                    const int n = static_cast<int>(size / block_size);
                    std::vector<struct iovec> iovs(n);
                    for (int i = 0; i < n; ++i) {
                        iovs[i].iov_base =
                            static_cast<char*>(base) + i * block_size;
                        iovs[i].iov_len  = block_size;
                    }
                    // Register the new region's buffers.
                    // io_uring_register_buffers_update (kernel >= 5.13) allows
                    // incremental updates; fall back to a full re-registration
                    // on older kernels or if the function is unavailable.
                    int ret = -ENOSYS;
#ifdef IORING_REGISTER_BUFFERS_UPDATE
                    ret = io_uring_register_buffers_update(
                        &poller->ring,
                        static_cast<unsigned>(buf_index_base),
                        iovs.data(),
                        static_cast<unsigned>(n));
#endif
                    if (ret < 0) {
                        // Full re-registration path.
                        // For the first region just call register_buffers.
                        // For subsequent regions we must rebuild the complete
                        // table; here we only have the new slice so we
                        // attempt a best-effort register and log on failure.
                        if (buf_index_base > 0) {
                            // Unregister previous table before re-registering.
                            io_uring_unregister_buffers(&poller->ring);
                        }
                        int r2 = io_uring_register_buffers(&poller->ring,
                                                           iovs.data(),
                                                           static_cast<unsigned>(n));
                        if (r2 < 0) {
                            LOG(WARNING)
                                << "io_uring_register_buffers failed for new "
                                   "region (buf_index_base=" << buf_index_base
                                << "): " << berror(-r2)
                                << " – WRITE_FIXED will fall back to WRITEV.";
                        }
                    }
                });

            // 2b. Initialise the per-ring read-slot pool.
            // Slot size matches the IOBuf block size so slots come from the
            // same registered slab and share the write-path registration.
            const int   initial = FLAGS_iouring_read_slot_num;
            const int   max     = FLAGS_iouring_read_slot_max;
            const size_t sz     =
                static_cast<size_t>(FLAGS_iouring_iobuf_block_size);

            if (!poller->slot_pool.Init(&poller->ring, initial, max, sz)) {
                LOG(ERROR) << "io_uring slot pool init failed; "
                              "io_uring disabled (--iouring_register_buffers=true "
                              "requires a working slot pool).";
                running->store(false, std::memory_order_relaxed);
                // Exit the poller lambda so the ring is torn down.
                return nullptr;
            }
            LOG(INFO) << "io_uring read slot pool ready: initial=" << initial
                      << " max=" << max << " slot_buf_size=" << sz;
        }

        if (poller->init_fn) {
            poller->init_fn(IouringPollerHandle(args->tag, args->index));
        }

        // 3. CQE reap strategy.
        const IouringPollingMode mode = GetPollingMode();
        const int hybrid_spins = FLAGS_iouring_hybrid_spin_count;
        struct io_uring_cqe* cqes[FLAGS_iouring_max_cqe_poll_once];  // used by SQPOLL/HYBRID peek only
        std::unordered_set<SocketId> tracked_sids;
        SidOp op;

        // 4. Main loop.
        while (running->load(std::memory_order_relaxed)) {
            // a) Drain op_queue.
            // All slot_pool operations happen here, on the Poller thread,
            // so no locking is ever needed for slot_pool.
            PollerDrainOpQueue(poller, tracked_sids);
            PollerDrainWriteQueue(poller);

            // b) Reap CQEs.
            bool got_cqe = false;  // used by SQPOLL/IOPOLL/HYBRID branches

            if (mode == IouringPollingMode::NONE) {
                // Interrupt-driven mode: block up to 1 ms waiting for a CQE.
                // The 1 ms timeout keeps the Poller loop responsive to new
                // connections arriving in op_queue without burning CPU when
                // there is no I/O traffic.
                struct io_uring_cqe* cqe = nullptr;
                struct __kernel_timespec ts{0, 1000000};  // 1 ms
                io_uring_wait_cqe_timeout(&poller->ring, &cqe, &ts);

            } else if (mode == IouringPollingMode::IOPOLL) {
                // IOPOLL (IORING_SETUP_IOPOLL): the kernel never generates
                // interrupts; CQEs are posted only after an explicit
                // io_uring_submit() triggers the poll.  Use peek to drain
                // whatever is already available, then yield to avoid
                // spinning at 100 % when there is no block I/O in flight.
                int cnt = io_uring_peek_batch_cqe(
                    &poller->ring, cqes,
                    static_cast<unsigned>(FLAGS_iouring_max_cqe_poll_once));
                got_cqe = (cnt > 0);
                if (!got_cqe && FLAGS_iouring_poller_yield) {
                    bthread_yield();
                }

            } else if (mode == IouringPollingMode::SQPOLL) {
                // SQPOLL (IORING_SETUP_SQPOLL): the kernel SQ thread submits
                // I/O automatically; just peek for completed CQEs.
                int cnt = io_uring_peek_batch_cqe(
                    &poller->ring, cqes,
                    static_cast<unsigned>(FLAGS_iouring_max_cqe_poll_once));
                got_cqe = (cnt > 0);
                if (!got_cqe && FLAGS_iouring_poller_yield) {
                    bthread_yield();
                }

            } else {
                // HYBRID: busy-spin N times, then fall back to a zero-timeout
                // wait so the Poller thread blocks rather than burning CPU
                // when no CQE arrives.
                for (int spin = 0; spin < hybrid_spins && !got_cqe; ++spin) {
                    int cnt = io_uring_peek_batch_cqe(
                        &poller->ring, cqes,
                        static_cast<unsigned>(FLAGS_iouring_max_cqe_poll_once));
                    got_cqe = (cnt > 0);
                }
                if (!got_cqe) {
                    struct io_uring_cqe* cqe = nullptr;
                    struct __kernel_timespec ts_zero{0, 0};
                    io_uring_wait_cqe_timeout(&poller->ring, &cqe, &ts_zero);
                }
            }

            // c) Drain this ring's CQ and route each bRPC CQE by socket_id.
            IouringEndpoint::PollCq(poller);

            // d) Optional user callback.  Runs on the Poller thread after
            // every PollCq pass.  The handle gives safe access to ring()
            // for CQ draining and Submit() for SQ submission.
            if (poller->callback) {
                poller->callback(IouringPollerHandle(args->tag, args->index));
            }

            if (FLAGS_iouring_poller_yield &&
                mode != IouringPollingMode::SQPOLL) {
                bthread_yield();
            }
        }  // while running

        // 5. Tear-down.
        if (poller->release_fn) {
            poller->release_fn(IouringPollerHandle(args->tag, args->index));
        }

        if (poller->ring_initialized) {
            // Unregister this ring from the global mem-pool before destroying
            // the ring so no future region growth tries to update a dead ring.
            if (IsFixedBuffersEnabled()) {
                IouringMemPool::Instance().RemoveRingRegistrar(&poller->ring);
            }
            // slot_pool.Destroy() calls io_uring_unregister_buffers before
            // we exit the ring – order matters.
            poller->slot_pool.Destroy();
            io_uring_queue_exit(&poller->ring);
            poller->ring_initialized = false;
        }

        return nullptr;
    };  // lambda

    // Start the single Poller bthread for this tag.
    for (int i = 0; i < 1; ++i) {
        auto* fargs = new FnArgs{&pollers[i], &running, tag, i};
        bthread_attr_t attr = BTHREAD_ATTR_NORMAL;
        attr.tag = tag;
        bthread_attr_set_name(&attr, "IouringPoller");
        pollers[i].callback   = callback;
        pollers[i].init_fn    = init_fn;
        pollers[i].release_fn = release_fn;

        if (bthread_start_background(&pollers[i].tid, &attr, fn, fargs) != 0) {
            LOG(ERROR) << "Fail to start io_uring poller bthread tag=" << tag
                       << " index=" << i;
            delete fargs;
            running.store(false, std::memory_order_relaxed);
            return -1;
        }
    }
    return 0;
}

void IouringEndpoint::PollingModeRelease(bthread_tag_t tag) {
    if (tag < 0 || tag >= static_cast<int>(_poller_groups.size())) { return; }

    auto& group   = _poller_groups[tag];
    auto& pollers = group.pollers;
    auto& running = group.running;

    running.store(false, std::memory_order_relaxed);

    if (pollers[0].tid != INVALID_BTHREAD) {
        bthread_join(pollers[0].tid, nullptr);
        pollers[0].tid = INVALID_BTHREAD;
    }
}

// ---------------------------------------------------------------------------
// PollerAddSid / PollerRemoveSid
// ---------------------------------------------------------------------------

void IouringEndpoint::PollerAddSid(int fd) {
    if (!BindPoller()) { return; }
    auto& pollers = _poller_groups[_poller_tag].pollers;
    pollers[_poller_index].op_queue.Enqueue(
        SidOp{_socket->id(), fd, SidOp::ADD});
}

void IouringEndpoint::PollerRemoveSid(const IouringReadSlot& slot) {
    if (!BindPoller()) { return; }
    auto& pollers = _poller_groups[_poller_tag].pollers;
    pollers[_poller_index].op_queue.Enqueue(
        SidOp{_socket->id(), -1, SidOp::REMOVE, slot});
}

bool IouringEndpoint::BindPoller() {
    if (_poller_bound) { return true; }
    if (_poller_groups.empty()) { return false; }

    bthread_tag_t tag = bthread_self_tag();
    if (tag < 0 || tag >= static_cast<int>(_poller_groups.size())) { tag = 0; }

    auto& pollers = _poller_groups[tag].pollers;
    if (pollers.empty()) { return false; }

    _poller_tag = tag;
    _poller_index = static_cast<int>(butil::fmix32(_socket->id()) %
                                     pollers.size());
    _poller_bound = true;
    return true;
}

}  // namespace iouring
}  // namespace brpc

#endif  // BRPC_WITH_IOURING
