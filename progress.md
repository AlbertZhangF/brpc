# 进度日志

## Session 1-3: 历史记录 (见之前)

## Session 4: 2026-04-22 TCP连接问题诊断与修复

### 完成的工作
- [x] 分析debug_info.md运行时错误日志
- [x] 识别4个根本原因：连接风暴、缺少预热、Channel过多、bthread_concurrency不足
- [x] 实现全局共享Channel架构
- [x] 实现异步并行连接预热机制
- [x] 修复bthread_concurrency默认值
- [x] 实现渐进式bthread启动
- [x] 实现容错错误处理（连续100次错误才停止）
- [x] 代码检视并修复4个问题

### 核心修复内容

#### 架构变更：全局共享Channel
| 项目 | 旧设计 | 新设计 |
|------|--------|--------|
| Channel归属 | 每线程独立创建 | 全局共享g_channels |
| Channel数量 | thread_num×thread_num | min(thread_num, 16) |
| Channel选择 | 每线程内_channel_counter | 全局g_channel_counter轮询 |
| 连接数 | 64×64=4096 | 最多16个Channel×连接池 |

#### 新增功能
1. **InitGlobalChannels()**: 创建全局共享Channel
2. **WarmupChannels()**: 异步并行预热所有Channel连接
3. **CleanupGlobalChannels()**: 清理全局Channel
4. **WarmupContext/WarmupDone**: 异步预热回调机制
5. **渐进式启动**: 每批8个bthread，间隔10ms
6. **容错错误处理**: 连续100次错误才停止

#### 参数变更
- `--channel_num`: 含义从"每线程Channel数"变为"全局共享Channel数"
- `--warmup_timeout_ms`: 新增，预热超时(默认30秒)
- `--warmup_batch_size`: 新增，并行预热批大小(默认10)
- `--perf_bthread_concurrency`: 默认值从0(不设置)改为CPU核数
