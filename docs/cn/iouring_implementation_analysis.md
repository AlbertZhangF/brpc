# brpc io_uring 实现分析

本文基于最新提交 `89b2765c feat: add io_uring transport layer support`，总结 brpc 新增 io_uring transport 的实现方式、和现有 epoll/TCP 路径的差异，以及它在性能模型上的优势和当前需要关注的风险点。

## 1. 变更范围

本次提交把 io_uring 作为新的 `Transport` 类型接入 brpc，而不是替换原有 TCP/epoll 实现。

主要变更如下：

- 构建系统：`CMakeLists.txt` 增加 `WITH_IOURING`，Bazel 增加 `BRPC_WITH_IOURING` config 和 liburing 依赖。
- transport 枚举：`src/brpc/socket_mode.h` 增加 `SOCKET_MODE_IOURING`。
- transport 工厂：`src/brpc/transport_factory.cpp` 在 `ContextInitOrDie` 和 `CreateTransport` 中注册 `IouringTransport`。
- 核心实现：
  - `src/brpc/iouring_transport.cpp`：实现 brpc `Transport` 接口。
  - `src/brpc/iouring/iouring_endpoint.cpp`：负责 SQE 提交、CQE 收割、读写路径和 Poller 主循环。
  - `src/brpc/iouring/iouring_helper.cpp`：负责全局初始化、能力探测、polling mode 管理。
  - `src/brpc/iouring/iouring_block_pool.cpp`：负责注册内存池和 READ_FIXED slot 池。
- 示例和文档：新增 `example/iouring_echo_c++/` 和 `docs/cn/iouring.md`。

## 2. 总体架构

io_uring 实现沿用了 brpc 现有的 `Socket`、`InputMessenger`、`Transport` 抽象。对上层 RPC 来说，消息解析、协议处理、写队列、bthread 调度仍然复用原逻辑；变化集中在“fd 如何读写”这一层。

核心对象关系：

```text
Server/Channel
  -> Socket(socket_mode = SOCKET_MODE_IOURING)
    -> IouringTransport
      -> IouringEndpoint
        -> per-bthread-tag Poller
          -> io_uring ring
```

每个启用 io_uring 的 `Socket` 持有一个 `IouringEndpoint`。每个 `bthread_tag` 对应一个 `PollerGroup`，当前每个 group 里只有一个 Poller bthread 和一个 io_uring ring。

设计上的关键约束是：所有 SQ 操作都在 Poller bthread 上执行。新连接和连接关闭不直接操作 ring，而是通过 `MPSCQueue<SidOp>` 向 Poller 投递 `ADD` / `REMOVE` 消息，由 Poller 在自己的循环中处理。这是为了避开 io_uring SQ 单生产者模型下的并发提交问题，同时也避免在热路径上给 ring 加锁。

## 3. 初始化和启用入口

编译时需要打开 `BRPC_WITH_IOURING`：

```bash
cmake -DWITH_IOURING=ON ..
```

运行时需要两步：

1. `GlobalIouringInitializeOrDie()`：探测内核 opcode，解析 polling mode，必要时初始化注册内存池。
2. `InitPollingModeWithTag(tag)`：为指定 bthread tag 创建 ring 并启动 Poller bthread。

示例程序在 `Server::Start()` 前显式调用这两个函数，然后设置：

```cpp
options.socket_mode = brpc::SOCKET_MODE_IOURING;
```

需要注意：`ServerOptions` 和 `ChannelOptions` 默认仍是 `SOCKET_MODE_TCP`。如果只编译了 io_uring 但没有设置 `socket_mode`，仍然走原 epoll/TCP 路径。

另一个重要边界是，`IouringTransport::ContextInitOrDie()` 只做全局初始化，不会自动为 server/channel 的 tag 调用 `InitPollingModeWithTag()`。如果设置了 `SOCKET_MODE_IOURING` 但没有启动对应 tag 的 Poller，socket 可能会跳过 epoll read 注册，但 ring 又没有被真正驱动，连接会停住。

## 4. 读路径

### 4.1 epoll/TCP 原路径

原 TCP transport 的读路径是：

```text
fd 加入 EventDispatcher(epoll)
  -> epoll_wait 返回 EPOLLIN
  -> Socket::StartInputEvent / Transport::ProcessEvent
  -> InputMessenger::OnNewMessages
  -> Socket::DoRead(read)
  -> InputMessenger::ProcessNewMessage
```

它依赖 readiness notification。epoll 只告诉用户态“fd 可能可读”，真正读取仍然由用户态 `read()` 完成，并且需要读到 `EAGAIN` 才知道本轮没有更多数据。

### 4.2 io_uring 新路径

io_uring transport 在 `Init()` 时创建 `IouringEndpoint`。如果 io_uring 可用，`IouringTransport` 会清空 `_on_edge_trigger`，使 `Socket::ResetFileDescriptor()` 不再把 fd 注册到 `EventDispatcher` 的 epoll read 集合，避免 epoll 的 `read(fd)` 和 io_uring 的 async read 同时读取同一个 socket。

读路径变成：

```text
IouringEndpoint::AllocateResources
  -> PollerAddSid(ADD)
  -> PollerDrainOpQueue
  -> SubmitRead
  -> kernel async read
  -> CQE
  -> PollCq
  -> append 到 Socket::_read_buf
  -> InputMessenger::ProcessNewMessage
  -> SubmitRead 下一轮
```

`SubmitRead()` 有两种模式：

- 非注册内存模式：提交 `IORING_OP_READ`，目标是每次 `malloc` 的 64 KiB bounce buffer。CQE 返回后把 buffer 交给 `IOBuf::append_user_data()`，由 IOBuf 在消费完成后 `free()`。
- 注册内存模式：提交 `IORING_OP_READ_FIXED`，目标是 Poller ring 私有 `IouringReadSlotPool` 里分配的 read slot。CQE 返回后把 slot 内存零拷贝挂到 `_read_buf`，再立即申请新的 slot 并提交下一次 read。

读完成后，`PollCq()` 复用现有 `InputMessenger::ProcessNewMessage()`，所以协议解析和用户回调路径没有重写。

## 5. 写路径

原 TCP transport 的写路径最终调用：

```text
IOBuf::cut_into_file_descriptor / cut_multiple_into_file_descriptor
  -> write/writev
  -> EAGAIN 时通过 epoll 等 EPOLLOUT
```

io_uring transport 的写路径由 `IouringTransport::CutFromIOBufList()` 转给 `IouringEndpoint::CutFromIOBufList()`：

- 非注册内存模式：把多个 `IOBuf` segment 收集成 `iovec`，提交一个 `IORING_OP_WRITEV`。
- 注册内存模式：每个 IOBuf block 都应来自 `IouringMemPool` 的已注册 slab，逐块提交 `IORING_OP_WRITE_FIXED`。

写完成后，`PollCq()` 会减少 `_inflight_writes`，并调用 `Socket::WakeAsEpollOut()` 唤醒原有写等待逻辑。这样 brpc 的上层写队列仍然可以使用 `_epollout_butex` 这套机制，只是等待的不再是真正的 `EPOLLOUT`，而是 io_uring 写完成事件。

实现里还保留了 TCP fallback：

- `IouringEndpoint` 创建或资源分配失败时，回退到 `TcpTransport`。
- 写路径如果 ring 满或不可写，部分场景会回退到同步 fd 写。
- `_inflight_writes` 默认上限是 64，用于避免无限制提交 write SQE。

## 6. 注册内存和零拷贝

`--iouring_register_buffers=true` 打开固定缓冲区模式。此时实现做了两件事：

1. `IouringMemPool` 接管 `butil::iobuf::blockmem_allocate` / `blockmem_deallocate`，让后续 IOBuf block 都从预注册 slab 分配。
2. 每个 Poller ring 有一个 `IouringReadSlotPool`，read slot 也从同一个注册内存池分配。

内存池的 region 会注册到每个 ring。写路径通过 `GetBufIndex(ptr)` 找到 block 对应的 `buf_index`，然后提交 `WRITE_FIXED`；读路径通过 read slot 的 `buf_index` 提交 `READ_FIXED`。

这样带来的收益是：

- I/O 时不需要每次 pin/unpin 用户页，减少 `get_user_pages` 等内核开销。
- 写路径可以直接从 IOBuf block 对应的 pinned memory 发出。
- 读路径可以把内核写入的 slot 直接挂到 IOBuf，避免从 bounce buffer 再拷贝。

代价是：

- 初始化必须早于普通 IOBuf 分配，否则可能混入非注册 block。
- 内存页会长期 pinned，内存占用和系统限制需要提前规划。
- 当前设计是全局开关，注册模式和非注册模式不混用。

## 7. Poller 和 polling mode

Poller bthread 的主循环大致是：

```text
while running:
  drain ADD/REMOVE op_queue
  wait/peek CQE
  for each tracked socket:
    PollCq(socket)
  run optional user callback
  optional yield
```

支持的模式：

- `none`：默认模式，使用 `io_uring_wait_cqe_timeout`，最多等 1 ms，避免空闲时忙转。
- `sqpoll`：使用 `IORING_SETUP_SQPOLL`，由内核 SQ polling thread 轮询 SQ，降低提交系统调用成本和延迟，需要相应权限。
- `iopoll`：使用 `IORING_SETUP_IOPOLL`，主要面向 O_DIRECT block I/O，不是普通 socket 场景的主要收益点。
- `hybrid`：先忙转若干轮，再退回等待，试图在延迟和 CPU 占用间折中。

## 8. 对比 epoll 的优势

### 8.1 从 readiness 模型变成 completion 模型

epoll 是 readiness notification：内核告诉用户态 fd 可能可读/可写，用户态随后还要调用 `read/write`，并处理 `EAGAIN`、短读短写、重复注册/取消事件等状态。

io_uring 是 completion notification：用户态先提交明确的 read/write 请求，CQE 返回时已经携带本次操作的结果。对 brpc 来说，这让读写路径更接近“提交请求 -> 处理完成”的状态机。

### 8.2 减少系统调用次数

epoll/TCP 路径通常涉及：

- `epoll_wait`
- `read/write/writev`
- 需要等待可写时的 `epoll_ctl`

io_uring 可以通过共享 SQ/CQ 批量提交和批量收割。尤其在 SQPOLL 模式下，提交路径可进一步减少 `io_uring_submit` 系统调用，适合追求低延迟的高并发场景。

### 8.3 降低 epoll_ctl 和事件切换开销

brpc 原实现已经用 edge-trigger 降低 epoll_ctl 压力，但写阻塞时仍需要注册/注销 `EPOLLOUT`。io_uring 写完成通过 CQE 驱动，不需要把 fd 临时加入 epoll 写事件集合，减少了 epoll 红黑树修改、跨线程事件分发和 wakeup 管理成本。

### 8.4 更好的批处理能力

io_uring 天然支持 SQE/CQE 批处理。当前实现里：

- 非注册写路径把多个 IOBuf segment 合成一个 `WRITEV` SQE。
- 注册写路径可以连续提交多个 `WRITE_FIXED` SQE 后统一 `io_uring_submit`。
- Poller 使用 `io_uring_peek_batch_cqe` 批量收割 CQE。

这对高 QPS、小包、多连接场景更友好。

### 8.5 可选固定缓冲区优化

epoll 本身只负责事件通知，不解决每次 I/O 对用户页的 pin/copy 成本。io_uring 的 fixed buffer 能把 IOBuf block 和 read slot 变成长期注册内存，进一步减少内核页管理开销，并支持读路径零拷贝挂载到 IOBuf。

### 8.6 更低延迟模式

`sqpoll` 和 `hybrid` 能用 CPU 换延迟，减少提交到完成之间的调度等待。epoll 也可以配合 busy poll 或自旋策略，但这通常不是 brpc 默认 TCP transport 的模型。

## 9. epoll 仍然更合适的场景

io_uring 不是无条件替代 epoll：

- epoll 更成熟，行为简单，内核版本要求低。
- 默认 epoll 路径空闲 CPU 占用低，不需要 pinned memory，也不需要 SQPOLL 权限。
- io_uring 固定缓冲区模式需要提前规划内存池大小和 block size。
- 当前实现一个 tag 一个 Poller/ring，极端高并发下可能需要通过 `--task_group_ntags` 横向扩展。
- 当前 Poller 每轮会遍历 `tracked_sids` 调用 `PollCq()`，大量长空闲连接下需要关注这部分扫描成本。

## 10. 当前实现需要关注的问题

以下是从代码阅读中发现的实现风险，建议在合入后继续修正或验证。

1. `none` 模式可能提前消费 CQE。

   `IouringEndpoint::PollingModeInitialize()` 的 `none` 分支调用 `io_uring_wait_cqe_timeout()` 后立即 `io_uring_cqe_seen()`，随后才遍历 socket 调用 `PollCq()`。这可能导致第一个 CQE 被标记 consumed，但没有经过 `PollCq()` 分发，表现为读写完成丢失。等待 CQE 的逻辑应只负责唤醒 Poller，不应在业务分发前消费 CQE。

2. 需要显式启动 Poller。

   `IouringTransport::ContextInitOrDie()` 不会自动调用 `InitPollingModeWithTag()`。如果调用方只设置 `SOCKET_MODE_IOURING`，但没有为对应 tag 启动 Poller，fd 会跳过 epoll read 注册，io_uring 也不会真正处理 CQE。

3. 客户端连接签名没有区分 io_uring。

   `Channel` 当前只在 `socket_mode == SOCKET_MODE_RDMA` 时把 `"|rdma"` 写入连接签名，没有为 `SOCKET_MODE_IOURING` 写入标记。不同 socket mode 的 Channel 可能在连接池 key 上不完全隔离，需要补上 `"|iouring"`。

4. CMake 的 `BRPC_PRIVATE_LIBS` 中 `-luring` 可能被覆盖。

   `WITH_IOURING` 分支先追加 `-luring`，随后 `BRPC_PRIVATE_LIBS` 被重新赋值为基础库列表，可能导致 pkg-config/private link 信息里丢失 liburing。

5. 固定缓冲区 region 增长路径需要压测。

   新 region 会通过 `io_uring_register_buffers_update()` 增量注册；如果内核不支持 update，代码尝试 unregister 后重新 register。这个 fallback 对多 region 情况需要重点验证，避免旧 region 的 buf_index 失效。

## 11. 建议验证项

- 编译验证：`WITH_IOURING=OFF` 和 `WITH_IOURING=ON` 都要覆盖，确认未启用时普通 TCP 构建不受影响。
- 功能验证：`example/iouring_echo_c++` 分别跑 TCP、io_uring none、sqpoll、hybrid、fixed buffer。
- 压测验证：高并发小包、多连接长空闲、send buffer 打满、连接频繁建立/关闭。
- 回归验证：Channel 连接复用、Server 停止、Socket Reset/SetFailed、EOF、短写和错误 CQE。
- 内存验证：固定缓冲区模式下 region 增长、max region、read slot 耗尽、IOBuf block 回收。

## 12. 结论

这次提交把 io_uring 作为 brpc 的第三种 transport 引入，保留了原有 Socket/InputMessenger/写队列语义，只替换底层 fd I/O 驱动方式。它的核心收益来自 completion 模型、SQ/CQ 批处理、减少 epoll_ctl/read/write 系统调用，以及可选 fixed buffer 零拷贝。

相对 epoll，io_uring 更适合高 QPS、低延迟、可接受较高内核版本和更复杂运行参数的场景；epoll 仍然是默认且更稳妥的通用路径。当前实现已经搭起完整框架，但 Poller 启动、CQE 消费、连接池签名和固定缓冲区增长等边界仍需要修正或充分验证后再作为生产默认路径使用。
