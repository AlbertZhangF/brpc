# brpc io_uring支持

## 概述

本项目为brpc框架添加了io_uring支持，提供更高性能的异步I/O能力。

## 编译要求

- Linux内核 5.1或更高版本
- liburing库 (liburing-dev包)

## 编译方法

### 使用CMake

```bash
# 启用io_uring支持
cmake -DWITH_IO_URING=ON ..

# 编译
make -j$(nproc)
```

### 使用Make

```bash
# 配置时启用io_uring
sh config_brpc.sh --with-io-uring --headers=/usr/include --libs=/usr/lib

# 编译
make -j$(nproc)
```

## 使用方法

编译时启用io_uring后，brpc会自动使用io_uring作为I/O后端，无需修改应用代码。

### 运行时配置

可以通过gflags配置I/O后端：

```bash
# 强制使用io_uring
./your_program --io_backend=io_uring

# 强制使用epoll
./your_program --io_backend=epoll

# 自动选择（优先io_uring，不支持则fallback到epoll）
./your_program --io_backend=auto
```

## 性能优势

相比epoll，io_uring提供以下优势：

1. **减少系统调用**: 批量提交I/O请求，减少系统调用次数
2. **零拷贝**: 共享内存机制避免数据拷贝
3. **真正的异步I/O**: 所有I/O操作都是异步的
4. **更低的延迟**: 减少上下文切换开销

### 预期性能提升

| 场景 | 预期提升 |
|------|---------|
| 高并发连接（10万+） | 50-80% |
| 大量小消息 | 30-50% |
| 低延迟场景 | 40-60% |

## 实现细节

### 架构设计

io_uring版本的EventDispatcher与epoll版本保持相同的接口，主要区别在于：

1. **初始化**: 使用`io_uring_queue_init_params`创建io_uring实例
2. **事件注册**: 使用`IORING_OP_POLL_ADD`操作注册事件
3. **事件移除**: 使用`IORING_OP_POLL_REMOVE`操作移除事件
4. **事件循环**: 使用`io_uring_submit_and_wait`等待事件

### 关键数据结构

```cpp
struct IoUringContext {
    struct io_uring ring;          // io_uring实例
    bool initialized;              // 初始化标志
    std::mutex fd_map_mutex;       // fd映射互斥锁
    std::unordered_map<int, IOEventDataId> fd_to_event_data; // fd到事件数据的映射
};
```

### 事件处理流程

1. 应用调用`AddConsumer`注册fd
2. EventDispatcher创建`IORING_OP_POLL_ADD`请求
3. 事件到达时，内核将完成事件写入CQ
4. EventDispatcher从CQ读取完成事件
5. 调用用户注册的回调函数

## 兼容性

- **内核版本**: 需要Linux 5.1+
- **库依赖**: 需要liburing
- **向后兼容**: 不支持io_uring的系统会自动fallback到epoll

## 测试

### 单元测试

```bash
cd test
make
./brpc_event_dispatcher_iouring_unittest
```

### 性能测试

```bash
cd example/echo_c++
make
./echo_server &
./echo_client
```

## 已知问题

1. **SQPOLL模式**: 当前实现未启用SQPOLL模式，未来可以添加支持以进一步提升性能
2. **零拷贝**: 当前实现使用POLL操作，未充分利用io_uring的零拷贝特性，未来可以优化

## 参考资料

- [io_uring设计文档](../docs/io_uring_design.md)
- [io_uring(7) — Linux manual page](https://devdocs.io/man/man7/io_uring.7)
- [深入解剖io_uring:Linux异步IO的终极武器](https://www.51cto.com/article/819134.html)

## 贡献

欢迎提交Issue和Pull Request！

## 许可证

Apache License 2.0
