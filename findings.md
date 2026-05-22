# Findings

## Initial source findings
- `example/rdma_performance/client.cpp` originally creates one `Channel` per `PerformanceTest`, but with `connection_type=single` and an empty `connection_group`, `Channel::InitSingle` can still reuse the same underlying `Socket` via `SocketMap`.
- The original benchmark model is closed-loop only: each response immediately triggers the next request, so steady-state inflight is bounded by `thread_num * queue_depth`.
- `src/brpc/socket.cpp` already exposes `rpc_waitepollout_count`, but does not expose wait duration or wakeup counts.
- `src/brpc/input_messenger.cpp` parses and dispatches messages in batches per read, but there is no built-in counter that tells how many messages each `ProcessNewMessage` call handled.

## Implementation choices
- Keep `test.proto` unchanged and expose extra stats through bvar/log output.
- Add explicit `connection_num` and `unique_connection_group` controls in the client to separate logical worker count from connection fanout.
- Add an `open_loop` sender that uses a global inflight ceiling to push load beyond the original closed-loop cap without unbounded growth.
- Reuse existing `rpc_channel_connection_count` as the least-invasive approximation of active client-side TCP connections during a run.
- Add two socket wait counters and five input-dispatch counters instead of introducing new socket APIs or changing RPC wire structures.

## Benchmark model facts captured in the report
- The current client model splits three dimensions that were previously easy to conflate: worker concurrency (`thread_num`), connection-slot fanout (`connection_num`), and logical request concurrency (`inflight` / `PeakInflight`).
- `closed_loop` fills `queue_depth` requests per worker at startup and replenishes on every completed response, so steady-state inflight is approximately bounded by `thread_num * queue_depth`.
- `open_loop` keeps attempting to send independently of response completions and relies on a global inflight ceiling (`max_inflight`, or the fallback `thread_num * queue_depth`) for backpressure.
- `InitConnectionSlots()` performs a synchronous warmup RPC on every slot before the timed run, which intentionally shifts part of connection establishment and protocol setup out of the measured interval.
- `unique_connection_group=true` is the key control for preventing SocketMap reuse in experiments that want channel fanout to map more closely to distinct underlying connections.
- The benchmark now emits both application-level results and framework-level observability: per-slot request stats, `rpc_channel_connection_count` delta, `rpc_waitepollout_*`, `rpc_process_new_message_*`, plus server-side request/attachment counters.

## Revert findings for `2af016d` observability
- `2af016d` added framework-level observability in `socket.h/socket.cpp/input_messenger.cpp` and corresponding client/server reporting, but these counters are not required for the benchmark model itself.
- After the revert, the benchmark still keeps the model changes that matter structurally: `open_loop`, `connection_num`, `unique_connection_group`, warmup RPC, per-slot stats, and explicit inflight accounting.
- The current benchmark still defaults to TCP connection reuse for the same server address because `connection_type=single` and `unique_connection_group=false` still lead to `Channel::InitSingle()` using the same `SocketMapKey(server_addr, zero_signature)` path.
- Only when `unique_connection_group=true` does the `connection_group` participate in `ChannelSignature`, which makes different `Channel` instances map to different `SocketMapKey` values and therefore tend toward separate underlying TCP connections.

## Channel/server business bthread flow findings
- `src/brpc/channel.cpp` is the client-side RPC entry and connection-selection layer. `Channel::CallMethod()` runs in the caller context, prepares the `Controller`, serializes the request, configures timers, and then calls `Controller::IssueRPC()`.
- `Controller::IssueRPC()` selects the target socket using either the single-server socket id or load balancer, applies `connection_type`, packs the request, and writes through `Socket::Write()`.
- Client-side async completion can create a `RunEndRPC` bthread in `Controller::OnVersionedRPCReturned()`, and that path runs `Controller::EndRPC()` and the user's `done->Run()` callback.
- `src/brpc/server.cpp` registers services and starts acceptors, but the per-request business bthread is not created directly in `server.cpp`.
- Server accepted sockets use `InputMessenger::OnNewMessages` as the TCP read-event callback. `Socket::StartInputEvent()` creates `ProcessEvent` bthreads for socket events.
- `InputMessenger::QueueMessage()` creates `ProcessInputMessage` bthreads for queued messages, while the last message in a batch can run in the current `ProcessEvent` bthread.
- `policy::ProcessRpcRequest()` deserializes the request, looks up service/method metadata registered by `Server::AddServiceInternal()`, creates the response callback, and by default calls `svc->CallMethod()` in the current request-processing bthread.
- `Socket::Write()` is shared by client request sends and server response sends. It may create `KeepWrite` bthreads only for background or incomplete writes, not for executing business logic.

## Expanded bthread creation flow findings
- Client response handling also uses the shared `socket.cpp` / `input_messenger.cpp` receive path before entering `ProcessRpcResponse()` and `Controller::OnVersionedRPCReturned()`.
- `Controller::RunOnCancel()` creates an urgent `RunOnCancelThread` bthread only when socket failure triggers cancellation; controller reset/destruction can run the cancel callback in place.
- `Controller::HandleSocketFailed()` can create a background helper bthread to run `OnVersionedRPCReturned()` when retry policy may block the current response/error handling context.
- `Socket::StartInputEvent()` creates at most one active `ProcessEvent` bthread per socket event-processing window by gating on `_nevent.fetch_add(...) == 0`; additional events are consumed by the active handler through `MoreReadEvents()`.
- `InputMessenger::QueueMessage()` creates `ProcessInputMessage` with `BTHREAD_NOSIGNAL` and later calls `bthread_flush()` so batched message scheduling does not wake workers one by one.
- `ProcessInputMessage` is a shared abstraction: on client it leads to response protocol processing and controller completion; on server it leads to request protocol processing and service method execution.
- `Socket::KeepWrite` is a shared continuation bthread for incomplete/background writes on both client request sends and server response sends; it is not a business execution bthread.

## rdma_performance bthread mapping findings
- `example/rdma_performance/client.cpp` explicitly creates benchmark bthreads in `Test()`: one optional `GenerateToken` bthread when `expected_qps > 0`, and `thread_num` worker bthreads running either `RunClosedLoopWorker` or `RunOpenLoopWorker`.
- `RunClosedLoopWorker()` only sends the initial `queue_depth` asynchronous RPCs and then returns; steady-state closed-loop replenishment is performed in `HandleResponse()` when the brpc async callback runs.
- `RunOpenLoopWorker()` keeps looping and sending while below the global inflight limit; in open-loop mode `HandleResponse()` records stats and releases inflight but does not schedule the next send.
- `SendRequest()` creates `Controller`, response, and `RespClosure` objects plus a protobuf callback, then calls `stub.Test()`. It does not create a benchmark bthread; framework send/completion bthreads are created later by brpc paths.
- `PickConnectionSlot()` round-robins over `ConnectionSlot` objects to choose a `Channel`; this chooses the logical channel/connection slot, not the bthread that executes the request.
- `example/rdma_performance/server.cpp` does not create per-request bthreads. It registers `PerfTestServiceImpl`, starts `brpc::Server`, and lets brpc `socket/input_messenger/protocol` paths invoke `PerfTestServiceImpl::Test()`.
- `PerfTestServiceImpl::Test()` runs in the brpc request-processing bthread, uses `ClosureGuard` to trigger `done->Run()` on return, and sends the response through `SendRpcResponse` / `Socket::Write`.

## Callback context and TaskGroup enqueue findings
- Normal successful client responses call `ControllerPrivateAccessor::OnResponse()`, which passes `new_bthread=false` into `Controller::OnVersionedRPCReturned()`. Therefore `rdma_performance` client `HandleResponse()` normally runs inline in the response-processing bthread, not in the original sending worker bthread and not in a newly created `RunEndRPC` bthread.
- `RunEndRPC` is still possible on special paths, such as asynchronous send failure before request dispatch, cancellation, or helper paths where `OnVersionedRPCReturned()` is invoked with `new_bthread=true`.
- If `bthread_start_background()` is called from an existing worker bthread with compatible tag, brpc uses the current `TaskGroup` and pushes the new task into the local `_rq`.
- If bthread creation happens from a non-worker thread or incompatible tag, brpc uses `TaskControl::choose_one_group(tag)` and pushes into that target group's `_remote_rq`.
- A bthread inserted into the current group's local `_rq` is not guaranteed to execute on that same group, because other groups can steal ready tasks from local run queues.
- Added PlantUML diagrams to `docs/cn/brpc_channel_server_bthread_flow.md` to show bthread creation and non-creation points in the benchmark client and server paths.

## steal_task experiment planning findings
- `steal_task` is entered after the current TaskGroup fails to pop a local ready task from `_rq`.
- The steal order is current group `_remote_rq`, tag priority queue, other groups' `_rq`, and other groups' `_remote_rq`.
- Server high-QPS request path mainly creates `ProcessInputMessage` from worker context, so most request tasks initially enter local `_rq`, not randomly selected `_remote_rq`.
- Existing bvars can provide coarse observation through `bthread_group_status`, `bthread_worker_usage*`, `bthread_count*`, and worker counts; perf is still required to attribute CPU to `steal_task` and request-processing frames.
- The remote experiment plan should first vary task granularity, inflight, connection fanout, event dispatcher count, worker count, runqueue capacity, and CPU affinity before adding new framework counters.

## C2C after worker affinity findings
- Worker pthread pinning only fixes OS-level migration of `brpc_wkr` pthreads. It does not bind bthread logical tasks to the worker that created them; `TaskControl::steal_task()` can still move ready tasks across TaskGroups.
- `WorkStealingQueue` exposes shared `_top` and `_bottom` cache lines to owner pop/push and remote steal operations. High steal frequency can therefore create C2C even when pthreads are pinned.
- `RemoteTaskQueue` is protected by a mutex and is read by the owner group plus other scheduling paths; remote enqueue/dequeue can create cross-core cache traffic if non-worker or incompatible-tag creation is frequent.
- Server request handling uses `Socket::StartInputEvent()` to create `ProcessEvent` and `InputMessenger::QueueMessage()` to create `ProcessInputMessage`. The message/socket data may be read on one worker and processed or stolen by another.
- `Socket` state, `_nevent`, `_write_head`, write requests, IOBuf block references, bvar counters, and resource-pool metadata are plausible shared cache-line sources on the server fast path.
- Event dispatcher threads are bthreads and may run on worker pthreads. `event_dispatcher_num` and connection distribution can change which workers create `ProcessEvent` and where socket read work starts.

## Minimal large attachment timeout findings
- brpc `ChannelOptions` defaults `connect_timeout_ms` to 200ms, while the benchmark previously only set `timeout_ms=rpc_timeout_ms`. In `pooled` mode with large echo payloads, new pooled sockets may hit connection timeout under accept/backlog/socket pressure before the RPC-level 2000ms timeout is reached.
- The benchmark should not add default per-connection-slot throttling when the purpose is to stress pooled mode itself; that would hide part of the framework behavior under test.
- The open-loop `max_inflight` check was previously approximate because workers checked the value before `SendRequest()` and incremented afterward. A CAS permit keeps `PeakInflight` within the user-configured global limit without reducing pressure below the requested limit.
- Large request+echo payloads can still exceed `rpc_timeout_ms` under real load. The benchmark now warns about 1MB+ echo payloads with the default 2000ms timeout instead of silently relying on a small timeout.
- For benchmark observability, runtime RPC failures should not abort the whole run. Counting failures and continuing provides QPS/latency/failure-rate evidence for overload regions; warmup failures remain fatal because the channel/route is not usable before measurement starts.

## rdma_performance README findings
- Current server-facing custom flags are `port`, `use_rdma`, `server_num_threads`, `server_bind_bthread_workers`, and `server_worker_affinity_cpus`; worker count can also be affected by global bthread flags such as `bthread_concurrency`.
- Current client-facing custom flags cover worker count, closed/open-loop load model, explicit connection slots, timeout controls, payload format, raw JSON input, and per-slot stats.
- The README should explicitly state that `connection_num` is a logical `Channel/ConnectionSlot` count, not a guaranteed true TCP connection count, especially under `pooled`.

## Timed shutdown findings
- With very large payloads and high `max_inflight`, reaching `test_seconds` only stops new sends; the old loop waited for `g_inflight==0`, so a large `rpc_timeout_ms` could keep the process alive for a long time while outstanding RPCs failed.
- brpc exposes `StartCancel(CallId)` for asynchronous RPC cancellation. Tracking call ids lets the benchmark cancel outstanding requests when the timed run stops.
- Error logging after timed stop should be suppressed because final `Failed/Timeout` counters are the useful signal; otherwise canceled or overcrowded callbacks can flood stderr after the measurement window.
- Per-RPC failure logging is too noisy for overload experiments such as `EOVERCROWDED`; a flag-controlled log keeps final counters while avoiding stderr flooding.
- The old QPS output used integer K-QPS arithmetic, so any successful throughput below 1000 QPS was displayed as `0k`. The formatter should calculate real QPS as floating point and only append `k` when QPS is at least 1000.

## Client hot-path overhead minimization findings
- Comparing against `da77da061393c8c0afd22ae01d41482a824f5a2e`, the largest new client hot-path cost was per-RPC `Controller::call_id()` plus global `mutex/unordered_set` insert/delete for cancellation tracking.
- Strict CAS inflight permits also add contention in open-loop high-concurrency runs. Restoring the old `RunOpenLoopWorker` pre-check plus `SendRequest()` `fetch_add` keeps the configured inflight as an approximate cap with lower overhead.
- `connect_timeout_ms`, QPS formatting, and per-RPC error-log suppression are not request hot-path costs or are negligible compared with RPC send/response handling, so they can remain.

## Minimal response mode findings
- In `baidu_std`, the framework still needs response meta and correlation id completion; the benchmark cannot remove response completion entirely without changing brpc protocol behavior.
- The benchmark can avoid its own response payload cost by passing `NULL` as the client response message. `ProcessRpcResponse()` then skips protobuf response deserialization while still completing the controller and callback.
- Server `minimal_response=true` avoids the periodic `process_cpu_usage` bvar lookup and only sets `PerfTestResponse.cpu_usage` to an empty initialized value to satisfy proto2 required-field semantics.
- `record_latency=false` avoids writing the shared latency recorder on every completed RPC, which is useful for open-loop QPS ceiling tests but removes latency percentile observability.

## Latency window findings
- `bvar::LatencyRecorder::latency(10)` returns the average latency in the recent 10 seconds, not the whole benchmark run.
- `LatencyRecorder::latency_percentile()` uses the recorder's constructor window, so a global recorder constructed before flag parsing cannot adapt its percentile window to per-run `test_seconds`.
- Creating the client latency recorder inside each `Test()` with `window_size=test_seconds` aligns average and percentile outputs with the configured benchmark duration while keeping `record_latency=false` as the no-recorder low-overhead path.
