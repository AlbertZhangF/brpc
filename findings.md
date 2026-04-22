# 研究发现：brpc RDMA性能瓶颈分析

## 代码架构分析

### 客户端请求流程
1. PerformanceTest::SendRequest() -> 创建Controller/Response/Closure
2. stub.Test() -> Channel::CallMethod() -> 序列化请求
3. Socket::Write() -> 将请求加入写队列
4. Socket::KeepWrite() -> 调用DoWrite()
5. DoWrite() -> RdmaEndpoint::CutFromIOBufList() -> ibv_post_send()
6. 响应到达 -> PollCq -> HandleCompletion -> ProcessNewMessage
7. HandleResponse回调 -> 再次SendRequest()

### RDMA窗口机制
- _window_size = min(local_SQ, remote_RQ) - RESERVED_WR_NUM(3)
- 每次ibv_post_send后_window_size减1
- 收到ACK后_window_size增加
- _window_size=0时CutFromIOBufList返回EAGAIN
- Socket::KeepWait等待epollout事件

### 关键GFlags及默认值
| Flag | 默认值 | 优化值 | 影响 |
|------|--------|--------|------|
| rdma_sq_size | 128 | 1024 | SQ容量，限制发送窗口 |
| rdma_rq_size | 128 | 1024 | RQ容量，限制接收窗口 |
| rdma_use_polling | false | true | 事件驱动vs轮询模式 |
| rdma_poller_num | 1 | 4 | 轮询线程数 |
| rdma_cqe_poll_once | 32 | 64 | 每次CQ轮询最大CQE数 |
| rdma_prepared_qp_size | 128 | 1024 | 预分配QP大小 |
| rdma_prepared_qp_cnt | 1024 | 1024 | 预分配QP数量 |

### 连接类型分析
- single: 所有请求共享一个Socket，一个RDMA Endpoint
- pooled: 从连接池获取Socket，每个Socket有独立RDMA Endpoint
- short: 每次请求创建新连接（不适合高性能）

## 瓶颈量化分析

### B1: 串行请求模式 (CRITICAL)
- 原始代码HandleResponse中仅发送1个新请求
- 稳态并发数=queue_depth*thread_num
- 默认queue_depth=1，即每线程仅1个在途请求
- 优化: queue_depth=64，每线程64个在途请求

### B2: 单连接类型 (CRITICAL)
- connection_type="single"导致所有bthread共享同一Socket
- RDMA窗口被所有线程共享，造成严重竞争
- 优化: 使用"pooled"连接类型，多Channel轮询

### B3: RDMA窗口大小限制 (CRITICAL)
- 默认窗口=min(128,128)-3=125
- 单连接下所有线程共享125个RDMA发送槽
- 优化: SQ/RQ=1024，窗口=1021

### B4: RDMA事件驱动模式 (HIGH)
- 默认rdma_use_polling=false
- CQ事件通过event dispatcher处理，增加延迟
- 优化: 启用轮询模式

### B5: 热路径内存分配 (MEDIUM)
- 每次SendRequest分配3个堆对象
- 优化: RequestContext统一结构体

### B6: 令牌桶限速 (MEDIUM)
- expected_qps>0时令牌桶限制QPS
- 优化: 默认expected_qps=0(不限速)

### B7: CQ轮询批量大小 (LOW)
- rdma_cqe_poll_once=32限制每次轮询CQE数
- 优化: 增大到64

### B8: RDMA轮询器数量 (LOW)
- rdma_poller_num=1，轮询模式下仅1个轮询线程
- 优化: 增大到4

## 发现的框架Bug

### BUG: AllocateQpCq中CQ大小不足
- **位置**: src/brpc/rdma/rdma_endpoint.cpp:AllocateQpCq()
- **问题**: CQ大小使用`2 * FLAGS_rdma_prepared_qp_size`，而非`sq_size + rq_size`
- **影响**: 当SQ/RQ大小超过rdma_prepared_qp_size时，CQ溢出
- **修复**: 改为使用`sq_size + rq_size`作为CQ大小
- **状态**: 已修复

### 理论性能分析
- 优化后窗口=1021，假设延迟50us
- 单连接理论QPS=1021/0.00005=20.4M
- pooled连接+多Channel，理论QPS可线性扩展
- 实际受限于CPU、内存带宽、RDMA网卡能力
