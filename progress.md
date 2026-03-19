# Progress Log

## Session: 2026-03-19

### Current Status
- **Phase:** 3 - 打点功能实现
- **Started:** 2026-03-19

### Actions Taken
1. 完成Phase 1代码调研：
   - 定位了ProcessNewMessage -> CutInputMessage -> QueueMessage -> bthread_start_background -> ProcessInputMessage -> ProcessRpcRequest的完整流程
   - 分析了bthread调度的核心源码：TaskGroup、TaskMeta、work-stealing队列等
   - 调研了brpc通用统计方式：bvar::LatencyRecorder、butil::cpuwide_time_us等
   - 将调研结果记录到findings.md
2. 完成Phase 2方案设计：
   - 确定了需要统计的四个关键时间点：任务创建、入队、出队、开始执行
   - 设计了统计数据结构：TaskMeta添加时间戳字段，全局LatencyRecorder统计各阶段耗时
   - 设计了输出格式：与现有Avg-Latency、P99格式保持一致
   - 将设计方案记录到findings.md
3. 完成Phase 3全部实现工作：
   - **bthread侧打点实现**：
     - 修改src/bthread/task_meta.h，在TaskMeta结构中添加create_us、enqueue_us、dequeue_us、start_exec_us四个时间戳字段
     - 修改src/bthread/bthread.cpp，添加三个全局LatencyRecorder：g_sched_latency、g_queue_latency、g_switch_latency
     - 修改src/bthread/task_group.cpp的start_background函数，记录任务创建时间
     - 修改ready_to_run和ready_to_run_remote函数，记录任务入队时间
     - 修改sched_to函数，记录任务出队时间
     - 修改task_runner函数，记录任务开始执行时间，并计算各阶段耗时更新到全局统计变量
   - **新增调度统计接口**：
     - 修改src/bthread/bthread.h，添加三个接口声明：bthread_sched_latency_us()、bthread_queue_latency_us()、bthread_switch_latency_us()
     - 修改src/bthread/bthread.cpp，实现这三个接口，方便上层应用获取当前bthread的调度耗时
   - **rdma_performance示例集成**：
     - 修改example/rdma_performance/client.cpp，添加三个LatencyRecorder统计每个请求的bthread调度耗时
     - 在HandleResponse回调中调用新增的接口获取调度耗时并记录
     - 在测试结束后的输出中添加bthread调度相关的Avg和P99时延统计，与原有格式保持一致
4. 更新task_plan.md，标记Phase 3完成，进入Phase 4测试验证阶段
5. 修复测试中发现的bug：
   - 负数时间bug：TaskMeta从对象池复用，旧时间戳未清零，在start_background中新增初始化逻辑，将四个时间戳清零
   - 时间单位调整：将所有时间统计从us改为ns，提高精度
   - 调整接口：将bthread_sched_latency_us等接口改为bthread_sched_latency_ns，返回ns单位
   - 调整client输出：统计时将ns转换为us存储，保持输出可读性
6. 所有修改已完成，等待重新编译测试

### Test Results
| Test | Expected | Actual | Status |
|------|----------|--------|--------|

### Errors
| Error | Resolution |
|-------|------------|
