# `example/rdma_performance` 当前 Benchmark 模型说明

## 1. 背景与目标

`example/rdma_performance` 原本更接近一个最小化的 RPC 压测样例：客户端持续发起 `PerfTestService::Test` 调用，服务端只做极轻量处理，并可选回显 attachment。这个模型能够快速得到一组 QPS、延迟和吞吐结果，但它默认把“线程数”“请求并发”“连接并发”混在一起，难以回答下面这些更具体的问题：

- 压力是被 worker 数限制，还是被真实 TCP 连接数限制。
- `connection_type=single` 时，多个 `Channel` 是否实际上仍然复用了同一条底层连接。
- 闭环模型下的 `thread_num * queue_depth` 是否先于 CPU/NIC 打满而成为瓶颈。
- 请求批量发出后，框架的写等待和收包分发行为是什么。

当前版本的 benchmark 仍然保留原始示例“轻业务、重链路”的基本定位，但目标已经从“快速打一组 QPS”扩展为“把负载模型、连接模型和框架观测拆开”。因此，它更适合回答：

- 当前测到的是闭环固定并发性能，还是开环持续加压性能。
- 当前 worker 数和真实连接数是否一致。
- 当前连接是否复用，还是显式扇出成多条独立连接。
- 当前瓶颈更偏向发送等待、输入批处理，还是服务端处理能力。

它**不**是一个通用 RPC benchmark 框架，也不直接给出“系统理论上限”。它更像一个针对 brpc TCP/RDMA 路径的可控实验 harness。

## 2. 当前模型总览

### 2.1 Client 侧核心对象

当前 client 模型主要由四类对象组成：

- `Worker`
  - 表示一个发压 worker。
  - 持有本 worker 的 attachment 缓冲区、起始时间、停止标记等。
  - 负责在闭环或开环模型下调用 `SendRequest()`。

- `ConnectionSlot`
  - 表示一个显式创建的连接槽位。
  - 每个 slot 持有一个 `brpc::Channel*`、目标 server 地址、可选的 `connection_group`，以及该 slot 上的 `sent/completed/failed/timeouts` 统计。
  - `connection_num` 控制的就是这类对象的数量。

- `RespClosure`
  - 是一次异步 RPC 的完成回调上下文。
  - 里面绑定了 `Controller`、响应对象、所属 `Worker` 和所属 `ConnectionSlot`。

- 全局原子计数器
  - 如 `g_total_cnt`、`g_failed_cnt`、`g_timeout_cnt`、`g_inflight`、`g_peak_inflight`、`g_request_budget`、`g_rr_index`、`g_token` 等。
  - 它们负责跨 worker 聚合统计、节流和终止条件。

### 2.2 Server 侧角色

server 仍然保持极轻业务模型：

- 服务方法是 `PerfTestServiceImpl::Test()`。
- 每个请求会更新轻量计数器 `rdma_perf_server_request_count`。
- 如果开启 `echo_attachment`，server 会把 `request_attachment` 直接写回到 `response_attachment`，同时累计 `rdma_perf_server_response_attachment_bytes`。
- server 还会周期性采样 `process_cpu_usage`，把 CPU 使用率字符串写回响应中的 `cpu_usage` 字段，供 client 侧聚合展示。

这意味着 benchmark 的主要压力仍然在网络路径、框架收发路径和请求调度路径，而不是复杂业务逻辑。

### 2.3 连接、worker、请求、响应、统计之间的关系

当前模型把几个经常被混淆的层次拆开了：

- `thread_num`
  - 控制 worker 数量。
  - worker 是“谁来发请求”的并发单位。

- `connection_num`
  - 控制 `ConnectionSlot/Channel` 数量。
  - connection slot 是“请求从哪条逻辑连接发出去”的并发单位。

- `inflight`
  - 表示当前已经发出但尚未完成的请求数量。
  - 是“逻辑请求并发”的运行时量。

- `QPS`
  - 是单位时间完成的 RPC 数量。
  - 反映的是完成速率，不等于 worker 数，也不等于连接数。

请求从 worker 产生，随后按 round-robin 分配到某个 `ConnectionSlot`，再由该 slot 内的 `Channel` 异步发出。响应完成后通过 `RespClosure` 回到 client，更新延迟、吞吐、失败和 per-connection 统计；若处于闭环模式，则响应完成还会驱动下一次发送。

## 3. 执行流程

下面按实际代码路径描述一次 benchmark 从启动到结束的完整流程。

### 3.1 进程启动与参数解析

client 在 `main()` 中首先解析 gflags，并校验 `load_mode` 只能是：

- `closed_loop`
- `open_loop`

如果是其他值，程序直接退出。

### 3.2 可选 RDMA 初始化分支

当前 benchmark 同时支持 `use_rdma=true/false` 两种模式：

- `use_rdma=true`
  - 先执行 `brpc::rdma::GlobalRdmaInitializeOrDie()`。
- `use_rdma=false`
  - 直接走普通 TCP 路径，不做 RDMA 初始化。

本文档聚焦 benchmark 模型本身，因此不区分后续 RPC 是否走 TCP 或 RDMA 数据面；两者在 client 侧的发压模型是一致的。

### 3.3 dummy server 启动

client 会调用 `brpc::StartDummyServerAt(FLAGS_dummy_port)`。

这个 dummy server 不是压测目标，而是 brpc client 进程在某些场景下需要的本地辅助监听端口。它不参与请求处理，但属于当前 benchmark 的启动流程之一。

### 3.4 server 列表解析

`FLAGS_servers` 采用 `+` 分隔，client 会把它拆成 `g_servers` 列表。例如：

```text
192.168.3.190:8002+192.168.3.191:8002
```

会变成两个 server 目标。后续创建 `ConnectionSlot` 时，slot 会按下标轮询映射到这些 server 地址。

### 3.5 计算本轮测试的连接与并发参数

进入 `Test(thread_num, attachment_size)` 后，client 会先计算两个核心量：

- `actual_connection_num`
  - 若 `connection_num > 0`，取 `connection_num`
  - 否则退化为 `thread_num`

- `open_loop_inflight_limit`
  - 若 `max_inflight > 0`，取 `max_inflight`
  - 否则退化为 `thread_num * queue_depth`

这里已经能看出当前模型与原始版本的关键区别：worker 数和连接数不再默认绑定为同一个概念。

### 3.6 全局状态清零

每轮 `Test()` 开始时，client 会重置：

- 成功/失败/超时统计
- inflight 和 peak inflight
- request budget
- round-robin 连接选择下标
- 开环 inflight 上限

同时抓取一次 bvar 快照，后续用于输出“本轮运行前后发生了多少连接/等待/批处理事件”。

### 3.7 初始化 `ConnectionSlot`

`InitConnectionSlots(actual_connection_num, FLAGS_echo_attachment)` 是当前 benchmark 的连接初始化核心。

每个 slot 的初始化流程如下：

1. 为 slot 选定目标 server
2. 根据 `unique_connection_group` 决定是否生成唯一的 `connection_group`
3. 构造 `ChannelOptions`
4. 调用 `Channel::Init()`
5. 立刻对该 `Channel` 发起一次同步 warmup RPC

warmup RPC 有两个作用：

- 尽早暴露“这个连接槽位根本不可用”的问题，而不是等正式压测开始后才随机失败。
- 让连接创建和协议初始化尽量在正式计时前完成。

如果任何一个 slot 的 warmup 失败，整个 benchmark 直接退出。

### 3.8 worker 创建

初始化完成后，client 会创建 `thread_num` 个 `Worker` 对象。

每个 worker：

- 保存自己的 `worker_index`
- 根据 `attachment_size` 预生成一段 attachment 数据
- 记录是否启用 `echo_attachment`

这些 worker 是后续 bthread 的执行主体。

### 3.9 可选的 token 生成线程

如果 `expected_qps > 0`，client 会额外启动一个后台 bthread 执行 `GenerateToken()`。

这个线程不是发请求线程，而是一个全局令牌补充器：

- 每 100ms 检查一次当前累积 token 数
- 尝试把令牌补齐到接近期望 QPS 对应的理论值

`SendRequest()` 在真正发请求前会尝试消耗一个 token。这样即使是开环模式，也可以在“持续加压”之外再加一层速率上限。

### 3.10 `closed_loop` 发压流程

闭环模型由 `RunClosedLoopWorker()` 驱动。

其流程是：

1. worker 记录 `start_time_us`
2. 启动时先发出 `queue_depth` 个请求
3. 每当某个请求响应完成，`HandleResponse()` 会在闭环模式下再次调用 `SendRequest()`

所以它的关键特征是：

- 每个 worker 在运行初期会把自己的“窗口”填满
- 后续发送节奏由响应完成节奏驱动
- 在无额外阻塞时，稳态 inflight 近似受 `thread_num * queue_depth` 约束

这就是“固定闭环并发”模型。

### 3.11 `open_loop` 发压流程

开环模型由 `RunOpenLoopWorker()` 驱动。

其流程是：

1. worker 记录 `start_time_us`
2. 在循环中不断检查：
   - 是否达到测试时长
   - 是否耗尽 `test_iterations`
   - 当前 `g_inflight` 是否已经到达全局上限
3. 只要未触发停止条件，且未达到 inflight 上限，就直接调用 `SendRequest()`

它与闭环模型的本质差异是：

- 不再依赖“完成一个请求，再补一个请求”
- worker 会独立持续推进发送
- 背压主要由 `max_inflight` 或默认的 `thread_num * queue_depth` 上限控制

因此它更像“持续给系统施加 offered load”，而不是“维持一个固定窗口的稳定闭环”。

### 3.12 单次请求的发送过程

所有模式最终都会落到 `SendRequest(worker)`。

它的步骤是：

1. 调用 `TryAcquireRequestPermit()`
   - 检查是否到达测试结束时间
   - 检查 `test_iterations` 预算
   - 检查 `expected_qps` 令牌
2. 通过 `PickConnectionSlot()` 以 round-robin 方式选一个 slot
3. 创建本次请求对应的 `Controller`、响应对象和 `RespClosure`
4. 构造 `PerfTestRequest`
5. 把 worker 的 attachment 附加到 `request_attachment`
6. 通过 `PerfTestService_Stub(slot->channel).Test(...)` 异步发出请求
7. 更新：
   - slot 的 `sent`
   - 全局 `g_inflight`
   - 全局 `g_peak_inflight`

这里要注意：请求的发送者是 worker，但请求最终落在哪个连接上，是由全局 round-robin 选 slot 决定的，而不是“某个 worker 永远绑定某个连接”。

### 3.13 响应处理流程

异步 RPC 完成后，会回调 `HandleResponse(RespClosure*)`。

它负责：

1. 释放 `Controller`、响应对象和 closure 资源
2. 先把 `g_inflight` 减 1
3. 若请求失败：
   - 更新 slot 失败计数
   - 更新全局失败计数
   - 若错误码是 `brpc::ERPCTIMEDOUT`，额外更新超时计数
   - 打印失败日志
   - 触发 worker 停止和全局停止
4. 若请求成功：
   - 更新 slot 的 `completed`
   - 更新全局延迟、吞吐、QPS、CPU 采样统计
   - 检查是否达到停止条件
   - 如果当前是 `closed_loop`，则立即再发一个请求

因此，`HandleResponse()` 既是统计汇聚点，也是闭环模型中的“继续发送”触发点。

### 3.14 停止条件

benchmark 的停止条件包括：

- 到达 `test_seconds`
- 到达 `test_iterations`
- 某次 RPC 失败并触发 `g_stop`
- 所有 worker 都停止，且 inflight 归零

主线程会轮询 worker 停止状态和全局 inflight，直到满足退出条件。

### 3.15 汇总输出

测试结束后，client 会输出：

- 延迟分位数
- Throughput
- QPS 或 Completed 数
- Failed / Timeout
- PeakInflight
- Server / Client CPU 利用率
- `rpc_channel_connection_count` 的 init/run delta
- 每个 `ConnectionSlot` 的 sent/completed/failed/timeouts
- 各种 bvar delta

这些输出共同构成“当前模型到底是如何打压、如何建连、框架如何处理”的可观测结果。

## 4. 并发与连接模型

这一节专门解释最容易被混淆的几个参数。

### 4.1 `thread_num` 控制什么

`thread_num` 控制的是 worker 数量。

它回答的是：

- 同时有多少个 worker 在推进发送逻辑

它**不直接等于**：

- TCP 连接数
- `Channel` 数
- inflight 上限
- 最终 QPS

### 4.2 `queue_depth` 在 `closed_loop` 中如何起作用

`queue_depth` 只在闭环模型中直接决定每个 worker 的初始窗口大小。

在 `closed_loop` 下：

- 每个 worker 启动后先发 `queue_depth` 个请求
- 响应完成后继续补发
- 因此稳态 inflight 常常接近 `thread_num * queue_depth`

在 `open_loop` 下，`queue_depth` 不再直接控制发送窗口；它只会作为 `max_inflight` 未显式设置时的默认退化值参与计算。

### 4.3 `connection_num` 控制什么

`connection_num` 控制预创建的 `ConnectionSlot/Channel` 数量。

它回答的是：

- benchmark 逻辑上准备了多少个独立的连接槽位供请求分配

它**不等同于**：

- worker 数
- 最终一定建立的 TCP 连接数

后者还会受 `connection_type` 和 `connection_group` 是否复用的影响。

### 4.4 `unique_connection_group` 如何影响 SocketMap 复用

当 `unique_connection_group=false` 时：

- 多个 `Channel` 可能带着相同的 server 地址和空 `connection_group`
- 在 `connection_type=single` 场景下，底层仍有机会通过 SocketMap 复用同一个 `Socket`

当 `unique_connection_group=true` 时：

- 每个 slot 都会生成独一无二的 `connection_group`
- 这样能显著降低不同 `Channel` 被合并复用到底层同一连接的概率
- benchmark 因而更接近“显式独占连接”的实验模型

所以，这个开关的意义不是“多生成几个对象”，而是“显式打破默认复用路径”。

### 4.5 `connection_type=single` 与 `connection_num` 的组合含义

在当前 benchmark 里，最常用的组合是：

- `connection_type=single`
- 再搭配不同的 `connection_num`
- 再决定是否 `unique_connection_group=true`

这组组合可以回答：

- 如果只增大 worker，不增大连接，QPS 是否仍然受限
- 如果显式把连接槽位扇出成多条独立连接，QPS/CPU/等待统计是否继续变化

因此，`connection_type=single` 在当前模型中不再只是“单连接模式”这么简单，它和 `connection_num`、`unique_connection_group` 一起定义了实验中的连接组织方式。

### 4.6 `max_inflight` 如何在 `open_loop` 中形成全局背压

`open_loop` 的发送不受响应回补驱动，因此必须有一个显式上限防止无限堆积。

这个上限就是：

- 若 `max_inflight > 0`，取 `max_inflight`
- 否则退化为 `thread_num * queue_depth`

worker 在发现 `g_inflight >= inflight_limit` 时会短暂休眠，然后重试发送。也就是说，`max_inflight` 控制的是：

- 整个 client 进程允许积压的全局逻辑请求并发上限

它不是单 worker 限额，也不是单连接限额。

## 5. 统计与观测项

### 5.1 延迟、QPS、Throughput、Failed、Timeout、PeakInflight

这些是 benchmark 输出的核心业务统计：

- `Avg-Latency` / `90th` / `99th` / `99.9th`
  - 来自 `g_latency_recorder`
  - 统计成功请求的 RPC 延迟

- `Throughput`
  - 由累计 attachment 字节数除以运行时长得到
  - 更适合在 attachment 非零时观察数据面吞吐

- `QPS`
  - 基于 `g_total_cnt` 和测试时长估算
  - 反映完成速率

- `Failed`
  - 累计失败请求数

- `Timeout`
  - 累计 `ERPCTIMEDOUT` 请求数

- `PeakInflight`
  - 运行期间观测到的 `g_inflight` 最大值
  - 是运行结果，不是预设参数本身

### 5.2 per-connection stats 的来源

每个 `ConnectionSlot` 都维护：

- `sent`
- `completed`
- `failed`
- `timeouts`

这些值由：

- `SendRequest()` 中的发送路径更新 `sent`
- `HandleResponse()` 中的成功/失败路径更新 `completed/failed/timeouts`

它们用于观察：

- 请求是否在连接之间均匀分布
- 某些连接是否明显更慢或更容易超时

### 5.3 `rpc_channel_connection_count`

client 会在测试前后读取 bvar：

- `rpc_channel_connection_count`

然后输出其 delta。这个值的意义是：

- 当前进程中 `Channel` 侧连接对象数量的近似变化

它可以帮助判断：

- 显式创建的 `connection_num` 是否真的带来了更多连接对象

但它**不应该被误读为**：

- 精确的 TCP 连接抓包结果
- 绝对准确的“真实物理连接数”

它是一个低侵入近似观测，而不是协议级精确计数。

### 5.4 `rpc_waitepollout_*`

当前模型新增并使用了以下 socket 写等待相关 bvar：

- `rpc_waitepollout_count`
- `rpc_waitepollout_time_us`
- `rpc_waitepollout_wakeup_count`

它们回答的是：

- 发送路径是否因为 socket 暂时不可写而进入等待
- 总共等了多久
- 被唤醒了多少次

它们主要反映的是 client 侧写路径背压情况，而不是 server 业务处理时间。

### 5.5 `rpc_process_new_message_*`

当前模型还新增并输出输入消息处理相关 bvar：

- `rpc_process_new_message_count`
- `rpc_process_new_message_parsed_messages`
- `rpc_process_new_message_batched_calls`
- `rpc_process_new_message_direct_process_count`
- `rpc_process_new_message_queued_bthread_count`

它们用于观察：

- 一次读事件后处理了多少消息
- 是否形成了批处理
- 请求处理更偏向直接执行，还是更多转交给 bthread 调度

它们反映的是框架输入侧解析和调度行为，不应被直接等同于“服务端真正处理了多少业务逻辑”。

## 6. 与原始 benchmark 的差异

### 6.1 原始模型的特点

原始 `rdma_performance` 示例可以概括为：

- one-test-one-channel 风格
- 以 worker 驱动发送
- 只有闭环模型
- 没有显式的连接扇出控制
- 没有把连接复用、写等待、输入批处理单独暴露出来

因此原始版本更适合做一个快速样例，但不适合严谨地区分：

- worker 并发
- 逻辑请求并发
- 真实连接并发

### 6.2 当前版本新增的连接扇出能力

当前版本新增了：

- `connection_num`
- `unique_connection_group`

这使得 benchmark 可以显式控制：

- 创建多少个 `Channel`
- 是否让这些 `Channel` 尽量独占连接而不是复用

这是一类结构性改变，因为它改变了实验解释的最小单位：以前更接近“一个 worker 带着一个 channel 去跑”，现在则变成“worker 和连接是两套可独立配置的维度”。

### 6.3 当前版本新增的开环模型

原始版本只有闭环模型，而当前版本新增了 `open_loop`：

- 原始版本更强调“固定窗口下的稳态完成速率”
- 当前版本还能测试“在全局 inflight 上限保护下，持续把 offered load 推向系统”

这意味着：

- 当前版本可以更容易暴露 server、网络或调度侧的滞后
- 但开环结果不能和闭环结果直接按同一语义解读

### 6.4 当前版本新增的框架观测项

当前版本不只统计应用层 QPS/延迟，还补充了框架观测：

- `rpc_channel_connection_count`
- `rpc_waitepollout_*`
- `rpc_process_new_message_*`
- server 侧 `rdma_perf_server_request_count`
- server 侧 `rdma_perf_server_response_attachment_bytes`

这些观测项的价值是：

- 帮助定位“负载是如何被送出去并被框架处理的”
- 不再只看最后一行 QPS

### 6.5 这些变化如何改变实验结果的解释边界

原始 benchmark 里，一组低 QPS 往往很难解释到底是：

- 并发窗口太小
- 连接复用太重
- 写路径堵塞
- 输入侧批处理不足

当前版本虽然没有直接自动给出根因，但至少把这些层次拆出来了。因此它更适合作为“可控实验平台”，而不是“只看一个数字的黑盒压测器”。

## 7. 用法与实验建议

下面给出几类推荐用法，并说明每类实验适合回答什么问题。

### 7.1 启动 server

```bash
./example/rdma_performance/rdma_performance_server \
  --use_rdma=false \
  --port=8002
```

这个 server 保持最小业务逻辑，适合作为所有 client 场景的共同被测端。

### 7.2 基线测试：闭环 + 共享连接行为

```bash
./example/rdma_performance/rdma_performance_client \
  --use_rdma=false \
  --protocol=baidu_std \
  --connection_type=single \
  --servers=127.0.0.1:8002 \
  --thread_num=16 \
  --queue_depth=16 \
  --load_mode=closed_loop \
  --connection_num=16 \
  --unique_connection_group=false \
  --attachment_size=0 \
  --echo_attachment=false \
  --test_seconds=20 \
  --report_connection_stats=true \
  --report_wait_stats=true
```

适合回答：

- 原始闭环模型下，默认连接组织方式的表现是什么。
- `thread_num * queue_depth` 这个窗口大致能把结果推到哪里。

### 7.3 连接复用对照：显式独占连接

```bash
./example/rdma_performance/rdma_performance_client \
  --use_rdma=false \
  --protocol=baidu_std \
  --connection_type=single \
  --servers=127.0.0.1:8002 \
  --thread_num=16 \
  --queue_depth=16 \
  --load_mode=closed_loop \
  --connection_num=16 \
  --unique_connection_group=true \
  --attachment_size=0 \
  --echo_attachment=false \
  --test_seconds=20 \
  --report_connection_stats=true \
  --report_wait_stats=true
```

适合回答：

- 如果不让不同 channel 复用 SocketMap，真实连接并发是否上升。
- QPS、CPU 和等待统计是否随之变化。

### 7.4 `pooled` 对照

```bash
./example/rdma_performance/rdma_performance_client \
  --use_rdma=false \
  --protocol=baidu_std \
  --connection_type=pooled \
  --servers=127.0.0.1:8002 \
  --thread_num=16 \
  --queue_depth=16 \
  --load_mode=closed_loop \
  --connection_num=16 \
  --unique_connection_group=true \
  --attachment_size=0 \
  --echo_attachment=false \
  --test_seconds=20 \
  --report_connection_stats=true \
  --report_wait_stats=true
```

适合回答：

- 在连接池模式下，连接组织方式是否与 `single` 有明显不同。
- 相同 worker/连接配置下，池化策略是否影响等待和吞吐。

### 7.5 开环压力测试

```bash
./example/rdma_performance/rdma_performance_client \
  --use_rdma=false \
  --protocol=baidu_std \
  --connection_type=single \
  --servers=127.0.0.1:8002 \
  --thread_num=16 \
  --queue_depth=16 \
  --load_mode=open_loop \
  --connection_num=32 \
  --max_inflight=4096 \
  --unique_connection_group=true \
  --attachment_size=0 \
  --echo_attachment=false \
  --test_seconds=20 \
  --report_connection_stats=true \
  --report_wait_stats=true
```

适合回答：

- 当不再受闭环回补节奏束缚时，系统能否继续吸收更高 offered load。
- 发送等待、输入批处理或失败超时会不会更快出现。

### 7.6 attachment 回显测试

```bash
./example/rdma_performance/rdma_performance_client \
  --use_rdma=false \
  --protocol=baidu_std \
  --connection_type=single \
  --servers=127.0.0.1:8002 \
  --thread_num=16 \
  --queue_depth=32 \
  --load_mode=open_loop \
  --connection_num=32 \
  --max_inflight=4096 \
  --unique_connection_group=true \
  --attachment_size=1024 \
  --echo_attachment=true \
  --test_seconds=20 \
  --report_connection_stats=true \
  --report_wait_stats=true
```

适合回答：

- 在真实负载字节数增大时，吞吐和框架等待统计如何变化。
- attachment 回显是否把瓶颈从请求调度推向网络收发。

## 8. 限制与注意事项

### 8.1 这不是通用 RPC benchmark 框架

当前工具聚焦 `PerfTestService::Test` 这一条极简链路，不提供：

- 丰富业务逻辑模型
- 多种请求分布模型
- 自动化结果归因

它的定位是“可控、可观测、便于比较不同连接/并发模型”的专项 benchmark。

### 8.2 “连接数”“并发度”“QPS”不是同一个概念

当前输出里至少有三种不同层次：

- 连接并发：`connection_num` 和近似连接计数
- 逻辑请求并发：`inflight` / `PeakInflight`
- 完成速率：QPS

它们之间相关，但不能互相替代。

### 8.3 `open_loop` 更容易暴露瓶颈，但语义不同

`open_loop` 的价值是更容易把系统推向极限并暴露滞后，但它和 `closed_loop` 的结果不能直接横向类比成“谁一定更好”。两者回答的是不同问题：

- `closed_loop` 更像固定窗口下的稳定完成性能
- `open_loop` 更像在显式背压保护下的持续加压性能

### 8.4 部分 bvar 是近似观测，不是协议级精确计数

例如：

- `rpc_channel_connection_count`
- `rpc_waitepollout_*`
- `rpc_process_new_message_*`

这些值对实验分析非常有帮助，但它们本质上是框架内部计数器，不应被当成抓包级、协议级绝对真值。

### 8.5 warmup RPC 会影响“建连阶段”与“正式运行阶段”的划分

由于每个 `ConnectionSlot` 会在正式计时前先做同步 warmup，因此：

- 一部分连接建立和初始化成本已经提前发生
- 运行输出中的 `after init delta` 与 `run delta` 要结合起来看

这是当前模型为了“尽早暴露不可用连接并减少冷启动噪声”而做的有意设计。

## 9. 一句话使用建议

如果你只想复现原始样例语义，优先用：

- `load_mode=closed_loop`
- `connection_num=thread_num`
- `unique_connection_group=false`

如果你要区分“线程并发”和“真实连接并发”，优先调：

- `connection_num`
- `unique_connection_group`
- `connection_type`

如果你要验证系统在更高 offered load 下的反应，优先切到：

- `load_mode=open_loop`
- 并显式设置 `max_inflight`

理解这三层区别，是正确解读当前 benchmark 结果的前提。
