# Findings & Decisions

## Requirements
- Analyze brpc's native compression support, especially whether snappy is supported and how to enable it.
- Based on `example/rdma_performance`, modify client and server benchmark flow to add compression/decompression during pressure testing.
- Add runtime parameters to choose which compression algorithm to use.
- Use `planning-with-files` for planning and record keeping across the work.
- Generate a commit message and perform `git commit` automatically at the end.
- Provide extra engineering suggestions if useful.

## Research Findings
- `CLAUDE.md` states snappy is an optional dependency for brpc.
- Top-level `CMakeLists.txt` exposes `WITH_SNAPPY` and links the system snappy library when enabled.
- The same `CMakeLists.txt` also compiles bundled snappy sources into the project source list, which indicates existing internal implementation support around snappy-related code paths.
- `src/brpc/controller.h` already provides `set_request_compress_type()` and `set_response_compress_type()` for RPC-level compression negotiation.
- `src/brpc/compress.h` is the framework-level compression registry/dispatch interface.
- `src/brpc/policy/snappy_compress.h` confirms a native snappy compressor/decompressor implementation exists in brpc.
- Existing tests and examples reference `COMPRESS_TYPE_GZIP`, `COMPRESS_TYPE_ZLIB`, and `COMPRESS_TYPE_SNAPPY`.
- `src/brpc/global.cpp` registers handlers for `COMPRESS_TYPE_GZIP`, `COMPRESS_TYPE_ZLIB`, and `COMPRESS_TYPE_SNAPPY`.
- `src/brpc/options.proto` defines `COMPRESS_TYPE_LZ4`, but repository search shows no registered LZ4 compress handler, so it is not framework-usable like the other three algorithms in this codebase state.
- In `src/brpc/policy/baidu_rpc_protocol.cpp` and `src/brpc/policy/hulu_pbrpc_protocol.cpp`, the protobuf message body is compressed first and request/response attachments are appended afterwards, so attachments do not benefit from the normal brpc RPC compression path.

## Technical Decisions
| Decision | Rationale |
|----------|-----------|
| Reuse `brpc::Controller` compression APIs instead of manual payload transforms | Keeps the example aligned with framework-native request/response compression behavior |
| Add a string-to-`CompressType` parsing layer in the example | Makes algorithm selection simple from gflags and avoids changing protobuf definitions |
| Configure client request compression and server response compression independently | Fits brpc's native controller API and gives clearer benchmark control over both directions |
| Add `payload` bytes fields to request/response protobufs | Ensures the benchmark's bulk data actually passes through brpc compression/decompression |
| Reject `payload_mode=attachment` when request compression is enabled | Prevents a misleading benchmark result where the main payload bypasses compression entirely |

## Issues Encountered
| Issue | Resolution |
|-------|------------|
| Requested `superpowers` skill is unavailable in the current AGENTS skill list | Continue with normal toolchain and record this explicitly |
| Example-specific CMake configure failed because the local workspace does not expose a discoverable brpc output path or Protobuf toolchain to that CMake flow | Limit verification to static/source-level checks and document the prerequisite gap |

## Resources
- `CLAUDE.md`
- `CMakeLists.txt`
- `src/brpc/compress.h`
- `src/brpc/controller.h`
- `src/brpc/policy/snappy_compress.h`
- `src/brpc/global.cpp`
- `src/brpc/options.proto`
- `src/brpc/policy/baidu_rpc_protocol.cpp`
- `src/brpc/policy/hulu_pbrpc_protocol.cpp`
- `example/rdma_performance/client.cpp`
- `example/rdma_performance/server.cpp`

## Visual/Browser Findings
- No browser or image inspection used in this task.
