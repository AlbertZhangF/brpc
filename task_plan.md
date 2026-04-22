# 任务计划：TCP连接问题诊断与修复

## 目标
解决debug_info.md中记录的TCP连接未成功建立问题，确保高并发压测场景下连接稳定可靠。

## 问题分析

### 根本原因
| 编号 | 原因 | 严重度 |
|------|------|--------|
| R1 | 连接风暴：64线程×64Channel=4096并发连接请求 | CRITICAL |
| R2 | 缺少连接预热：仅Channel[0]做1次同步验证 | HIGH |
| R3 | Channel数量过多：每线程独立创建Channel | HIGH |
| R4 | bthread_concurrency默认值仅8+2=10 | MEDIUM |

## 执行阶段

### Phase 7: 问题诊断与方案设计 ✅ completed
- ✅ 分析debug_info.md错误日志
- ✅ 识别4个根本原因(R1-R4)
- ✅ 设计解决方案

### Phase 8: 实现连接预热机制 ✅ completed
- ✅ 全局共享Channel：所有线程共享一组Channel
- ✅ Channel数量：min(thread_num, 16)，减少连接数
- ✅ 异步并行预热：WarmupChannels()分批异步ping
- ✅ 预热超时保护：warmup_timeout_ms(默认30秒)
- ✅ 预热失败处理：超过半数失败则退出

### Phase 9: 添加连接建立等待机制 ✅ completed
- ✅ 预热确保所有Channel连接建立
- ✅ bthread_concurrency默认值改为CPU核数
- ✅ 渐进式启动bthread：每批8个，间隔10ms

### Phase 10: 控制并发连接数 ✅ completed
- ✅ Channel共享减少总连接数
- ✅ warmup_batch_size控制并行预热数(默认10)
- ✅ 分批启动避免连接风暴

### Phase 11: 代码检视 ✅ completed
**检视发现与修复**:

| # | 问题 | 严重度 | 状态 |
|---|------|--------|------|
| 1 | WarmupChannels使用同步RPC，批次内串行 | HIGH | ✅ 修复：改为异步并行+回调 |
| 2 | 单个RPC错误即停止整个线程 | HIGH | ✅ 修复：连续100次错误才停止 |
| 3 | bthread_concurrency默认0不设置 | MEDIUM | ✅ 修复：默认CPU核数 |
| 4 | Channel数量=thread_num导致过多 | MEDIUM | ✅ 修复：min(thread_num, 16) |

## 关键设计决策

### D6: 全局共享Channel
- 所有PerformanceTest共享g_channels全局Channel列表
- Channel是线程安全的，可被多bthread共享
- g_channel_counter原子变量轮询选择Channel
- 减少Channel数量从thread_num×thread_num到min(thread_num, 16)

### D7: 异步并行预热
- WarmupChannels()分批异步发送ping
- 每批warmup_batch_size个Channel同时预热
- WarmupContext+WarmupDone回调实现异步等待
- 预热成功后才启动压测

### D8: 容错错误处理
- 连续MAX_CONSECUTIVE_ERRORS(100)次错误才停止
- ELOGOFF/ERPCTIMEDOUT/EHOSTDOWN/ECONNECT错误仅计数不打印
- 成功请求重置consecutive_errors计数器
- 避免偶发错误导致压测中断

### D9: 渐进式启动
- 每批启动8个bthread
- 批次间间隔10ms
- 避免所有线程同时发起连接

## 修改文件清单
1. example/rdma_performance/client.cpp - 全局Channel、预热、容错、渐进启动
