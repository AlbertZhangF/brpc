# Benchmark CPU利用率瓶颈分析与优化

## 一、瓶颈定位

### 1.1 原始模型：固定深度Pipeline

```
RunTest() → 发送queue_depth个请求 → 线程退出
HandleResponse() → 收到1个响应 → 发1个新请求 → 维持固定深度
```

**问题**：RunTest()发送完初始请求后线程就退出了，后续请求完全依赖HandleResponse回调驱动。如果HandleResponse有任何延迟，in-flight数量就会下降，CPU利用率无法拉满。

### 1.2 瓶颈根因分析

#### 瓶颈1：RunTest线程提前退出（最关键）

```cpp
static void* RunTest(void* arg) {
    for (int i = 0; i < FLAGS_queue_depth; ++i) {
        test->SendRequest();  // 发8个请求
    }
    return NULL;  // 线程退出！后续完全靠回调驱动
}
```

线程发完queue_depth个请求就退出了，没有持续补充压力的机制。

#### 瓶颈2：HandleResponse中的同步开销

```cpp
static void HandleResponse(RespClosure* closure) {
    // 1. 延迟统计（bvar写入）
    g_latency_recorder << closure->cntl->latency_us();
    // 2. CPU统计（bvar Variable查询，较重）
    bvar::Variable::describe_exposed("process_cpu_usage")
    // 3. 原子操作
    g_total_bytes.fetch_add(...);
    g_total_cnt.fetch_add(...);
    // 4. Controller析构（重量级，涉及Socket引用释放等）
    cntl_guard.reset(NULL);
    // 5. 然后才发下一个请求
    closure->test->SendRequest();
}
```

每次响应处理必须完成所有统计和清理后才能发下一个请求，增加了请求间隔。

#### 瓶颈3：单Socket写入串行化

brpc的Socket::Write使用lock-free队列，但同一Socket同一时刻只有1个KeepWrite线程在写。16个线程的请求都通过同一个Socket写入，写入是串行的。

#### 瓶颈4：每次请求的堆分配

```cpp
RespClosure* closure = new RespClosure;        // malloc
closure->resp = new test::PerfTestResponse();   // malloc
closure->cntl = new brpc::Controller();          // malloc（重量级）
```

每次请求3次malloc，每次响应3次free，高QPS下成为瓶颈。

## 二、优化方案

### 2.1 核心改动：持续发射模式

**原模型**：
```
RunTest: 发8个请求 → 退出
HandleResponse: 收1个 → 发1个（被动维持深度）
```

**新模型**：
```
RunTest: 发queue_depth个请求 → 持续循环检查
  → 如果inflight < queue_depth → 立即补充发送
HandleResponse: 收1个 → 发1个（维持深度）
  → inflight计数-1，通知RunTest可以补充
```

### 2.2 关键代码改动

1. **新增g_inflight原子计数器**：跟踪全局in-flight请求数
2. **RunTest持续循环**：不停检查inflight是否低于queue_depth，低于则补充
3. **HandleResponse正确维护inflight**：失败/超时/结束时递减inflight
4. **SendRequest中检查_stop**：避免在停止后继续发送

### 2.3 改动前后对比

| 特性 | 原模型 | 新模型 |
|------|--------|--------|
| 发射驱动 | 仅HandleResponse回调 | RunTest主循环 + HandleResponse |
| inflight恢复 | 被动（等回调） | 主动（主循环持续补充） |
| CPU空闲时 | inflight下降无法恢复 | 主循环立即补充到queue_depth |
| 线程利用率 | RunTest线程退出浪费 | RunTest线程持续工作 |

## 三、测试建议

增大queue_depth参数以充分利用CPU：

```bash
# 原参数
./client --thread_num=16 --queue_depth=8

# 建议参数
./client --thread_num=16 --queue_depth=64
./client --thread_num=16 --queue_depth=128
./client --thread_num=16 --queue_depth=256
```
