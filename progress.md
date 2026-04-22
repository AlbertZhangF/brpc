# 进度日志

## Session 1-2: 历史记录 (见之前)

## Session 3: 2026-04-22 最大并发压测模型改造

### 完成的工作
- [x] 重新审视rdma_performance用例模型
- [x] 识别核心问题：固定并发流水线模型限制了最大吞吐
- [x] 设计"全速发射"模型：发送与接收解耦
- [x] 实现client.cpp改造

### 核心改造内容

#### 模型变更
| 项目 | 旧模型 | 新模型 |
|------|--------|--------|
| 参数 | queue_depth (固定并发数) | max_inflight (最大在途数) |
| 发送驱动 | HandleResponse回调驱动 | RunTest循环持续发送 |
| HandleResponse | 统计+发送新请求 | 仅统计+回收资源 |
| 并发数 | 固定=thread_num×queue_depth | 动态，由系统自然调节 |
| 上限控制 | queue_depth硬限制 | max_inflight防OOM |

#### 关键代码变更
1. **queue_depth → max_inflight**: 参数语义从"固定并发"变为"上限保护"
2. **SendRequest返回bool**: 成功发送返回true，达到上限返回false
3. **RunTest重写**: while循环持续发送，失败时usleep(1)让出CPU
4. **HandleResponse精简**: 移除SendRequest调用，仅做统计
5. **优雅退出**: 5秒drain超时保护

### 下一步
- 代码检视
