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
