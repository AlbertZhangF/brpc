# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Apache brpc is an industrial-grade RPC framework for C++ that supports multiple protocols (protobuf, thrift, HTTP/JSON, gRPC, Redis, Memcached, etc.). The codebase is organized in layers:

- **butil**: Base utilities (containers, strings, file I/O, synchronization primitives) - located in `src/butil/`
- **bthread**: M:N threading library with stackful coroutines - located in `src/bthread/`
- **bvar**: Statistics/metrics framework for multi-threaded programs - located in `src/bvar/`
- **brpc**: Main RPC framework with server/client/channel implementations - located in `src/brpc/`

## Build System

The project supports three build systems:

### 1. Make (Legacy but stable)

```bash
# Configure and build
sh config_brpc.sh --headers=/usr/include --libs=/usr/lib
make -j$(nproc)

# With options
sh config_brpc.sh --headers=/usr/include --libs=/usr/lib --with-glog --with-thrift --with-rdma
make -j$(nproc)
```

Common `config_brpc.sh` options:
- `--with-glog`: Use glog for logging instead of brpc's default
- `--with-thrift`: Enable Thrift protocol support
- `--with-rdma`: Enable RDMA support
- `--with-bthread-tracer`: Enable bthread tracing (Linux x86_64 only)
- `--with-asan`: Enable AddressSanitizer
- `--with-debug-bthread-sche-safety`: Debug bthread scheduling safety
- `--with-debug-lock`: Debug lock issues
- `--nodebugsymbols`: Exclude debug symbols for smaller binaries
- `--cc/--cxx`: Specify compiler (e.g., `--cc=clang --cxx=clang++`)
- `--werror`: Treat warnings as errors

### 2. CMake (Recommended for IDE integration)

```bash
mkdir build && cd build
cmake -DBUILD_UNIT_TESTS=ON .. && make -j$(nproc)

# Generate compile_commands.json for IDE/LSP support
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -B build

# With options
cmake -DWITH_GLOG=ON -DWITH_THRIFT=ON -DWITH_RDMA=ON -DBUILD_UNIT_TESTS=ON ..
```

### 3. Bazel

```bash
bazel build -- //... -//example/...

# With options
bazel build --define with_glog=true --define with_thrift=true -- //... -//example/...
```

## Testing

### Build and Run All Tests (Make)

```bash
# After configuring with config_brpc.sh and building the main library
cd test && make -j$(nproc)
sh run_tests.sh
```

### Build and Run All Tests (CMake)

```bash
mkdir build && cd build
cmake -DBUILD_UNIT_TESTS=ON .. && make -j$(nproc)
make test
```

### Running Specific Tests

Tests are organized into separate binaries:
- `test_butil` - Base utility tests
- `test_bvar` - Metrics/statistics tests
- `bthread*unittest` - Threading tests (e.g., `bthread_unittest`, `bthread_butex_unittest`)
- `brpc*unittest` - RPC tests (e.g., `brpc_channel_unittest`, `brpc_server_unittest`)

To run a specific test binary:
```bash
cd test
./test_butil                    # Run all butil tests
./brpc_channel_unittest         # Run channel tests
./bthread_unittest              # Run bthread tests
```

To run a specific test case:
```bash
./test_butil --gtest_filter="StringUtilTest.*"
./brpc_channel_unittest --gtest_filter="ChannelTest.*"
```

## Running Examples

```bash
# Using Make
cd example/echo_c++
make
./echo_server &
./echo_client

# Using CMake
cd example/echo_c++
cmake -B build && cmake --build build -j4
./build/echo_server &
./build/echo_client
```

## Code Architecture

### Key Components

**bthread** (M:N Threading):
- Stackful coroutines that multiplex on pthreads
- Key APIs: `bthread_start_background()`, `bthread_usleep()`, `bthread_mutex_*`, `bthread_cond_*`
- Uses work-stealing queue for scheduling
- File: `src/bthread/bthread.h`, `src/bthread/task_group.cpp`

**brpc Server**:
- `brpc::Server` - Main server class
- Services inherit from `google::protobuf::Service`
- Supports multiple protocols simultaneously on same port
- Built-in services: /status, /vars, /rpcz, /connections

**brpc Channel**:
- `brpc::Channel` - Client communication
- Supports connection pooling, load balancing, circuit breaking
- Naming services for service discovery

**bvar**:
- Statistics variables that are efficient under high contention
- Types: `bvar::Adder`, `bvar::Counter`, `bvar::LatencyRecorder`, `bvar::GFlag`
- Exposed via built-in HTTP services

### Protocol Support

brpc supports multiple protocols simultaneously:
- **baidu_std**: Default protobuf-based protocol
- **http**: HTTP/1.1 and HTTP/2
- **h2_grpc**: gRPC protocol
- **thrift**: Apache Thrift framed protocol (requires `--with-thrift`)
- **redis**: Redis protocol (stateful connection)
- **memcache**: Memcached protocol
- **esp**: Baidu ESP protocol
- **nova_pbrpc**: Nova protobuf RPC
- **sofa_pbrpc**: SOFA protobuf RPC
- **public_pbrpc**: Public protobuf RPC

Protocol implementations are in `src/brpc/policy/*_protocol.cpp`.

### IOBuf (Zero-copy Buffer)

`brpc::IOBuf` is a non-contiguous zero-copy buffer used throughout:
- `src/brpc/iobuf.h`, `src/butil/iobuf.h`
- Similar to `struct iovec` but with reference counting
- Efficient for network I/O with scatter-gather

## Code Style

- Follow Google C++ Style Guide (4-space indentation)
- All public headers use include guards
- Code location should match its purpose: protocol-specific code in policy/, general code in top-level

## Dependencies

Required:
- gflags
- protobuf (3.5.1+)
- leveldb (for rpcz)
- OpenSSL

Optional:
- glog (alternative logging)
- thrift (Thrift protocol)
- gperftools (CPU/heap profiling)
- libunwind + absl (bthread tracer)
- snappy (compression)
