# 任务计划：修复io_uring运行时超时问题

## 任务信息
- **创建日期**: 2026-04-10
- **任务**: 修复io_uring运行时RPC超时问题
- **错误**: E1008 Reached timeout=2000ms

## 错误分析

### 日志信息
```
Server: io_uring created: ring_fd=4, sq_entries=128, cq_entries=256
Server: Server[test::PerfTestServiceImpl] is serving on port=8002
Client: io_uring created: ring_fd=4, sq_entries=128, cq_entries=256
Client: RPC call failed: [E1008]Reached timeout=2000ms @127.0.0.1:8002
```

### 问题分析

**现象**：
- io_uring成功初始化
- Server在8002端口监听
- 但Client无法连接，超时2秒

**可能原因**：
1. POLL_ADD注册后事件未正确触发
2. RearmFd未正确重新注册FD
3. CQE处理循环中的锁竞争问题
4. 全局单例导致多个EventDispatcher共享同一个ring

### 待检查项

1. 检查epoll实现与io_uring实现的差异
2. 检查POLL_ADD是否正确工作
3. 检查RearmFd调用时机
4. 检查事件回调是否正确触发

## 执行阶段

## 修复内容

### 修复1: 析构函数中不销毁全局ring
**问题**: 全局单例g_iouring_ctx在第一个EventDispatcher析构时被销毁，导致其他实例使用已销毁的ring

**修复**: 从析构函数中移除io_uring_queue_exit调用

```cpp
EventDispatcher::~EventDispatcher() {
    Stop();
    Join();
    
    // 注意：不要在这里销毁io_uring ring
    // 因为多个EventDispatcher可能共享同一个全局ring
    // ring的生命周期应该由最后一个使用的EventDispatcher管理
    // 或者使用单独的ReleaseIoUringContext()来显式释放
    
    if (_wakeup_fds[0] > 0) {
        close(_wakeup_fds[0]);
        close(_wakeup_fds[1]);
        _wakeup_fds[0] = -1;
        _wakeup_fds[1] = -1;
    }
}
```

### 修复2: 添加调试日志
**问题**: 无法确定问题发生的具体位置

**修复**: 添加调试日志帮助诊断

```cpp
// Run()函数开始时
LOG(INFO) << "EventDispatcher::Run started, _stop=" << _stop;
LOG(INFO) << "io_uring returned " << ret << " events";

// AddConsumer()调用时
LOG(INFO) << "AddConsumer: fd=" << fd << ", event_data_id=" << event_data_id;
LOG(INFO) << "AddConsumer: submitted " << ret << " requests for fd=" << fd;
```

## 待验证

1. 重新编译brpc库
2. 重新编译example
3. 运行测试观察日志输出
