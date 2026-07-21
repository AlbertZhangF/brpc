# Findings & Decisions

## Requirements
- 中文完整资料，结合当前仓库代码分析。
- 覆盖整体组件、各组件原理与技术细节。
- 覆盖整体运行时序和流程，逐部分解释。
- 以 `example/rdma_performance` 为例跟踪端到端数据变化、bthread 创建和调度。
- 图全部使用 PlantUML。
- 补充对理解、调优和排障重要的内容。
- 2026-07-15 新增：进一步细化 bthread 创建与调度、IOBuf 设计原理与工作机制、TLS 实现和生命周期，并继续使用 PlantUML 与当前源码锚点。
- 2026-07-15 新增：增加真实微服务业务实践章节，覆盖常用接口、生命周期、调用模式、治理能力和运维观测。
- 2026-07-15 新增：以听众视角形成全面提问清单，覆盖框架组件、源码实现、端到端运行时、bthread、IOBuf、TLS、三类 transport、真实微服务和故障性能追问。
- 2026-07-17 新增：产出一张独立 PlantUML 全量运行总图，覆盖所有核心运行时组件及 bthread、IOBuf、Socket/epoll、transport、可靠性和生命周期完整流程。

## Research Findings
- 微服务实践章节可用仓库自带 `example/echo_c++` 作为最小 protobuf RPC 骨架，用 `example/auto_concurrency_limiter` 说明过载保护，用 `example/http_c++` 说明 HTTP 接入；治理/观测可锚定 `docs/cn/client.md`、`server.md`、`load_balancing.md`、`circuit_breaker.md`、`bvar.md`、`rpcz.md`、`builtin_service.md`、`thread_local.md`。
- 章节应从应用 API 面而非内部实现展开：日常代码主要接触 proto 生成的 Service/Stub、`Server`、`Channel`、`Controller`、Closure、IOBuf attachment、bvar 和少量 bthread；`Socket`、`InputMessenger`、`EventDispatcher`、`TaskGroup` 通常不由业务直接调用。
- `example/echo_c++` 给出最典型生命周期：proto 开启 `cc_generic_services` 定义 service；服务端实现生成的 Service，`ClosureGuard` 确保 done，`Server::AddService -> Start -> RunUntilAskedToQuit`；客户端进程级共享 `Channel`/Stub，每次 RPC 新建 request/response/Controller，`done=NULL` 同步调用并检查 `Failed/ErrorText`。
- `Channel` 和生成 Stub 是 thread-safe、适合长生命周期共享的昂贵治理/连接入口；`Controller` 非可复制且表达一次 RPC，不能跨并发调用复用。常见设置是 Channel 级 protocol/connection_type/timeout/max_retry/load balancer，单次请求再用 Controller 覆盖 timeout/retry/log_id、attachment、checksum/compression。
- 服务实现中 `done` 是响应完成权：同步完成用 `ClosureGuard`；若转交业务线程池/bthread 异步处理，必须 `done_guard.release()` 并让 request/response/Controller 的框架生命周期延续到最终 `done->Run()`。
- `ChannelOptions` 的生产常用面包括 connect/RPC timeout、backup request、max retry、circuit breaker、protocol、connection type、naming filter、health check、auth/SSL 和 connection_group；这些在 Init 后固定。每请求变化的覆盖项放 `Controller`，例如 timeout/retry/backup/request_code/log_id。
- `ServerOptions` 的生产常用面包括 idle timeout、认证/拦截器、worker concurrency hint、server/method max concurrency、session/thread-local factory、独立 internal_port/builtin services、SSL 和 transport。`num_threads` 是共享 bthread worker 数提示，不等于该 Server 独占线程池；`max_concurrency` 才是请求在途处理上限。
- 服务集群 Channel 应作为长生命周期对象：`Init(naming_service_url, load_balancer, options)` 后由 naming service 持续更新节点并由 LB 选址。仓库支持 file/list/http/consul/nacos 等 scheme，具体部署接入可扩展 NamingService；一致性哈希类 LB 需设置 Controller request_code。
- 当前 `channel.h` 的默认 `timeout_ms` 注释为 500ms，而 `docs/cn/client.md` 仍写 1 秒，存在文档漂移；新增章节不硬编码默认值，要求生产配置显式设置并以当前头文件/版本为准。
- 真实服务需要同时配置 worker 容量与请求过载保护：`ServerOptions::num_threads` 影响进程共享 worker 基线，`max_concurrency`/`Server::MaxConcurrencyOf()` 限制 server/方法并发，达到限制时框架以 ELIMIT 拒绝而不进入业务方法；自动并发限制器示例也通过 method max concurrency 配置。
- 生产观测的高频接口是业务侧 `bvar::Adder`、`LatencyRecorder` 等，以及框架内置 `/status`、`/vars`、`/connections`、`/flags`、`/rpcz`、`/health`。对外服务宜用 `ServerOptions::internal_port` 将内置服务隔离到内网管理端口，而不是直接暴露在业务公网端口。
- rpcz 适合请求级阶段诊断和 TRACEPRINTF annotation，bvar 适合持续聚合计数/延时/QPS；两者不是日志或分布式 tracing 的完全替代品。只有 client 的进程可启动 DummyServer 暴露 bvar/内置页。
- client 常见调用有三种：同步 RPC（request/response/Controller 可放栈上）、异步 RPC（response/Controller/done 生命周期跨过 CallMethod，通常堆分配并在 callback 删除）、半同步 fan-out（多个 Controller 先保存 call_id/用 DoNothing 发起，再逐个 `brpc::Join`）。call_id 必须在发起 RPC 前取得，取消也用预先保存的 id。
- 同步 RPC 不应在持有 pthread mutex 时调用，否则 bthread 切换与锁依赖可能死锁；异步回调不应假设与发起处同一 worker/pthread。回调做长计算虽不会阻塞原发起者，但仍消耗 worker，重活应有独立限流/队列。
- server-side done 与 client-side done 方向相反：Server 的 done 由框架创建，业务调用它触发回包；Client 的 done 由用户创建，框架在成功/失败/超时完成时调用。生产文档必须显式区分。
- 异步 Service 若需要跨出 Service 回调保存请求上下文，应使用 session-local 或自己的 closure/context；`brpc::thread_local_data()` 只在 Service 回调执行范围有效，不适合作为异步 done 的请求上下文。
- HTTP 服务仍复用 Service/Controller/ClosureGuard；业务通过 `http_request().uri()/method`、`http_response()` 和 request/response attachment 处理 HTTP 元数据与 body。HTTP client 常直接 `Channel::CallMethod(NULL, ...)`，而不是 protobuf Stub。
- ProgressiveAttachment、Streaming RPC、Parallel/Selective/Partition Channel 属于按需高级能力：大文件/流式消息用 Progressive/StreamCreate/StreamWrite/StreamClose；多下游聚合或跨集群分流可用组合 Channel，但大多数普通 unary 微服务不需要直接使用。
- 实际业务最常见的是服务 A 同时作为 Server 和多个下游的 Client：进程启动阶段初始化一个 Server 和若干长生命周期 Channel，单个请求 bthread 在 Service 回调中做校验/本地逻辑并同步或异步 fan-out，下游结果汇总后调用 server done 回包。
- 深化章节应在现有第 6 节后扩展，并在 Socket/RDMA 之前增加独立 IOBuf 与 TLS 章节；当前文档已有 16 个 PlantUML 图，可在不改原有端到端结论的前提下补充对象结构、调度状态机和 TLS 切换图。
- bthread 深化入口集中在 `src/bthread/bthread.cpp`、`task_group.cpp`、`task_control.cpp`、`task_meta.h`、`key.cpp` 和 `stack.cpp`；创建不是创建 pthread，而是从 ResourcePool 取得 `TaskMeta`，按调用上下文进入本地或 remote run queue。
- IOBuf 核心是 `src/butil/iobuf.h` 的 `BlockRef{offset,length,block}` 队列；2 个以内引用内嵌在 `SmallView`，更多引用使用环形 `BigView`。`src/butil/iobuf.cpp` 还维护 pthread 级 TLS block cache，这与 bthread key TLS 是两套不同机制，文档必须明确区分。
- bthread TLS 的公开接口和实现位于 `src/bthread/key.cpp`/`unstable.h`：每个逻辑 bthread 的 `TaskMeta::local_storage.keytable` 随调度切换映射到 worker pthread 上的 `tls_bls`，keytable 可由 pool 缓存复用，但析构语义仍在任务结束/归还时执行。
- `TaskMeta` 同时保存 `tid`、用户函数/参数、栈、attr、统计和 `LocalStorage`；`bthread_t` 由 ResourcePool slot 与 `version_butex` 版本组合，任务结束先清 TLS，再增加版本并唤醒 joiner，旧 tid 因版本不匹配而失效。
- 每个 worker 的 `TaskGroup::run_main_task()` 在 ParkingLot 等待，取到 tid 后 `sched_to()`；如果目标是首次执行且还没有栈，`task_runner()` 执行用户函数，结束后完成 TLS/trace/version 清理并直接寻找下一个任务，避免无谓回主栈。
- `start_background<REMOTE>` 两条路径初始化字段完全相同，差异只在入队：非 worker pthread 或跨组入口走带锁 `_remote_rq`，当前 worker 走自己的 work-stealing `_rq`；`BTHREAD_NOSIGNAL` 仅延迟 ParkingLot 唤醒，不延迟入队。
- `start_foreground`（urgent）在普通 bthread 内把当前任务安排回队，并立刻 `sched_to` 新任务；在 pthread-stack 任务中不会立即切走，只按 background 语义入队。结束调度可把当前结束任务的栈直接转给同栈类型的下一个任务。
- 调度取任务顺序是当前组本地队列，然后本组 remote queue/同 tag priority queue，再扫描同 tag 其他组的本地与 remote 队列；tag 是硬调度域，跨 tag 不窃取。
- `sched_to()` 显式保存每个 bthread 的 errno、CPU/切换统计与 `tls_bls`，再 `jump_stack()`；恢复时重新读取 `tls_task_group`，因为同一个 bthread 可能已迁移到另一个 worker/TaskGroup。
- 非 worker pthread 创建 NOSIGNAL 任务时用 `tls_task_group_nosignal` 固定同一目标组，以便批量进入同一 remote queue；`bthread_flush()` 根据是否处于 worker，分别 flush 本地或该 remote queue。
- `signal_task()` 为避免过度唤醒，一次最多 signal 2 个 parked worker；启用 min-concurrency 模式且仍无人可唤醒时，才可能动态增加一个 worker。
- bthread 阻塞并不占住 worker：`bthread_usleep()` 先设置 remained callback，再调度走；切走后才向全局 TimerThread 注册唤醒事件，超时回调从非 worker 线程把原 TaskMeta 放回 remote queue。`bthread_join()` 则在版本 butex 上等待，结束任务递增版本并 wake。
- `bthread_stop/interrupt` 不强杀用户函数，而是标记 `stop/interrupted`，尝试从 sleep timer 或 butex wait 队列移除并重新入队；如果任务当前未阻塞，中断状态保留到下一次可中断阻塞点。
- `IOBuf` 对象本体用同尺寸 union 表示两种视图：最多 2 个 `BlockRef` 时完全内嵌，超过 2 个后使用初始容量 32 的环形 `BigView`，避免小消息额外堆分配，同时让首尾切分为 O(1) 引用操作。
- `append(IOBuf)`/`append_to(IOBuf*)` 共享 payload 并增加 Block 引用；`append(void*, n)`、`copy_to(void*)` 和转 `std::string` 才复制字节。IOBuf 仅 thread-compatible，同一个实例并发修改不安全。
- 普通 IOBuf block 由可替换的 `blockmem_allocate/deallocate` 分配，默认 malloc/free；每个 OS pthread 用 `static __thread TLSData` 缓存可继续写或可复用的 block 链，并在 pthread 退出时清理。bthread 在不同 worker 间迁移时不会携带这份 cache，因此它是“当前 worker 的分配缓存”，不是 bthread-local 状态。
- 相邻且属于同一 Block 的连续 BlockRef 会在 push 时合并，降低 iovec/引用计数开销；第 3 个非连续 ref 才从 SmallView 升级为 BigView，ref 数降到 2 时又降级回 SmallView。
- `cutn(IOBuf*)` 对完整 ref 直接转移所有权，对半截只新建共享 ref 并推进原 ref 的 offset；`cutn(void*)` 才逐段 memcpy。可移动 append 在目标为空时直接 swap，否则 move ref 并清空源，不增加 payload copy。
- `IOPortal` 预先把多个 block 的空闲尾部组成 iovec，让 `readv` 直接把内核数据写进最终 IOBuf block；输出路径把 BlockRef 映射为 iovec 后 `writev`，成功多少就 `pop_front` 多少。这里省掉的是用户态 flatten/copy，普通 TCP 的内核拷贝仍存在。
- `IOBufAsZeroCopyOutputStream::Next()` 把可写 block 尾部直接交给 protobuf，`BackUp()` 收回未使用尾部；这避免 protobuf 先序列化到连续中间 string 再复制进 IOBuf，但元数据和 ref 管理仍有成本。
- 默认 block 总分配尺寸为 8192 字节（包含 Block 头）；Block 保存原子引用计数、flags、size/cap、portal 链或 user-data meta 和 data 指针。普通块引用归零后经可替换 allocator 释放，user-data 块则调用用户 deleter。
- 每个 pthread 的 IOBuf cache 软上限为 8 个非满 block；开启 IOBuf profiler 时上限变为 0，以便采样准确。输出 iovec 上限设为 256，源码明确是为了避免在小 bthread 栈上按系统 IOV_MAX=1024 分配过大栈数组。
- `append_user_data` 可把外部内存零复制包装成特殊 Block，但调用方必须通过 deleter 转移/管理生命周期；这不是让任意裸指针自动变安全。
- bthread KeyTable 使用稀疏两级表：31 个一级指针，每个二级表 32 个槽，最多 992 个 key；二级表按需分配，key 和槽都带 version，删除并复用 key index 后旧 key 不会错误读到新值。
- bthread TLS 析构仿照 pthread，最多执行 `PTHREAD_DESTRUCTOR_ITERATIONS` 轮，因为析构函数可能再次 setspecific。每轮先清槽再调用 dtor，避免同一个旧值重复析构。
- `bthread_getspecific()` 在有 server keytable pool 时可以惰性借用预热的 KeyTable；`setspecific()` 若没有表则直接创建，保证不会因一次 borrow 失败后另一处成功覆盖而泄漏旧对象。非 bthread pthread 创建的表注册 thread_atexit，bthread 的表在任务结束时归还/析构。
- keytable pool 采用“两级缓存”：优先从当前 worker pthread 的 `ThreadLocal<KeyTableList>` 借用；不足时从全局 free list 一批搬入；当前列表过长时再把一半搬回全局。归还到 pool 不析构内部 TLS 对象，因此数据可供后续 RPC bthread 复用；只有无 pool、pool 已销毁或 Server Join 销毁 pool 时才执行析构。
- Server 启动总是创建 keytable pool；若配置 `thread_local_data_factory`，再创建带 `DestroyServerTLS` 的 bthread key，并可按 `reserved_thread_local_data` 预建 KeyTable+业务对象。接收连接与 transport 创建的消息处理 bthread 都携带该 pool。
- 协议处理进入服务方法前调用 `bthread_assign_data(&server->thread_local_options())` 标识当前 Server；`brpc::thread_local_data()` 先据此取得 TLS key/factory，再从当前 KeyTable 取对象，缺失时惰性创建。这个“thread local”是可跨 RPC bthread 复用的对象池语义，不等于固定绑定某个 pthread，也不等于每个 RPC 独占。
- `tls_bls` 的实现变量本身是 worker pthread TLS，但只是当前 bthread 的运行时镜像；`TaskMeta::local_storage` 是挂在逻辑 bthread 上的持久副本。运行期间应以 `tls_bls` 为准，切出时才回写 TaskMeta。
- TCP、RDMA 和 io_uring 的 `QueueMessage()` 都把 `Socket::_keytable_pool` 写入消息处理 bthread 的 attr；Socket input event 创建 consumer bthread 时也传递该 pool，因此服务端请求路径能统一复用 Server 的 TLS 表。
- 用户代码若直接使用 C/C++ `__thread`/`thread_local`，变量绑定 worker pthread：同一 bthread 迁移后可能看到另一个值，不同 bthread 复用同一 worker 又会互相看见。需要逻辑 bthread 隔离时必须使用 `bthread_key_*` 或上层 `brpc::thread_local_data()` 机制。
- 扩写后主文档为 1774 行、7789 词，PlantUML 从 16 增至 25 个，Markdown fence 为 64 个；start/end 和 fence 数量均配对，`git diff --check` 无输出。环境仍无 PlantUML 可执行文件，需继续做语法级静态检查。
- 新增章节的主要源码路径全部存在，一级章节从 0 到 16 连续。校验观测项时发现 IOBuf BigView 的实际 bvar 名是 `iobuf_newbigview_second`，TLS cache 阈值项是 `iobuf_block_count_hit_tls_threshold`，已按 `src/brpc/global.cpp` 修正文档。
- 全文当前引用 31 个 `src/`/`example/` 路径，均存在；新增关键入口锚点逐项匹配。两处锚点原先指向模板/返回类型起始行而非函数名所在行，已从 `bthread.cpp:268`、`task_group.cpp:563` 分别精确调整为 269、564。
- 逐段复核新增三章后，确认 25 个 PlantUML fence 内都恰有一组 `@startuml/@enduml`；为降低未实际渲染时的语法风险，已把调度选择图中的 repeat 结构简化为线性回环说明。
- 现有 `rdma_performance` 第 11.8 节已列出 client/server bthread 创建点；为满足“更详细的创建和调度”，还应补一段把示例的 `RunTest -> SendRequest -> ProcessInputMessage -> callback -> SendRequest` 映射到新第 6 章的 remote queue、本地执行、NOSIGNAL/flush 和回调内联语义。
- 示例 `Test()` 运行在 main pthread，创建 `RunTest` 因此走 `start_from_non_worker -> remote queue`；`RunTest` 仅同步发出初始 `queue_depth` 个异步调用后退出。之后每条 lane 没有常驻发送 bthread，而是 response 的 `ProcessInputMessage`/input consumer 执行 `HandleResponse()`，回调在同一逻辑任务中再次 `SendRequest()`。
- 当前工作区为 `/home/zfz/code/brpc/brpc_github/brpc`，分支 `master`，相对 `origin/master` ahead 1。
- 根目录原先不存在 `task_plan.md`、`findings.md`、`progress.md`，本任务新建。
- 历史索引只用于定位：当前 checkout 曾增加独立 io_uring transport；由于实现活跃变化，必须重新核验当前代码。
- 核心源码分层入口已定位：`Server`/`Acceptor`、`Channel`、`InputMessenger`、`Socket`、`EventDispatcher`、协议 policy，以及 `TaskControl`/`TaskGroup`。
- `rdma_performance` 的 proto 只有 `echo_attachment` 布尔请求字段和 `cpu_usage` 字符串响应字段；大块负载不在 protobuf message 中，而在 `Controller::request_attachment()` 的 `IOBuf` 中。
- 客户端默认 `use_rdma=true`、`connection_type=single`、`protocol=baidu_std`；每个 `PerformanceTest` 独占一个 `Channel`，启动 `queue_depth` 条异步调用链，响应回调再次调用 `SendRequest()`，形成固定在途数的闭环负载。
- 示例显式创建的 bthread：可选 token 生成器 1 个、每个测试实例 1 个 `RunTest`、测试结束时每实例 1 个 `DeleteTest`。RPC 内部还会创建/唤醒框架 bthread，需在核心链路阶段继续确认。
- 服务端业务方法用 `ClosureGuard` 保证返回；按 `echo_attachment` 将请求附件的 IOBuf 引用追加到响应附件，同时低频写入 CPU 使用率字符串。
- `socket_mode` 在两端都由 `use_rdma` 决定；即使选择 RDMA，建立连接/握手仍涉及 TCP，并存在协商失败回退 TCP 的实现分支。
- `Server::StartInternal` 先初始化 transport context、TLS/keytable、并发限制器和监听 fd，再构造 `Acceptor`；监听 fd 也被包装成 `Socket`，其 edge-trigger 回调是 `Acceptor::OnNewConnections`。
- epoll `EventDispatcher` 自身是带 `BTHREAD_NEVER_QUIT | BTHREAD_GLOBAL_PRIORITY` 的 bthread；`epoll_wait` 得到事件后调用 input/output callback 分派函数。监听事件最终在 consumer bthread 中循环 `accept` 到 EAGAIN。
- 每个已接收连接由 `Socket::Create` 建立，并继承 server 的 `socket_mode`、`bthread_tag`、keytable pool；代码明确承认 Socket 创建后读消息可能先于加入 Acceptor 连接表完成。
- TCP 基线读路径：`Socket::DoRead` 追加到 `_read_buf` -> `InputMessenger::CutInputMessage` 做协议识别/切帧 -> `InputMessageBase` 绑定 process/arg/socket -> transport 的 `QueueMessage` 决定另起 bthread或延迟内联执行。
- 同一次读取若切出多条消息，前面的消息批量创建 NOSIGNAL bthread，最后一条通常由 `InputMessageClosure` 析构时在当前 consumer bthread 内处理；最后统一 `bthread_flush()`。RDMA 模式会在函数尾强制把最后一条也排到新 bthread，避免业务同步原语阻塞 RDMA polling bthread。
- 读包大小不是固定值：按连接历史平均消息大小的 16 倍估计，并限制在 `MIN_ONCE_READ` 与 `MAX_ONCE_READ` 之间。
- `Channel::InitSingle` 不为每个 Channel 无条件创建新连接，而是用 endpoint + `ChannelSignature` 插入全局 `SocketMap`；相同签名的 Channel 可共享主 Socket。命名服务模式则创建 `LoadBalancerWithNaming` 并把 transport/socket 选项传给 NamingServiceThread。
- `Channel::CallMethod` 的阶段顺序是：锁定/扩展 correlation id 版本范围 -> 合并 Channel/Controller 选项 -> 序列化 protobuf 到 `_request_buf` -> 注册 timeout/backup timer -> `Controller::IssueRPC` -> 同步调用 `Join` 或异步立即返回。
- `Controller::call_id()` 基于 `bthread_id_create2` 创建带错误回调的相关 ID；首发/重试使用版本化 ID，迟到的旧响应因此可被识别和忽略。
- `IssueRPC` 先直连取 Socket 或经负载均衡选 Socket，再按 single/pooled/short 连接模型选择实际 sending socket，随后协议 `pack_request` 将 meta、序列化 body、attachment 封装为 IOBuf/SocketMessage，最后 `Socket::Write`。
- 同步 RPC 不是忙等：`Join(correlation_id)` 等待响应完成时销毁/唤醒 bthread_id；异步 RPC 的 `done` 通常在响应处理执行流上调用，若配置/死锁规避需要则由 `RunEndRPC` 新建 bthread 执行。
- timeout timer 触发 `bthread_id_error(..., ERPCTIMEDOUT)`；错误回调可新建 bthread 跑 retry policy 和 `OnVersionedRPCReturned`，最终 `EndRPC` 删除 timer、回收/失效连接、反馈 LB/熔断器并运行 done 或唤醒同步调用方。
- `baidu_std` 线格式已确认：12 字节固定头 `"PRPC" + body_size(u32) + meta_size(u32)`，之后依次是序列化 `RpcMeta`、protobuf body、attachment；`attachment_size` 只记录在 meta，接收端据此从 payload 尾部切分。
- 请求序列化使用 `IOBufAsZeroCopyOutputStream`，协议层把已有 IOBuf 片段 append 到最终 packet，通常是 block 引用拼接而非把附件复制成连续大缓冲；是否真正零拷贝到网卡取决于 transport 与内存注册状态。
- 服务端 `ProcessRpcRequest` 解析 meta，创建 server-side `Controller`，做全局/方法并发限制、服务和方法查找，把 payload 前部反序列化为请求 protobuf、尾部 swap 成请求附件，然后创建 `SendRpcResponse` closure 并调用生成的 protobuf service `CallMethod`。
- 业务 `done->Run()` 进入 `SendRpcResponse`：序列化响应 protobuf，构造响应 meta/同样的 PRPC 帧，移动追加响应附件，写回原 Socket；scope-exit 中释放并发额度、Controller 和请求/响应 message。
- 客户端 `ProcessRpcResponse` 用 meta.correlation_id 锁回原 Controller，按 attachment_size 切分 response body/attachment，反序列化 protobuf，随后 `OnResponse` 进入重试判定或最终 `EndRPC`。
- `GlobalInitializeOrDie` 将一个 `Protocol` 结构注册为 baidu_std，其函数表串起 parse、serialize request、pack request、process request、process response；brpc 的多协议支持本质上是同一 Socket/InputMessenger 管线上的可插拔函数表。
- transport 是 `Socket` 下的策略对象：`TransportFactory` 当前可按编译开关创建 TCP、RDMA、io_uring；协议层不重写传输逻辑，只通过统一的 `CutFromIOBuf(List)`、`WaitEpollOut`、`QueueMessage` 等接口工作。
- TCP 写快路径在调用 `Socket::Write` 的当前执行流中直接尝试一次；`_write_head.exchange` 串行化多生产者，若部分写/EAGAIN/SSL/后台写则创建 `KeepWrite` bthread，最多合并 256 个 WriteRequest 的 IOBuf 做批量 writev 风格输出。
- epoll input event 用 `_nevent` 合并重复通知；首次事件由 transport `ProcessEvent` 创建 urgent/background consumer bthread，设置 keytable pool 和当前 tag。它循环读到 EAGAIN，并用 `MoreReadEvents` 处理消费期间累积的新事件。
- `TcpTransport::QueueMessage` 以 `BTHREAD_NOSIGNAL` 创建 `ProcessInputMessage`，由 `InputMessenger` 最后一次 `bthread_flush()` 批量唤醒，降低逐消息 signal 开销。
- `RdmaTransport` 内含 `RdmaEndpoint` 和 TCP fallback transport：状态为 RDMA_ON/UNKNOWN 时走 endpoint，RDMA_OFF 时走 TCP；连接建立、协商失败和部分控制路径因此仍依赖 TCP fd/epoll。
- RDMA 非 polling 模式允许最后一条消息留在当前 input consumer bthread；polling 模式强制最后一条也创建 `ProcessInputMessage`，除非显式 `rdma_disable_bthread` 选择内联执行。
- bthread 第一次使用时创建进程级 `TaskControl`、全局 timer thread 和配置数量的 worker pthread；每个 worker pthread 创建一个 `TaskGroup`，含本地 work-stealing queue、remote queue、main/idle context，并按 tag 分区。
- `bthread_start_background` 只申请/初始化一个可复用 `TaskMeta`（fn/arg/attr/tid/TLS/stat），入本地或远端 run queue 并 signal parking lot；一般不会一对一创建 pthread。`start_urgent` 在 worker 内可立即把当前 bthread 回队并切换到新任务。
- worker 的 `run_main_task` 无任务时停在 ParkingLot，有任务时优先取本地队列，否则从同 tag 的其他 TaskGroup 本地/remote queue 或 priority queue 窃取；tag 既是 worker/队列隔离边界，也是 server/RDMA poller 的调度归属。
- 首次真正运行某 TaskMeta 时才取得/复用 ContextualStack；`sched_to` 保存 bthread 专属 errno/TLS/统计后通过 `jump_stack` 用户态切换。任务结束递增 version_butex 唤醒 joiner，析构 TLS，延迟归还 TaskMeta/stack，并直接选择下一个任务。
- `BTHREAD_NOSIGNAL` 只入队累计不唤醒 worker；`bthread_flush` 将累计任务一次 signal。`signal_task` 单次最多唤醒 2 个 parking worker，并可在 min-concurrency 模式下按需增 worker。
- `bthread_usleep` 在普通 bthread 中进入调度器 sleep/timer 路径并让出 worker；在 pthread-stack 任务或非 bthread pthread 中退化为系统 `usleep`。
- brpc 全局初始化由 `pthread_once` 保证，只做一次 SIGPIPE/SSL/常量初始化，并注册 naming service、load balancer、压缩、校验和、协议扩展；全局 client-side `InputMessenger` 随后按注册协议安装 response handler。
- 全局 EventDispatcher 也是惰性 `pthread_once` 创建，数量为 `task_group_ntags * event_dispatcher_num`；fd 通过 hash 选择 tag 内 dispatcher。`IOEventData` 保存 input/output callback，SocketId 作为用户数据避免裸 fd 复用误投。
- `SocketId` 是 32 位 version + 32 位 ResourcePool slot；`Socket::OnCreated` 先创建 IOEvent/transport，再初始化认证 ID、写队列、状态和 fd，`ResetFileDescriptor` 最后才设置 nonblocking/no-delay/socket options 并加入 dispatcher，避免回调看到半初始化对象。
- `IOBuf` 是小型 `BlockRef` 队列，每个引用包含 block/offset/length；`append(IOBuf)` 共享 block，`cutn(IOBuf*)` 移动完整 BlockRef 或拆引用，`pop_front` 仅推进 offset。只有 `append(void*, n)`、cut 到普通内存等边界发生实际字节复制。
- TCP 输出把多个 BlockRef 映射为 iovec；因此“应用到协议帧”通常是引用级拼装，“用户态到内核”仍有 writev copy。RDMA 注册块可进一步让 NIC 直接访问用户内存，但未注册或碎片条件下仍可能拷贝。
- 直连 Channel 的全局 SocketMap 按 key 复用主 Socket并计数；失效且无健康检查的 Socket 会被替换。命名服务更新将 endpoint 变成 SocketId 集合并通知 LB，调用时 LB 选择的是 Socket 抽象而不是每次创建 fd。
- RDMA 全局初始化动态加载 verbs、选择 HCA/port/GID、创建 Protection Domain，初始化并注册 RDMA block pool，把 IOBuf 默认 block allocator 替换为注册内存池，并把 HCA async fd 也包装成 Socket 纳入事件分发。
- RDMA 连接先用普通 TCP fd：client `RdmaConnect::StartConnect` 新建握手 bthread；双方交换含版本、block size、SQ/RQ size、LID/GID/QP number 的 hello，再由 client 发 ACK。版本/资源/QP bring-up 不满足时状态切到 RDMA_OFF/FALLBACK_TCP，保留同一 Socket 上的 TCP 数据路径。
- 每条 RDMA Socket 对应一个 `RdmaEndpoint`，内部有 RC QP、CQ（event 模式为 send/recv CQ + completion channel，polling 模式为共享 polling CQ）、预投递 recv buffers、send buffer 引用数组和窗口计数器。
- RDMA 发送 `CutFromIOBufList` 从 PRPC packet 的 BlockRef 生成 ibv_sge，要求每段内存有 lkey；每个 WR 最多受 max_sge 和对端 recv block size 限制，使用 `IBV_WR_SEND_WITH_IMM` 夹带接收窗口 ACK。`_sbuf` 持有被发送 BlockRef，直到 SEND completion 才 clear，保证 NIC DMA 期间内存有效。
- 为降 CQE/中断成本，发送不是每个 WR 都 `IBV_SEND_SIGNALED`/`SOLICITED`：累计到窗口比例、字节阈值或边界才请求完成/接收通知；CQE 的 wr_id 表示一次可回收多少连续 send buffer。
- RDMA RECV completion：大于阈值时把预注册 `_rbuf` 的 BlockRef `cutn` 到 Socket `_read_buf`（接收零拷贝），小消息复制；随后重新 post recv、回传窗口 ACK，并由 `PollCq` 把累计字节交回统一 `InputMessenger::ProcessNewMessage`。
- event 模式借 completion channel fd 进入 EventDispatcher；polling 模式按 bthread tag 创建 `rdma_poller_num` 个长期 bthread/PTHREAD 任务，MPSC op queue 动态维护 CQ SocketId 集合并循环 `PollCq`。业务消息必须从 poller 执行流隔离出去。
- `PollCq` 每次最多批量取 `rdma_cqe_poll_once` 个 WC，聚合所有 RECV 字节后只调用一次 `ProcessNewMessage`，避免每个 CQE 都 flush；event 模式按 recv CQ -> send CQ 顺序 drain、重新 arm one-shot notification 后再 poll 一次以封闭竞态。
- `rdma_performance` 的 warmup `stub.Test(..., done=NULL)` 是同步调用，它触发延迟 TCP connect + RDMA handshake；因此正式计时前连接/QP 已建立。每个 PerformanceTest 一个 Channel/主 Socket，默认 single connection。
- 每个 `RunTest` bthread 只负责发出最初 `queue_depth` 个异步 RPC 就结束；之后每条 logical lane 由 response callback 末尾递归 `SendRequest` 维持，因此总在途量约为 `thread_num * queue_depth`，不是“thread_num 个持续发送线程”。
- 非 polling RDMA 下，CQ event consumer bthread 可处理单条消息并直接执行最后一条业务/response callback；polling RDMA 下，每批最后消息也通过 NOSIGNAL `ProcessInputMessage` 新 bthread 隔离并 flush。若一次批次含多消息，除最后消息外本来就各有任务。
- `brpc::NewCallback` 只自删除生成的 Closure 包装器，不拥有绑定的裸指针。示例为每次请求 `new RespClosure`，`HandleResponse` 删除 Controller/Response 但未删除 RespClosure，本代码存在每请求一个小对象的泄漏，应在资料的基准注意事项中明确。
- 示例还有测量边界风险：`g_stop` 在一次 `Test` 后设 true 但多轮 sweep 前未重置，限 QPS token bthread 后续轮次会立即退出；同一 PerformanceTest 的 `_iterations`/`_stop` 会被多条并发 callback lane 访问却只是普通/volatile 字段，不提供原子同步。
- 文档代码基线固定为 `89b2765ca51659494b2b2f5028fdd87aa3df5992`（`feat: add io_uring transport layer support`，2026-06-01）；本分支不存在历史记录提到的 `docs/cn/iouring_implementation_analysis.md`，最终资料不引用该文件。
- 主文档初稿已覆盖 14 个章节和多类 PlantUML 图：组件/对象关系、Server 启动、客户端状态机、PRPC 布局、服务端时序、correlation 状态、bthread 架构/生命周期、RDMA 架构/握手、示例启动/lane/端到端时序、排障和总时序。
- 最终资料为 1218 行、5395 词，含 16 个 PlantUML 图；静态验证确认 44 个 Markdown fence 成对、28 个引用路径存在、关键行号锚点匹配且无尾随空白。
- 2026-07-15 最终扩写文档为 1815 行、8075 词、26 个 PlantUML 图和 66 个 Markdown fence；一级章节 0-16 连续，32 个引用路径存在，16 个新增关键行号锚点全部匹配，无尾随空白且 `git diff --check` 无输出。唯一验证边界仍是本机没有 PlantUML，未做实际渲染。

## Technical Decisions
| Decision | Rationale |
|----------|-----------|
| 使用“概念 -> 对象 -> 调用链 -> 执行上下文 -> 数据形态”五层写法 | 避免只罗列类名，便于建立运行时心智模型 |
| 对每个关键步骤标注线程/bthread 上下文 | 用户明确关心 bthread 何时创建、如何调度 |
| PlantUML 至少覆盖组件、总体时序、服务端接收、bthread 调度、示例端到端 | 不同关系适合不同图型，单图无法准确表达全部内容 |

## 真实微服务实践补充发现
- `example/cascade_echo_c++/server.cpp` 展示了真实服务常见的“双重角色”：进程既通过 `Server + ServiceImpl` 接收入站 RPC，又持有进程级长生命周期 `Channel` 调用下游；业务方法用 `brpc::Controller cntl2(cntl->inheritable())` 继承可传播上下文，单独设置下游超时/重试，并把下游错误码与错误文本回传给上游。
- `Server::Stop()` 只停止接收新连接/请求而不等待存量请求结束，`Server::Join()` 才等待在途请求收敛；新请求在退出阶段得到 `ELOGOFF`。实际部署通常由业务编排先摘流量，再执行 Stop/Join 完成优雅退出。
- `Server::AddService()` 只能在 Server 非运行状态调用；`SERVER_OWNS_SERVICE` 与 `SERVER_DOESNT_OWN_SERVICE` 明确决定 Service 生命周期，真实业务必须让 `ServiceImpl` 至少活到 `Server::Join()` 完成。
- 新章节应把业务 API 分为三层：几乎每个服务都会使用的 `Server/Channel/Stub/Controller/ClosureGuard`，按场景启用的命名服务、LB、bvar/rpcz、HTTP/流式/组合 Channel，以及业务代码通常不直接调用的 `Socket/InputMessenger/EventDispatcher/TaskGroup` 内部接口。
- 新章节插入现有“运行时总时序总结”之后最自然：编号为第 14 章，原“源码阅读顺序/术语速查/最终结论”顺延为 15/16/17；这样先完成内部机制，再映射到业务工程，最后给出继续阅读路线。
- `Controller` 的高频公开调用面已由当前头文件核对：调用前设置 `set_timeout_ms`、`set_max_retry`、`set_log_id`、一致性哈希所需 `set_request_code` 和 `request_attachment`；调用后检查 `Failed/ErrorCode/ErrorText`、`retried_count`、`latency_us`、`remote_side/local_side` 和 `response_attachment`。
- fan-out/cancel 必须在发起 RPC 前保存 `call_id()`；等待使用 `brpc::Join(call_id)`，取消使用全局 `brpc::StartCancel(call_id)`，不能把 `Controller::StartCancel()` 当作安全取消接口。
- `ChannelOptions.timeout_ms/max_retry` 是 Channel 级默认值，`Controller` 可覆盖单次请求。仓库旧文档与当前头文件对默认 timeout 的描述存在漂移，因此实践章节不依赖隐式默认值，要求生产配置显式给出超时和重试策略。
- 当前 `echo_c++` 示例源码明确说明 `Channel` 与生成的 Stub 都可跨线程共享；同步 RPC 的 request/response/Controller 可放栈上，异步 RPC 则必须让 Controller、response 和 done 捕获的数据存活到回调结束。
- `Controller(service_cntl->inheritable())` 在当前头文件中的明确用途是继承请求/session 上下文并支持 `CLOG*` 的 request-id 串联；不能把它表述为自动完成剩余 deadline 预算计算，真实业务仍需显式设置下游 timeout。
- 生产观测的常用组合是业务侧 `bvar::LatencyRecorder`/`Adder` 加框架内置 `/vars`、`/status`、`/connections`、`/rpcz`、`/health`；`internal_port` 应用于隔离这些管理接口，rpcz 更适合单请求诊断而不是替代聚合监控。
- HTTP body/附件、`ProgressiveAttachment`、Streaming RPC、`ParallelChannel`、`SelectiveChannel`、`PartitionChannel` 都是有效公开能力，但属于特定业务形态；应在章节末单列“按需启用”，避免让读者误以为常规 protobuf unary RPC 必须使用这些组件。
- 新章节写入后主文档为 2306 行、9906 词，PlantUML 从 26 增至 29 个，Markdown fence 从 66 增至 88 个；29 个 PlantUML 块均各含一组 start/end，`git diff --check` 无输出。
- 当前头文件/示例已再次确认 `set_request_id`、`backup_request_ms`、`enable_circuit_breaker`、`connection_group`、`thread_local_data_factory`、`MaxConcurrencyOf`、`CreateProgressiveAttachment`、`StreamWrite/StreamClose` 均存在。
- 首轮章节连续性校验因 awk 的 `expected` 未在 BEGIN 中显式初始化而产生假阳性；首轮路径校验只剥离单行锚点，未剥离 `:220-230` 范围锚点，也产生假 missing。这两个都是校验脚本问题，不是文档路径问题，需修正脚本后重跑。
- 修正 checker 后确认一级章节 0-17 连续，全文 47 个唯一 `src/`/`example/`/`docs/` 引用路径全部存在；人工复核新增章节时仅发现空格和示意 API 写法问题，已把方法级限流写成实际的完整方法名形式并修正文案。
- 最终校验结果：2306 行、9907 词、44 个 Markdown code block、29 个 PlantUML block、一级章节 0-17 连续、无尾随空白；`Channel/Stub` 跨线程共享、全局 `StartCancel/Join`、`Stop/Join`、`MaxConcurrencyOf`、熔断与 connection group 等关键 API 均在当前源码命中。
- 当前工作树仍只包含本任务新建的主文档和三个规划文件，框架源码未修改。PlantUML 命令不可用，因此图只完成 marker/fence 和人工语法校验，未实际渲染。

## Issues Encountered
| Issue | Resolution |
|-------|------------|
| 历史资料可能与当前分支漂移 | 所有最终结论重新从当前源码确认 |
| 初次检索包含不存在的 `src/bthread/worker.cpp` 和 `remote_task_queue.cpp` | 改用实际实现文件 `task_control.cpp`、`task_group.cpp` 和头文件继续分析 |

## Resources
- `src/brpc/`
- `src/bthread/`
- `src/butil/iobuf.cpp`
- `example/rdma_performance/`
- `docs/cn/iouring.md`

## Visual/Browser Findings
- 本任务不依赖网页或外部图片；所有图将由代码关系生成 PlantUML 源码。

## 听众问题集覆盖矩阵
- 问题集不能只复述主文档标题，应同时覆盖五类追问：对象职责、调用路径、执行上下文、数据/所有权变化、异常与性能边界。
- 框架主链应覆盖：全局初始化、Server/Acceptor、Channel/Controller、命名服务和 LB、Socket/SocketMap、Protocol、InputMessenger、EventDispatcher、Timer/correlation id、业务 done 和响应完成。
- 核心基础设施应独立深挖：bthread 创建/入队/唤醒/窃取/切换/阻塞/退出，IOBuf BlockRef/引用计数/零拷贝边界，pthread TLS/bthread TLS/Server keytable pool。
- transport 应分别提问 TCP/epoll、RDMA 和 io_uring，并追问它们如何复用统一 Socket/InputMessenger/Protocol 上层、在哪些位置发生分叉与回退。
- 公开能力还要覆盖 protobuf/附件/压缩/校验和、同步异步取消、连接模型、HTTP/SSL/认证、Streaming、组合 Channel、限流熔断健康检查、builtin services 和 bvar/rpcz。
- `rdma_performance` 要单列场景题，覆盖 warmup、固定在途闭环、attachment 数据变化、client callback 接力、server done、RDMA handshake/CQ 和示例自身的内存/并发测量缺陷。
- 挑战性问题应重点测试常见误解：一次 RPC 不等于一条 pthread；挂起 bthread 不等于阻塞 worker；Channel 不等于连接；IOBuf 引用拼接不等于全链路零拷贝；NOSIGNAL 不等于延迟入队；retry/backup 不保证 exactly-once；`thread_local` 不等于 bthread-local。

## 全量运行总图设计发现
- 单图应采用“组件分区 + 编号流程 + 跨区连接”而不是单一时序图：时序图适合一条主链，但无法同时完整表达 bthread 状态机、IOBuf 对象结构、三种 transport 和生命周期。
- 组件分区确定为：全局初始化/扩展注册、业务 API/控制面、服务端生命周期、连接/传输/I/O、协议/消息、IOBuf 数据面、bthread 调度面、完成/可靠性、治理/观测。
- 主请求和响应用相反方向/颜色闭环；bthread 创建与回收、IOBuf 数据变化、timeout/retry/cancel、Server Stop/Join 使用独立颜色，避免交叉箭头失去含义。
- bthread 子图必须显式包含 `TaskMeta ResourcePool -> local/remote queue -> signal/ParkingLot -> worker TaskGroup -> pop/steal -> lazy stack -> sched_to -> task_runner -> wait/requeue 或 TLS/version/join/stack 回收`。
- IOBuf 子图必须显式包含 protobuf ZeroCopyOutputStream、SmallView/BigView、BlockRef/Block、pthread TLS block cache、协议引用拼装、IOPortal/readv、切帧、body/attachment 拆分以及 writev/SGE/SQE 三个输出边界。
- Socket/epoll 子图必须把两个闭环同时画出：发送 `StartWrite -> leader/fast write -> writev -> short/EAGAIN -> KeepWrite -> EPOLLOUT -> 继续写`；接收 `epoll_wait -> SocketId -> OnInputEvent/_nevent -> consumer bthread -> readv 到 IOPortal -> ProcessNewMessage -> parse/cut -> ProcessInputMessage`。
- 当前 checkout 的 transport 入口已核对为 `TcpTransport`、`RdmaTransport/RdmaEndpoint`、`IouringTransport/IouringEndpoint`；三者最终统一把接收字节交回 `Socket::_read_buf + InputMessenger`，并复用协议/业务完成链。
- 独立总图初稿为 429 行、2378 词，包含 9 个分区、189 条显式流程箭头和 4 个关键边界说明；所有箭头端点别名均已声明且无重复，花括号 10/10 配对，无尾随空白。
- 当前环境仍没有 `plantuml` 命令，`/usr/share` 和 `/opt` 也未发现可用 PlantUML jar；最终只能进行结构/别名/语法人工检查，并提供 SVG 渲染命令。
- 最终总图为 438 行、2437 词，包含 9 个 package、124 个唯一别名和 197 条显式箭头；start/end、4 组 note、legend、10 组花括号全部配对，所有箭头端点有定义且无重复别名。
- 编号校验确认 `B0-B28`、`I1-I16`、`T1-T24`、`C1-C17` 连续；关键组件/流程语义清单全部命中。主文档新增第 15 节后一级章节 0-18 连续，46 个 Markdown code block 闭合，引用路径存在且无尾随空白。
- 工作区仍只新增本任务文档、总图和三个规划文件，框架源代码未修改；实际 PlantUML 渲染是唯一未执行的验证项。
# 2026-07-17：总图收敛为纯 TCP/epoll

- 用户要求总图去除 RDMA 和 io_uring，只保留 TCP 数据面。
- 修改边界限定为独立 PlantUML 总图及主文档第 15 章读图说明；主资料中既有的 RDMA、io_uring 和 `rdma_performance` 专章继续保留。
- 总图应删除整个非 TCP transport 分区、工厂分叉和 poller 到 bthread 的跨区箭头，同时将 IOBuf 输出适配、allocator 和零拷贝说明改为 TCP/readv/writev 语义。
- 收敛后总图有 8 个分区、108 个别名和 168 条显式箭头；别名定义/引用闭合，`B0-B28`、`I1-I16`、`C1-C17` 和 `S0-S24` 主编号保持完整。
- 对总图严格扫描 `rdma|io_uring|iouring|SGE|SQE|CQE|lkey|fixed buffer|registered buffer` 无结果。

## 2026-07-17：总图简化设计

- 当前纯 TCP 总图仍有 8 个分区、108 个别名和 168 条箭头，主要问题不是 transport 数量，而是组件粒度过细、主请求链与多个内部状态机交叉连接。
- 简化版应把客户端、协议、服务端放在一条上方主链；IOBuf、TCP/epoll、bthread、可靠性作为四个独立折叠子流程，仅通过少量虚线说明与主链的关系。
- bthread 和 Socket 的完整生命周期不再为每个细节点单独跨区连线，而是在各自分区内用线性主链和少量回边表达。
- 目标是将节点、箭头和图例颜色显著减少，同时保留：请求/响应闭环、writev/EPOLLOUT、epoll_wait/readv/切帧、BlockRef 所有权、TaskMeta 创建/调度/等待/回收、timeout/retry/EndRPC、Server Start/Stop/Join。
- 完成后的总图从 387 行、108 个别名、168 条可见箭头，降为 218 行、71 个别名、62 条可见箭头，降幅分别约为 44%、34% 和 63%。
- 69 条箭头端点（含 7 条布局用 hidden 箭头）全部有唯一别名定义；11 组花括号、3 组 note、legend 和 PlantUML marker 均成对。
- 关键语义清单覆盖 Stub/Channel/Controller、Protocol/IOBuf/BlockRef、StartWrite/writev/EPOLLOUT、epoll_wait/readv/IOPortal/InputMessenger、TaskMeta/ParkingLot/TaskGroup/steal/sched_to、EndRPC/Retry/Backup/Stop/Join。

## 2026-07-18：rdma_performance 单请求数据演化图

- 本图固定分析两端 `use_rdma=false`、`echo_attachment=true`、`attachment_size=1024`、默认 `baidu_std`；只描述 TCP/epoll，不展示其他 transport。
- `Init()` 的同步 warmup 设置 `echo_attachment=true` 但没有向 Controller 追加 attachment；它负责提前建立连接，本图从随后 `RunTest -> SendRequest()` 的首条正式异步请求开始。
- `PerformanceTest` 构造时先生成 `_addr[1024]` 随机字节，再通过 `_attachment.append(_addr, 1024)` 复制到长期 IOBuf；每次 `SendRequest()` 的 `request_attachment().append(_attachment)` 只共享 BlockRef，不再复制 1024 字节。
- request protobuf 只有 required bool `echo_attachment=1`；response protobuf 只有 required string `cpu_usage=1`。大块压测数据不在 protobuf body 中，而在 Controller attachment 中。
- request protobuf 的 `true` 编码为 `08 01` 两字节；请求 PRPC 帧为 `[12B header][Mreq RpcMeta][2B protobuf][1024B attachment]`，总长为 `1038 + Mreq`。
- TCP 发送将 IOBuf BlockRef 映射为 `iovec[]`，`writev` 复制到内核发送缓冲区；接收由 `readv` 填充 IOPortal block。一次 PRPC 帧不等于一个 TCP segment/read。
- 服务端从 payload 前端 cut 出 2 字节请求 body，把余下 1024 字节 swap 到 request attachment；echo 时 response attachment append 共享服务端接收 block。
- 响应帧为 `[12B header][Mresp RpcMeta][P 字节 cpu_usage protobuf][1024B attachment]`；通常空字符串 body 为两字节，周期采样时 P 动态变化。
- 图嵌入主文档第 11.7 节，按“业务对象 -> PRPC 帧 -> writev/TCP/readv -> 服务端对象 -> response frame -> client completion”展开，不修改简化总图。
- 新图使用 8 个 participant、两个 `alt` 和一个完整帧等待 `loop`：分别表达 client/server、TCP、请求与响应执行流，部分写/KeepWrite，以及单条内联与批次 NOSIGNAL 派发。
- 图后 12 行表格按同一编号记录执行上下文、输入形态、处理、输出形态和复制/所有权；BlockA、BlockS、BlockC 明确表示三组不同物理内存。
- 静态校验确认新 block 有 8 个唯一 participant、2 组 alt/else、1 组 loop、12 组 note，所有引用闭合；12 个阶段表格行和关键语义清单全部命中。
- 主文档从 29 增至 30 个 PlantUML block，94 条 Markdown fence 即 47 对，编号章节仍为 0-18 共 19 个；无尾随空白。

## 2026-07-20：TCP 运行机制精要版

- 新文档固定使用 `example/rdma_performance` 的纯 TCP 1KB attachment 场景贯穿端到端、IOBuf、bthread 和连接生命周期，避免不同示例之间的语义漂移。
- 文档目标为 650–900 行、4 个主章节、6 张 PlantUML 图；完整资料只在开头增加精要版链接，不改写既有章节和简化总图。
- 源码复核范围限定为当前 HEAD 的 example、baidu_std、IOBuf、bthread、Channel/SocketMap、Socket/TcpTransport、InputMessenger/EventDispatcher、Acceptor/Server。
- 先前关于默认分散式 bthread 队列、InputMessenger 的 `BTHREAD_NOSIGNAL + bthread_flush()` 和 KeepWrite 等待域的结论仅作为检索提示，本轮仍以当前 checkout 源码为准。
- 当前源码再次确认：请求由 `PackRpcRequest()` 把 RpcMeta、protobuf body 和 `request_attachment()` 以 IOBuf 引用拼成 PRPC 帧；服务端 parse 先切 meta/payload，再把 payload 中剩余部分 `swap` 为 request attachment。
- IOBuf 的 `append(const IOBuf&)`、`cutn(IOBuf*)` 与 `swap` 操作 BlockRef；`append(void*, size)` 复制字节。发送走 writev 边界，接收由 IOPortal/readv 填 block，因此 TCP 两端 block 不是同一物理内存。
- bthread background 创建会从 ResourcePool 取得 TaskMeta、生成 versioned tid，再按调用者是否为 worker 进入 local `_rq` 或 remote `_remote_rq`；默认可运行队列是分散式，priority queue 是可选路径。
- Socket 首写通过 MPSC `_write_head` 争取写权并延迟 `ConnectIfNot`；连接中经非阻塞 connect/EPOLLOUT/`CheckConnected`/`ResetFileDescriptor`，部分写或 EAGAIN 转入 KeepWrite。
- 接收链为 epoll_wait -> Socket IOEvent -> `OnInputEvent` -> InputMessenger consumer -> IOPortal read -> parse；前序消息可用 NOSIGNAL bthread 批量唤醒，最后一条可在 consumer 执行流中处理。
- client Channel 析构只执行 SocketMapRemove；物理 fd 的关闭还受共享引用、失败状态和延迟关闭控制。server Stop 停 accept 并 SetFailed 连接，Join 等待 listener 和连接集合收敛。
- 示例构造函数中的 `_attachment.append(_addr, attachment_size)` 明确是一次 memcpy；正式请求的 `request_attachment().append(_attachment)` 与服务端 echo 的 `response_attachment().append(request_attachment())` 都只增加 BlockRef 引用。
- baidu_std 头部确认为 12 字节 `PRPC + body_size + meta_size`；`body_size` 包含 meta 之后的 protobuf body 与 attachment。parser 只有在 `12 + body_size` 齐备后才从输入缓冲区切出 meta/payload。
- `IOPortal::pappend_from_file_descriptor()` 先为可写 block 空间构造最多 64 个 iovec，再调用 readv；成功后才为实际收到的字节建立 BlockRef，因此 readv 的内核到用户复制与后续引用切分是两个边界。
- `TaskGroup::sched_to()` 在跳栈前保存当前 TaskMeta 的 bthread-local storage，并恢复下一个 TaskMeta 的 local storage；这使 worker pthread 能在不同 bthread 间复用而不混淆 bthread TLS。
- 精要版最终采用“每章一个主问题 + 必要表格 + 对应图 + 源码入口”的结构；首稿超长的原因主要是图内空行和多行 note，压缩后为 886 行，未删除任何流程分支。
- 复核示例 callback 对象时确认 Controller 和 response 由 `unique_ptr` 删除，但 `new RespClosure` 没有对应 delete；精要版将其标成 benchmark 示例泄漏，避免把“所有本次对象均释放”写成框架事实。
- 最终静态校验结果为 887 行、3913 词、4 章、6 图、14 对 Markdown fence；所有显式源码路径存在，图的控制块和花括号配对，未发现非 TCP 数据面实现残留或尾随空白。
- 环境仍未提供 PlantUML renderer；交付内容是通过静态检查的 PlantUML 源码，不能表述为已实际渲染 SVG。
- 精要版第 1.2 节现在显式引用完整资料第 11.7.1 节，便于讲解时从精简图直接跳转到 12 阶段专项分析。
- 用户要求的是参考 11.7.1 的图形构造而非只增加链接；当前图的 client completion 位于 server 参与者右侧，破坏了“请求 client -> server、响应 server -> client”的视觉方向，需要按进程分区重排。
- 重排后 participant 顺序为 `Client(C/CP/CS/CC) -> TCP -> Server(SS/SB/SVC)`；这使请求箭头自然向右、响应 writev 与 TCP 回包箭头自然向左，client/server 前后时序不再依赖读者从 participant 名称推断。

## 2026-07-20：IOBuf 数据结构与 TLS 交互扩写

- 本轮只扩写精要文档第 2 章：图 2 后增加数据结构说明，并将 TLS block cache 的获取、使用和归还嵌入 append、IOPortal readv、切帧和生命周期流程。
- 必须明确区分两类 TLS：IOBuf allocator/cache 的 pthread-local `TLSData`，以及随 TaskMeta 在 bthread 切换时保存/恢复的 bthread local storage。
- `g_tls_data` 在 `src/butil/iobuf.cpp` 中声明为 `static __thread TLSData`，字段为 block 链表头、缓存数量和 thread-atexit 注册状态；它绑定实际 worker pthread，不随 bthread TaskMeta 迁移。
- `share_tls_block()` 借用当前 pthread cache 中的未满 Block但不摘链，适合 `append(void*, n)` 等顺序追加；`acquire_tls_block()` 会把一个未满 Block 从 cache 摘下交给 IOPortal 独占构造 readv 写入区。
- `IOPortal::return_cached_blocks()` 经 `release_tls_block_chain()` 把尚未形成有效数据引用的非满 Block 归还当前 pthread cache；超过每线程阈值时直接 dec_ref，而不是无限缓存。
- IOBuf 的 BlockRef 引用计数保证 payload 生命周期独立于 TLS cache：即使 bthread 后续迁移到另一 worker，已形成 BlockRef 的 Block 仍由引用计数持有，不依赖原 pthread cache。
- bthread local storage 则保存在 `TaskMeta::local_storage`，`sched_to()` 在切换时保存当前 `tls_bls` 并恢复下一个任务的 `local_storage`；这与 IOBuf 的 pthread-local allocator cache 是两套机制。
- IOBuf 的每线程 Block cache 默认软上限是 8；启用 IOBuf profiler 时 `max_blocks_per_thread()` 返回 0。线程退出时 `remove_tls_block_chain()` 对缓存 Block 逐个 dec_ref。
- 最终第 2 章同时覆盖结构、复制语义、TLS 分配、IOPortal 接收、缓存归还、bthread 迁移和两类 TLS 对照；全文 899 行，仍满足原 900 行上限。

## 2026-07-20：图 4 bthread 生命周期详解

- 用户要求对 3.2 图 4 的所有流程和细节逐段解释；新增内容应紧跟图后，以状态节点到源码动作的映射为主，避免与后续 3.3–3.7 仅做文字重复。
- 新增要求使 900 行上限不再合理，本轮将精要版控制线调整为 1000 行，保持 4 章和 6 图不变。
- API 分流首先检查当前是否位于 `tls_task_group` 且 tag 可本地运行；urgent 在 worker 内走 `start_foreground()` 立即切换，background 走 `start_background()` 入队，外部 pthread 统一经 `start_from_non_worker()` 选组。
- TaskMeta 来自 ResourcePool，创建时 stack 仍为空；tid 由 slot 与 `version_butex` 组合，资源复用后 version 变化使旧 tid 无法错误命中新任务。
- local `_rq` 是 owner worker 快路径，remote `_remote_rq` 用 mutex 保护；NOSIGNAL 仍然入队，只累计待唤醒数，`bthread_flush()` 才统一 `signal_task()`。
- worker 的 `wait_task()` 在 ParkingLot 睡眠前保存/检查 state 并再次 steal，避免任务到达与入睡之间丢失唤醒；获得任务后 local pop、跨组 local/remote steal，最后才回 main/idle 栈。
- `sched_to()` 保存 errno、统计和当前 `tls_bls`，恢复 next TaskMeta local storage 后 `jump_stack()`；切栈后才执行 remained callback，因此可安全完成重新入队或释放旧上下文。
- sleep 必须先切走再由 remained callback 注册 TimerThread，防止 timer 过早唤醒仍在运行的当前栈；Timer/I/O/butex 完成最终都把 TaskMeta 重新放入 runnable queue。
- 退出严格按 TLS/KeyTable 析构、version 递增、joiner 唤醒、ending_sched、切到下一栈、`_release_last_context` 回收 stack/TaskMeta 的顺序执行。
- 最终在 3.2 节加入 16 阶段表和 10 条不变量；全文 933 行，满足本轮调整后的 1000 行上限，且没有增加图数量或修改框架源码。

## 2026-07-20：图 4 生命周期整体说明

- 用户需要的不只是源码动作列表，还需要能独立阅读和讲解的整体文字说明；新增内容应回答“各对象为何存在、控制权如何流动、等待为何不占 worker、退出为何分两阶段”。
- 现有 16 阶段表和不变量继续作为总览后的查阅索引，不能用更多列表替代连续叙事。
- 最终 3.2 节形成“状态图 -> 连续生命周期总览 -> 16 阶段源码表 -> 关键不变量”的四层结构，既可直接讲解，也可用于源码定位。

## 2026-07-20：第 2 章独立 TLS 说明

- 当前 TLS 内容已覆盖关键事实，但分散在结构、readv 和生命周期段落中；独立小节需要把“为什么缓存、缓存属于谁、何时获取归还、如何清理、与 bthread TLS 如何交互”串成连续说明。
- 既有两类 TLS 对照表应移动到独立小节，避免同一信息重复占用篇幅。
- 独立小节最终形成“设计目的 -> 五步缓存生命周期 -> cache 与数据所有权 -> bthread 迁移 -> 两类 TLS 对照”的完整结构。

## 2026-07-20：图 4 生命周期图简化

- 原图把 API 分流、TaskMeta 初始化、local/remote queue、signal、选取、栈准备、上下文切换和退出步骤都展开成嵌套状态，源码信息完整但视觉主线不突出。
- 简化图以 bthread 自身状态为主，保留 CREATED、READY、SCHEDULING、RUNNING、SUSPENDED、END、RECYCLED 七个节点；worker 的 ParkingLot 等行为作为 SCHEDULING 注释，不再伪装成任务状态。
- background 走 CREATED -> READY，兼容环境中的 urgent 可直接进入 SCHEDULING；yield 和事件唤醒回到 READY，函数返回则经过 END 后在切离旧栈时进入 RECYCLED。
- TaskMeta/versioned tid、local/remote queue、NOSIGNAL/flush、pop/steal/ParkingLot、lazy stack、TLS 切换和两阶段回收仍保留在图中注释，下方 16 阶段表继续承载源码级细节。
