# 任务计划：分析bthread的TLS机制与Work Stealing安全性

## 目标
深入分析brpc框架中bthread的TLS（Thread Local Storage）机制，理解在Work Stealing调度下如何保证TLS的安全性，以及bthread与pthread TLS的区别。

## 背景问题
为了保证各核心负载均衡，bthread实现了Work Stealing机制：
- 当某个pthread worker空闲时，它会去"偷"其他忙碌worker队列里的bthread来执行
- 这意味着，一个bthread在执行过程中（如阻塞后），完全可能被另一个核心上的worker偷走并继续执行
- 从而导致它和最初创建它的pthread不在同一个核心上
- 如果bthread访问的是创建它的那个pthread的线程局部变量，会存在严重风险

## 阶段划分

### Phase 1: 理解问题背景 [in_progress]
**目标**: 深入理解Work Stealing机制和TLS安全性问题

**任务列表**:
- [x] 创建任务计划文件
- [ ] 分析Work Stealing机制的实现
- [ ] 分析pthread TLS的机制和风险
- [ ] 分析bthread TLS的设计目标

**关键问题**:
1. Work Stealing机制如何实现？
2. pthread TLS在Work Stealing下有什么风险？
3. bthread如何解决TLS安全性问题？

**预期输出**:
- Work Stealing机制代码路径
- pthread TLS风险分析
- bthread TLS解决方案

### Phase 2: 分析bthread TLS实现 [pending]
**目标**: 深入分析bthread TLS的具体实现

**任务列表**:
- [ ] 分析bthread_key_create/bthread_getspecific/bthread_setspecific实现
- [ ] 分析TaskMeta中的local_storage字段
- [ ] 分析TLS在bthread调度时的同步机制
- [ ] 对比bthread TLS和pthread TLS的区别

### Phase 3: 分析实际应用场景 [pending]
**目标**: 分析brpc中TLS的实际使用场景

**任务列表**:
- [ ] 分析brpc中bthread TLS的使用案例
- [ ] 分析哪些数据适合存储在bthread TLS
- [ ] 分析哪些数据适合存储在pthread TLS
- [ ] 总结最佳实践

### Phase 4: 文档更新 [pending]
**目标**: 更新brpc_architecture_and_workflow.md文档

**任务列表**:
- [ ] 补充bthread TLS机制说明
- [ ] 补充Work Stealing下的TLS安全性分析
- [ ] 添加代码位置引用
- [ ] 添加最佳实践建议

## 当前进度
- ✓ Phase 1: 理解问题背景 [complete]
- ✓ Phase 2: 分析bthread TLS实现 [complete]
- ✓ Phase 3: 分析实际应用场景 [complete]
- ✓ Phase 4: 文档更新 [complete]

**任务已完成！**

## 发现与笔记
### Work Stealing机制
- 位置: src/bthread/task_group.cpp
- 关键函数: steal_task()

### bthread TLS机制
- 位置: src/bthread/key.cpp
- 关键函数: bthread_key_create(), bthread_getspecific(), bthread_setspecific()

## 错误记录
暂无
