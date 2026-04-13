# io_uring性能分析与优化方案

## 测试数据

| 指标 | epoll | io_uring | 差异 |
|------|-------|----------|------|
| QPS | 237k | 217k | -8.4% |
| Avg-Latency | 538μs | 585μs | +8.7% |
| 99th-Latency | 1236μs | 1322μs | +7.0% |
| 99.9th-Latency | 1862μs | 3397μs | +82.4% |
| Throughput | 231MB/s | 212MB/s | -8.2% |
| Client CPU | 174% | 176% | +1.1% |

## 性能瓶颈分析

### 瓶颈1: 单CQE处理（最严重）

当前Run()每次只处理1个CQE：
```cpp
io_uring_wait_cqe(&ctx->ring, &cqe);  // 等待1个
io_uring_cqe_seen(&ctx->ring, cqe);    // 处理1个
```

而epoll每次批量处理最多32个事件：
```cpp
const int n = epoll_wait(..., e, 32, -1);  // 批量获取
for (int i = 0; i < n; ++i) { ... }        // 批量处理
```

**影响**：io_uring每次syscall只处理1个事件，而epoll处理32个。
syscall开销约为1-5μs，高QPS下差距巨大。

### 瓶颈2: RearmFd导致双倍syscall

io_uring的POLL_ADD是一次性的，事件触发后需要重新提交：
```cpp
RearmFd(ctx, fd_to_rearm, event_data_id, events_to_rearm);
// 内部调用 io_uring_submit() → 系统调用
```

每个事件需要2次syscall：1次wait + 1次submit(rearm)
而epoll的事件是持久的，不需要rearm。

**影响**：syscall数量翻倍。

### 瓶颈3: 每次AddConsumer单独submit

```cpp
int ret = io_uring_submit(&ctx->ring);  // 每次AddConsumer都submit
```

而epoll的epoll_ctl是O(1)操作，不需要额外submit。

**影响**：频繁的submit syscall。

### 瓶颈4: 线性搜索fd_info_vec

```cpp
for (size_t i = 0; i < ctx->fd_info_vec.size(); ++i) {
    if (ctx->fd_info_vec[i].event_data_id == event_data_id) {
```

每次CQE处理都需要线性搜索fd映射。

**影响**：O(n)复杂度，fd多时性能下降。

### 瓶颈5: 互斥锁竞争

```cpp
pthread_mutex_lock(&ctx->fd_map_mutex);
// 搜索和修改fd_info_vec
pthread_mutex_unlock(&ctx->fd_map_mutex);
```

每个CQE处理都需要加锁。

**影响**：锁竞争导致延迟增加。

### 瓶颈6: ring大小不足

sq_entries=128, cq_entries=256
对于高并发场景可能不够。

**影响**：SQ满时需要额外submit，CQ满时丢失事件。

## 优化方案

### 优化1: 批量处理CQE（最重要）

使用io_uring_for_each_cqe批量获取所有CQE，
然后批量处理，最后一次性io_uring_cq_advance。

### 优化2: 延迟submit

AddConsumer/RemoveConsumer/RegisterEvent时不立即submit，
而是在Run()循环开始时统一submit。

### 优化3: 使用unordered_map替代vector

将fd_info_vec改为unordered_map<IOEventDataId, IoUringFdInfo>，
O(1)查找替代O(n)线性搜索。

### 优化4: 增大ring大小

sq_entries=512, cq_entries=1024

### 优化5: 批量RearmFd

处理完所有CQE后，批量提交所有rearm请求，
最后统一submit一次。

### 优化6: 考虑SQPOLL模式

使用IORING_SETUP_SQPOLL标志，内核线程轮询SQ，
避免submit syscall。但需要内核5.11+。
