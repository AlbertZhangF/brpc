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
| 在TaskMeta中添加调度时间戳字段 | 新增uint64_t create_ns、enqueue_ns、dequeue_ns、start_exec_ns字段，记录bthread创建时间、入队时间、出队时间、开始执行时间，计算各阶段耗时 |
| 使用bvar::LatencyRecorder统计各阶段耗时 | 新增三个全局统计变量：g_sched_latency(总调度耗时)、g_queue_latency(队列等待耗时)、g_switch_latency(上下文切换耗时)，复用brpc现有统计组件，内置支持avg、p99等指标 |
| 新增bthread调度统计接口 | 提供bthread_sched_latency_ns()、bthread_queue_latency_ns()、bthread_switch_latency_ns()接口，返回各阶段调度耗时 |
| 在rdma_performance客户端添加统计结果输出 | 扩展现有统计逻辑，收集每个请求的调度耗时，测试结束后与原有Avg-Latency、P99同步输出 |
| 新增细粒度调度统计 | 添加队列满重试、任务偷取、远程队列锁竞争等统计，深入定位调度瓶颈 |

## Issues Encountered
| Issue | Resolution |
|-------|------------|
| 统计出现负数时间 | TaskMeta对象从资源池复用，旧的时间戳未清零，需要在start_background中初始化时间戳为0 |
| 时间精度不足 | 原使用us单位，改为ns单位，提高统计精度 |
| bvar重复暴露错误 | client中定义的LatencyRecorder名称自动加后缀后与bthread内部暴露的bvar变量重名，将client侧的统计变量重命名为client_前缀 |
| std::invalid_argument stod异常 | 当bvar变量不存在时describe_exposed返回空字符串，添加空字符串判断，空值时使用默认值0 |
| 细粒度时延打点均为0 | LatencyRecorder暴露的变量名会自动添加_latency后缀，之前获取变量名时缺少该后缀，修正变量名加上_latency后缀即可正确获取统计值 |

## Resources
- [bthread文档](docs/cn/bthread.md)
- [brpc性能调优文档](docs/cn/performance.md)

## Performance Analysis (2026-03-23)
### 测试结果差异分析
1. **Bthread Sched vs Server Sched Breakdown差异原因**：
   - **Bthread Sched部分**：统计的是**客户端进程**自己的bthread调度耗时（Avg:193us, P99:515us）
     - 客户端需要异步发送大量请求、处理响应、更新统计，bthread调度压力大
     - 队列等待时间占比99%以上（Avg:196us），是客户端调度的主要瓶颈
   - **Server Sched Breakdown部分**：统计的是**服务端进程**的bthread调度耗时（Avg:45us, P99:251us）
     - 服务端逻辑简单，仅处理请求并返回响应，调度压力小
     - 队列等待时间占比99.7%（Avg:45us），上下文切换仅0.3us
   - 两者属于不同进程的统计数据，数值差异巨大属于正常现象

2. **Link Sched Latency说明**：
   - 统计对象：**服务端进程**从cut_in_msg到ProcessRpcRequest的调度耗时
   - 组成过程：
     1. Enqueue Prepare（Avg:70ns）：TaskMeta初始化、属性设置、栈分配等入队前准备工作
     2. Queue Wait（Avg:45us）：任务在运行队列中等待被worker线程调度的时间（核心耗时）
     3. Context Switch（Avg:132ns）：任务从队列取出后，寄存器切换、TLS切换等上下文切换开销
   - 总和验证：三个阶段之和45221ns与总调度44934ns的差值仅287ns，误差<1%，在时间统计精度范围内

### 性能结论
1. **核心瓶颈**：无论是客户端还是服务端，队列等待时间都占调度总耗时的99%以上，是高并发下调度耗时非线性增长的根本原因
2. **根因分析**：高并发下运行队列锁竞争加剧、worker线程不足、任务分配不均导致队列等待时间陡增
3. **优化方向**：优化运行队列锁机制、调整worker线程数量、均衡任务分配可以有效降低调度耗时
4. **其他耗时占比**：入队准备和上下文切换耗时占比<1%，不是性能瓶颈，无需重点优化

## bthread任务生命周期与打点分析
### 1. 任务从创建到执行的完整阶段
| 阶段 | 流程说明 | 当前打点位置 | 时间戳 |
|------|----------|--------------|--------|
| **1. 任务创建阶段** | 调用bthread_start_background()创建bthread，从对象池获取TaskMeta，初始化属性、分配栈空间 | `TaskGroup::start_background`中TaskMeta初始化完成后 | `create_ns` |
| **2. 入队准备阶段** | 完成任务属性设置、栈分配，准备加入运行队列 | `start_background`结束到`ready_to_run`开始 | - |
| **3. 入队阶段** | 调用ready_to_run/ready_to_run_remote将任务加入本地/远程运行队列 | `ready_to_run`/`ready_to_run_remote`中push_rq之前 | `enqueue_ns` |
| **4. 队列等待阶段** | 任务在运行队列中等待worker线程调度 | 入队后到出队前 | - |
| **5. 出队阶段** | worker线程从本地队列取任务或从其他worker队列偷取任务 | `sched_to`中获取到next_meta后 | `dequeue_ns` |
| **6. 上下文切换阶段** | 切换bthread上下文，保存当前任务寄存器，加载新任务寄存器、TLS等 | 出队后到任务执行前 | - |
| **7. 执行阶段** | 开始执行任务函数 | `task_runner`最开头 | `start_exec_ns` |

### 2. 各阶段耗时统计
| 耗时统计 | 计算方式 | 说明 |
|----------|----------|------|
| 入队准备耗时 | `enqueue_ns - create_ns` | 任务创建到入队的准备时间 |
| 队列等待耗时 | `dequeue_ns - enqueue_ns` | 任务在队列中等待的时间（占比99%+） |
| 上下文切换耗时 | `start_exec_ns - dequeue_ns` | 出队到开始执行的切换时间 |
| 总调度耗时 | `start_exec_ns - create_ns` | 从创建到执行的总调度时间 |

## Queue Wait耗时原因分析
队列等待时间是调度耗时的核心瓶颈，可能的原因包括：

### 1. 运行队列锁竞争
- **场景**：高并发下大量线程同时往队列push任务，或者多个worker同时偷取任务，导致队列锁严重竞争
- **排查方法**：
  - 添加打点统计`ready_to_run`中`_rq.push`的锁等待时间
  - 统计`steal_task`中锁获取失败的次数
- **优化方向**：使用无锁队列、分段锁、减小锁粒度

### 2. Worker线程不足
- **场景**：worker线程数量太少，任务产生速度远大于worker处理速度，导致队列堆积
- **排查方法**：
  - 统计worker线程的CPU利用率（应该接近100%）
  - 对比任务入队速度和出队速度
- **优化方向**：增加`FLAGS_bthread_concurrency`配置，提高worker线程数量

### 3. 任务分配不均
- **场景**：部分worker队列任务堆积，其他worker空闲，导致任务等待时间过长
- **排查方法**：
  - 统计每个worker队列的长度和任务处理速度
  - 统计任务偷取的成功率和频率
- **优化方向**：优化任务偷取策略、均衡任务分配

### 4. 批量提交延迟
- **场景**：使用`BTHREAD_NOSIGNAL`批量提交任务，任务不会立即被调度，等待flush时才唤醒worker
- **排查方法**：
  - 统计批量提交的任务数量和flush延迟
- **优化方向**：调整批量提交的阈值，避免延迟过高

### 5. 任务优先级反转
- **场景**：低优先级任务长时间占用worker，高优先级任务在队列等待
- **排查方法**：
  - 统计不同优先级任务的等待时间差异
- **优化方向**：实现优先级队列、抢占式调度

### 进一步排查打点建议
如果需要深入定位Queue Wait的具体原因，可以添加以下打点：
1. 统计运行队列push操作的锁等待时间
2. 统计每个worker队列的长度变化
3. 统计任务偷取的成功率、失败次数、偷取耗时
4. 统计worker线程的空闲率
5. 统计批量提交的flush延迟
