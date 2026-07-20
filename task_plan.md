# Task Plan: brpc 框架运行原理与端到端代码分析

## Goal
基于当前 checkout 的真实代码，产出一份包含 PlantUML 图、组件实现原理、运行时序、数据变化及 `example/rdma_performance` 端到端流程的完整中文资料。

## Current Phase
Phase 25 已完成，等待交付

## Phases

### Phase 1: 仓库边界与组件清单
- [x] 确认当前分支、变更边界和文档落点
- [x] 识别客户端、服务端、协议、Socket、事件分发、bthread、负载均衡等核心入口
- [x] 记录准确的代码锚点
- **Status:** complete

### Phase 2: 核心运行链路源码分析
- [x] 分析 Server 启动、监听、accept、读包、解析和业务派发
- [x] 分析 Channel 初始化、寻址、发包、回包和 completion
- [x] 分析 bthread TaskControl/TaskGroup/TaskMeta 创建与调度机制
- [x] 分析 IOBuf、Controller、Closure、Timer 与错误/超时处理
- **Status:** complete

### Phase 3: rdma_performance 端到端专项分析
- [x] 分析 proto、client、server 和构建配置
- [x] 跟踪请求/响应在 protobuf、IOBuf、协议帧和网络传输中的形态变化
- [x] 明确 RDMA/TCP 分叉、bthread 创建点与调度时机
- **Status:** complete

### Phase 4: 编写完整资料与 PlantUML 图
- [x] 编写组件架构、线程/执行模型和生命周期说明
- [x] 编写整体流程图、客户端/服务端时序图和 bthread 调度图
- [x] 编写 rdma_performance 端到端时序图和逐步说明
- [x] 补充连接模型、并发模型、内存模型、超时取消、可观测性和阅读路线
- **Status:** complete

### Phase 5: 静态校验与交付
- [x] 检查所有代码路径、符号和行号锚点
- [x] 检查 PlantUML 块结构与 Markdown 完整性
- [x] 执行 git diff --check 并记录验证边界
- **Status:** complete

### Phase 6: bthread / IOBuf / TLS 深化分析
- [x] 从公开 API 跟踪 bthread 创建、TaskMeta 初始化、本地/远端入队和 worker 唤醒
- [x] 跟踪 worker 主循环、优先级、work stealing、栈切换、阻塞/睡眠/join 和任务回收
- [x] 跟踪 IOBuf Block/BlockRef/SmallView、引用计数、切分拼接、零拷贝边界和系统调用适配
- [x] 跟踪 bthread TLS/keytable、pthread TLS 兼容层、切换保存/恢复和析构生命周期
- [x] 扩写主文档并新增 PlantUML 图、源码锚点及常见误区
- **Status:** complete

### Phase 7: 扩写内容静态校验
- [x] 校验新增源码路径、符号与行号锚点
- [x] 校验 PlantUML/Markdown fence 配对和文档结构
- [x] 执行 `git diff --check` 并记录验证边界
- **Status:** complete

### Phase 8: 真实微服务使用模式分析
- [x] 核对仓库中典型 server/client/naming/并发/观测示例与公开 API
- [x] 总结真实业务的进程生命周期、对象生命周期和高频调用面
- [x] 区分常用业务 API、按需高级能力和框架内部接口
- **Status:** complete

### Phase 9: 新增微服务实践章节并校验
- [x] 新增章节、PlantUML 架构/时序图和可复用代码骨架
- [x] 更新阅读顺序、术语/最终结论及章节编号
- [x] 校验引用路径、锚点、PlantUML/Markdown 和 `git diff --check`
- **Status:** complete

### Phase 10: 听众问题覆盖矩阵设计
- [x] 按现有资料章节建立组件、运行路径、执行上下文和数据形态覆盖矩阵
- [x] 补充听众常见的边界、反例、性能、可靠性和源码追问
- [x] 区分基础理解题、实现细节题和挑战性追问题
- **Status:** complete

### Phase 11: 输出全面问题清单
- [x] 形成按主题分组、可直接用于演练的中文问题集
- [x] 检查 TCP/RDMA/io_uring、bthread、IOBuf、TLS 和微服务实践均被覆盖
- [x] 给出建议演练方法和最值得优先准备的问题
- **Status:** complete

### Phase 12: 全量运行总图覆盖设计
- [x] 核对核心组件清单和主请求/响应链的连接关系
- [x] 将 bthread、IOBuf、TCP/epoll、RDMA/io_uring、可靠性和生命周期映射到一张图
- [x] 设计颜色、编号、执行上下文和图例，保证超大图仍可追踪
- **Status:** complete

### Phase 13: 编写独立 PlantUML 总图
- [x] 新建可独立渲染的 `.puml` 文件
- [x] 补充主文档入口、阅读顺序和渲染说明
- [x] 检查所有主要流程有闭环、跨子系统箭头有明确含义
- **Status:** complete

### Phase 14: 总图静态校验与交付
- [x] 校验 PlantUML marker、别名、箭头引用和块结构
- [x] 尝试本机渲染；不可用时记录边界并提供渲染命令
- [x] 校验文档结构、路径、空白和工作区差异
- **Status:** complete

### Phase 15: 总图收敛为纯 TCP/epoll
- [x] 删除 RDMA、io_uring 分区、组件、箭头和专属术语
- [x] 将 IOBuf 输出边界、消息派发和 transport 表达改为 TCP/epoll 语义
- [x] 同步更新主文档中的总图分区表和读图顺序
- **Status:** complete

### Phase 16: 纯 TCP 总图静态校验与交付
- [x] 校验 PlantUML marker、别名、箭头引用和分区编号
- [x] 校验图中无 RDMA/io_uring 及其专属数据结构残留
- [x] 校验文档结构、路径、空白和工作区差异
- **Status:** complete

### Phase 17: 总图分层简化
- [x] 将 8 个平铺分区收敛为主流程、IOBuf、TCP/epoll、bthread 和可靠性 5 个视图区
- [x] 合并同层组件，减少跨区箭头、颜色和编号密度
- [x] 保留请求/响应闭环、TCP 收发、IOBuf 所有权和 bthread 生命周期
- [x] 同步更新主文档的总图结构与读图方式
- **Status:** complete

### Phase 18: 简化版总图静态校验
- [x] 校验 PlantUML marker、别名、箭头引用和块结构
- [x] 校验纯 TCP 范围和关键语义覆盖
- [x] 对比简化前后的节点、箭头和行数
- **Status:** complete

### Phase 19: rdma_performance 单请求数据形态核验
- [x] 核对示例 request/response、attachment 和回调对象的真实生命周期
- [x] 核对 baidu_std 的序列化、PRPC 帧布局、切帧和 attachment 拆分
- [x] 核对 `use_rdma=false` 下 writev/readv、epoll、所有权和 bthread 执行上下文
- **Status:** complete

### Phase 20: 单请求数据演化图与文档入口
- [x] 在主文档第 11.7 节嵌入一张 PlantUML 时序图，逐阶段标注数据形态、复制/引用语义和执行上下文
- [x] 增加逐阶段数据形态表及 warmup、TCP 字节流、批处理和 KeepWrite 边界说明
- [x] 校验 PlantUML 结构、关键语义、文档结构、空白和工作区边界
- **Status:** complete

### Phase 21: TCP 精要版源码复核
- [x] 复核 `rdma_performance` 纯 TCP 请求/响应数据形态和 warmup 边界
- [x] 复核 IOBuf BlockRef、引用计数、TLS block cache 和 readv/writev 复制边界
- [x] 复核 bthread 创建、入队、唤醒、切换、等待、恢复和回收链
- [x] 复核 Channel/SocketMap、非阻塞 connect、epoll 收发及连接关闭链
- **Status:** complete

### Phase 22: 编写 TCP 运行机制精要文档
- [x] 新建 `docs/cn/brpc_tcp_runtime_guide.md`
- [x] 完成 4 个主章节、6 张 PlantUML 图和阶段/复制矩阵
- [x] 在完整资料开头增加精要版入口，并在精要版回链完整资料
- **Status:** complete

### Phase 23: 静态校验与交付
- [x] 校验行数、章节、PlantUML marker/块结构、Markdown fence 和关键术语
- [x] 校验引用路径、TCP/epoll 范围、尾随空白及 `git diff --check`
- [x] 记录 PlantUML 未实际渲染的环境边界并运行 planning completeness 检查
- **Status:** complete

### Phase 24: 明确端到端图的参考基准
- [x] 在精要版图 1 前链接完整资料第 11.7.1 节
- [x] 明确精要版只收敛图形表达，不改变场景参数、数据形态和调度分支
- [x] 复核行数、链接、PlantUML/Markdown 结构和空白
- **Status:** complete

### Phase 25: 重构端到端图的 Client/Server 前后时序
- [x] 参照完整资料 11.7.1，将 client 参与者集中到左侧分区、server 集中到右侧分区
- [x] 以 TCP 字节流为中轴，按 1–12 显式表达请求左到右、响应右到左
- [x] 保留部分写、完整帧等待、消息批处理和 callback 接力分支
- [x] 校验文档行数、PlantUML 控制块、Markdown 和空白
- **Status:** complete

## Key Questions
1. brpc 如何把用户 RPC 调用转换为协议帧、Socket 写入和异步完成事件？
2. 服务端从监听 fd 可读到业务方法执行，分别在哪些 pthread/bthread 上发生？
3. bthread 在哪些代码点创建，TaskMeta 如何入队、窃取、切换和回收？
4. `rdma_performance` 默认运行模式及 RDMA/TCP 分叉如何影响数据路径？
5. 请求、响应和附件在每一层的物理/逻辑形态如何变化？
6. bthread TLS 与 pthread TLS 的隔离、继承、切换及析构语义分别是什么？
7. IOBuf 的“零拷贝”具体省略了哪些复制，在哪些边界仍然发生复制？
8. 在真实微服务中，哪些 brpc API 是日常高频接口，哪些能力只在特定场景启用？
9. Channel、Stub、Controller、Server 和 bthread 在业务进程中的推荐生命周期是什么？
10. 听众在完整讲解后可能从哪些角度追问，怎样保证问题集覆盖概念、源码、运行时、性能和故障边界？
11. 如何在一张 PlantUML 图中同时完整表达核心组件、主请求链、bthread、IOBuf、Socket/epoll 和其他关键分支，又保持可读性？

## Decisions Made
| Decision | Rationale |
|----------|-----------|
| 新建 `docs/cn/brpc_runtime_architecture.md` | 与已有专题文档隔离，形成可独立维护的完整资料 |
| PlantUML 源码内嵌 Markdown | 用户可直接渲染，且图与代码说明保持同一版本 |
| 以当前 checkout 为唯一事实来源 | 分支实现可能与上游文章或历史记忆不同 |
| 同时说明 TCP 基线与 RDMA 分叉 | `rdma_performance` 可切换传输，必须避免把两条路径混为一谈 |
| 将 IOBuf 与 TLS 提升为独立章节，并顺延后续章节编号 | 两者都有独立对象模型、生命周期和性能边界，塞进 bthread 小节会掩盖 pthread TLS 与 bthread TLS 的区别 |
| 新建独立 `.puml` 全量总图并在主文档链接，而不复制大段 PlantUML | 保持一个可渲染事实源，避免主文档与独立图内容漂移 |
| 总图限定为 TCP/epoll，保留主资料中的其他 transport 专章 | 用户本轮要求修改“图”，不应误删此前完整资料中的专题分析 |
| 总图改为“主链 + 4 个折叠子流程”，细节由节点内步骤表达 | 降低跨区连线和视觉跳转，同时保留完整运行闭环 |
| rdma_performance 单请求数据演化图嵌入主文档第 11.7 节 | 保持简化总图不变，并让纯 TCP 专项图紧邻对应分析 |
| 新建独立 TCP 精要版并控制为 4 章 6 图 | 支持 30–45 分钟讲解，同时保留到完整资料的深挖入口 |
| 精要版统一使用 `rdma_performance` 的 1KB TCP 场景 | 避免各章节使用不同示例造成执行上下文和数据形态断裂 |

## Errors Encountered
| Error | Attempt | Resolution |
|-------|---------|------------|
| `rg` 报 `src/bthread/worker.cpp`、`remote_task_queue.cpp` 不存在 | 1 | 根据 `rg --files` 和符号位置改读 `task_control.cpp`、`task_group.cpp` |
| 一次多 hunk 文档补丁因上下文匹配失败 | 1 | 拆成路径/PlantUML 修正和运行说明两个小补丁，均已成功应用 |
| TLS 文件检索包含不存在的顶层 `include/` 目录 | 1 | 移除该目录，改为仅检索当前 checkout 的 `src/bthread`、`src/brpc` |
| 一次合并更新规划记录的 patch 因进度表上下文未匹配而失败 | 1 | 不影响主文档，改为按文件拆分小补丁更新状态和结果 |
| 微服务章节首轮 heading/path 校验出现假阳性 | 1 | heading checker 显式初始化 expected，路径 checker 支持剥离 `:N-M` 行号范围后重跑 |
| 一次 `rg` 命令在双引号中包含 Markdown 反引号，shell 将其误作命令替换 | 1 | 只影响只读统计输出；后续正则使用单引号，避免再次触发 |
| TCP 精要版首稿为 1019 行，超过 900 行上限 | 1 | 压缩 PlantUML 内部空行和重复文案，不删除流程节点或技术边界 |
| 图 1 整块替换补丁因 participant 顺序上下文不匹配而失败 | 1 | 重新读取当前图的精确文本，使用完整精确上下文替换 |

## Notes
- 不修改框架行为，不执行外部网络操作。
- 每完成一个分析阶段同步更新 `findings.md` 和 `progress.md`。
- 当前环境无 `plantuml` 可执行文件；已完成 16 组 start/end、Markdown fence 和人工语法检查，未进行实际图片渲染。
- 2026-07-15 扩写后共 26 组 PlantUML；环境仍无 `plantuml`，已完成块级 marker/fence、路径、行号和人工语法检查，未实际渲染。
- 2026-07-15 新增微服务实践章节后共 29 组 PlantUML、44 个代码块和 18 个一级章节；环境仍无 `plantuml`，未实际渲染。
- 2026-07-17 简化版纯 TCP/epoll 总图为 218 行、5 个顶层视图区、71 个别名和 62 条可见箭头；相比上一版分别减少约 44%、34% 和 63%，环境无 `plantuml`/jar，未实际渲染。
- 2026-07-18 新增 1 张纯 TCP 单请求 PlantUML 图和 12 阶段数据形态表；主文档现有 30 个 PlantUML block、47 对 Markdown fence，环境无 `plantuml`/jar，未实际渲染。
- 2026-07-20 新增 TCP 精要版：887 行、3913 词、4 个主章节、6 个 PlantUML block、14 对 Markdown fence；静态结构、引用路径、范围和空白检查通过，环境仍无 `plantuml`，未实际渲染 SVG。
- 2026-07-20 在精要版图 1 前明确引用完整资料第 11.7.1 节；场景参数和技术语义不变。
- 2026-07-20 参照 11.7.1 重构精要版图 1 为 Client process / TCP / Server process 三段布局；文档为 871 行，图内 2 个 box、3 组 alt/loop 控制块均配对。
