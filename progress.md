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
