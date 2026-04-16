# Benchmark CPU压力瓶颈深度分析报告

## 一、端到端请求流程

```
Client端:
  SendRequest() → Channel::CallMethod → Controller::IssueRPC
    → PackRpcRequest(malloc+serialize) → Socket::Write → Socket::StartWrite
    → writev() syscall → KeepWrite bthread

  HandleResponse() ← ProcessRpcResponse ← InputMessenger::OnNewMessages
    ← Socket::ProcessEvent ← EventDispatcher::Run
    → readv() syscall → ParseRpcMessage → QueueMessage(bthread_start_background)
    → SendRequest() [循环]

Server端:
  EventDispatcher::Run → Socket::OnInputEvent → Socket::ProcessEvent
    → InputMessenger::OnNewMessages → readv() syscall
    → ParseRpcMessage → QueueMessage(bthread_start_background)
    → ProcessRpcRequest → PerfTestServiceImpl::Test
    → SendRpcResponse → Socket::Write → Socket::StartWrite
    → writev() syscall → KeepWrite bthread
```

## 二、瓶颈点识别

### 瓶颈1：EventDispatcher单线程瓶颈（★★★★★ 最严重）

**代码位置**：[event_dispatcher.cpp:38](file:///home/zfz/code/brpc/apache-brpc-1.15.0-src/src/brpc/event_dispatcher.cpp#L38)

```cpp
DEFINE_int32(event_dispatcher_num, 1, "Number of event dispatcher");
```

**问题**：默认只有1个EventDispatcher线程处理所有连接的事件通知。
无论是epoll还是io_uring，所有fd的事件都由单线程分发。

**影响**：
- 所有新连接的事件（可读/可写）都经过同一个EventDispatcher
- EventDispatcher成为全局串行点
- 高并发时事件分发延迟增大

**优化**：增加`--event_dispatcher_num`到4或8

```bash
./server --event_dispatcher_num=8 --io_backend=io_uring
```

### 瓶颈2：Socket::_nevent去重机制限制并发（★★★★☆）

**代码位置**：[socket.cpp:2256](file:///home/zfz/code/brpc/apache-brpc-1.15.0-src/src/brpc/socket.cpp#L2256)

```cpp
if (s->_nevent.fetch_add(1, butil::memory_order_acq_rel) == 0) {
    // 只有_nevent从0变1时才启动ProcessEvent
    bthread_start_urgent(&tid, &attr, ProcessEvent, p);
}
```

**问题**：每个Socket同一时刻只有1个ProcessEvent在运行。
如果Socket上有大量数据待读取，但前一个ProcessEvent还没处理完，
后续事件通知被_nevent计数器吸收，不会启动新的处理线程。

**影响**：
- 单个Socket的读取是串行的
- 对于少量连接高并发场景，处理能力受限于单Socket的读取速度
- readv一次只读`_avg_msg_size * 16`字节（最小4096，最大524288）

### 瓶颈3：Socket写入串行化（★★★★☆）

**代码位置**：[socket.cpp:1696](file:///home/zfz/code/brpc/apache-brpc-1.15.0-src/src/brpc/socket.cpp#L1696)

```cpp
int Socket::StartWrite(WriteRequest* req, const WriteOptions& opt) {
    WriteRequest* const prev_head =
        _write_head.exchange(req, butil::memory_order_release);
    if (prev_head != NULL) {
        // 有人正在写，把请求挂到链表上，KeepWrite线程会处理
        req->next = prev_head;
        return 0;
    }
    // 获得写权限，直接写
    ...
}
```

**问题**：每个Socket同一时刻只有1个KeepWrite线程在写。
多个bthread同时写同一个Socket时，只有一个能获得写权限，
其余的请求被挂到链表上等待。

**影响**：
- 写入是串行的，无法并行
- KeepWrite线程需要处理完所有挂起的WriteRequest才能退出
- 对于单连接高并发场景，写入带宽受限于单线程writev

### 瓶颈4：_overcrowded限流机制（★★★☆☆）

**代码位置**：[socket.cpp:87](file:///home/zfz/code/brpc/apache-brpc-1.15.0-src/src/brpc/socket.cpp#L87)

```cpp
DEFINE_int64(socket_max_unwritten_bytes, 64 * 1024 * 1024,
             "Max unwritten bytes in each socket, if the limit is reached,"
             " Socket.Write fails with EOVERCROWDED");
```

**问题**：当Socket未写出的数据超过64MB时，新的Write请求会被拒绝（EOVERCROWDED）。
在benchmark中，如果server端处理速度跟不上client端发送速度，
写缓冲会快速填满，触发限流。

**影响**：
- 请求被拒绝后client端需要重试或等待
- 限流导致CPU空闲（无法发送更多请求）

### 瓶颈5：bthread调度开销（★★★☆☆）

**火焰图数据**：
- epoll_server: steal_task占17.4%
- epoll_client: steal_task占5.2%

**问题**：bthread的work-stealing调度在负载不均时产生大量开销。
每个bthread完成工作后需要从其他worker偷任务，这涉及原子操作和缓存行竞争。

**影响**：
- 17%的CPU时间浪费在任务偷取上
- 偷取失败时worker空转

### 瓶颈6：内存分配开销（★★☆☆☆）

**火焰图数据**：
- client端: malloc占5.1B采样（PackRpcRequest→malloc）
- server端: IOBuf::cutn→_push_or_move_back_ref占693M采样

**问题**：每个RPC请求都需要：
1. malloc分配Controller、Response、WriteRequest
2. IOBuf内存块分配（acquire_tls_block）
3. protobuf序列化时的ArenaStringPtr::Set→malloc

**影响**：
- 频繁的malloc/free增加锁竞争
- 内存分配器成为瓶颈

### 瓶颈7：io_uring SQPOLL空转（★★★★★ 仅io_uring）

**火焰图数据**：io_sq_thread空转占65.2% CPU

**问题**：SQPOLL内核线程持续轮询，浪费CPU资源

**影响**：直接抢占业务线程的CPU时间

## 三、根因总结

### 为什么CPU压力上不来

```
请求处理瓶颈链：

Client端:
  SendRequest → malloc(瓶颈6) → Socket::Write(串行化瓶颈3)
    → writev syscall → 等待响应

  响应到达 → EventDispatcher(单线程瓶颈1) → ProcessEvent(去重瓶颈2)
    → readv → ParseRpcMessage → HandleResponse → SendRequest(循环)

Server端:
  请求到达 → EventDispatcher(单线程瓶颈1) → ProcessEvent(去重瓶颈2)
    → readv → ParseRpcMessage → ProcessRpcRequest
    → SendRpcResponse → Socket::Write(串行化瓶颈3)
    → writev syscall

额外开销:
  io_uring: io_sq_thread空转(瓶颈7) → 抢占CPU
  bthread: steal_task(瓶颈5) → 浪费CPU
  限流: _overcrowded(瓶颈4) → 请求被拒
```

**核心结论**：CPU压力上不来的根本原因是**请求处理流水线中存在多个串行点**，
导致CPU无法被充分利用。最关键的串行点是EventDispatcher单线程和Socket单线程读写。

## 四、解决方案

### 方案1：增加EventDispatcher数量（立即可做，效果最大）

```bash
# Server端
./server --event_dispatcher_num=8 --io_backend=io_uring

# Client端
./client --event_dispatcher_num=4 --io_backend=io_uring
```

**预期效果**：事件分发吞吐量提升4-8倍

### 方案2：关闭SQPOLL（io_uring场景）

```bash
./server --io_backend=io_uring --io_uring_sqpoll=false
```

**预期效果**：释放被空转线程占用的CPU

### 方案3：增大socket写缓冲限制

```bash
./server --socket_max_unwritten_bytes=268435456  # 256MB
./client --socket_max_unwritten_bytes=268435456
```

**预期效果**：减少EOVERCROWDED限流

### 方案4：增加bthread工作线程数

```bash
./server --bthread_concurrency=32
./client --bthread_concurrency=32
```

**预期效果**：增加并行处理能力

### 方案5：使用多连接

```bash
# Client端使用多个连接到同一个server
./client --connection_type=pooled --servers=192.168.3.190:8002
```

**预期效果**：绕过单Socket串行写入限制

### 综合调优命令

```bash
# Server端（推荐配置）
taskset -c 240-319 ./server \
    --use_rdma=false --port=8002 \
    --io_backend=io_uring --io_uring_sqpoll=false \
    --event_dispatcher_num=8 \
    --bthread_concurrency=32 \
    --socket_max_unwritten_bytes=268435456

# Client端（推荐配置）
taskset -c 96-191 ./client \
    --use_rdma=false --servers=192.168.3.190:8002 \
    --thread_num=32 --queue_depth=32 --attachment_size=1024 \
    --test_seconds=10 --io_backend=io_uring --io_uring_sqpoll=false \
    --event_dispatcher_num=4 \
    --bthread_concurrency=32 \
    --socket_max_unwritten_bytes=268435456
```

## 五、瓶颈优先级

| 优先级 | 瓶颈 | 影响 | 解决方案 | 难度 |
|--------|------|------|---------|------|
| **P0** | EventDispatcher单线程 | 全局串行点 | --event_dispatcher_num=8 | 低 |
| **P0** | io_sq_thread空转 | 65%CPU浪费 | --io_uring_sqpoll=false | 低 |
| **P1** | Socket写入串行化 | 单连接写带宽受限 | 多连接/连接池 | 低 |
| **P1** | _overcrowded限流 | 请求被拒 | 增大缓冲限制 | 低 |
| **P2** | bthread调度开销 | 17%CPU浪费 | 增加工作线程 | 低 |
| **P3** | 内存分配开销 | malloc锁竞争 | 对象池/arena | 高 |
