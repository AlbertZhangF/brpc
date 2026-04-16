# io_uring vs epoll 火焰图对比分析报告

## 一、数据概览

### 采集环境
- 架构：ARM64 (el0t_64_sync)
- 测试场景：rdma_performance benchmark server端
- io_uring模式：SQPOLL启用

### 采样总量估算

| 后端 | 前50条热点总采样数 | 主要消耗者 |
|------|-------------------|-----------|
| io_uring | ~48,000,000,000 | io_sq_thread占绝对主导 |
| epoll | ~4,400,000,000 | 均匀分布在brpc工作线程 |

---

## 二、io_uring 热点分析（Top 10）

| 排名 | 热点路径 | 采样数 | 占比 | 类别 |
|------|---------|--------|------|------|
| 1 | io_sq_thread（空转） | 31,313,675,432 | **65.2%** | 内核io_uring线程 |
| 2 | io_sq_thread→__wake_up | 4,275,540,306 | 8.9% | 内核唤醒 |
| 3 | readv→tcp_recvmsg→__arch_copy_to_user | 2,854,093,589 | 5.9% | 数据读取 |
| 4 | io_sq_thread→__io_submit_flush_completions | 1,083,538,685 | 2.3% | 完成事件刷新 |
| 5 | readv→lock_sock_nested→spin_lock_slowpath | 945,499,113 | 2.0% | socket锁竞争 |
| 6 | io_sq_thread→io_poll_add→tcp_poll | 878,540,680 | 1.8% | poll注册 |
| 7 | readv→__arch_copy_to_user（子路径） | 848,768,768 | 1.8% | 数据读取 |
| 8 | TaskControl::steal_task | 809,676,943 | 1.7% | bthread调度 |
| 9 | Socket::AddInputMessages | 764,749,768 | 1.6% | 消息入队 |
| 10 | io_sq_thread→add_wait_queue | 644,274,360 | 1.3% | 等待队列 |

### io_uring热点分类汇总

| 类别 | 采样数合计 | 占比 |
|------|-----------|------|
| **io_sq_thread内核线程** | ~38,000,000,000 | **79.2%** |
| **readv数据读取** | ~5,700,000,000 | 11.9% |
| **bthread调度/消息处理** | ~1,600,000,000 | 3.3% |
| **其他** | ~2,700,000,000 | 5.6% |

---

## 三、epoll 热点分析（Top 10）

| 排名 | 热点路径 | 采样数 | 占比 | 类别 |
|------|---------|--------|------|------|
| 1 | TaskControl::steal_task | 1,237,832,777 | **28.1%** | bthread调度 |
| 2 | readv→lock_sock_nested→spin_lock_slowpath | 394,205,180 | 9.0% | socket锁竞争 |
| 3 | readv→tcp_mstamp_refresh→ktime_get | 346,728,776 | 7.9% | 时间戳获取 |
| 4 | readv→lock_sock_nested | 281,925,629 | 6.4% | socket锁 |
| 5 | readv→tcp_clean_rtx_queue | 269,722,863 | 6.1% | TCP重传清理 |
| 6 | readv→__arch_copy_to_user | 206,213,379 | 4.7% | 数据读取 |
| 7 | ProcessRpcRequest | 193,457,001 | 4.4% | RPC请求处理 |
| 8 | writev→rseq_ip_fixup | 158,666508 | 3.6% | writev重启序列 |
| 9 | TaskControl::steal_task（主线程） | 99,005,432 | 2.3% | bthread调度 |
| 10 | TaskGroup::task_runner | 82,338,198 | 1.9% | bthread运行 |

### epoll热点分类汇总

| 类别 | 采样数合计 | 占比 |
|------|-----------|------|
| **bthread调度（steal_task等）** | ~1,420,000,000 | **32.3%** |
| **readv数据读取** | ~1,500,000,000 | 34.1% |
| **writev数据写入** | ~300,000,000 | 6.8% |
| **RPC处理** | ~200,000,000 | 4.5% |
| **其他** | ~980,000,000 | 22.3% |

---

## 四、关键对比分析

### 4.1 io_sq_thread空转问题（最严重）

**现象**：io_sq_thread空转占io_uring总采样的**65.2%**，这是最大的CPU浪费。

**路径**：`io_sq_thread`（无子调用）= 31,313,675,432

**原因**：SQPOLL内核线程在空闲时持续轮询SQ ring，不释放CPU。当前`sq_thread_idle=2000ms`意味着内核线程在2秒无新请求时才睡眠。

**对比**：epoll没有这个问题，因为epoll_wait在没有事件时自动睡眠。

**优化方向**：
1. 降低`sq_thread_idle`（如200ms或500ms），让内核线程更快睡眠
2. 考虑不使用SQPOLL模式，改用默认中断模式
3. 如果使用SQPOLL，确保有足够的事件驱动负载来利用轮询

### 4.2 io_uring poll注册开销

**现象**：io_poll_add相关路径占比约3.1%，而epoll完全没有这个开销。

**路径**：
- `io_poll_add→tcp_poll` = 878,540,680
- `io_poll_add→add_wait_queue` = 644,274,360
- `io_poll_add→io_poll_remove_entries` = 322,130,601
- `io_poll_add→__io_arm_poll_handler` = 234,285,096

**原因**：io_uring的POLL_ADD是一次性的，每次事件触发后必须重新注册（rearm），导致反复执行`io_poll_add→tcp_poll→add_wait_queue`。

**对比**：epoll的EPOLL_CTL_ADD是持久的，注册一次后不需要rearm。

**优化方向**：
1. 使用内核5.19+的multishot poll（IORING_POLL_ADD_MULTI），一次注册持续触发
2. 批量rearm减少内核调用次数（当前已实现）

### 4.3 io_uring fget/fput开销

**现象**：io_uring中fd引用计数操作占显著比例。

**路径**：
- `io_file_get_normal→fget→__fget_files` = 556,394,868
- `io_free_batch_list→fput` = 234,278,581
- `io_uring_enter→fget→__fget_files` = 76,214,014

**合计**：~866,887,463，约占1.8%

**原因**：io_uring每次POLL_ADD都需要通过fget获取fd引用，完成后fput释放。因为POLL_ADD是一次性的，每次rearm都要重复fget/fput。

**对比**：epoll注册fd时只做一次fget，后续不需要。

**优化方向**：
1. 使用`IORING_REGISTER_FILES`注册固定fd集合，避免每次fget/fput
2. multishot poll同样可以消除此开销

### 4.4 readv数据读取对比

| 指标 | io_uring | epoll |
|------|----------|-------|
| readv总采样 | ~5,700,000,000 | ~1,500,000,000 |
| __arch_copy_to_user | 2,854,093,589 | 206,213,379 |
| lock_sock竞争 | 945,499,113 | 394,205,180 |
| tcp_mstamp_refresh | 612,753,467 | 346,728,776 |

**分析**：io_uring的readv采样数是epoll的3.8倍，但这主要是因为io_uring总采样数远大于epoll（io_sq_thread空转膨胀了总量）。按比例计算，两者在readv上的开销相当。

### 4.5 bthread调度开销对比

| 指标 | io_uring | epoll |
|------|----------|-------|
| steal_task采样 | 809,676,943 | 1,237,832,777 |
| 占各自总量比例 | 1.7% | 28.1% |

**分析**：epoll中steal_task占比28.1%，说明epoll的工作线程有大量时间在偷任务（空闲等待），意味着CPU利用率不高。而io_uring中steal_task占比仅1.7%，说明工作线程更忙碌（但被io_sq_thread空转抵消了）。

### 4.6 io_uring_enter等待

**路径**：`iouring_backend::Run→io_uring_enter→io_cqring_wait` = 105,833,800

**分析**：这是Run()线程在等待CQE时的阻塞，占比很小（0.2%），说明CQE处理及时。

### 4.7 io_uring mutex开销

**路径**：
- `mutex_lock` = 292,845,254
- `mutex_unlock` = 263,562,151

**分析**：这是io_uring内核内部的互斥锁，与io_submit_sqes相关。占比约0.6%，不是主要瓶颈。

---

## 五、核心结论

### 5.1 io_uring SQPOLL模式CPU效率低下

io_sq_thread空转消耗了**65.2%**的CPU时间，这是io_uring性能不如epoll的根本原因。即使业务线程处理效率相当，额外的CPU空转导致：
- 整体CPU利用率虚高（Server CPU-utilization: 123% vs 101%）
- 在CPU核心受限的场景下，空转线程抢占业务线程的CPU时间

### 5.2 POLL_ADD一次性是架构性缺陷

每次事件触发后的rearm导致：
- 反复的io_poll_add/tcp_poll/add_wait_queue
- 反复的fget/fput
- 这些开销在epoll中完全不存在

### 5.3 优化优先级

| 优先级 | 优化方向 | 预期收益 | 实施难度 |
|--------|---------|---------|---------|
| **P0** | 关闭SQPOLL或降低sq_thread_idle | CPU利用率降低30-50% | 低 |
| **P1** | 使用multishot poll（内核5.19+） | 消除rearm和fget/fput开销 | 中 |
| **P2** | IORING_REGISTER_FILES | 消除fget/fput开销 | 中 |
| **P3** | 使用io_uring进行readv/writev | 减少数据读写syscall | 高 |

---

## 六、优化建议

### 6.1 立即可做：关闭SQPOLL

当前测试场景（16线程、8队列深度）下，SQPOLL的空转开销远大于其节省的submit syscall开销。建议：

```bash
# 不使用SQPOLL
./server --io_backend=io_uring --io_uring_sqpoll=false
```

### 6.2 短期优化：调整SQPOLL参数

如果仍想使用SQPOLL，降低idle时间：

```bash
./server --io_backend=io_uring --io_uring_sqpoll=true --io_uring_sqpoll_idle=200
```

### 6.3 中期优化：multishot poll

内核5.19+支持IORING_POLL_ADD_MULTI标志，一次POLL_ADD持续触发事件，不需要rearm。这将消除：
- io_poll_add/tcp_poll/add_wait_queue的反复调用
- fget/fput的反复调用
- io_poll_remove_entries的调用

### 6.4 长期优化：io_uring readv/writev

将brpc的readv/writev系统调用也通过io_uring提交，实现真正的零syscall数据路径。这是io_uring的终极优化目标，但需要大幅重构brpc的I/O层。
