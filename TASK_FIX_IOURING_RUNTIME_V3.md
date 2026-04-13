# 任务计划：修复io_uring运行时超时问题 (第三轮 - 根因修复)

## 任务信息
- **创建日期**: 2026-04-13
- **任务**: 修复io_uring运行时RPC超时问题
- **错误**: E1008 Reached timeout=2000ms

## 根因分析

### 日志关键信息

**Server**:
```
AddConsumer: fd=3, event_data_id=0
CQE received: event_data_id=0, res=1    ← POLLIN事件！被跳过！
```

**Client**:
```
AddConsumer: fd=3, event_data_id=0
CQE received: event_data_id=2, res=4    ← 正常
CQE received: event_data_id=0, res=-2   ← POLL_REMOVE完成
CQE received: event_data_id=2, res=4    ← 正常
CQE received: event_data_id=0, res=4    ← POLLIN事件！被跳过！
```

### 根因

**问题1: event_data_id=0是有效ID，但被跳过**

原代码中：
```cpp
if (event_data_id != 0 && res >= 0) {  // 错误！跳过了event_data_id=0的事件
```

IOEventData::Create生成的第一个ID就是0，这是完全有效的ID。
但Run()循环中`event_data_id != 0`的检查导致所有event_data_id=0的事件被跳过。
结果：fd=3上的POLLIN事件永远不会被处理，连接无法建立。

**问题2: POLL_REMOVE和wakeup事件使用user_data=0**

原代码中：
```cpp
// Stop()中
sqe->user_data = 0;  // 与AddConsumer的event_data_id=0冲突！

// RemoveConsumer()中
sqe->user_data = 0;  // 与AddConsumer的event_data_id=0冲突！
```

POLL_REMOVE完成和wakeup事件也使用user_data=0，
与第一个AddConsumer的event_data_id=0冲突，导致无法区分。

**问题3: RearmFd条件检查错误**

原代码中：
```cpp
if (fd_to_rearm >= 0 && eid_to_rearm != 0) {  // eid_to_rearm=0时不rearm！
```

当event_data_id=0时，eid_to_rearm也为0，导致不会重新注册POLL_ADD，
fd上的后续事件永远不会被监听。

## 修复方案

### 修复1: 引入内部事件哨兵值

```cpp
static const uint64_t IOURING_INTERNAL_EVENT = 0xFFFFFFFFFFFFFFFEULL;
```

用于标识POLL_REMOVE完成和wakeup事件，与有效的event_data_id区分。

### 修复2: Stop()和RemoveConsumer()使用哨兵值

```cpp
// Stop()中
sqe->user_data = IOURING_INTERNAL_EVENT;

// RemoveConsumer()中
sqe->user_data = IOURING_INTERNAL_EVENT;
```

### 修复3: Run()中只跳过内部事件

```cpp
if (event_data_id == IOURING_INTERNAL_EVENT) {
    continue;  // 只跳过内部事件，不跳过event_data_id=0
}
```

### 修复4: RearmFd使用found标志

```cpp
bool found = false;
for (...) {
    if (ctx.fd_info_vec[i].event_data_id == event_data_id) {
        found = true;
        break;
    }
}
if (found) {
    RearmFd(ctx, fd_to_rearm, event_data_id, events_to_rearm);
}
```

### 修复5: RemoveConsumer使用found标志

```cpp
bool found = false;
for (...) {
    if (ctx.fd_info_vec[i].fd == fd) {
        found = true;
        break;
    }
}
if (!found) {
    return 0;
}
```

### 修复6: 移除不必要的io_uring_submit调用

在Run()循环开始时不再调用io_uring_submit()，
因为AddConsumer/RemoveConsumer/RegisterEvent已经各自调用了io_uring_submit。

## 修改文件

- src/brpc/event_dispatcher_iouring.cpp (完整重写)

## 待验证

1. 重新编译brpc库
2. 重新编译example
3. 运行测试验证连接是否建立
