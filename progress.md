# Progress Log

## 2026-04-23
- Loaded `using-superpowers`, `planning-with-files`, `executing-plans`, and `using-git-worktrees`.
- Inspected `example/rdma_performance/client.cpp`, `example/rdma_performance/server.cpp`, `src/brpc/socket.cpp`, `src/brpc/input_messenger.cpp`, and channel/socket-map code paths.
- Confirmed repository state is clean on branch `benchmark_cx`.
- Began implementing planning artifacts and preparing client/framework edits.
- Added planning files in the repository root.
- Reworked `example/rdma_performance/client.cpp` to support:
  - `load_mode=closed_loop|open_loop`
  - explicit `connection_num`
  - unique `connection_group` fanout
  - connection-level request counters
  - bvar delta reporting for TCP wait and input batching
- Added lightweight TCP wait observability in `src/brpc/socket.{h,cpp}`:
  - `rpc_waitepollout_time_us`
  - `rpc_waitepollout_wakeup_count`
- Added lightweight batching observability in `src/brpc/input_messenger.cpp`:
  - `rpc_process_new_message_count`
  - `rpc_process_new_message_bytes`
  - `rpc_process_new_message_parsed_messages`
  - `rpc_process_new_message_batched_calls`
  - `rpc_process_new_message_direct_process_count`
  - `rpc_process_new_message_queued_bthread_count`
- Added lightweight server-side counters in `example/rdma_performance/server.cpp`:
  - `rdma_perf_server_request_count`
  - `rdma_perf_server_response_attachment_bytes`
- Skipped compile/test execution intentionally per user request; final response will include manual run commands only.
- Re-read the planning files and the current `example/rdma_performance` client/server implementation before writing documentation, to ensure the report matched the repository state.
- Added `docs/cn/rdma_performance_benchmark_model.md`, a detailed Chinese report covering:
  - benchmark goals and scope
  - startup-to-shutdown execution flow
  - closed-loop vs open-loop semantics
  - worker/connection/inflight parameter boundaries
  - observability output and interpretation limits
  - differences from the original sample model
  - recommended experiment patterns and example commands
- Reverted the extra observability added in `2af016d` from:
  - `src/brpc/socket.h`
  - `src/brpc/socket.cpp`
  - `src/brpc/input_messenger.cpp`
  - `example/rdma_performance/client.cpp`
  - `example/rdma_performance/server.cpp`
- Kept the benchmark-model changes intact:
  - `load_mode=closed_loop|open_loop`
  - explicit `connection_num`
  - `unique_connection_group`
  - warmup RPC in `InitConnectionSlots()`
  - per-connection stats and inflight accounting
- Static post-change review confirmed:
  - removed bvar names no longer appear in the touched code
  - default connection model is still reuse for same-address `single` channels unless `unique_connection_group=true`

## 2026-04-29
- Re-read planning files before starting the channel/server bthread documentation task.
- Inspected the current client-side flow in:
  - `src/brpc/channel.cpp`
  - `src/brpc/controller.cpp`
  - `src/brpc/socket.cpp`
- Inspected the current server-side flow in:
  - `src/brpc/server.cpp`
  - `src/brpc/acceptor.cpp`
  - `src/brpc/input_messenger.cpp`
  - `src/brpc/policy/baidu_rpc_protocol.cpp`
- Added `docs/cn/brpc_channel_server_bthread_flow.md`, covering:
  - client `Channel` initialization and RPC send path
  - client async completion callback bthread path
  - server service registration and acceptor startup path
  - socket event, input messenger, and RPC dispatch bthread creation paths
  - client/server differences from the business bthread perspective
- Updated `task_plan.md` and `findings.md` with the new documentation phase and source-level conclusions.
- Did not run compilation because this change only adds documentation and planning records.

## 2026-04-30
- Expanded `docs/cn/brpc_channel_server_bthread_flow.md` to include:
  - `controller.cpp` client completion, cancel, and retry helper bthread creation paths
  - `socket.cpp` `ProcessEvent` creation conditions and `KeepWrite` write-continuation behavior
  - `input_messenger.cpp` `QueueMessage` / `ProcessInputMessage` batching and `BTHREAD_NOSIGNAL` behavior
  - full client and server bthread creation sequences from event dispatch to business callback/method execution
- Updated `task_plan.md` and `findings.md` with the expanded source-level conclusions.
- Static validation target remains source/document consistency; no compile is needed because only documentation and planning records changed.

## 2026-04-30 rdma_performance mapping
- Inspected `example/rdma_performance/client.cpp` and `server.cpp` to map benchmark-created bthreads onto the brpc framework bthread flow.
- Updated `docs/cn/brpc_channel_server_bthread_flow.md` with:
  - client `GenerateToken`, `RunClosedLoopWorker`, and `RunOpenLoopWorker` creation and responsibilities
  - closed-loop callback-driven replenishment via `HandleResponse`
  - open-loop worker-driven continuous sending
  - `SendRequest` object/callback creation and `ConnectionSlot` round-robin selection
  - server `PerfTestServiceImpl::Test` execution context and `ClosureGuard` response path
- Updated `task_plan.md` and `findings.md` with the benchmark-specific bthread mapping.
- No compile run; this remains a documentation-only change.

## 2026-04-30 callback context and PlantUML
- Inspected bthread creation and enqueue logic in:
  - `src/bthread/bthread.cpp`
  - `src/bthread/task_group.cpp`
  - `src/bthread/task_control.cpp`
  - `src/brpc/details/controller_private_accessor.h`
  - `src/brpc/policy/baidu_rpc_protocol.cpp`
- Confirmed normal successful response uses `OnVersionedRPCReturned(info, false, ...)`, so `rdma_performance` client `HandleResponse()` normally executes inline in the response-processing bthread rather than the original sending worker bthread.
- Updated `docs/cn/brpc_channel_server_bthread_flow.md` to correct callback wording and add TaskGroup enqueue semantics.
- Added two PlantUML diagrams covering complete benchmark client and server bthread creation flows with per-step creation/non-creation annotations.
- No compile run; this remains a documentation-only change.

## 2026-05-07 steal_task experiment plan
- Inspected existing bthread and brpc observability points for `steal_task` remote diagnosis.
- Added `docs/cn/steal_task_remote_experiment_plan.md` with:
  - source-level mechanism summary
  - remote command matrix
  - perf, bvar, pidstat, ss collection commands
  - experiments for task granularity, inflight, connection distribution, event dispatchers, worker count, runqueue capacity, and CPU affinity
  - data recording template and conclusion rules
- Updated `task_plan.md` and `findings.md` with the new documentation phase and experiment assumptions.
- No compile run; this is a documentation-only change.

## 2026-05-15 C2C after worker affinity
- Investigated why strict brpc worker pthread affinity did not reduce C2C or improve throughput.
- Re-read bthread scheduling, runqueue, remote queue, socket event, input messenger, and event dispatcher code paths.
- Key conclusion: high C2C is likely not caused by OS worker migration. More likely sources are bthread task stealing, shared runqueue metadata, socket/event state shared across workers, IOBuf/resource-pool cache lines, and global bvar/stat counters.
- Updated `task_plan.md` and `findings.md` with the new C2C diagnosis phase and source-level hypotheses.

## 2026-05-18 minimal pooled open_loop timeout fix
- Updated `example/rdma_performance/client.cpp` to add `--connect_timeout_ms`; default `-1` makes connection establishment timeout follow `--rpc_timeout_ms`.
- Changed open-loop inflight accounting to acquire a CAS permit before creating RPC objects, so the benchmark honors the configured global `--max_inflight` without adding default per-connection-slot throttling.
- Counted both `brpc::ERPCTIMEDOUT` and system `ETIMEDOUT` as timeout failures.
- Added a warning for 1MB+ echo payloads with `--rpc_timeout_ms<=2000`.
- `git diff --check` passed; local build is still unavailable because `build/` lacks generated `Makefile`/`build.ninja`.

## 2026-05-18 continue after RPC failures
- Updated `HandleResponse()` so RPC failures and raw JSON echo-check failures increment counters and continue the benchmark instead of setting `g_stop=true`.
- Added shared response-completion helpers to keep closed-loop replenishment behavior consistent across success and failure paths.

## 2026-05-18 rdma_performance README
- Added `example/rdma_performance/readme.md`.
- Documented server flags, client flags, payload modes, load modes, connection semantics, output fields, example commands, and system-side observation commands.

## 2026-05-18 timed shutdown for large inflight runs
- Added tracking of outstanding async RPC `CallId` values in `rdma_performance_client`.
- Added `--cancel_inflight_on_stop=true` and `--stop_grace_ms=5000`.
- On timed stop, the client now sets `g_stop`, cancels outstanding RPCs, suppresses post-stop failure log spam, and exits after the grace period with a summary if in-flight requests still remain.

## 2026-05-18 suppress per-RPC error logs
- Added `--log_rpc_error=false` to control whether each RPC failure is printed.
- Default behavior now suppresses per-request `RPC call failed` logs and keeps final `Failed/Timeout` counters.

## 2026-05-18 low-QPS formatting
- Added QPS formatting that computes real QPS with floating point precision.
- QPS values below 1000 now print as raw QPS instead of `0k`; values at or above 1000 still print in `k`.

## 2026-05-20 client hot-path overhead minimization
- Compared current client against `da77da061393c8c0afd22ae01d41482a824f5a2e`.
- Removed default per-RPC call-id tracking, global mutex, and unordered set maintenance from `rdma_performance_client`.
- Restored open-loop inflight accounting to the lower-overhead `fetch_add` style used by the earlier benchmark model.
- Kept lightweight fixes: connection timeout option, failure counting without per-RPC logging, low-QPS formatting, and stop grace timeout.
