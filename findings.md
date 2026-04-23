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
