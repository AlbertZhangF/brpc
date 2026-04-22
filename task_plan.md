# 任务计划：Segfault修复与pthread配置

## 目标
1. 修复client预热后Segfault崩溃
2. 修复channel_num未受上限约束
3. 补充client/server运行时pthread数量配置

## 问题分析

### P1: Segfault (CRITICAL)
- 768个bthread同时启动，全速发送异步RPC
- max_inflight=0表示无限制，768线程×无限制=内存耗尽
- 每个RequestContext约1KB，7.68M并发=7.68GB→OOM→Segfault
- 修复：添加g_max_total_inflight全局上限，默认min(thread_num*10000, 500000)

### P2: channel_num未受上限 (HIGH)  
- 用户传入--channel_num=768，绕过了min(thread_num,16)上限
- 768个Channel创建768个连接池，资源浪费
- 修复：始终限制channel_num <= max_channel_num(默认64)

### P3: 缺少pthread数量 (MEDIUM)
- server端num_threads默认0但未设置合理默认值
- server端缺少bthread_concurrency设置
- 修复：默认值改为CPU核数，添加perf_bthread_concurrency

## 执行阶段

### Phase 12: 修复Segfault和channel_num ✅ completed
- ✅ 添加g_max_total_inflight全局inflight上限
- ✅ 默认值min(thread_num*10000, 500000)，防止OOM
- ✅ channel_num始终受max_channel_num(64)上限约束
- ✅ HandleResponse中递减g_total_inflight
- ✅ token不足时回退g_total_inflight

### Phase 13: 补充pthread配置 ✅ completed
- ✅ server端num_threads默认值改为CPU核数
- ✅ server端添加perf_bthread_concurrency参数
- ✅ server端ApplyCommonFlags中设置bthread_concurrency

### Phase 14: 代码检视 ✅ completed
**检视发现与修复**:

| # | 问题 | 严重度 | 状态 |
|---|------|--------|------|
| 1 | max_inflight=0时768线程无限制→OOM→Segfault | CRITICAL | ✅ 修复：g_max_total_inflight上限500000 |
| 2 | channel_num=768绕过上限 | HIGH | ✅ 修复：始终min(channel_num, max_channel_num) |
| 3 | server端num_threads默认0无合理值 | MEDIUM | ✅ 修复：默认CPU核数 |
| 4 | server端缺少bthread_concurrency设置 | MEDIUM | ✅ 修复：添加perf_bthread_concurrency |
