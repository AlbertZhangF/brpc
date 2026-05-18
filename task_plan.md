# TCP Path Performance Investigation Plan

## Goal
Implement the approved `use_rdma=false` benchmarking and observability changes for `example/rdma_performance`, document the resulting benchmark model in detail, and leave a reproducible trail of the decisions and manual verification commands.

## Phases
| Phase | Status | Notes |
|---|---|---|
| 1. Restore context and inspect target files | completed | Confirmed clean branch `benchmark_cx`, no existing planning files, inspected client/server/socket/input_messenger code paths. |
| 2. Add planning artifacts | completed | Created and started maintaining `task_plan.md`, `findings.md`, and `progress.md`. |
| 3. Rework rdma_performance client model | completed | Added load modes, explicit connection fanout, unique connection-group control, and runtime stats. |
| 4. Add lightweight framework observability | completed | Added TCP wait counters and input batching counters in `socket` and `input_messenger`. |
| 5. Add server-side counters | completed | Kept protocol stable and exposed lightweight server stats through bvar/logging. |
| 6. Summarize changes and manual run commands | completed | Local compile verification intentionally skipped per user request. |
| 7. Write benchmark model report | completed | Added `docs/cn/rdma_performance_benchmark_model.md` describing the current model, flow, parameters, observability, and usage. |
| 8. Revert extra observability counters | completed | Removed `2af016d`-introduced framework counters from `socket`/`input_messenger` and the corresponding client/server output while keeping the benchmark model changes. |
| 9. Document channel/server bthread flow | completed | Added `docs/cn/brpc_channel_server_bthread_flow.md` explaining client/server main flows and business bthread creation paths. |
| 10. Expand full bthread creation flow document | completed | Extended the channel/server document to include `controller.cpp`, `socket.cpp`, and `input_messenger.cpp` creation points on both client and server paths. |
| 11. Add rdma_performance bthread mapping | completed | Supplemented the bthread flow document with benchmark client worker/token bthreads and server service execution context. |
| 12. Add PlantUML bthread creation diagrams | completed | Added client/server PlantUML flows and clarified normal client callback execution context and TaskGroup enqueue semantics. |
| 13. Write steal_task remote experiment plan | completed | Added `docs/cn/steal_task_remote_experiment_plan.md` with remote commands, metrics, test matrix, and conclusion rules. |
| 14. Analyze high C2C after worker affinity | in_progress | Worker pthread pinning did not reduce C2C or improve performance; investigate non-migration causes and design remote validation experiments. |
| 15. Minimal pooled open_loop large attachment timeout fix | completed | Added configurable connect timeout and strict enforcement of user-provided global `max_inflight` without adding default per-connection throttling. |
| 16. Continue benchmark after RPC failures | completed | Changed client response handling to count failures/timeouts and continue the run instead of stopping on first RPC failure. |

## Decisions
- Implement in the current clean feature branch instead of creating a new worktree.
- Keep `test.proto` unchanged unless client/server observability forces a protocol change.
- Skip compile/test execution locally; provide explicit manual commands instead.
- Write the benchmark-model report against the current repository state rather than reconstructing a historical spec from memory.
- Revert only the extra observability introduced in `2af016d`; keep `open_loop`, explicit connection slots, warmup, and connection-group controls.
- For the channel/server flow document, analyze the default TCP path with `usercode_in_coroutine=false` and `usercode_in_pthread=false`; call out alternate modes as caveats only.
- Treat `socket.cpp` and `input_messenger.cpp` as shared infrastructure used by both client response handling and server request handling; distinguish the final protocol destination instead of duplicating the shared mechanics.
- For `example/rdma_performance`, distinguish benchmark-created bthreads from framework-created bthreads and document where callback-driven closed-loop replenishment happens.
- For callback context, treat normal successful response as `new_bthread=false` because `ControllerPrivateAccessor::OnResponse()` calls `OnVersionedRPCReturned(info, false, ...)`; reserve `RunEndRPC` for special error/cancel paths.
- For steal_task diagnosis, prefer remote perf/bvar experiments first and defer framework bvar changes until existing observations cannot prove queue distribution.

## Risks
- The client change is behaviorally large, so keep default flag values close to the original semantics.
- "Actual TCP connection count" is approximated via existing bvar counters, not a new intrusive socket API.
- The model report is a static code-level description and does not replace runtime verification on target servers.
- The untracked benchmark-model report may now describe observability fields that were removed from code; treat the code as the source of truth unless the document is refreshed later.
- The channel/server bthread document is a static source walkthrough and does not require local compilation because no C++ behavior changes are made.
- The expanded bthread document describes normal TCP behavior first; RDMA and alternate user-code execution modes remain caveats.
- The benchmark mapping is based on current `example/rdma_performance` code and should be refreshed if the client open-loop permit implementation changes later.
- PlantUML diagrams document source-level creation points only; actual execution group may differ due to bthread work stealing.
- The steal_task experiment plan assumes remote machines can run perf, curl brpc vars, pidstat, ss, and the current rdma_performance binaries.
- Worker pthread affinity only eliminates OS migration of `brpc_wkr` pthreads. It does not prevent bthread task stealing, socket ownership movement, shared bvar/resource-pool access, or cross-core cache-line transfers in shared queues and counters.
- For large `pooled + open_loop` payloads, keep client-side pressure controls minimal: honor the configured global `max_inflight`, but do not add default per-connection throttling.
