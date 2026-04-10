# 任务计划：修复io_uring运行时超时问题 (第二轮)

## 任务信息
- **创建日期**: 2026-04-11
- **任务**: 修复io_uring运行时RPC超时问题（连接未建立）
- **错误**: E1008 Reached timeout=2000ms

## 日志分析

### Server日志
```
EventDispatcher] io_uring created: ring_fd=4, sq_entries=128, cq_entries=256
AddConsumer] AddConsumer: fd=3, event_data_id=0
AddConsumer] AddConsumer: submitted 1 requests for fd=3
Server[test::PerfTestServiceImpl] is serving on port=8002.
EventDispatcher::Run started, _stop=0
io_uring returned 0 events  (只有一次，且在2秒后没有更多输出)
```

### Client日志
```
EventDispatcher] io_uring created: ring_fd=4, sq_entries=128, cq_entries=256
AddConsumer] AddConsumer: fd=3, event_data_id=0
AddConsumer] AddConsumer: submitted 1 requests for fd=3
Server is serving on port=8001.
EventDispatcher::Run started, _stop=0
AddConsumer] AddConsumer: fd=7, event_data_id=1  (第二个fd)
RPC call failed: [E1008]Reached timeout=2000ms @127.0.0.1:8002
```

### 关键发现

1. **event_data_id=0的问题**：
   - Server的AddConsumer中fd=3的event_data_id=0
   - 但在Run循环中：`if (event_data_id != 0)` - 如果是0，事件会被跳过！

2. **io_uring_wait_cqe阻塞问题**：
   - 日志显示"io_uring returned 0 events"只出现一次
   - 之后Server进程没有更多日志
   - 说明io_uring_wait_cqe可能被阻塞或者返回了但没有CQE可处理

3. **fd=7在Client上被注册**：
   - Client添加了fd=7 (event_data_id=1)
   - 这应该是连接到Server的socket

## 问题分析

### 问题1: event_data_id=0导致事件被跳过

在Run循环中：
```cpp
if (event_data_id != 0) {
    // 处理事件
}
```

如果event_data_id=0，事件会被完全跳过！

### 问题2: io_uring_wait_cqe返回值检查

io_uring_wait_cqe成功时返回0，且*cqe_ptr会指向有效的CQE。
但如果返回0但cqe->user_data=0，事件会被跳过。

### 问题3: POLL_ADD的边沿触发问题

POLL_ADD是**边沿触发**的：事件触发一次后就完成，
如果不重新注册(Rearm)，不会再触发。

## 已修复

### 修复1: 添加CQE调试日志
```cpp
if (cqe) {
    IOEventDataId event_data_id = cqe->user_data;
    int32_t res = cqe->res;
    LOG(INFO) << "CQE received: event_data_id=" << event_data_id << ", res=" << res;
    if (event_data_id != 0 && res >= 0) {
        // 处理事件
    }
    io_uring_cqe_seen(&ctx.ring, cqe);
}
```

### 修复2: io_uring_cqe_seen调用
确保每次处理完CQE后调用io_uring_cqe_seen通知内核。

### 修复3: RearmFd调用位置
将RearmFd调用移到条件分支内，确保只在有有效事件时rearm。

## 待验证

1. 重新编译brpc库
2. 重新编译example
3. 运行测试观察新的CQE日志
4. 检查event_data_id是否为0
