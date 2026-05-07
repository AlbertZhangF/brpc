# steal_task 成因分析与远程实验方案

## 背景与目标

本文用于在远程环境中系统验证 `steal_task` 高占比的来源，并区分三类原因：

- 任务粒度过短，调度和 work stealing 成本被放大。
- 任务创建或入队分布不均，导致跨 `TaskGroup` steal 增多。
- worker、event dispatcher、连接数、inflight、runqueue 等参数配置导致调度压力。

默认实验对象是当前 `example/rdma_performance`，并且固定分析 `use_rdma=false` 的 TCP 路径。

核心判断口径：

- `steal_task` 高不一定表示异常。它表示当前 `TaskGroup` 本地 `_rq` 没有 ready task，正在查找本 group `_remote_rq`、tag priority queue、其他 group `_rq/_remote_rq`。
- 在轻业务 benchmark 中，`ProcessInputMessage -> ProcessRpcRequest -> PerfTestServiceImpl::Test -> SendRpcResponse` 很短，worker 会频繁回到调度器，因此容易放大 `steal_task`。
- 需要同时观察 QPS、延迟、CPU、火焰图、`bthread_group_status`、worker usage、连接数，不能只凭 `steal_task` 占比判断瓶颈。

## 源码机制摘要

server 主路径中的高频 bthread 是：

```text
ProcessEvent
  -> InputMessenger::OnNewMessages
  -> ProcessNewMessage
  -> QueueMessage
  -> ProcessInputMessage
  -> ProcessRpcRequest
  -> PerfTestServiceImpl::Test
  -> SendRpcResponse
```

`ProcessInputMessage` 通常由 `ProcessEvent` bthread 创建，进入当前 group 的本地 `_rq`。其他 group 可以通过 `steal_task` 偷取这些 ready task。

non-worker 随机投递到 `_remote_rq` 主要发生在启动、维护、timer 或 main 线程创建 bthread，通常不是高 QPS 请求主路径的主要来源。

`steal_task` 的直接触发条件：

```text
TaskGroup::sched / ending_sched
  -> 当前 group _rq.pop() 失败
  -> TaskGroup::steal_task()
```

扫描顺序：

```text
1. 当前 group _remote_rq
2. tag priority_queue
3. 其他 group _rq
4. 其他 group _remote_rq
```

相关数据结构：

| 数据结构 | 类型 | 作用 |
|---|---|---|
| `_rq` | `WorkStealingQueue<bthread_t>` | 当前 group 本地 ready queue，固定容量无锁 ring |
| `_remote_rq` | `RemoteTaskQueue` | 外部线程或远程上下文投递到该 group 的队列，`BoundedQueue + mutex` |
| `_priority_queues[tag]` | `WorkStealingQueue<bthread_t>` | tag 级优先任务队列 |
| `_tagged_groups[tag]` | `TaskGroup*` 数组 | 同 tag 下所有可被遍历的 group |

## 实验变量

远程执行前先设置统一变量：

```bash
SERVER_IP=192.168.3.220
PORT=8003
CLIENT_BIN=./example/rdma_performance/rdma_performance_client
SERVER_BIN=./example/rdma_performance/rdma_performance_server
```

建议每次实验单独保存目录：

```bash
CASE=baseline
mkdir -p ~/steal_task_exp/$CASE
```

## 通用采集命令

server 进程：

```bash
SERVER_PID=$(pgrep -f rdma_performance_server | head -n1)
```

perf 采样：

```bash
perf record -F 99 -g -p $SERVER_PID -- sleep 30
perf report --stdio > server.perf.txt
```

热点摘要：

```bash
egrep 'steal_task|wait_task|ending_sched|ProcessInputMessage|ProcessRpcRequest|ProcessEvent|Socket::Write|SendRpcResponse' server.perf.txt | head -80
```

bvar：

```bash
curl -s "http://127.0.0.1:$PORT/vars/bthread_group_status"
curl -s "http://127.0.0.1:$PORT/vars/bthread_worker_usage*"
curl -s "http://127.0.0.1:$PORT/vars/bthread_count*"
curl -s "http://127.0.0.1:$PORT/vars/bthread_worker_count*"
```

线程 CPU：

```bash
pidstat -t -p $SERVER_PID 1 30
top -H -p $SERVER_PID
```

连接数：

```bash
ss -tan | grep ":$PORT" | wc -l
ss -tinp | grep rdma_performance | head -50
```

## 实验 1：基线与观测

server：

```bash
$SERVER_BIN \
  --use_rdma=false \
  --port=$PORT \
  --bthread_concurrency=32 \
  --event_dispatcher_num=1 \
  --show_per_worker_usage_in_vars=true
```

client：

```bash
$CLIENT_BIN \
  --use_rdma=false \
  --protocol=baidu_std \
  --connection_type=single \
  --servers=$SERVER_IP:$PORT \
  --thread_num=16 \
  --queue_depth=16 \
  --load_mode=closed_loop \
  --connection_num=16 \
  --unique_connection_group=true \
  --attachment_size=0 \
  --echo_attachment=false \
  --test_seconds=30
```

判定：

- 记录 QPS、CPU、延迟、`steal_task` 占比。
- 观察 `bthread_group_status` 是否存在明显队列不均。
- 观察 `bthread_worker_usage*` 是否部分 worker 明显更忙。

## 实验 2：任务粒度

目的：验证 `steal_task` 是否因为业务任务过短而被放大。

固定 server 基线参数，client 扫描：

```bash
for size in 0 1024 4096; do
  if [ "$size" = "0" ]; then
    ECHO=false
  else
    ECHO=true
  fi
  $CLIENT_BIN \
    --use_rdma=false \
    --protocol=baidu_std \
    --connection_type=single \
    --servers=$SERVER_IP:$PORT \
    --thread_num=16 \
    --queue_depth=16 \
    --load_mode=closed_loop \
    --connection_num=16 \
    --unique_connection_group=true \
    --attachment_size=$size \
    --echo_attachment=$ECHO \
    --test_seconds=30
done
```

判定：

- 如果 attachment 增大后 `steal_task` 占比下降，且业务/IOBuf/Socket 写占比上升，主因是短任务调度成本。
- 如果 `steal_task` 不降，继续看连接/event 分布和 worker 数。

## 实验 3：inflight 与发压模型

目的：验证 ready task 生成速度对 steal 的影响。

闭环扫描：

```bash
for qd in 1 4 16 64; do
  $CLIENT_BIN \
    --use_rdma=false \
    --protocol=baidu_std \
    --connection_type=single \
    --servers=$SERVER_IP:$PORT \
    --thread_num=16 \
    --queue_depth=$qd \
    --load_mode=closed_loop \
    --connection_num=16 \
    --unique_connection_group=true \
    --attachment_size=0 \
    --echo_attachment=false \
    --test_seconds=30
done
```

开环扫描：

```bash
for inflight in 512 2048 8192 32768; do
  $CLIENT_BIN \
    --use_rdma=false \
    --protocol=baidu_std \
    --connection_type=single \
    --servers=$SERVER_IP:$PORT \
    --thread_num=16 \
    --queue_depth=16 \
    --load_mode=open_loop \
    --connection_num=32 \
    --max_inflight=$inflight \
    --unique_connection_group=true \
    --attachment_size=0 \
    --echo_attachment=false \
    --test_seconds=30
done
```

判定：

- 如果 inflight 增大后 `steal_task`、延迟、队列长度同步上升，说明任务生成速度超过消费能力或分布不均。
- 如果 QPS 不升但 `steal_task` 升，说明更多 inflight 只增加调度压力。

## 实验 4：连接与 event 分布

目的：验证请求是否集中在少数 socket/event dispatcher。

扫描连接数：

```bash
for conn in 1 4 16 64; do
  $CLIENT_BIN \
    --use_rdma=false \
    --protocol=baidu_std \
    --connection_type=single \
    --servers=$SERVER_IP:$PORT \
    --thread_num=16 \
    --queue_depth=16 \
    --load_mode=closed_loop \
    --connection_num=$conn \
    --unique_connection_group=true \
    --attachment_size=0 \
    --echo_attachment=false \
    --test_seconds=30
done
```

对照连接复用：

```bash
$CLIENT_BIN \
  --use_rdma=false \
  --protocol=baidu_std \
  --connection_type=single \
  --servers=$SERVER_IP:$PORT \
  --thread_num=16 \
  --queue_depth=16 \
  --load_mode=closed_loop \
  --connection_num=16 \
  --unique_connection_group=false \
  --attachment_size=0 \
  --echo_attachment=false \
  --test_seconds=30
```

server 侧重启扫描 `event_dispatcher_num`：

```bash
for ed in 1 2 4; do
  $SERVER_BIN \
    --use_rdma=false \
    --port=$PORT \
    --bthread_concurrency=32 \
    --event_dispatcher_num=$ed \
    --show_per_worker_usage_in_vars=true
done
```

判定：

- 连接数或 dispatcher 增加后，如果 `bthread_group_status` 更均匀且 `steal_task` 降低，说明原先 socket/event 任务集中。
- 如果连接数增加导致写路径占比上升或系统连接压力上升，需要记录 `ss` 输出。

## 实验 5：worker 数

目的：验证 worker 数过多或过少对 steal 的影响。

server 重启扫描：

```bash
for bc in 8 16 32 64 96; do
  $SERVER_BIN \
    --use_rdma=false \
    --port=$PORT \
    --bthread_concurrency=$bc \
    --event_dispatcher_num=1 \
    --show_per_worker_usage_in_vars=true
done
```

每次使用基线 client 命令。

判定：

- worker 太少：CPU 满、队列堆积、延迟高。
- worker 适中：QPS 高、steal 占比可控。
- worker 太多：本地任务更稀疏，空扫和 `steal_task` 占比可能升高。

## 实验 6：runqueue 容量

目的：判断 `_rq` 容量是否影响突发短任务。

server 重启扫描：

```bash
for cap in 1024 4096 8192 16384; do
  $SERVER_BIN \
    --use_rdma=false \
    --port=$PORT \
    --bthread_concurrency=32 \
    --event_dispatcher_num=1 \
    --task_group_runqueue_capacity=$cap \
    --show_per_worker_usage_in_vars=true
done
```

判定：

- 如果只增大 capacity 不改变 `steal_task`，说明瓶颈不是队列容量，而是任务粒度或调度频率。
- 如果 `_rq is full` 消失但延迟上升，需要记录排队副作用。

## 实验 7：CPU 亲和性

目的：排除 OS 调度和 NUMA 干扰。

32 核绑定：

```bash
taskset -c 0-31 $SERVER_BIN \
  --use_rdma=false \
  --port=$PORT \
  --bthread_concurrency=32 \
  --event_dispatcher_num=1 \
  --show_per_worker_usage_in_vars=true
```

16 核绑定：

```bash
taskset -c 0-15 $SERVER_BIN \
  --use_rdma=false \
  --port=$PORT \
  --bthread_concurrency=16 \
  --event_dispatcher_num=1 \
  --show_per_worker_usage_in_vars=true
```

判定：

- 如果绑核后 worker usage 更均匀、`steal_task` 降低，说明 OS 线程迁移或 NUMA 影响明显。
- 如果无变化，优先回到任务粒度和入队分布分析。

## 数据记录模板

每组实验记录：

```text
case_name:
server_flags:
client_flags:
QPS:
Avg latency:
P90 latency:
P99 latency:
P999 latency:
Server CPU:
Client CPU:
steal_task %:
wait_task %:
ending_sched %:
ProcessInputMessage %:
ProcessRpcRequest %:
ProcessEvent %:
SendRpcResponse %:
Socket::Write %:
bthread_group_status:
bthread_worker_usage summary:
ss connection count:
errors:
conclusion:
```

## 结论判定规则

| 现象 | 结论倾向 | 后续方向 |
|---|---|---|
| 增大 attachment 或 echo 后 `steal_task` 明显下降 | 短任务导致调度成本放大 | 考虑合批、减少每 message 一个 bthread、增加任务粒度 |
| 增加 `connection_num` / `event_dispatcher_num` 后队列更均匀 | socket/event 分布集中 | 调整连接扇出和 dispatcher 数 |
| worker 数增加后 QPS 不升但 `steal_task` 升 | worker 过多导致空扫增多 | 降低 `bthread_concurrency` 或提高任务粒度 |
| `_rq is full` 出现 | 任务创建瞬时超过消费能力 | 降低 inflight、增大任务粒度、优化 QueueMessage |
| 所有参数下 `steal_task` 高但 QPS/延迟稳定 | 轻业务 benchmark 正常调度画像 | 不直接视为性能 bug |

## 后续可选代码观测

如果仅靠现有 bvar 和 perf 无法证明 `_rq/_remote_rq` 分布，下一轮再考虑新增轻量 bvar。建议指标：

- 每个 group 本地 `_rq.pop()` 成功/失败次数。
- 每个 group `_remote_rq.pop()` 成功/失败次数。
- `TaskControl::steal_task()` 成功/失败次数。
- steal 来源队列：priority、other `_rq`、other `_remote_rq`。
- `ProcessInputMessage` 创建数和直接 inline 处理数。

这些指标应只用于实验分支，避免长期引入框架层统计开销。
