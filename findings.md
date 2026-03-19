# Findings & Decisions

## Requirements
- 分析brpc中从ProcessNewMessage->cut_in_msg之后到ProcessRpcRequest之前的bthread调度耗时非线性增长问题
- 在bthread框架中添加精细的打点统计，使用brpc通用打点方式
- 在example/rdma_performance/client.cpp中输出bthread调度各阶段的Avg-Latency和P99时延
- 如有其他可疑耗时路径，一并打点统计

## Research Findings
- **消息处理流程**:
  1. ProcessNewMessage中调用CutInputMessage（cut_in_msg）解析消息成功后，调用QueueMessage将消息加入调度
  2. QueueMessage调用bthread_start_background创建bthread，执行ProcessInputMessage函数
  3. ProcessInputMessage调用msg->_process，也就是ProcessRpcRequest处理请求
  4. 目标耗时区间：从bthread创建到ProcessRpcRequest开始执行的调度等待时间
- **bthread调度关键路径**:
  1. bthread_start_background -> TaskGroup::start_background：创建TaskMeta，分配栈空间
  2. TaskMeta被加入到本地WorkStealingQueue或者RemoteTaskQueue等待调度
  3. 工作线程通过wait_task/steal_task从队列中获取任务
  4. 上下文切换后执行任务函数
- **brpc通用统计方式**:
  1. 使用`bvar::LatencyRecorder`统计时延，内置支持avg、p50、p99、p999等百分位指标
  2. 使用`butil::cpuwide_time_us()`获取高精度微秒级时间戳
  3. 统计结果可以通过`latency_recorder.get_description()`获取格式化输出
- **rdma_performance示例统计逻辑**:
  1. client.cpp中使用循环发送请求，收集每个请求的时延
  2. 测试结束后计算avg、p99等指标并打印
  3. 可以扩展该逻辑加入bthread调度时延的统计

## Technical Decisions
| Decision | Rationale |
|----------|-----------|
| 在TaskMeta中添加调度时间戳字段 | 新增uint64_t create_us、enqueue_us、dequeue_us、start_exec_us字段，记录bthread创建时间、入队时间、出队时间、开始执行时间，计算各阶段耗时 |
| 使用bvar::LatencyRecorder统计各阶段耗时 | 新增三个全局统计变量：g_sched_latency(总调度耗时)、g_queue_latency(队列等待耗时)、g_switch_latency(上下文切换耗时)，复用brpc现有统计组件，内置支持avg、p99等指标 |
| 新增bthread调度统计接口 | 提供bthread_get_sched_stats()接口，返回统计结果的格式化字符串，方便上层应用获取 |
| 在InputMessageBase中添加调度耗时字段 | 记录每个请求的调度总耗时，方便rdma_performance示例统计每个请求的调度时延 |
| 在rdma_performance客户端添加统计结果输出 | 扩展现有统计逻辑，收集每个请求的调度耗时，测试结束后与原有Avg-Latency、P99同步输出 |

## Issues Encountered
| Issue | Resolution |
|-------|------------|
| 统计出现负数时间 | TaskMeta对象从资源池复用，旧的时间戳未清零，需要在start_background中初始化时间戳为0 |
| 时间精度不足 | 原使用us单位，改为ns单位，提高统计精度 |

## Resources
- [bthread文档](docs/cn/bthread.md)
- [brpc性能调优文档](docs/cn/performance.md)
