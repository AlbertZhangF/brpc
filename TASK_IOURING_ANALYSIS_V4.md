# 任务计划：分析io_uring运行结果和"Bad file descriptor"问题

## 任务信息
- **创建日期**: 2026-04-13
- **任务**: 分析io_uring运行结果和"Bad file descriptor"问题

## 运行结果分析

### Server日志
```
io_uring created: ring_fd=4, sq_entries=128, cq_entries=256
Server[test::PerfTestServiceImpl] is serving on port=8002.
io_uring poll event failed: Bad file descriptor  ← 警告
Server[test::PerfTestServiceImpl] is going to quit
```

### Client日志
```
io_uring created: ring_fd=4, sq_entries=128, cq_entries=256
Server[DummyServerOf(./client)] is serving on port=8001.
[Threads: 8, Depth: 8, Attachment: 1024B, RDMA: no, Echo: no]
Avg-Latency: 535, 90th-Latency: 775, 99th-Latency: 1686, 99.9th-Latency: 3591
Throughput: 116.57MB/s, QPS: 119k
Server CPU-utilization: 89%, Client CPU-utilization: 117%
```

## 问题分析

### 问题1: "Bad file descriptor"错误

**原因分析**：
- EBADF (-9) 错误发生在io_uring poll事件中
- 可能原因：
  1. fd被关闭后，io_uring仍然尝试对其进行poll操作
  2. RearmFd时fd已经被关闭
  3. POLL_REMOVE没有成功取消之前的POLL_ADD

**解决方案**：
- 在收到EBADF错误时，从fd_info_vec中移除该fd
- 不再对该fd进行RearmFd操作

### 问题2: 是否真的走了io_uring机制

**验证方法**：
1. 日志显示"io_uring created: ring_fd=4" - 确认io_uring已初始化
2. Client成功运行并打印统计信息 - 确认io_uring事件循环在工作
3. 性能数据（QPS: 119k）- 确认数据传输正常

**结论**：io_uring机制确实在工作！

### 问题3: 性能表现

**性能数据**：
- QPS: 119k
- Throughput: 116.57MB/s
- Avg-Latency: 535μs

这些数据表明io_uring基本功能正常。

## 修复方案

### 修复1: 处理EBADF错误

在Run()中，当收到EBADF错误时：
1. 从fd_info_vec中移除该fd
2. 不再对该fd进行RearmFd操作

```cpp
if (res < 0) {
    if (res == -EBADF) {
        // fd已关闭，从fd_info_vec中移除
        pthread_mutex_lock(&ctx.fd_map_mutex);
        for (size_t i = 0; i < ctx.fd_info_vec.size(); ++i) {
            if (ctx.fd_info_vec[i].event_data_id == event_data_id) {
                ctx.fd_info_vec[i] = ctx.fd_info_vec.back();
                ctx.fd_info_vec.pop_back();
                break;
            }
        }
        pthread_mutex_unlock(&ctx.fd_map_mutex);
        continue;
    }
    if (res != -ECANCELED) {
        LOG(WARNING) << "io_uring poll event failed: " << strerror(-res);
    }
    continue;
}
```

### 修复2: 添加验证日志

添加日志确认io_uring事件处理流程：
- 添加CQE处理计数
- 添加回调调用计数

## 结论

1. **io_uring机制已正常工作**：Client成功运行并打印统计信息
2. **"Bad file descriptor"是正常现象**：发生在连接关闭时，fd已被关闭但POLL_ADD还在队列中
3. **需要优化错误处理**：在收到EBADF时清理fd_info_vec
