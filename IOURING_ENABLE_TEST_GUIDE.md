# io_uring 使能和测试指南

## 1. 概述

本文档说明如何在brpc框架中使能io_uring支持，以及如何运行相关测试。

## 2. 系统要求

- **Linux内核**: 5.1 或更高版本
- **liburing库**: liburing-dev 或 liburing-devel

### 2.1 检查内核版本

```bash
uname -r
```

### 2.2 安装liburing库

**Ubuntu/Debian:**
```bash
sudo apt-get install liburing-dev
```

**CentOS/RHEL:**
```bash
sudo yum install liburing-devel
```

**Fedora:**
```bash
sudo dnf install liburing-devel
```

**从源码编译:**
```bash
git clone https://github.com/axboe/liburing.git
cd liburing
./configure
make -j$(nproc)
sudo make install
```

## 3. 编译配置

### 3.1 使用CMake使能io_uring

```bash
# 创建构建目录
mkdir build && cd build

# 配置cmake，启用io_uring支持
cmake -DWITH_IO_URING=ON ..

# 编译
make -j$(nproc)
```

### 3.2 使用config_brpc.sh使能io_uring

```bash
# 配置brpc，启用io_uring
./config_brpc.sh --with-io-uring --headers=/usr/include --libs=/usr/lib

# 编译
make -j$(nproc)
```

### 3.3 验证编译定义

编译时应该看到以下定义：
```
-DBRPC_WITH_IO_URING=1
```

可以在CMakeLists.txt中验证：
```cmake
if(WITH_IO_URING)
    set(CMAKE_CPP_FLAGS "${CMAKE_CPP_FLAGS} -DBRPC_WITH_IO_URING=1")
endif()
```

## 4. 运行测试

### 4.1 运行io_uring单元测试

```bash
# 进入构建目录
cd build

# 运行特定测试
./test/brpc_event_dispatcher_iouring_unittest

# 运行所有事件分发器测试
./test/brpc_event_dispatcher_unittest

# 运行所有测试
make test
```

### 4.2 预期的测试输出

```
[==========] Running 20 tests from 1 test suite.
[----------] Global test environment set-up
[----------] 20 tests from IoUringEventDispatcherTest
[----------] 1/20 test IoUringEventDispatcherTest.ConstructorAndDestructor
[  PASSED  ] IoUringEventDispatcherTest.ConstructorAndDestructor (1 ms)
...
[----------] Global test environment tear-down
[==========] 20 tests from IoUringEventDispatcherTest ran. (100 ms total)
[  PASSED  ] 20 tests.
```

### 4.3 如果测试被跳过

如果看到以下输出，说明io_uring未启用：

```
[----------] 1 test from IoUringTest
[  SKIPPED ] IoUringTest.DISABLED_IoUringNotSupported (0 ms)
```

检查：
1. CMake配置是否添加了 `-DWITH_IO_URING=ON`
2. liburing库是否正确安装

## 5. 测试覆盖范围

### 5.1 测试列表

| 测试名称 | 测试内容 | 状态 |
|---------|---------|------|
| ConstructorAndDestructor | 构造函数和析构函数 | ✅ |
| StartAndStop | 启动和停止 | ✅ |
| StartTwice | 重复启动检测 | ✅ |
| AddConsumerWithInvalidFd | 无效FD添加 | ✅ |
| AddConsumerWithPipe | Pipe添加消费者 | ✅ |
| AddConsumerMultipleFd | 多FD添加 | ✅ |
| RemoveConsumer | 移除消费者 | ✅ |
| RemoveEventWithInvalidFd | 无效FD移除 | ✅ |
| RemoveConsumerNotAdded | 移除未添加的FD | ✅ |
| RegisterEventWithPollin | 注册读事件 | ✅ |
| RegisterEventWithoutPollin | 注册写事件 | ✅ |
| RegisterEventWithInvalidFd | 无效FD注册 | ✅ |
| UnregisterEventWithPollin | 注销读事件 | ✅ |
| UnregisterEventWithoutPollin | 注销写事件 | ✅ |
| EventCallbackWithPipe | 事件回调 | ✅ |
| ConcurrentFdRegistration | 并发注册 | ✅ |
| StressTestWithManyFd | 压力测试 | ✅ |
| RunningStateCheck | 运行状态检查 | ✅ |
| JoinWithoutStart | 未启动就Join | ✅ |
| StopWithoutStart | 未启动就Stop | ✅ |
| WakeupPipeFunctionality | Wakeup管道 | ✅ |
| AddRemoveConsumerSequence | 顺序添加移除 | ✅ |

### 5.2 未覆盖的边界情况

以下场景尚未有测试覆盖：

1. **RearmFd失败场景** - POLL_ADD重注册失败
2. **SQ满场景** - GetSqeWithRetry失败
3. **多次RemoveConsumer** - 重复移除同一FD
4. **AddConsumer重复FD** - 同一FD添加多次

## 6. 运行时配置

### 6.1 通过GFLAGS配置

```bash
# 自动选择后端（默认）
./your_program --io_backend=auto

# 强制使用io_uring
./your_program --io_backend=io_uring

# 强制使用epoll
./your_program --io_backend=epoll
```

### 6.2 检查运行时使用的后端

在日志中搜索：
```
[INFO] io_uring created: ring_fd=xxx
```

如果看到上述日志，说明正在使用io_uring。

## 7. 调试

### 7.1 启用详细日志

```bash
./your_program --log_level=DEBUG
```

### 7.2 常见问题

#### 问题1: 测试编译失败

**错误信息:**
```
fatal error: liburing.h: No such file or directory
```

**解决方案:**
```bash
# 安装liburing开发库
sudo apt-get install liburing-dev

# 或者设置PKG_CONFIG_PATH
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

#### 问题2: io_uring初始化失败

**错误信息:**
```
Fail to create io_uring: Operation not permitted
```

**解决方案:**
- 检查内核版本是否 >= 5.1
- 检查是否在容器中运行，容器是否限制了io_uring

#### 问题3: 所有测试被跳过

**错误信息:**
```
[  SKIPPED ] IoUringTest.DISABLED_IoUringNotSupported
```

**解决方案:**
- 确认编译时添加了 `-DWITH_IO_URING=ON`
- 确认CMake输出中有 `BRPC_WITH_IO_URING=1`

## 8. 性能测试

### 8.1 基准测试

```bash
# 使用echo_c++示例进行基准测试
cd examples/echo_c++

# 编译
make clean && make -j$(nproc)

# 运行服务器
./echo_server --io_backend=io_uring &

# 运行基准测试
./echo_client -N 1000000 -c 100 -q 10
```

### 8.2 对比epoll和io_uring

```bash
# 测试epoll
./echo_server --io_backend=epoll &
sleep 2
./echo_client -N 100000 -c 50 -q 10

# 停止服务器
pkill echo_server

# 测试io_uring
./echo_server --io_backend=io_uring &
sleep 2
./echo_client -N 100000 -c 50 -q 10
```

## 9. 参考资料

- [io_uring设计文档](../docs/io_uring_design.md)
- [代码对比分析报告](../IOURING_ANALYSIS_REPORT.md)
- [测试与实现对比报告](../TEST_IMPL_COMPARISON_REPORT.md)
- [liburing官方文档](https://github.com/axboe/liburing)
