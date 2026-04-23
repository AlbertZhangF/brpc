# TCP Path Performance Investigation Plan

## Goal
Implement the approved `use_rdma=false` benchmarking and observability changes for `example/rdma_performance`, and leave a reproducible trail of the decisions and manual verification commands.

## Phases
| Phase | Status | Notes |
|---|---|---|
| 1. Restore context and inspect target files | completed | Confirmed clean branch `benchmark_cx`, no existing planning files, inspected client/server/socket/input_messenger code paths. |
| 2. Add planning artifacts | completed | Created and started maintaining `task_plan.md`, `findings.md`, and `progress.md`. |
| 3. Rework rdma_performance client model | completed | Added load modes, explicit connection fanout, unique connection-group control, and runtime stats. |
| 4. Add lightweight framework observability | completed | Added TCP wait counters and input batching counters in `socket` and `input_messenger`. |
| 5. Add server-side counters | completed | Kept protocol stable and exposed lightweight server stats through bvar/logging. |
| 6. Summarize changes and manual run commands | in_progress | Skip compile verification per user request. |

## Decisions
- Implement in the current clean feature branch instead of creating a new worktree.
- Keep `test.proto` unchanged unless client/server observability forces a protocol change.
- Skip compile/test execution locally; provide explicit manual commands instead.

## Risks
- The client change is behaviorally large, so keep default flag values close to the original semantics.
- "Actual TCP connection count" is approximated via existing bvar counters, not a new intrusive socket API.
