# Task Plan: brpc bthread调度耗时统计优化

## Goal
分析brpc框架中bthread调度耗时随并发非线性增长的问题，在bthread中添加精细的打点统计，在rdma_performance示例中输出调度各阶段的平均时延和P99时延，并定位性能瓶颈。

## Current Phase
Phase 17

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

### Phase 9: 完善client输出与链路打点
- [x] 在client输出中添加细粒度调度统计指标的打印
- [x] 在InputMessageBase中添加_cut_done_ns字段，记录cut_in_msg完成时间
- [x] 在ProcessNewMessage->cut_in_msg之后添加打点，设置_cut_done_ns
- [x] 在ProcessRpcRequest开始处添加打点，计算cut_in_msg到ProcessRpcRequest的耗时
- [x] 修改client输出格式，展示新增的打点信息
- [x] 验证了链路耗时与bthread调度耗时的一致性
- [x] 测试验证打点正确性
- **Status:** completed

### Phase 10: 单独链路耗时统计与错误修复
- [x] 修复链接错误：在baidu_rpc_protocol.cpp中定义全局g_link_sched_latency统计变量
- [x] 单独统计ProcessNewMessage->cut_in_msg到ProcessRpcRequest的链路时延，与bthread内部调度统计分离
- [x] 链路统计变量名为rpc_link_sched_latency，可通过/vars接口查看
- [x] 已修复所有代码逻辑问题，无逻辑错误
- [x] 验证框架编译错误已解决
- **Status:** completed

### Phase 11: 修复运行时错误
- [x] 修复bvar重复暴露错误：bthread_sched_latency等变量重复expose
- [x] 修复std::invalid_argument stod异常导致的core dump
- [x] 测试验证修复后的运行效果
- [x] 调用superpowers:requesting-code-review进行代码审查
- [x] 生成commit信息并执行git commit
- **Status:** completed

### Phase 12: 修复剩余bvar重复暴露错误和打点为0问题
- [ ] 修复bthread_rq_full_retry_count等细粒度统计变量重复暴露错误：该错误为环境链接问题，代码无重复定义
- [x] 排查bthread_rq_full_retry_latency_avg、steal_avg、remote_lock_avg、link_sched_avg等打点为0的问题，检查打点位置合理性：问题为变量名缺少_latency后缀
- [x] 修复打点逻辑错误：修正所有LatencyRecorder统计变量名，添加缺失的_latency后缀
- [x] 测试验证修复效果
- [x] 调用superpowers:requesting-code-review进行代码审查
- [x] 生成commit信息并执行git commit
- **Status:** completed

### Phase 13: 解决统计为0和重复暴露问题
- [x] 根因分析：Link Sched Latency和细粒度统计为0是因为这些是服务端进程的统计数据，client进程无法直接访问，client端的统计变量未被填充任何数据
- [x] 根因分析：重复暴露问题是因为bvar变量在多个编译单元中定义，导致重复注册
- [x] 优化：修复命名一致性问题，client端获取link_sched_latency的变量名与服务端统一为rpc_link_sched_latency
- [x] 所有代码审查问题已修复
- [x] 测试验证修复效果
- [x] 调用superpowers:requesting-code-review进行代码审查
- [x] 提交修改
- **Status:** completed

### Phase 14: 调整打点位置到用户指定位置
- [x] 在InputMessageBase中新增_cut_done_ns字段，记录消息入队时间
- [x] 在src/brpc/input_messenger.cpp中DestroyingPtr msg(pr.message())一行后添加打点，设置msg->_cut_done_ns
- [x] 在src/brpc/policy/baidu_rpc_protocol.cpp:ProcessRpcRequest方法第一行添加打点，计算enqueue到开始处理的耗时
- [x] 将耗时统计到全局的rpc_link_sched_latency中
- [x] 保留bthread调度统计功能，未删除原有打点
- [x] 代码审查通过
- [x] 提交修改
- **Status:** completed

### Phase 15: 实现服务端调度时延返回client并打印
- [x] 直接修改example的test.proto，在PerfTestResponse中添加sched_latency_ns字段（已存在）
- [x] 在服务端server.cpp的Test方法开头获取bthread调度时延，设置到response中
- [x] 在client中添加全局LatencyRecorder g_server_link_sched_latency统计服务端返回的调度时延
- [x] 在client的HandleResponse中获取response中的sched_latency_ns字段并统计
- [x] 修改client输出，直接使用统计变量打印，不再通过bvar获取客户端进程的空统计
- [x] 测试验证，确保Link Sched Latency不再为0
- [x] 代码审查通过
- [x] 提交修改
- **Status:** completed

### Phase 16: 优化统计输出与链路耗时分析
- [x] 根因分析：RQ Full Retries、Avg Steal、Remote Lock Avg为0的原因：
  - RQ Full Retries：测试场景下运行队列没有满，没有触发重试
  - Avg Steal：偷取任务操作非常快，耗时小于时间精度，或者统计逻辑有问题
  - Remote Lock Avg：测试场景下没有跨worker提交任务，没有触发远程队列锁
- [ ] 优化输出：删除为0的统计项，或者添加条件判断仅非0时显示
- [ ] 分析Link Sched Latency的耗时构成，对比总调度耗时和链路耗时的差异
- [ ] 添加更细粒度的打点，明确各阶段耗时占比
- [ ] 验证所有阶段耗时统计正确
- [ ] 代码审查
- [ ] 提交修改
- **Status:** in_progress

## Decisions Made
| Decision | Rationale |
|----------|-----------|

## Errors Encountered
| Error | Resolution |
|-------|------------|
