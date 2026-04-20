# 任务计划：极限并发Benchmark工具开发与brpc并发限制分析

## 目标
1. 开发无限制最大发送模式的benchmark工具 ✅
2. 识别并验证brpc框架中限制并发度的机制 ✅

## Phase 1: brpc框架并发限制机制深度分析
**状态**: ✅ complete

### 已识别7大限制机制
1. Socket::_overcrowded限流（64MB默认）— 最关键
2. 单EventDispatcher（默认1个线程）
3. 单连接写入串行化（KeepWrite单线程）
4. 连接池大小限制（默认100）
5. bthread调度器并发度
6. Controller堆分配开销
7. Socket发送/接收缓冲区

## Phase 2: 开发极限并发Benchmark工具
**状态**: ✅ complete

### 已创建文件
- extreme_client.cpp：无限制发送模式客户端
- extreme_server.cpp：极简服务端
- CMakeLists.txt：已添加编译目标

### 核心设计
- 无限制持续发送（不维持固定depth）
- EOVERCROWDED感知+退避重试
- 多Channel支持（--channel_per_thread）
- 实时每秒监控输出
- 内置brpc参数调优

## Phase 3: 测试验证与报告
**状态**: pending

### 编译命令
```bash
cd /home/zfz/brpc/example/rdma_performance/build
cmake .. && make -j$(nproc)
```

### 测试方案
1. 默认single连接
2. +ignore_eovercrowded +大缓冲区
3. +pooled连接
4. +多Channel
5. +多EventDispatcher
