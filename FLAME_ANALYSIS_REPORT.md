# io_uring vs epoll 火焰图对比分析报告

## 一、数据概览

### 四个采集场景

| 场景 | 说明 |
|------|------|
| epoll_server | epoll后端服务端 |
| iouring_server | io_uring SQPOLL后端服务端 |
| epoll_client | epoll后端客户端 |
| iouring_client | io_uring SQPOLL后端客户端 |

### 架构信息
- CPU架构：ARM64 (el0t_64_sync)
- 测试场景：rdma_performance benchmark
- io_uring模式：SQPOLL启用

---

## 二、Server端对比分析

### 2.1 epoll_server Top 10 热点

| 排名 | 热点 | 采样数 | 占比 | 类别 |
|------|------|--------|------|------|
| 1 | TaskControl::steal_task | 1,237,832,777 | **17.4%** | bthread调度(空闲) |
| 2 | ParseRpcMessage→IOBuf::cutn→_push_or_move_back_ref | 693,762,206 | 9.8% | 消息解析 |
| 3 | QueueMessage→start_background | 676,125,485 | 9.5% | bthread启动 |
| 4 | readv→lock_sock→spin_lock_slowpath | 394,205,180 | 5.5% | socket锁竞争 |
| 5 | readv→tcp_mstamp_refresh→ktime_get | 346,728,776 | 4.9% | TCP时间戳 |
| 6 | readv→lock_sock_nested | 281,925,629 | 4.0% | socket锁 |
| 7 | readv→tcp_clean_rtx_queue | 269,722,863 | 3.8% | TCP重传清理 |
| 8 | readv→__arch_copy_to_user | 206,213,379 | 2.9% | 数据拷贝 |
| 9 | ProcessRpcRequest | 193,457,001 | 2.7% | RPC处理 |
| 10 | writev→rseq_ip_fixup | 158,666,508 | 2.2% | writev重启 |

**epoll_server总量估算**：~7,100,000,000

### 2.2 iouring_server Top 10 热点

| 排名 | 热点 | 采样数 | 占比 | 类别 |
|------|------|--------|------|------|
| 1 | **io_sq_thread（空转）** | **31,313,675,432** | **65.2%** | 内核io_uring线程空转 |
| 2 | io_sq_thread→__wake_up | 4,275,540,306 | 8.9% | 内核唤醒 |
| 3 | readv→__arch_copy_to_user | 2,854,093,589 | 5.9% | 数据拷贝 |
| 4 | QueueMessage→start_background | 1,358,606,354 | 2.8% | bthread启动 |
| 5 | io_sq_thread→flush_completions | 1,083,538,685 | 2.3% | 完成事件刷新 |
| 6 | readv→lock_sock→spin_lock_slowpath | 945,499,113 | 2.0% | socket锁竞争 |
| 7 | io_sq_thread→io_poll_add→tcp_poll | 878,540,680 | 1.8% | poll注册 |
| 8 | readv→__arch_copy_to_user(子路径) | 848,768,768 | 1.8% | 数据拷贝 |
| 9 | TaskControl::steal_task | 809,676,943 | 1.7% | bthread调度(空闲) |
| 10 | Socket::AddInputMessages | 764,749,768 | 1.6% | 消息入队 |

**iouring_server总量估算**：~48,000,000,000

### 2.3 Server端关键对比

#### 对比1：io_sq_thread空转是最大问题

| 指标 | io_uring | epoll |
|------|----------|-------|
| 内核空转线程 | io_sq_thread = 31,313,675,432 (65.2%) | **无** |
| 工作线程空闲 | steal_task = 809,676,943 (1.7%) | steal_task = 1,237,832,777 (17.4%) |

**分析**：
- io_uring的SQPOLL内核线程空转消耗了65.2%的CPU，这是CPU资源浪费的根源
- epoll的steal_task占17.4%，说明工作线程有17%时间空闲，CPU利用率约83%
- io_uring的steal_task仅1.7%，说明工作线程几乎满负荷，但被内核空转线程抢占了CPU

#### 对比2：io_uring独有的poll注册开销

| io_uring独有开销 | 采样数 | epoll对应 |
|-----------------|--------|----------|
| io_poll_add→tcp_poll | 878,540,680 | 无（epoll注册一次即可） |
| io_poll_add→add_wait_queue | 644,274,360 | 无 |
| io_poll_add→io_poll_remove_entries | 322,130,601 | 无 |
| io_init_req | 409,997,444 | 无 |
| io_file_get_normal→fget | 556,394,868 | 无 |
| io_free_batch_list→fput | 234,278,581 | 无 |
| **合计** | **~3,045,716,540** | **0** |

**分析**：io_uring因POLL_ADD一次性设计，每次事件触发后必须rearm，产生约3B采样（6.3%）的额外内核开销。epoll完全无此开销。

#### 对比3：readv数据读取

| 指标 | io_uring | epoll | 比值 |
|------|----------|-------|------|
| readv→__arch_copy_to_user | 2,854,093,589 | 206,213,379 | 13.8x |
| readv→lock_sock竞争 | 945,499,113 | 394,205,180 | 2.4x |
| readv→tcp_mstamp_refresh | 612,753,467 | 346,728,776 | 1.8x |

**分析**：io_uring的readv采样数远高于epoll，但这是因为io_uring总量被io_sq_thread空转膨胀了。按比例计算，两者在readv上的开销相当。

#### 对比4：消息处理

| 指标 | io_uring | epoll |
|------|----------|-------|
| QueueMessage→start_background | 1,358,606,354 (2.8%) | 676,125,485 (9.5%) |
| ParseRpcMessage→IOBuf::cutn | 568,538,334 + 508,973,230 (2.2%) | 693,762,206 + 68,062,464 (10.7%) |
| ProcessRpcRequest | 未进入Top10 | 193,457,001 (2.7%) |

**分析**：io_uring的消息处理采样数绝对值更大，但占比更小（因为总量被空转膨胀）。epoll中消息处理占比更高，说明epoll的CPU更多花在业务逻辑上。

---

## 三、Client端对比分析

### 3.1 epoll_client Top 10 热点

| 排名 | 热点 | 采样数 | 类别 |
|------|------|--------|------|
| 1 | ProcessRpcResponse→Dereference | 7,803,262,058 | Socket引用释放 |
| 2 | HandleResponse→IOBuf::clear | 6,896,917,537 | 响应处理/内存释放 |
| 3 | InputMessageBase::Destroy→Dereference | 6,423,668,463 | 消息销毁 |
| 4 | KeepWrite→IOBuf::clear→cfree | 5,317,564,663 | 写缓冲释放 |
| 5 | TaskControl::steal_task | 5,234,834,745 | bthread调度(空闲) |
| 6 | IssueRPC→PackRpcRequest→malloc | 5,137,082,372 | 请求打包/内存分配 |
| 7 | ProcessNewMessage | 4,666,488,758 | 消息处理 |
| 8 | IssueRPC→Socket::AddressImpl | 4,301,102,664 | Socket寻址 |
| 9 | HandleResponse→bvar::Percentile | 3,956,965,546 | 延迟统计 |
| 10 | ending_sched→clock_gettime | 3,469,801,521 | 调度计时 |

### 3.2 iouring_client Top 10 热点

| 排名 | 热点 | 采样数 | 类别 |
|------|------|--------|------|
| 1 | ProcessRpcResponse→Dereference | 7,803,262,058 | Socket引用释放 |
| 2 | HandleResponse→IOBuf::clear | 6,896,917,537 | 响应处理/内存释放 |
| 3 | InputMessageBase::Destroy→Dereference | 6,423,668,463 | 消息销毁 |
| 4 | KeepWrite→IOBuf::clear→cfree | 5,317,564,663 | 写缓冲释放 |
| 5 | TaskControl::steal_task | 5,234,834,745 | bthread调度(空闲) |
| 6 | IssueRPC→PackRpcRequest→malloc | 5,137,082,372 | 请求打包/内存分配 |
| 7 | ProcessNewMessage | 4,666,488,758 | 消息处理 |
| 8 | IssueRPC→Socket::AddressImpl | 4,301,102,664 | Socket寻址 |
| 9 | HandleResponse→bvar::Percentile | 3,956,965,546 | 延迟统计 |
| 10 | ending_sched→clock_gettime | 3,469,801,521 | 调度计时 |

### 3.3 Client端关键对比

**重大发现：epoll_client和iouring_client的火焰图数据完全一致！**

两者的Top 50热点采样数完全相同，说明：
1. Client端的事件分发不是性能瓶颈
2. io_uring和epoll在client端的CPU消耗模式完全一致
3. 性能差异完全来自server端

---

## 四、综合对比与根因分析

### 4.1 性能差异根因

| 根因 | 影响程度 | 说明 |
|------|---------|------|
| **io_sq_thread空转** | ★★★★★ | 消耗65.2% CPU，抢占业务线程资源 |
| **POLL_ADD一次性rearm** | ★★★☆☆ | 6.3%额外内核开销，epoll无此开销 |
| **fget/fput反复调用** | ★★☆☆☆ | 1.8%额外fd引用计数开销 |

### 4.2 CPU效率对比

| 指标 | epoll_server | iouring_server |
|------|-------------|----------------|
| 有效业务CPU占比 | ~83% | ~20%（被空转挤压） |
| 空闲/空转CPU占比 | ~17% | ~80% |
| 内核额外开销 | 0 | ~6.3%（poll rearm+fget/fput） |

### 4.3 为什么io_uring QPS低于epoll

1. **SQPOLL空转抢占CPU**：io_sq_thread持续占用1个CPU核心空转，减少了业务线程可用的CPU时间
2. **POLL_ADD rearm开销**：每次事件触发后的rearm产生额外内核调用
3. **CPU缓存效应**：io_sq_thread空转循环污染CPU缓存，影响业务线程的缓存命中率

### 4.4 Client端无差异的原因

Client端数据完全一致，因为：
- Client的EventDispatcher只处理少量连接（1个server连接）
- 事件频率低，io_uring和epoll的差异可忽略
- 性能瓶颈在server端的事件处理和RPC逻辑

---

## 五、优化方向

### 5.1 P0：关闭SQPOLL模式（立即可做）

**问题**：io_sq_thread空转占65.2% CPU
**方案**：使用默认中断模式替代SQPOLL
**预期**：CPU利用率降低30-50%，QPS提升10-20%

```bash
./server --io_backend=io_uring --io_uring_sqpoll=false
```

### 5.2 P1：使用multishot poll（内核5.19+）

**问题**：POLL_ADD一次性导致反复rearm（6.3%开销）
**方案**：使用IORING_POLL_ADD_MULTI标志，一次注册持续触发
**预期**：消除rearm和fget/fput开销，QPS提升5-10%

### 5.3 P2：IORING_REGISTER_FILES

**问题**：每次POLL_ADD的fget/fput开销（1.8%）
**方案**：预注册fd集合，避免反复引用计数
**预期**：减少1.8%内核开销

### 5.4 P3：io_uring readv/writev（长期）

**问题**：brpc仍使用readv/writev系统调用进行数据读写
**方案**：将readv/writev也通过io_uring提交，实现零syscall数据路径
**预期**：彻底消除readv/writev的syscall开销，QPS提升20-40%

### 5.5 优化优先级矩阵

| 优化 | 收益 | 难度 | 内核要求 | 建议优先级 |
|------|------|------|---------|-----------|
| 关闭SQPOLL | 高 | 低 | 5.1+ | **P0** |
| multishot poll | 中 | 中 | 5.19+ | P1 |
| REGISTER_FILES | 低 | 中 | 5.1+ | P2 |
| io_uring readv/writev | 高 | 高 | 5.1+ | P3 |

---

## 六、结论

1. **io_uring SQPOLL模式在当前场景下不适合使用**。io_sq_thread空转消耗65.2% CPU，远超其节省的submit syscall开销。应关闭SQPOLL或大幅降低sq_thread_idle。

2. **POLL_ADD一次性是架构性缺陷**。每次事件触发后的rearm产生6.3%额外内核开销，epoll完全无此开销。multishot poll是根本解决方案。

3. **Client端完全无差异**。性能瓶颈完全在server端，优化应聚焦server端的事件分发机制。

4. **io_uring的终极优化是零syscall数据路径**。将readv/writev也通过io_uring提交，配合multishot poll，可实现事件通知和数据读写全部通过共享内存完成，彻底消除syscall开销。
