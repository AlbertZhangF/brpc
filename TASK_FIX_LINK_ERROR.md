# 任务计划：修复io_uring链接错误

## 任务信息
- **创建日期**: 2026-04-08
- **任务**: 修复brpc io_uring编译链接错误
- **错误**: undefined reference to io_uring函数

## 错误分析

### 编译错误信息
```
undefined reference to `io_uring_submit'
undefined reference to `io_uring_queue_init_params'
undefined reference to `io_uring_queue_exit'
undefined reference to `io_uring_submit_and_wait'
```

### 错误原因
链接器找不到liburing库的符号，原因是brpc-static和brpc-shared库没有链接liburing。

### 涉及的源文件
- src/brpc/event_dispatcher_iouring.cpp

### 涉及的函数
- io_uring_submit
- io_uring_queue_init_params
- io_uring_queue_exit
- io_uring_submit_and_wait

## 执行阶段

### Phase 1: 检查CMakeLists.txt配置
**状态**: ✅ completed
**目标**: 确认liburing库链接配置
**步骤**:
- ✅ 检查主CMakeLists.txt中WITH_IO_URING的配置
- ✅ 检查src/CMakeLists.txt的配置
- ✅ 发现问题：brpc-static和brpc-shared没有链接liburing

### Phase 2: 修复链接配置
**状态**: ✅ completed
**目标**: 确保liburing正确链接
**步骤**:
- ✅ 为brpc-static添加target_link_libraries
- ✅ 为brpc-shared添加target_link_libraries
- ✅ 修改文件：src/CMakeLists.txt

### Phase 3: 测试编译
**状态**: pending
**目标**: 验证修复有效
**步骤**:
- 重新编译测试

## 修复内容

### 修复1: 为brpc-static添加liburing链接

**文件**: src/CMakeLists.txt
**位置**: 第65-68行

```cmake
if(WITH_IO_URING)
   target_link_libraries(brpc-static ${LIBURING_LIB})
endif()
```

### 修复2: 为brpc-shared添加liburing链接

**文件**: src/CMakeLists.txt
**位置**: 第92-95行

```cmake
if(WITH_IO_URING)
    target_link_libraries(brpc-shared ${LIBURING_LIB})
endif()
```

## 决策记录

### 决策1: 分析CMakeLists.txt配置
**日期**: 2026-04-08
**问题**: liburing库链接配置问题
**分析**: 检查配置发现WITH_IO_URING选项存在，find_library能找到liburing，但brpc-static和brpc-shared库没有链接它
**决定**: 需要在src/CMakeLists.txt中为brpc-static和brpc-shared添加target_link_libraries
