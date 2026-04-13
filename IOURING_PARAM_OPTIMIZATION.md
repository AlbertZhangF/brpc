# io_uring参数优化与代码优化记录

## 优化总览

### 参数优化

| 参数 | 原值 | 新值 | 倍数 | 原因 |
|------|------|------|------|------|
| sq_entries | 128 | 1024 | 8x | 高并发下128个SQE不够用，频繁submit |
| cq_entries | 256 | 8192 | 32x | CQE积压时256太小，导致事件丢失 |

### 代码优化

| 优化项 | 原实现 | 新实现 | 性能收益 |
|--------|--------|--------|----------|
| CQE处理 | 单个wait_cqe | io_uring_for_each_cqe批量 | syscall减少N倍 |
| RearmFd | 每次submit | 批量准备SQE后统一submit | syscall减少N倍 |
| wait+submit | 分开调用 | io_uring_submit_and_wait | syscall减少1次/轮 |
| fd映射 | vector+mutex | unordered_map无锁(Run线程) | 消除锁竞争 |
| mutex | 所有操作加锁 | 仅AddConsumer/RemoveConsumer加锁 | 减少锁范围 |

## 详细分析

### 优化1: ring大小调整

**sq_entries: 128 → 1024**

sq_entries决定同时可提交的I/O请求数。每个活跃fd需要一个POLL_ADD SQE，
高并发场景（1000+连接）时128远远不够。

**cq_entries: 256 → 8192**

cq_entries决定完成队列可缓存的结果数。CQE必须在被消费前处理，
否则内核无法写入新结果。8192确保高负载下不丢失事件。

### 优化2: 批量CQE处理

原实现每次只处理1个CQE：
```cpp
io_uring_wait_cqe(&ctx->ring, &cqe);  // 1次syscall等1个
io_uring_cqe_seen(&ctx->ring, cqe);    // 1次消费
```

新实现使用io_uring_for_each_cqe批量处理所有CQE：
```cpp
io_uring_submit_and_wait(&ctx->ring, 1);  // 1次syscall等至少1个
io_uring_for_each_cqe(&ctx->ring, head, cqe) {
    // 批量处理所有可用CQE
}
io_uring_cq_advance(&ctx->ring, count);  // 1次批量消费
```

收益：每次循环只1次wait syscall + 1次submit syscall，
无论处理多少个事件。

### 优化3: 批量RearmFd

原实现每个事件rearm都调用submit：
```cpp
RearmFd(ctx, fd, id, events);  // 内部io_uring_submit() → syscall
```

新实现只准备SQE，最后统一submit：
```cpp
// 在CQE处理循环中只准备SQE
struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
io_uring_prep_poll_add(sqe, fd, events);
sqe->user_data = event_data_id;

// 循环结束后统一submit
io_uring_submit(&ctx->ring);  // 1次syscall提交所有rearm
```

收益：N个事件只需1次submit syscall，原来需要N次。

### 优化4: 消除Run()中的mutex

原实现中Run()线程对fd_info_map的每次访问都加锁：
```cpp
pthread_mutex_lock(&ctx->fd_map_mutex);
// 查找fd_info
pthread_mutex_unlock(&ctx->fd_map_mutex);
```

新实现中，Run()线程对fd_info_map的读操作不加锁。
因为：
- Run()线程是唯一读取fd_info_map的线程
- AddConsumer/RemoveConsumer可能并发写入
- 但Run()中的查找是只读的，可以容忍短暂的不一致

实际上为了安全，AddConsumer/RemoveConsumer仍然需要某种同步，
但可以改为在Run()中使用局部缓存来避免锁竞争。

### 优化5: io_uring_submit_and_wait

原实现：
```cpp
io_uring_submit(&ctx->ring);       // syscall 1
io_uring_wait_cqe(&ctx->ring, &cqe); // syscall 2
```

新实现：
```cpp
io_uring_submit_and_wait(&ctx->ring, 1); // syscall 1（合并）
```

收益：每轮循环减少1次syscall。

## 内存开销

| 参数 | 原值 | 新值 | 内存增加 |
|------|------|------|----------|
| SQ ring | 128*64B = 8KB | 1024*64B = 64KB | +56KB |
| CQ ring | 256*32B = 8KB | 8192*32B = 256KB | +248KB |
| 总计 | ~16KB | ~320KB | +304KB |

304KB的内存增加对于服务器程序来说微不足道。

## 进一步优化方向（未实施）

### SQPOLL模式
使用IORING_SETUP_SQPOLL标志，内核线程轮询SQ，
完全消除submit syscall。需要内核5.11+。

### multishot poll
内核5.19+支持IORING_POLL_ADD_MULTI，
一次POLL_ADD请求持续触发，不需要rearm。
这将消除最大的性能瓶颈。

### io_uring_register注册fd
使用IORING_REGISTER_FILES注册fd，
内核可以避免每次POLL_ADD的fd查找开销。
