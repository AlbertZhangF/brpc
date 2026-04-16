# 任务计划：benchmark CPU压力瓶颈分析

## 任务信息
- **创建日期**: 2026-04-16
- **目标**: 识别benchmark端到端流程中限制CPU利用率的关键瓶颈

## Phase 1: 端到端请求流程梳理
**状态**: ✅ complete

### 完成内容
- 梳理了Client端和Server端的完整请求处理流程
- 识别了7个关键瓶颈点
- 分析了每个瓶颈的代码位置和影响

## Phase 2: 瓶颈识别
**状态**: ✅ complete

### 7个瓶颈点（按严重程度排序）
1. **EventDispatcher单线程**（★★★★★）- 全局串行点
2. **io_sq_thread空转**（★★★★★）- 65%CPU浪费
3. **Socket::_nevent去重**（★★★★☆）- 单Socket串行读取
4. **Socket写入串行化**（★★★★☆）- 单连接写带宽受限
5. **_overcrowded限流**（★★★☆☆）- 请求被拒
6. **bthread调度开销**（★★★☆☆）- 17%CPU浪费
7. **内存分配开销**（★★☆☆☆）- malloc锁竞争

## Phase 3: 解决方案
**状态**: ✅ complete

### 立即可做的优化
1. --event_dispatcher_num=8
2. --io_uring_sqpoll=false
3. --socket_max_unwritten_bytes=268435456
4. --bthread_concurrency=32
5. 使用多连接（connection_type=pooled）

## 交付物
- BOTTLENECK_ANALYSIS_REPORT.md - 完整分析报告
