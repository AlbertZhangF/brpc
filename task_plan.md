# Task Plan: brpc bthread调度耗时统计优化

## Goal
分析brpc框架中bthread调度耗时随并发非线性增长的问题，在bthread中添加精细的打点统计，在rdma_performance示例中输出调度各阶段的平均时延和P99时延，并定位性能瓶颈。

## Current Phase
Phase 8

## Phases

### Phase 1: 代码调研与分析
- [x] 定位ProcessNewMessage到ProcessRpcRequest之间的完整代码路径
- [x] 分析bthread调度相关源码，识别调度流程中的关键节点
- [x] 调研brpc通用的打点统计方式（bvar、LatencyRecorder等）
- [x] 分析example/rdma_performance/client.cpp的现有统计输出逻辑
- [x] 将调研结果记录到findings.md
- **Status:** completed

### Phase 2: 打点方案设计
- [x] 确定bthread调度过程中需要统计的关键阶段（创建、入队、出队、执行）
- [x] 设计统计数据结构，复用brpc现有统计组件（TaskMeta加时间戳、bvar::LatencyRecorder）
- [x] 设计统计结果输出格式，与现有Avg-Latency、P99输出保持一致
- [x] 确定统计数据的收集和聚合方式（全局统计+每个请求单独统计）
- **Status:** completed

### Phase 3: 打点功能实现
- [x] 在bthread相关源码中添加打点统计代码
  - 已在TaskMeta中添加create_us、enqueue_us、dequeue_us、start_exec_us时间戳字段
  - 已在start_background中设置任务创建时间
  - 已在ready_to_run/ready_to_run_remote中设置入队时间
  - 已在sched_to中设置出队时间
  - 已在task_runner中设置执行开始时间，并统计各阶段耗时到全局LatencyRecorder
- [x] 新增bthread调度耗时查询接口
  - 添加bthread_sched_latency_us()：获取总调度耗时
  - 添加bthread_queue_latency_us()：获取队列等待耗时
  - 添加bthread_switch_latency_us()：获取上下文切换耗时
- [x] 在rdma_performance/client.cpp中添加调度统计结果输出逻辑
  - 添加三个LatencyRecorder统计bthread各阶段耗时
  - 在HandleResponse中获取并记录每个请求的调度耗时
  - 在测试结束后输出调度相关的Avg-Latency和P99时延
- [x] 统计覆盖了所有主要耗时路径
  - 任务创建到执行的总调度耗时
  - 队列等待耗时（入队到出队）
  - 上下文切换耗时（出队到执行）
- **Status:** completed

### Phase 4: 编译与测试验证
- [ ] 编译brpc和rdma_performance示例，确保功能正常
- [ ] 运行单并发测试，验证打点数据正确性
- [ ] 运行不同并发度测试，确认耗时统计符合预期
- [ ] 验证统计结果输出格式符合要求
- **Status:** in_progress

### Phase 5: 结果分析与优化
- [ ] 分析不同并发下的调度耗时数据，定位瓶颈
- [ ] 如有必要补充更多打点
- [ ] 优化统计逻辑，减少打点本身的性能开销
- **Status:** pending

### Phase 6: 交付与文档
- [ ] 整理统计结果说明
- [ ] 提供使用说明文档
- **Status:** pending

### Phase 7: Bug修复与单位调整
- [x] 修复负数时间统计bug（TaskMeta复用导致旧时间戳未清零）
- [x] 将所有时间统计单位从微秒(us)改为纳秒(ns)，提高统计精度
- [x] 修正接口返回值和输出格式，适配ns单位
- [x] 修复client编译错误，添加接口extern声明
- [x] 重新编译测试验证修复效果
- **Status:** completed

### Phase 8: 细粒度调度过程打点扩展
- [x] 添加队列满重试统计：统计push_rq时的重试次数和重试耗时
- [x] 添加任务偷取统计：统计steal_task的成功/失败次数、偷取耗时
- [x] 添加远程队列锁竞争统计：统计ready_to_run_remote中锁等待时间
- [x] 新增对应的bvar统计变量，支持全局查看
- [ ] 更新client输出，展示新增的细粒度统计指标（可选，根据需要添加）
- **Status:** completed

## Decisions Made
| Decision | Rationale |
|----------|-----------|

## Errors Encountered
| Error | Resolution |
|-------|------------|
