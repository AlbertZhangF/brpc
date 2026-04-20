# brpc框架并发限制机制深度分析报告

## 一、已识别的7大并发限制机制

### 1.1 Socket::_overcrowded限流（最关键）

**代码位置**：[socket.cpp:85](file:///home/zfz/code/brpc/apache-brpc-1.15.0-src/src/brpc/socket.cpp#L85)

```cpp
DEFINE_int64(socket_max_unwritten_bytes, 64 * 1024 * 1024, ...);
```

**触发逻辑**：
```
WriteRequest::Setup() → _unwritten_bytes.fetch_add(data.size())
  → if (before + size >= 64MB) → _overcrowded = true

Socket::Write() → if (_overcrowded) → return EOVERCROWDED  // 拒绝写入！
```

**恢复逻辑**：
```
KeepWrite → DoWrite → writev成功 → AddOutputBytes → CancelUnwrittenBytes
  → if (before - bytes + 64MB > 0) → _overcrowded = false
```

**影响**：当写入速度超过网络发送速度，64MB缓冲区满后所有新请求被拒绝。这是QPS无法继续上升的最直接原因。

**验证方法**：`--ignore_eovercrowded=true` 或 `--socket_max_unwritten_bytes=268435456`

### 1.2 单EventDispatcher瓶颈

**代码位置**：[event_dispatcher.cpp:36](file:///home/zfz/code/brpc/apache-brpc-1.15.0-src/src/brpc/event_dispatcher.cpp#L36)

```cpp
DEFINE_int32(event_dispatcher_num, 1, "Number of event dispatcher");
```

**影响**：默认1个EventDispatcher线程处理所有fd的事件通知。当连接数多时，单线程成为瓶颈。

**验证方法**：`--event_dispatcher_num=4`

### 1.3 单连接写入串行化

**代码位置**：[socket.cpp:1690](file:///home/zfz/code/brpc/apache-brpc-1.15.0-src/src/brpc/socket.cpp#L1690) (StartWrite)

```cpp
WriteRequest* const prev_head = _write_head.exchange(req, butil::memory_order_release);
if (prev_head != NULL) {
    req->next = prev_head;  // 追加到链表
    return 0;               // KeepWrite线程统一处理
}
// 获得写权限，启动KeepWrite线程
```

**影响**：同一Socket同一时刻只有1个KeepWrite线程在写。所有请求通过同一个TCP连接串行写入。

**验证方法**：`--connection_type=pooled`（使用连接池，多TCP连接）

### 1.4 连接池大小限制

**代码位置**：[socket.cpp:93](file:///home/zfz/code/brpc/apache-brpc-1.15.0-src/src/brpc/socket.cpp#L93)

```cpp
DEFINE_int32(max_connection_pool_size, 100, ...);
```

**影响**：pooled模式下，到同一server的连接池最大100个。超过100个并发请求需要等待连接释放。

**验证方法**：`--max_connection_pool_size=500`

### 1.5 bthread调度器限制

brpc使用bthread作为协程调度器，默认并发度受CPU核心数限制。

**影响**：bthread工作线程数有限，高并发时调度开销增加。

**验证方法**：`--bthread_concurrency=64`

### 1.6 Controller堆分配开销

每次RPC调用需要：
- `new brpc::Controller()` — 重量级对象，包含Socket引用、计时器等
- `new PerfTestResponse()` — protobuf对象
- `new RespClosure` — 闭包对象

**影响**：高QPS下malloc/free成为瓶颈，火焰图显示`malloc`/`cfree`占比较高。

### 1.7 Socket发送/接收缓冲区

**代码位置**：[socket.cpp:76-80](file:///home/zfz/code/brpc/apache-brpc-1.15.0-src/src/brpc/socket.cpp#L76)

```cpp
DEFINE_int32(socket_recv_buffer_size, -1, ...);
DEFINE_int32(socket_send_buffer_size, -1, ...);
```

**影响**：默认使用系统默认值（通常128KB-16MB），可能不够大。

**验证方法**：`--socket_send_buffer_size=8388608 --socket_recv_buffer_size=8388608`

---

## 二、限制机制优先级排序

| 优先级 | 机制 | 影响程度 | 是否可调 | 调优参数 |
|--------|------|---------|---------|---------|
| **P0** | _overcrowded限流 | ★★★★★ | 是 | --socket_max_unwritten_bytes |
| **P1** | 单连接串行化 | ★★★★☆ | 是 | --connection_type=pooled |
| **P2** | 单EventDispatcher | ★★★☆☆ | 是 | --event_dispatcher_num |
| **P3** | 连接池大小 | ★★☆☆☆ | 是 | --max_connection_pool_size |
| **P4** | Socket缓冲区 | ★★☆☆☆ | 是 | --socket_send_buffer_size |
| **P5** | bthread并发度 | ★☆☆☆☆ | 是 | --bthread_concurrency |
| **P6** | 堆分配开销 | ★☆☆☆☆ | 否 | 需改代码 |

---

## 三、极限Benchmark工具设计

### 3.1 核心设计原则

1. **无限制发送**：不维持固定in-flight数量，以最大能力持续发送
2. **EOVERCROWDED感知**：检测并统计拥挤拒绝次数，自动退避重试
3. **多连接支持**：支持pooled模式，突破单连接串行化限制
4. **实时监控**：每秒输出QPS、inflight、错误数、拥挤数

### 3.2 关键特性

| 特性 | 原benchmark | 极限benchmark |
|------|------------|--------------|
| 发送模式 | 固定depth pipeline | 无限制持续发送 |
| 拥挤处理 | 直接失败 | 检测+退避重试 |
| 连接模式 | 仅single | single/pooled/short |
| 多Channel | 不支持 | --channel_per_thread |
| 参数调优 | 不支持 | --socket_max_unwritten_bytes等 |
| 实时监控 | 仅最终结果 | 每秒输出 |

### 3.3 测试方案

```bash
# 测试1：默认配置（single连接，64MB限制）
./extreme_client --thread_num=16 --attachment_size=1024 --test_seconds=10

# 测试2：突破overcrowded限制
./extreme_client --thread_num=16 --attachment_size=1024 --test_seconds=10 \
    --ignore_eovercrowded=true --socket_max_unwritten_bytes=268435456

# 测试3：pooled连接（多TCP连接）
./extreme_client --thread_num=16 --attachment_size=1024 --test_seconds=10 \
    --connection_type=pooled --max_connection_pool_size=500

# 测试4：多Channel + pooled（最大并发）
./extreme_client --thread_num=16 --attachment_size=1024 --test_seconds=10 \
    --connection_type=pooled --channel_per_thread=4 \
    --ignore_eovercrowded=true --socket_max_unwritten_bytes=268435456

# 测试5：多EventDispatcher
./extreme_client --thread_num=16 --attachment_size=1024 --test_seconds=10 \
    --event_dispatcher_num=4
```

---

## 四、预期结果

| 配置 | 预期QPS | 预期瓶颈 |
|------|---------|---------|
| 默认single | ~500k | _overcrowded限流 |
| +ignore_eovercrowded | ~600k | 单连接写入带宽 |
| +pooled连接 | ~800k | EventDispatcher |
| +多Channel+大缓冲 | ~1000k+ | CPU/网络带宽 |
