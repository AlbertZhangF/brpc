# rdma_performance 使用说明

本文档说明 `example/rdma_performance` 当前 client/server 的参数、可配置内容和推荐用法。当前示例既支持原始 protobuf attachment 压测，也支持 HTTP/H2 raw JSON body 压测；`use_rdma=false` 时走 TCP 路径。

## 1. 程序组成

- `rdma_performance_server`：启动 brpc server，注册 `PerfTestService`，处理 protobuf RPC 或 HTTP/H2 raw JSON 请求。
- `rdma_performance_client`：按指定并发模型、连接模型和 payload 模式发起请求，并输出 QPS、吞吐、延迟、失败数、超时数、CPU 和连接槽位统计。
- `test.proto`：仍用于 brpc 服务注册和 protobuf 模式。raw JSON 模式下网络 payload 不使用 protobuf，只把 `.proto` 作为 HTTP 路由入口。

## 2. Server 参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--port` | `8002` | server 监听端口。 |
| `--use_rdma` | `true` | 是否使用 RDMA。TCP 压测时设置为 `false`。 |
| `--server_num_threads` | `-1` | 控制 `brpc::ServerOptions.num_threads`。`-1` 保持 brpc 默认；`0` 表示不由 server 主动设置 worker 数，主要由 `--bthread_concurrency` 控制；`>0` 显式设置 server worker 数。 |
| `--server_bind_bthread_workers` | `false` | 是否在 brpc worker pthread 启动时把每个 worker 绑定到单独 CPU。 |
| `--server_worker_affinity_cpus` | 空 | worker 绑核 CPU 列表，例如 `112-127` 或 `112-119,124,126`。为空时继承当前进程 affinity mask，适合配合 `taskset -c 112-127` 使用。 |

常用 brpc/bthread 全局参数：

| 参数 | 说明 |
|---|---|
| `--bthread_concurrency=<N>` | bthread worker pthread 数。若 server 未设置 `server_num_threads`，可能被 `ServerOptions.num_threads` 默认值影响。 |
| `--event_dispatcher_num=<N>` | 事件分发器数量，影响 socket readiness 事件入口分布。 |
| `--show_per_worker_usage_in_vars=true` | 暴露 per-worker usage 相关 bvar，便于查看 worker 使用情况。 |

Server 行为：

- protobuf 请求：读取 `PerfTestRequest.echo_attachment()`，为 true 时把 request attachment 原样写入 response attachment。
- HTTP/H2 raw JSON 请求：读取 HTTP body，也就是 `request_attachment()`；当 URL query 中 `echo_attachment=true` 时原样回写 body，否则返回 `{"cpu_usage":"..."}`。
- HTTP/H2 响应会设置 `Content-Type: application/json`，并通过 `X-Server-Cpu-Usage` header 返回 server CPU 采样。
- `ServiceOptions.allow_http_body_to_pb=false`，因此 raw JSON body 不会被 brpc 自动转换成 protobuf message。

### Server 启动示例

TCP 基础启动：

```bash
./example/rdma_performance/rdma_performance_server \
  --use_rdma=false \
  --port=8003
```

固定 16 个 server worker：

```bash
./example/rdma_performance/rdma_performance_server \
  --use_rdma=false \
  --port=8003 \
  --server_num_threads=16 \
  --bthread_concurrency=16
```

配合 `taskset` 做一 worker 一核绑定：

```bash
taskset -c 112-127 ./example/rdma_performance/rdma_performance_server \
  --use_rdma=false \
  --port=8003 \
  --server_num_threads=16 \
  --bthread_concurrency=16 \
  --server_bind_bthread_workers=true \
  --event_dispatcher_num=1 \
  --show_per_worker_usage_in_vars=true
```

## 3. Client 参数

### 3.1 基础压测参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--thread_num` | `0` | client worker bthread 数。`>0` 时只跑该线程数；`<=0` 时按 `1,2,4...max_thread_num` 扫描。 |
| `--max_thread_num` | `16` | `thread_num<=0` 时扫描的最大 worker 数。 |
| `--queue_depth` | `1` | `closed_loop` 下每个 worker 初始发出的 pending 请求数。 |
| `--expected_qps` | `0` | 目标 QPS。`0` 表示不做 token 限速；`>0` 时用 token 控制发压速率。 |
| `--test_seconds` | `20` | 基于时间的压测时长。 |
| `--test_iterations` | `0` | 总请求预算。`0` 表示按时间运行；`>0` 表示完成固定请求数后结束。 |
| `--dummy_port` | `8001` | client 侧启动 dummy brpc server 的端口。 |

### 3.2 网络和协议参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--servers` | `0.0.0.0:8002+0.0.0.0:8002` | 目标 server 列表，多个地址用 `+` 分隔。 |
| `--use_rdma` | `true` | 是否使用 RDMA。TCP 压测设置为 `false`。 |
| `--protocol` | `baidu_std` | brpc 协议。常用 `baidu_std`、`http`、`h2`。raw JSON 模式要求 `http` 或 `h2`。 |
| `--connection_type` | `single` | brpc 连接类型，可用 `single`、`pooled`、`short`。 |
| `--rpc_timeout_ms` | `2000` | 单次 RPC 总超时。大 attachment + echo 时建议提高。 |
| `--connect_timeout_ms` | `-1` | TCP 连接建立超时。`-1` 表示使用 `rpc_timeout_ms`。 |
| `--stop_grace_ms` | `5000` | 到达 `test_seconds` 后等待 in-flight RPC 结束的最长时间，单位 ms。`-1` 表示一直等待。 |
| `--cancel_inflight_on_stop` | `true` | 到达 `test_seconds` 后是否对未完成异步 RPC 调用 `brpc::StartCancel()`。 |

### 3.3 Payload 参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--attachment_size` | `-1` | 请求 payload 大小，单位 Byte。`>=0` 跑指定大小；`<0` 时按 `1,4,16...1024` 扫描。 |
| `--echo_attachment` | `false` | server 是否回显请求 payload。为 true 时响应体也会携带同等大小数据。 |
| `--payload_format` | `attachment` | payload 模式：`attachment` 或 `raw_json`。 |
| `--json_file` | 空 | raw JSON 模式下读取本地 `.json` 文件作为 HTTP body。为空时按 `attachment_size` 自动生成 `{"payload":"..."}`。 |
| `--json_echo_check` | `false` | raw JSON + echo 模式下校验 response body 是否与 request body 完全一致。大 payload 吞吐测试建议关闭，因为会复制完整响应 body。 |

Payload 模式说明：

- `payload_format=attachment`：使用 protobuf stub 调用 `PerfTestService::Test`，payload 放在 brpc attachment 中。
- `payload_format=raw_json`：不使用 protobuf stub；client 使用 `Channel::CallMethod(NULL, ...)` 发 HTTP/H2 POST，请求 body 直接放 JSON 字符串。
- raw JSON 只支持 `--protocol=http` 或 `--protocol=h2`，不支持 `baidu_std`。

### 3.4 负载模型参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--load_mode` | `closed_loop` | 压测模型：`closed_loop` 或 `open_loop`。 |
| `--max_inflight` | `0` | `open_loop` 全局 inflight 上限。`0` 表示使用 `thread_num * queue_depth`。 |

`closed_loop` 模型：

- 每个 worker 启动时发送 `queue_depth` 个异步请求。
- 每收到一个响应，在回调中补发一个请求。
- 稳态逻辑并发约为 `thread_num * queue_depth`。
- RPC 失败后不会中断整个压测，会计入 `Failed/Timeout` 并继续补发。

`open_loop` 模型：

- worker 持续尝试发送请求，不等待“完成一个再补一个”。
- 通过全局 `max_inflight` 控制最大未完成请求数。
- 当前实现使用 CAS permit，`PeakInflight` 不应超过配置的 `max_inflight`。
- 若 `expected_qps=0`，会尽力发压直到达到 `max_inflight`。

### 3.5 连接模型参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--connection_num` | `0` | 预创建的 `Channel/ConnectionSlot` 数量。`0` 表示使用 `thread_num`。 |
| `--unique_connection_group` | `false` | 是否为每个 Channel 设置唯一 `connection_group`，用于避免同地址 `single` 连接被 SocketMap 复用。 |
| `--report_connection_stats` | `true` | 结束时打印每个 connection slot 的 sent/completed/failed/timeouts。 |

连接语义说明：

- `connection_num` 是逻辑 `Channel/ConnectionSlot` 数，不一定等于真实 TCP 连接数。
- `connection_type=single` 且 `unique_connection_group=false` 时，同一 server 地址通常会复用底层 SocketMap 中的同一个 TCP 连接。
- `connection_type=single` 且 `unique_connection_group=true` 时，每个 Channel 使用不同 `connection_group`，更接近“一个 slot 一个独立 TCP 连接”。
- `connection_type=pooled` 时，一个 in-flight RPC 通常会占用一个 pooled socket；没有空闲 socket 时框架可能创建新的 TCP 连接。因此真实 TCP 连接数可能远大于 `connection_num`。
- 如果 `servers` 中包含不同地址，不同地址天然不会复用同一个 SocketMap key。

## 4. 输出指标

client 结束时输出：

| 指标 | 含义 |
|---|---|
| `Avg-Latency` | client 观察到的平均延迟。 |
| `90th/99th/99.9th-Latency` | 延迟分位数。 |
| `Throughput` | 按请求 payload 字节估算的吞吐，单位 MB/s。 |
| `QPS` | 成功完成请求数除以运行时间。 |
| `Completed` | `test_iterations>0` 时输出，表示成功完成请求数。 |
| `Failed` | RPC 失败次数，包括连接失败、超时、echo check 失败等。 |
| `Timeout` | 超时次数，包括 brpc RPC timeout 和系统 `ETIMEDOUT`。 |
| `PeakInflight` | 运行期间观测到的最大未完成请求数。 |
| `Server CPU-utilization` | server 通过响应字段/header 返回的 CPU 采样。 |
| `Client CPU-utilization` | client 进程 CPU 采样。 |

`Connection stats` 中每个 slot 输出：

| 字段 | 含义 |
|---|---|
| `conn` | connection slot 编号。 |
| `server` | 该 slot 对应的 server 地址。 |
| `group` | connection group。`<shared>` 表示没有设置唯一 group。 |
| `sent` | 该 slot 发出的请求数。 |
| `completed` | 该 slot 成功完成的请求数。 |
| `failed` | 该 slot 失败的请求数。 |
| `timeouts` | 该 slot 超时的请求数。 |

## 5. 常用命令

### 5.1 protobuf attachment + baidu_std + single

```bash
./example/rdma_performance/rdma_performance_client \
  --use_rdma=false \
  --protocol=baidu_std \
  --connection_type=single \
  --servers=127.0.0.1:8003 \
  --thread_num=16 \
  --queue_depth=16 \
  --load_mode=closed_loop \
  --connection_num=16 \
  --unique_connection_group=true \
  --attachment_size=1024 \
  --echo_attachment=true \
  --test_seconds=20
```

适合验证 protobuf/baidu_std 主路径和 single 连接模型。

### 5.2 raw JSON + HTTP + pooled

```bash
./example/rdma_performance/rdma_performance_client \
  --use_rdma=false \
  --protocol=http \
  --payload_format=raw_json \
  --connection_type=pooled \
  --servers=127.0.0.1:8003 \
  --thread_num=16 \
  --queue_depth=16 \
  --load_mode=closed_loop \
  --connection_num=1 \
  --attachment_size=1024 \
  --echo_attachment=true \
  --json_echo_check=false \
  --test_seconds=20
```

适合验证 HTTP raw JSON body，不经过 protobuf JSON 自动转换。

### 5.3 open_loop 大 payload 压测

```bash
./example/rdma_performance/rdma_performance_client \
  --use_rdma=false \
  --protocol=http \
  --payload_format=raw_json \
  --connection_type=pooled \
  --servers=127.0.0.1:8003 \
  --thread_num=32 \
  --queue_depth=32 \
  --load_mode=open_loop \
  --connection_num=16 \
  --max_inflight=4096 \
  --attachment_size=1024000 \
  --echo_attachment=true \
  --json_echo_check=false \
  --rpc_timeout_ms=10000 \
  --connect_timeout_ms=-1 \
  --test_seconds=10
```

适合验证 pooled 模式在大 payload 下的框架能力。`connect_timeout_ms=-1` 表示连接建立超时跟随 `rpc_timeout_ms=10000`。

### 5.4 固定请求数

```bash
./example/rdma_performance/rdma_performance_client \
  --use_rdma=false \
  --protocol=baidu_std \
  --connection_type=single \
  --servers=127.0.0.1:8003 \
  --thread_num=16 \
  --queue_depth=16 \
  --load_mode=open_loop \
  --max_inflight=1024 \
  --attachment_size=0 \
  --echo_attachment=false \
  --test_iterations=1000000
```

适合做固定请求数对照，避免不同运行时长导致统计口径变化。

## 6. 系统侧观测命令

查看 TCP 连接数：

```bash
ss -tan | grep ':8003' | wc -l
ss -tinp | grep rdma_performance | head -50
```

查看线程 CPU：

```bash
SERVER_PID=$(pgrep -f rdma_performance_server | head -n1)
CLIENT_PID=$(pgrep -f rdma_performance_client | head -n1)

pidstat -t -p $SERVER_PID 1
pidstat -t -p $CLIENT_PID 1
top -H -p $SERVER_PID
top -H -p $CLIENT_PID
```

查看 brpc bvar：

```bash
curl -s "http://127.0.0.1:8003/vars/bthread_worker_count*"
curl -s "http://127.0.0.1:8003/vars/bthread_worker_usage*"
curl -s "http://127.0.0.1:8003/vars/bthread_group_status"
```

查看 worker 是否绑核：

```bash
SERVER_PID=$(pgrep -f rdma_performance_server | head -n1)
ps -L -p $SERVER_PID -o pid,tid,psr,comm | grep brpc_wkr
for tid in $(ls /proc/$SERVER_PID/task); do taskset -pc $tid 2>/dev/null; done
```

## 7. 注意事项

- `connection_num` 不是严格真实 TCP 连接数。真实连接数取决于 `connection_type`、`unique_connection_group`、server 地址数量和 brpc pooled 连接池行为。
- `pooled` 模式下大 payload + 高 `max_inflight` 会显著增加 active socket、fd、端口、accept backlog 和网络队列压力，这是框架能力测试的一部分。
- `rpc_timeout_ms=2000` 对 1MB 以上 echo payload 可能偏小，建议从 `10000` 或 `30000` 开始对照。
- `test_seconds` 到达后 client 会停止新发请求，并默认取消未完成 RPC；若 `stop_grace_ms` 到期仍有 in-flight，请求会打印当前汇总并退出，避免等待很大的 `rpc_timeout_ms`。
- raw JSON 模式不经过 protobuf 序列化，但服务注册仍依赖 `test.proto`。
- RPC 失败不会中断正式压测，会计入 `Failed/Timeout` 并继续运行；warmup RPC 失败仍会直接退出。
- `json_echo_check=true` 会把完整 response body 转成字符串比较，大 payload 吞吐测试建议关闭。
