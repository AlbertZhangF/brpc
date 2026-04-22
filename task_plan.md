# 任务计划：最大并发压测用例改造

## 目标
将rdma_performance示例从"维持固定并发数"模型改造为"尽可能发送更多请求"的最大吞吐压测模型。

## 执行阶段

### Phase 5: 改造client.cpp为最大并发模型 ✅ completed
**完成内容**:
- ✅ queue_depth → max_inflight参数替换（默认10000，0=无限制）
- ✅ RunTest重写为持续发送循环（while循环+SendRequest）
- ✅ HandleResponse移除SendRequest调用，仅做统计+回收资源
- ✅ SendRequest返回bool，失败时RunTest短暂usleep(1)
- ✅ inflight计数器使用CAS原子操作防竞态
- ✅ _remaining_iterations改为原子变量，在SendRequest中检查
- ✅ HandleResponse中增加iterations耗尽+inflight归零的_stop检测
- ✅ 优雅退出：5秒drain超时保护

### Phase 6: 代码检视 ✅ completed
**检视发现与修复**:

| # | 问题 | 严重度 | 状态 |
|---|------|--------|------|
| 1 | inflight检查与递增非原子，多线程可能超过max_inflight | HIGH | ✅ 修复：使用CAS原子操作 |
| 2 | _iterations非原子变量，多线程并发修改竞态 | HIGH | ✅ 修复：改为atomic<uint32_t> _remaining_iterations |
| 3 | iterations模式下iterations耗尽但inflight>0时_stop不设置 | MEDIUM | ✅ 修复：HandleResponse中增加检测 |
| 4 | expected_qps令牌不足时inflight已递增但未回退 | MEDIUM | ✅ 修复：return前fetch_sub(1) |
| 5 | iterations检查从HandleResponse移至SendRequest | LOW | ✅ 修复：发送端控制更合理 |

## 关键设计决策

### D1: 发送与接收解耦
- RunTest线程负责持续发送请求
- HandleResponse仅负责统计和回收资源
- 不再由回调驱动发送

### D2: max_inflight防OOM
- 每个线程维护inflight原子计数器
- 使用CAS操作保证inflight <= max_inflight
- max_inflight默认10000（足够大，仅防内存耗尽）

### D3: inflight原子递增策略
- 先CAS递增inflight，再检查token
- token不足时回退inflight
- 保证inflight计数器始终准确

### D4: iterations原子控制
- _remaining_iterations为atomic<uint32_t>
- 在SendRequest中fetch_sub检查，避免竞态
- HandleResponse中检测iterations耗尽+inflight归零设置_stop

### D5: 优雅退出
- _stop标志通知发送线程停止
- RunTest等待inflight归零（drain阶段）
- 5秒超时保护防止无限等待
