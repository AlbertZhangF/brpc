# TaskGroup 数据结构详解

## 概述

`TaskGroup` 是 bthread 的核心数据结构，每个 pthread（工作线程）对应一个 `TaskGroup` 实例。它负责管理该工作线程上的所有 bthread 任务，包括任务调度、队列管理、统计信息等。

## 类定义位置

- 头文件：`/home/zfz/code/brpc/apache-brpc-1.15.0-src/src/bthread/task_group.h`
- 实现文件：`/home/zfz/code/brpc/apache-brpc-1.15.0-src/src/bthread/task_group.cpp`

## 核心数据成员

### 1. 当前任务管理

#### `TaskMeta* _cur_meta`
- **类型**：`TaskMeta*`
- **默认值**：`NULL`
- **说明**：指向当前正在执行的 bthread 的元数据结构
- **作用**：用于快速访问当前任务的状态、栈、统计信息等
- **访问方法**：`current_task()` 函数返回此指针

### 2. 控制器关联

#### `TaskControl* _control`
- **类型**：`TaskControl*`
- **默认值**：`NULL`
- **说明**：指向所属的 `TaskControl` 对象
- **作用**：`TaskControl` 管理所有的 `TaskGroup`，通过此指针可以访问全局控制功能
- **访问方法**：`control()` 函数返回此指针

### 3. 信号与通知管理

#### `int _num_nosignal`
- **类型**：`int`
- **默认值**：`0`
- **说明**：本地队列中未发送信号的任务数量
- **作用**：用于批量任务提交优化，减少频繁的信号唤醒开销

#### `int _nsignaled`
- **类型**：`int`
- **默认值**：`0`
- **说明**：已发送信号的任务数量
- **作用**：与 `_`num_nosignal 配合使用，实现批量信号管理

#### `int _remote_num_nosignal`
- **类型**：`int`
- **默认值**：`0`
- **说明**：远程队列中未发送信号的任务数量
- **作用**：用于远程任务提交的批量优化

#### `int _remote_nsignaled`
- **类型**：`int`
- **默认值**：`0`
- **说明**：远程队列已发送信号的任务数量
- **作用**：与 `_remote_num_nosignal` 配合使用

### 4. CPU 时间统计

#### `AtomicCPUTimeStat _cpu_time_stat`
- **类型**：`AtomicCPUTimeStat`
- **说明**：原子操作的 CPU 时间统计信息
- **作用**：记录该 TaskGroup 的累计 CPU 时间和最后调度时间
- **内部结构**：
  - `int64_t _last_run_ns_and_type`：最后调度时间（低63位）和任务类型（最高位）
  - `int64_t _cumulated_cputime_ns`：累计 CPU 时间（纳秒）

#### `int64_t _last_cpu_clock_ns`
- **类型**：`int64_t`
- **默认值**：`0`
- **说明**最后记录的线程 CPU 时钟时间
- **作用**：用于计算当前任务的 CPU 使用时间
- **访问方法**：`current_task_cpu_clock_ns()` 函数使用此值

### 5. 调度统计

#### `size_t _nswitch`
- **类型**：`size_t`
- **默认值**：`0`
- **说明**：任务切换次数计数器
- **作用**：统计该 TaskGroup 中发生的任务切换次数，用于性能分析

### 6. 上下文恢复机制

#### `RemainedFn _last_context_remained`
- **类型**：`RemainedFn`（函数指针类型）
- **默认值**：`NULL`
- **说明**：上下文恢复回调函数指针
- **作用**：在下一次运行的 bthread 开始时执行的回调函数

#### `void* _last_context_remained_arg`
- **类型**：`void*`
- **默认值**：`NULL`
- **说明**：上下文恢复回调函数的参数
- **作用**：传递给 `_last_context_remained` 回调函数的参数

### 7. 停车场（ParkingLot）机制

#### `ParkingLot* _pl`
- **类型**：`ParkingLot*`
- **默认值**：`NULL`
- **说明**：指向 ParkingLot 对象的指针
- **作用**：ParkingLot 用于管理线程的等待和唤醒机制，提高并发性能

#### `ParkingLot::State _last_pl_state`
- **类型**：`ParkingLot::State`
- **条件编译**：`#ifndef BTHREAD_DONT_SAVE_PARKING_STATE`
- **说明**：保存的 ParkingLot 状态
- **作用**：用于任务窃取时保存和恢复停车场状态

### 8. 工作窃取机制

#### `size_t _steal_seed`
- **类型**：`size_t`
- **默认值**：`butil::fast_rand()`
- **说明**：工作窃取的随机种子
- **作用**：用于随机选择要窃取任务的 TaskGroup，实现负载均衡

#### `size_t _steal_offset`
- **类型**：`size_t`
- **默认值**：`prime_offset(_steal_seed)`
- **说明**：工作窃取的偏移量（基于素数计算）
- **作用**：配合 `_steal_seed` 实现更好的随机分布

### 9. 主线程栈管理

#### `ContextualStack* _main_stack`
- **类型**：`ContextualStack*`
- **默认值**：`NULL`
- **说明**：主线程的栈指针
- **作用**：指向该 pthread 的原始栈，用于判断当前是否在 pthread 模式下运行

#### `bthread_t _main_tid`
- **类型**：`bthread_t`
- **默认值**：`INVALID_BTHREAD`
- **说明**：主线程的 bthread ID
- **作用**：标识该 TaskGroup 的主任务 ID
- **访问方法**：`main_tid()` 函数返回此值

### 10. 任务队列

#### `WorkStealingQueue<bthread_t> _rq`
- **类型**：`WorkStealingQueue<bthread_t>`
- **说明**：本地运行队列（工作窃取队列）
- **作用**：存储该 TaskGroup 本地待执行的 bthread ID
- **特性**：
  - 支持无锁操作
  - 支持工作窃取（其他 TaskGroup 可以从此队列窃取任务）
  - 容量由 `FLAGS_task_group_runqueue_capacity` 控制（默认 4096）
- **访问方法**：`rq_size()` 返回队列大小

#### `RemoteTaskQueue _remote_rq`
- **类型**：`RemoteTaskQueue`
- **说明**：远程任务队列
- **作用**：存储从其他线程提交到此 TaskGroup 的任务
- **特性**：
  - 使用互斥锁保护
  - 用于非工作线程提交任务
  - 有界队列，容量与本地队列相同

### 11. 调度保护

#### `int _sched_recursive_guard`
- **类型**：`int`
- **默认值**：`0`
- **说明**：调度递归保护计数器
- **作用**：防止调度函数的递归调用，避免栈溢出

### 12. 标签管理

#### `bthread_tag_t _tag`
- **类型**：`bthread_tag_t`
- **默认值**：`BTHREAD_TAG_DEFAULT`
- **说明**：TaskGroup 的标签
- **作用**：支持按标签分组管理 TaskGroup，实现不同优先级或隔离的任务组
- **访问方法**：`tag()` 函数返回此值

### 13. 线程标识

#### `pthread_t _tid`
- **类型**：`pthread_t`
- **默认值**：`{}`
- **说明**：对应的 pthread 线程 ID
- **作用**：标识该 TaskGroup 所属的工作线程
- **访问方法**：`tid()` 函数返回此值

## 内部类

### AtomicInteger128

```cpp
class AtomicInteger128 {
public:
    struct BAIDU_CACHELINE_ALIGNMENT Value {
        int64_t v1;
        int64_t v2;
    };
private:
    Value _value{};
    FastPthreadMutex _mutex;
};
```

- **作用**：提供 128 位原子整数操作
- **平台支持**：
  - x86_64：使用 SSE 指令
  - ARM NEON：使用 neon 指令
  - 其他平台：使用互斥锁保护

### CPUTimeStat

```cpp
class CPUTimeStat {
private:
    int64_t _last_run_ns_and_type;  // 最后调度时间和任务类型
    int64_t _cumulated_cputime_ns;   // 累计 CPU 时间
};
```

- **作用**：封装 CPU 时间统计信息
- **位布局**：
  - `_last_run_ns_and_type`：最高位表示任务类型（主任务=1，普通任务=0），低63位为最后调度时间

### AtomicCPUTimeStat

```cpp
class AtomicCPUTimeStat {
private:
    AtomicInteger128 _cpu_time_stat;
};
```

- **作用**：提供原子操作的 CPU 时间统计

## 主要方法

### 任务创建
- `start_foreground()`：创建前台任务，立即切换执行
- `start_background<REMOTE>()`：创建后台任务，放入队列等待调度

### 任务调度
- `sched()`：挂起当前任务，执行下一个任务
- `sched_to()`：直接切换到指定任务
- `exchange()`：交换当前任务和指定任务
- `yield()`：让出 CPU，执行其他任务

### 队列操作
- `ready_to_run()`：将任务放入本地运行队列
- `ready_to_run_remote()`：将任务放入远程运行队列
- `ready_to_run_general()`：自动判断本地或远程并放入相应队列
- `steal_task()`：从其他 TaskGroup 窃取任务

### 任务等待
- `usleep()`：挂起当前任务指定时间
- `join()`：等待指定任务结束

### 统计信息
- `current_uptime_ns()`：当前任务运行时间
- `cumulated_cputime_ns()`：TaskGroup 累计 CPU 时间
- `current_task_cpu_clock_ns()`：当前任务 CPU 时钟时间

## 性能优化要点

### 1. 无锁队列
- 使用 `WorkStealingQueue` 实现无锁本地队列
- 减少锁竞争，提高并发性能

### 2. 批量信号
- 通过 `_num_nosignal` 和 `_nsignaled` 实现批量信号管理
- 减少频繁的线程唤醒开销

### 3. 工作窃取
- 随机选择目标 TaskGroup 窃取任务
- 实现负载均衡，避免某些线程空闲而其他线程过载

### 4. 线程局部存储
- 每个线程独立的 TaskGroup
- 减少线程间数据共享和竞争

### 5. 原子操作
- 使用 `AtomicInteger128` 实现 CPU 时间统计的原子更新
- 避免统计信息的不一致

## 配置参数

### FLAGS_task_group_runqueue_capacity
- **默认值**：4096
- **说明**：每个 TaskGroup 运行队列的容量
- **影响**：决定了本地队列和远程队列的大小

### FLAGS_task_group_ntags
- **默认值**：1
- **说明**：TaskGroup 的标签数量
- **影响**：决定了不同标签的 TaskGroup 数量

### FLAGS_task_group_delete_delay
- **默认值**：1（秒）
- **说明**：TaskGroup 删除延迟时间
- **影响**：避免删除时的竞态条件

## 潜在性能问题

### 1. 队列容量限制
- 当任务数量超过队列容量时，push 操作会失败并重试
- 高负载下可能导致性能下降

### 2. 工作窃取开销
- 窃取操作需要访问其他 TaskGroup 的队列
- 可能产生缓存未命中和锁竞争

### 3. 远程队列锁队
- `RemoteTaskQueue` 使用互斥锁保护
- 高并发下可能成为瓶颈

### 4. 统计开销
- 频繁的 CPU 时间统计可能影响性能
- 可通过配置关闭部分统计功能

## 总结

`TaskGroup` 是 bthread M:N 调度模型的核心组件，通过精心设计的数据结构和算法实现了高效的任务调度：

1. **本地优先**：优先执行本地队列中的任务，减少跨线程通信
2. **工作窃取**：通过随机窃取实现负载均衡
3. **批量优化**：批量信号管理减少唤醒开销
4. **无锁设计**：关键路径使用无锁队列，提高并发性能

理解 `TaskGroup` 的数据结构对于优化 bthread 应用性能、诊断性能问题具有重要意义。
