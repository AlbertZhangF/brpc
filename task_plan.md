# 任务计划：brpc RDMA性能瓶颈分析与优化

## 目标
深入分析rdma_performance示例的QPS瓶颈，实施针对性优化，并开发高性能压力测试用例，使QPS能够随配置参数线性提升。

## 已识别的关键瓶颈

### B1: 客户端串行请求模式 (CRITICAL) ✅ 已修复
- HandleResponse回调中仅发送1个新请求，稳态并发数=queue_depth*thread_num
- 每次SendRequest都进行堆内存分配(new Controller/Response/Closure)
- 修复: queue_depth从1增到64，RequestContext统一结构体

### B2: 单连接类型 (CRITICAL) ✅ 已修复
- connection_type="single"导致所有bthread共享同一Socket/RDMA Endpoint
- RDMA窗口被所有线程共享，造成严重竞争
- 修复: 使用"pooled"连接类型，多Channel轮询

### B3: RDMA窗口大小限制 (CRITICAL) ✅ 已修复
- 默认rdma_sq_size=128, rdma_rq_size=128
- 窗口容量=min(SQ,remote_RQ)-3=125，单连接下所有线程共享
- 修复: SQ/RQ增大到1024，窗口容量=1021

### B4: RDMA事件驱动模式 (HIGH) ✅ 已修复
- 默认rdma_use_polling=false，CQ事件通过事件分发器处理
- 修复: 启用RDMA轮询模式

### B5: 热路径内存分配 (MEDIUM) ✅ 已修复
- 每次SendRequest分配3个堆对象(Controller/Response/Closure)
- 修复: RequestContext统一结构体，减少分配次数

### B6: 令牌桶限速 (MEDIUM) ✅ 已修复
- expected_qps>0时令牌桶机制人为限制QPS
- 修复: 默认expected_qps=0(不限速)

### B7: CQ轮询批量大小 (LOW) ✅ 已修复
- rdma_cqe_poll_once=32限制每次轮询处理的CQE数量
- 修复: 增大到64

### B8: RDMA轮询器数量 (LOW) ✅ 已修复
- rdma_poller_num=1，轮询模式下仅1个轮询线程
- 修复: 增大到4

### BUG: AllocateQpCq CQ大小不足 ✅ 已修复
- CQ大小使用2*FLAGS_rdma_prepared_qp_size而非sq_size+rq_size
- 当SQ/RQ>rdma_prepared_qp_size时CQ溢出
- 修复: 改用sq_size+rq_size

## 执行阶段

### Phase 1: 创建优化版客户端 ✅ completed
**目标**: 实现高性能RDMA压力测试客户端
**完成内容**:
- ✅ 使用pooled连接类型(默认)
- ✅ 增大RDMA SQ/RQ尺寸(1024)
- ✅ 启用RDMA轮询模式
- ✅ 增大CQE轮询批量(64)
- ✅ 增大轮询器数量(4)
- ✅ RequestContext统一结构体
- ✅ 多Channel支持
- ✅ inflight计数器
- ✅ ApplyRdmaFlags在初始化前设置参数
- ✅ SendRequest检查_stop标志

### Phase 2: 创建优化版服务端 ✅ completed
**目标**: 实现高性能RDMA压力测试服务端
**完成内容**:
- ✅ 可配置num_threads
- ✅ 增大RDMA SQ/RQ尺寸(1024)
- ✅ 启用RDMA轮询模式
- ✅ 增大CQE轮询批量(64)
- ✅ 增大轮询器数量(4)
- ✅ ApplyRdmaFlags在初始化前设置参数

### Phase 3: 框架Bug修复 ✅ completed
**目标**: 修复发现的RDMA框架Bug
**完成内容**:
- ✅ 修复AllocateQpCq中CQ大小不足问题

### Phase 4: 代码检视 ✅ completed
**目标**: 确保代码质量
**完成内容**:
- ✅ 修复gflags命名冲突(使用perf_前缀)
- ✅ 修复前向声明问题
- ✅ 移除不存在的LatencyRecorder::reset()调用
- ✅ 初始化_channel_counter
- ✅ SendRequest检查_stop标志
- ✅ CQ大小修复

## 关键设计决策
- D1: 使用pooled连接替代single连接，每个bthread可获得独立Socket
- D2: RDMA SQ/RQ增大到1024，窗口容量可达1021
- D3: 启用RDMA轮询模式，减少事件驱动延迟
- D4: 使用RequestContext统一结构体，减少分配开销
- D5: 客户端使用纯异步流水线模式，最大化并发
- D6: 使用perf_前缀避免与框架gflags冲突
- D7: ApplyRdmaFlags在GlobalRdmaInitializeOrDie之前调用
- D8: 同时设置rdma_prepared_qp_size以匹配SQ/RQ大小

## 修改的文件清单
1. example/rdma_performance/client.cpp - 优化版客户端
2. example/rdma_performance/server.cpp - 优化版服务端
3. src/brpc/rdma/rdma_endpoint.cpp - 修复CQ大小Bug
