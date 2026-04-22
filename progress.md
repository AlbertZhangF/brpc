# 进度日志

## Session 1: 2026-04-22

### 完成的工作
- [x] 阅读CLAUDE.md了解项目结构
- [x] 分析rdma_performance/client.cpp (321行)
- [x] 分析rdma_performance/server.cpp (97行)
- [x] 分析rdma_performance/test.proto
- [x] 分析RdmaEndpoint核心实现 (rdma_endpoint.cpp 1667行)
- [x] 分析RdmaHelper (rdma_helper.cpp 734行)
- [x] 分析Socket写入路径 (socket.cpp)
- [x] 分析Channel::CallMethod (channel.cpp)
- [x] 识别8个关键瓶颈(B1-B8)
- [x] 创建任务计划

### 关键发现
1. 单连接类型是最大瓶颈 - 所有线程共享125个RDMA窗口
2. 串行请求模式限制了并发度
3. 事件驱动模式增加了延迟
4. 热路径内存分配影响性能

## Session 2: 2026-04-22

### 完成的工作
- [x] 实现优化版client.cpp
- [x] 实现优化版server.cpp
- [x] 修复gflags命名冲突(使用perf_前缀)
- [x] 修复前向声明问题
- [x] 移除LatencyRecorder::reset()调用(不存在该方法)

### 优化内容详情

#### client.cpp 优化
1. **连接类型**: "single" -> "pooled" (B2修复)
2. **队列深度**: 1 -> 64 (B1缓解)
3. **RDMA SQ/RQ**: 128 -> 1024 (B3修复)
4. **RDMA轮询模式**: false -> true (B4修复)
5. **CQE轮询批量**: 32 -> 64 (B7修复)
6. **轮询器数量**: 1 -> 4 (B8修复)
7. **多Channel支持**: 每个PerformanceTest可创建多个Channel (B2增强)
8. **inflight计数器**: 跟踪在途请求数，确保优雅退出
9. **错误计数**: 新增g_error_cnt统计错误请求
10. **ApplyRdmaFlags**: 在GlobalRdmaInitializeOrDie之前设置RDMA参数
11. **RequestContext**: 统一的请求上下文结构体

#### server.cpp 优化
1. **RDMA SQ/RQ**: 128 -> 1024 (B3修复)
2. **RDMA轮询模式**: false -> true (B4修复)
3. **CQE轮询批量**: 32 -> 64 (B7修复)
4. **轮询器数量**: 1 -> 4 (B8修复)
5. **num_threads**: 可配置工作线程数
6. **ApplyRdmaFlags**: 在GlobalRdmaInitializeOrDie之前设置RDMA参数

### 下一步
- 代码检视
- 提供构建和运行命令
