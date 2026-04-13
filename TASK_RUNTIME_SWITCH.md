# 任务计划：实现io_uring运行时切换功能

## 任务信息
- **创建日期**: 2026-04-13
- **目标**: 实现`--io_backend`参数的运行时切换功能，无需重新编译即可在epoll和io_uring之间切换

## 设计方案

### 核心实现

1. **修改event_dispatcher.h**：
   - 添加`_backend_type`和`_iouring_ctx`成员
   - 添加`CallInputEventCallback`和`CallOutputEventCallback`为public方法
   - 添加后端命名空间的前向声明和friend声明

2. **修改event_dispatcher.cpp**：
   - 添加`ResolveIoBackend()`函数解析`FLAGS_io_backend`
   - 在构造函数中根据后端类型调用对应的Init函数
   - 每个方法根据`_backend_type`分发到对应后端

3. **创建event_dispatcher_epoll_impl.cpp**：
   - 将epoll实现封装在`epoll_backend`命名空间中
   - 提供Init, Destroy, Start, Stop, AddConsumer等函数

4. **创建event_dispatcher_iouring_impl.cpp**：
   - 将io_uring实现封装在`iouring_backend`命名空间中
   - 提供Init, Destroy, Start, Stop, AddConsumer等函数

## 执行阶段

### Phase 1: 分析现有代码结构
**状态**: ✅ completed
**步骤**:
- [x] 分析EventDispatcher类的接口
- [x] 分析epoll和io_uring实现的差异
- [x] 确定分发点

### Phase 2: 修改代码实现运行时切换
**状态**: ✅ completed
**步骤**:
- [x] 修改event_dispatcher.h添加后端类型和访问器
- [x] 修改event_dispatcher.cpp实现分发逻辑
- [x] 创建event_dispatcher_epoll_impl.cpp
- [x] 创建event_dispatcher_iouring_impl.cpp

### Phase 3: 编译验证
**状态**: 🔄 in_progress
**步骤**:
- [ ] 编译brpc库
- [ ] 运行测试验证切换功能

## 使用方法

编译时需要启用`WITH_IO_URING=ON`：

```bash
cmake -DWITH_IO_URING=ON ..
make -j$(nproc)
```

运行时选择后端：

```bash
# 使用io_uring（默认）
./server --io_backend=io_uring

# 使用epoll
./server --io_backend=epoll

# 自动选择（编译时启用io_uring则使用io_uring，否则epoll）
./server --io_backend=auto
```

## 关键约束

1. 必须在编译时启用`WITH_IO_URING=ON`才能同时支持两种后端
2. 如果未启用`WITH_IO_URING`，`--io_backend=io_uring`会回退到epoll
3. 切换只能在程序启动时生效，不能运行中动态切换
