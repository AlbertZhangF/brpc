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

---

## brpc框架模块架构分析
### 核心模块划分
brpc框架采用分层架构，核心模块分为以下几层：

#### 1. 基础工具层 (butil)
- 位置：`src/butil/`
- 职责：提供底层基础工具，是所有上层模块的依赖
- 核心功能：
  - 容器类：无锁队列、动态数组、哈希表等
  - 字符串处理：字符串切割、格式化、编码转换
  - 文件I/O：文件读写、目录操作
  - 同步原语：原子操作、自旋锁、条件变量
  - 时间工具：高精度时间戳获取
  - 内存管理：内存池、对象池、智能指针
  - 错误处理：错误码定义、异常处理

#### 2. 协程调度层 (bthread)
- 位置：`src/bthread/`
- 职责：M:N用户态协程库，提供轻量级线程调度能力
- 核心功能：
  - 协程创建与管理：bthread_start_background/urgent等接口
  - 工作窃取调度：每个Worker线程有独立运行队列，空闲时偷取其他队列任务
  - 协程同步原语：bthread_mutex_t、bthread_cond_t、bthread_countdown_event等
  - 本地存储：bthread特有TLS（线程局部存储）
  - 栈管理：栈内存分配与复用
  - 系统调用hook：将阻塞的系统调用转换为非阻塞，避免阻塞Worker线程

#### 3. 统计监控层 (bvar)
- 位置：`src/bvar/`
- 职责：高性能统计变量框架，支持多线程下低开销统计
- 核心功能：
  - 统计类型：Adder（累加器）、Counter（计数器）、LatencyRecorder（时延统计）、Gauge（量表）等
  - 低开销：采用线程局部缓存，统计操作接近无锁
  - 自动暴露：统计变量自动通过HTTP接口`/vars`暴露
  - 百分位统计：内置支持P50/P99/P999等百分位计算
  - 窗口统计：支持按时间窗口统计数据

#### 4. RPC核心层 (brpc)
- 位置：`src/brpc/`
- 职责：RPC框架核心实现，包含服务端、客户端、协议解析、传输层等
- 核心子模块：
  - **服务端**：Server类、服务注册、请求处理流程
  - **客户端**：Channel类、负载均衡、熔断、重试机制
  - **协议层**：支持baidu_std、HTTP、gRPC、Thrift、Redis等多种协议
  - **传输层**：TCP、RDMA、UDP等传输方式实现
  - **命名服务**：支持文件、DNS、Consul、Nacos等多种服务发现方式
  - **内置服务**：/status（服务状态）、/rpcz（请求追踪）、/connections（连接管理）等
  - **IOBuf**：零拷贝缓冲区，高效网络数据传输

#### 5. 序列化层
- 位置：`src/json2pb/`、`src/mcpack2pb/`
- 职责：不同序列化格式与Protobuf之间的转换
- 核心功能：
  - json2pb：JSON与Protobuf消息的互相转换
  - mcpack2pb：mcpack（百度自研序列化格式）与Protobuf的互相转换

### 模块交互关系
```
┌─────────────────────────────────────────────────────────┐
│                    业务应用层                            │
└─────────┬───────────────────────────────────────────────┘
          │
┌─────────▼───────────────────────────────────────────────┐
│                    RPC核心层 (brpc)                     │
│  ┌──────────┐  ┌──────────┐  ┌─────────┐  ┌──────────┐  │
│  │  服务端  │  │  客户端  │  │ 协议层  │  │  传输层  │  │
│  └──────────┘  └──────────┘  └─────────┘  └──────────┘  │
└─────────┬───────────────────────────┬───────────────────┘
          │                           │
┌─────────▼─────────┐     ┌───────────▼──────────┐
│  协程调度层(bthread) │     │  统计监控层(bvar)   │
└─────────┬─────────┘     └───────────┬──────────┘
          │                           │
┌─────────▼───────────────────────────▼───────────────────┐
│                    基础工具层 (butil)                    │
└─────────────────────────────────────────────────────────┘
```

- **所有模块都依赖butil**：butil提供最基础的工具函数和数据结构，是整个框架的基石
- **brpc依赖bthread和bvar**：brpc使用bthread进行异步请求处理，使用bvar统计各种运行指标
- **bthread依赖butil**：协程调度需要使用butil提供的同步原语、时间工具和内存管理
- **bvar依赖butil**：统计框架需要使用butil提供的原子操作和容器

---

## rdma_performance示例分析
### 示例结构
- 位置：`example/rdma_performance/`
- 包含文件：
  - `test.proto`：RPC服务定义，包含PerfTestRequest和PerfTestResponse消息
  - `server.cpp`：服务端实现，处理RPC请求
  - `client.cpp`：客户端实现，发送大量请求测试性能
  - `run.sh`：运行脚本，启动服务端和客户端

### 端到端工作流分析
#### 1. 服务端启动流程
```
1. 解析命令行参数，配置是否启用RDMA
2. 创建brpc::Server实例
3. 注册PerfTestService服务到Server
4. 配置ServerOptions，启用RDMA支持
5. 调用server.Start()监听指定端口
   - 初始化传输层：创建RDMA监听端点，绑定IP和端口
   - 启动Worker线程池：初始化bthread调度器，创建指定数量的Worker线程
   - 启动IO线程：监听网络事件，处理RDMA completion queue事件
6. 调用server.RunUntilAskedToQuit()进入服务循环
```

#### 2. 客户端启动流程
```
1. 解析命令行参数，配置并发数、队列深度、是否启用RDMA等
2. 初始化全局统计变量（LatencyRecorder等）
3. 启动令牌生成线程，控制请求发送速率（当指定expected_qps时）
4. 为每个并发线程创建PerformanceTest实例
5. 调用performance_test.Init()初始化Channel
   - 创建brpc::Channel实例，配置RDMA、协议、连接类型等参数
   - 建立到服务端的RDMA连接：完成RDMA握手，注册内存区域，创建QP（Queue Pair）
   - 发送测试请求，验证连接可用性
6. 启动所有发送线程，开始压测
```

#### 3. 请求发送流程（客户端）
```
1. 发送线程循环调用SendRequest()
2. 令牌桶限流（如果启用expected_qps）：等待获取令牌
3. 构造PerfTestRequest请求，填充请求参数和attachment
4. 创建brpc::Controller和Response对象
5. 创建回调closure，指定HandleResponse为响应处理函数
6. 调用stub.Test()异步发送请求
   - 协议层：将请求序列化为baidu_std格式
   - 传输层：将数据放入RDMA发送队列，通过RDMA WRITE/SEND操作发送到服务端
   - 立即返回，不阻塞发送线程
```

#### 4. 服务端请求处理流程
```
1. RDMA网卡接收数据，写入预先注册的内存缓冲区
2. 完成队列(CQ)产生完成事件，通知IO线程
3. IO线程调用ProcessNewMessage处理接收到的消息
   - 调用cut_in_msg解析消息格式，验证完整性
   - 设置_cut_done_ns时间戳，记录消息解析完成时间
   - 调用QueueMessage将消息加入调度队列
   - 调用bthread_start_background创建bthread，执行ProcessInputMessage函数
4. bthread调度执行
   - 任务进入运行队列，等待Worker线程调度
   - Worker线程从队列中取出任务，切换上下文执行
   - 调用ProcessInputMessage，最终调用ProcessRpcRequest处理请求
5. 执行业务逻辑（Test方法）
   - 获取bthread调度各阶段耗时，设置到response中
   - 处理请求参数，如果指定complexity则进行CPU密集计算
   - 如果需要echo_attachment，将请求attachment复制到响应
6. 发送响应
   - 调用done->Run()触发响应发送
   - 协议层序列化响应消息
   - 传输层通过RDMA将响应发送回客户端
```

#### 5. 客户端响应处理流程
```
1. RDMA网卡接收响应数据，写入内存缓冲区
2. CQ产生完成事件，通知IO线程
3. IO线程解析响应，触发回调函数HandleResponse
4. HandleResponse处理响应
   - 检查请求是否成功
   - 记录请求总时延到g_latency_recorder
   - 记录客户端bthread调度耗时
   - 记录服务端返回的调度耗时数据
   - 统计总请求数和总字节数
5. 继续发送下一个请求
```

#### 6. 测试结束流程
```
1. 达到指定的测试时间或测试次数后，设置g_stop标志
2. 等待所有发送线程退出
3. 收集所有统计数据
4. 输出测试结果：QPS、平均时延、P99时延、bthread调度各阶段耗时等
5. 释放资源，退出进程
```

### RDMA与TCP流程差异
| 阶段 | RDMA路径 | TCP路径 |
|------|----------|---------|
| 数据传输 | 用户态直接操作RDMA网卡，内核零拷贝 | 数据需要经过内核TCP/IP协议栈，多次内存拷贝 |
| 通知机制 | 基于Completion Queue的异步通知 | 基于epoll/poll的IO事件通知 |
| 上下文开销 | 不需要内核上下文切换，开销极低 | 系统调用需要内核上下文切换，开销较高 |
| 内存管理 | 需要提前注册内存区域，网卡直接访问 | 内存由内核管理，不需要提前注册 |
| 连接建立 | 需要RDMA专用握手流程，建立QP、CQ等资源 | 标准TCP三次握手 |

---

## 后续计划
下一步将整合以上分析结果，生成完整的brpc架构和工作流分析文档，包含模块交互图和端到端流程图。
