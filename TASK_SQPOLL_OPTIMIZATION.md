# 任务计划：io_uring SQPOLL轮询模式优化

## 任务信息
- **创建日期**: 2026-04-14
- **目标**: 将io_uring从默认中断模式切换为SQPOLL轮询模式，提升QPS

## Phase 1: 理论分析
**状态**: ✅ complete

### 核心结论：SQPOLL能显著提升QPS

**syscall开销对比**：

| 模式 | 每事件周期syscall数 | 说明 |
|------|---------------------|------|
| epoll | 1次 (epoll_wait) | 事件持久，无需rearm |
| io_uring默认 | 2次 (submit_and_wait + submit) | POLL_ADD一次性，需rearm |
| io_uring SQPOLL | 0-1次 | 内核线程轮询SQ，无需submit |

**SQPOLL消除的syscall**：
1. `io_uring_submit()` → 完全消除（内核线程自动取SQE）
2. `io_uring_submit_and_wait()` → 替换为`io_uring_sqring_wait()`（仅内核线程睡眠时需要）

**预期QPS提升**：10-30%

## Phase 2: 实现SQPOLL模式
**状态**: ✅ complete

### 实现内容

1. **IoUringContext新增sqpoll字段和方法**：
   - `sqpoll`: 是否启用SQPOLL模式
   - `Submit()`: SQPOLL模式下跳过io_uring_submit()
   - `SubmitAndWait()`: SQPOLL模式下使用io_uring_sqring_wait()

2. **Init()中SQPOLL参数设置**：
   - `IORING_SETUP_SQPOLL`: 启用SQPOLL
   - `IORING_SETUP_SQ_AFF`: 绑定CPU核心
   - `sq_thread_idle`: 内核线程空闲超时
   - 内核版本检测（5.11+）
   - 回退机制（SQPOLL不可用时自动回退）

3. **新增gflags**：
   - `--io_uring_sqpoll`: 是否启用SQPOLL（默认false）
   - `--io_uring_sqpoll_cpu`: SQPOLL内核线程CPU亲和性（-1=自动）
   - `--io_uring_sqpoll_idle`: SQPOLL内核线程空闲超时（ms）

4. **Run()循环优化**：
   - SQPOLL模式下Submit()为空操作
   - SubmitAndWait()使用io_uring_sqring_wait()

### 修改的文件
- src/brpc/event_dispatcher_iouring_impl.cpp
- src/brpc/event_dispatcher.cpp（新增gflags）

## Phase 3: 性能验证
**状态**: pending

### 测试命令
```bash
# 编译
cd /home/zfz/brpc/build && make -j$(nproc)

# SQPOLL模式测试
taskset -c 304-311 ./server --use_rdma=false --port=8002 --io_backend=io_uring --io_uring_sqpoll=true --io_uring_sqpoll_cpu=305
taskset -c 96-111 ./client --use_rdma=false --servers=127.0.0.1:8002 --thread_num=16 --queue_depth=8 --attachment_size=1024 --test_seconds=10 --io_backend=io_uring --io_uring_sqpoll=true --io_uring_sqpoll_cpu=97

# epoll对比
taskset -c 304-311 ./server --use_rdma=false --port=8002 --io_backend=epoll
taskset -c 96-111 ./client --use_rdma=false --servers=127.0.0.1:8002 --thread_num=16 --queue_depth=8 --attachment_size=1024 --test_seconds=10 --io_backend=epoll
```

### 预期结果
| 指标 | epoll | io_uring默认 | io_uring SQPOLL |
|------|-------|-------------|-----------------|
| QPS | 237k | 217k | 250k+ |
| Avg-Latency | 538μs | 585μs | <500μs |
| 99.9th-Latency | 1862μs | 3397μs | <1500μs |
