# Progress Log

## Session: 2026-07-14

### Phase 1: 仓库边界与组件清单
- **Status:** complete
- **Started:** 2026-07-14
- Actions taken:
  - 读取 `planning-with-files` 完整说明并执行上下文恢复检查。
  - 确认没有旧规划文件需要保留或合并。
  - 检查分支状态并检索本 checkout 的相关历史索引。
  - 确定新文档路径和五阶段分析计划。
  - 枚举核心源码并定位 Server、Channel、Socket、InputMessenger、EventDispatcher、TaskGroup 等入口。
  - 完整读取 `example/rdma_performance` 的 proto、client、server 和构建文件，提取负载模型与显式 bthread 创建点。
  - 跟踪 Server 启动、监听 Socket、Acceptor、epoll EventDispatcher 和 InputMessenger 读包/切帧/派发路径。
  - 跟踪 Channel 初始化/共享 Socket、CallMethod、correlation id、超时定时器、连接选择、协议打包、Socket 写入及同步/异步完成路径。
  - 核验 baidu_std 的 12 字节头、RpcMeta/body/attachment 布局，以及服务端反序列化/业务调用/响应发送和客户端 completion 的闭环。
  - 分析 transport 策略、Socket 多生产者写队列/KeepWrite、epoll event 合并和 TCP/RDMA 的消息派发差异。
  - 分析 bthread TaskControl 初始化、worker/TaskGroup、TaskMeta 入队、ParkingLot 唤醒、work stealing、用户态栈切换和任务回收。
  - 核验全局扩展注册、EventDispatcher 分片、SocketId 生命周期、IOBuf BlockRef 和 SocketMap/LB 共享模型。
- Files created/modified:
  - `task_plan.md`（新建）
  - `findings.md`（新建）
  - `progress.md`（新建）

### Phase 2: 核心运行链路源码分析
- **Status:** complete
- Actions taken:
  - 已完成 Server、Channel、协议、Socket、bthread、IOBuf、Controller、Timer 主路径分析，待收束 RDMA 专项边界。
  - 核验 RDMA 全局内存注册、TCP hello/ACK 协商、QP/CQ、SGE/窗口、completion 和 polling/event 两种模式。
- Files created/modified:
  -

### Phase 3: rdma_performance 端到端专项分析
- **Status:** complete
- Actions taken:
  - 核验 RDMA PollCq 批处理/重新 arm 逻辑和 example 的 warmup、固定在途闭环与回调执行上下文。
  - 审核示例的对象生命周期，记录 RespClosure 泄漏及 sweep/并发状态测量风险。
  - 固定文档代码基线为 HEAD `89b2765ca516`，汇总最终源码行号锚点。
- Files created/modified:
  -

### Phase 4: 编写完整资料与 PlantUML 图
- **Status:** complete
- Actions taken:
  - 新建 `docs/cn/brpc_runtime_architecture.md`，完成框架心智模型、组件表、对象关系、初始化、客户端/服务端主链、完成语义和 bthread 调度章节。
  - 补充 TCP/RDMA/io_uring transport 边界、RDMA 初始化/协商/SGE/CQ、`rdma_performance` 数据变化与 bthread 时序。
  - 补充示例基准陷阱、并发/生命周期/观测/排障建议、源码阅读路线和术语表。
  - 修正对象关系图中缺失的 worker pthread 节点，统一组件表源码全路径。
  - 增加 rdma_performance 构建/运行前提和 TCP A/B 注意事项。
- Files created/modified:
  - `docs/cn/brpc_runtime_architecture.md`（新建）

### Phase 5: 静态校验与交付
- **Status:** complete
- Actions taken:
  - 检查 16 个 PlantUML 块的 start/end 配对和 44 个 Markdown fence，均成对。
  - 验证文档引用的 28 个源码/示例路径存在，并抽查 7 个关键函数行号锚点。
  - 检查文档及规划文件无尾随空白，`git diff --check` 无输出。
  - 环境无 PlantUML 可执行文件，未实际渲染 PNG/SVG；已逐图人工检查语法并修正 1 处对象关系节点缺失。
  - 最终复检通过：文档 1218 行、5395 词，16 个 PlantUML 图、44 个 fence、28 个有效源码路径，未发现 TODO/错误历史文件引用或尾随空白。
- Files created/modified:
  - `docs/cn/brpc_runtime_architecture.md`
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

## Test Results
| Test | Input | Expected | Actual | Status |
|------|-------|----------|--------|--------|
| 上下文恢复 | session-catchup.py | 识别旧规划或确认无旧规划 | 无旧规划文件 | 通过 |
| 工作区边界 | git status --short --branch | 不覆盖已有修改 | 仅发现分支 ahead 1 | 通过 |
| PlantUML 结构 | 16 个图块 | start/end 成对 | 16/16 | 通过 |
| Markdown 结构 | code fences | 偶数且闭合 | 44 个 | 通过 |
| 引用路径 | 28 个源码/示例路径 | 全部存在 | 全部存在 | 通过 |
| 代码锚点 | 7 个核心入口 | 指定行包含目标符号 | 全部匹配 | 通过 |
| 空白检查 | rg + git diff --check | 无尾随空白/patch 错误 | 无输出 | 通过 |
| PlantUML 实际渲染 | plantuml | 渲染所有图 | 环境未安装 plantuml | 未执行 |
| 扩写后章节结构 | 一级章节 0-16 | 连续且无冲突 | `[0..16]` 连续 | 通过 |
| 扩写后 PlantUML | 26 个图块 | start/end 严格配对 | 26/26，块内 marker 唯一 | 通过 |
| 扩写后 Markdown | code fences | 偶数且闭合 | 66 个 | 通过 |
| 扩写后引用路径 | 32 个源码/示例路径 | 全部存在 | 全部存在 | 通过 |
| 扩写后代码锚点 | 16 个新增入口 | 行号命中目标符号 | 16/16 | 通过 |
| 扩写后空白检查 | 文档 + `git diff --check` | 无尾随空白/patch 错误 | 无输出 | 通过 |

## Session: 2026-07-15

### Phase 6: bthread / IOBuf / TLS 深化分析
- **Status:** complete
- Actions taken:
  - 恢复上一轮规划、发现与交付状态。
  - 根据新增要求，将 bthread 调度、IOBuf 内存模型和 TLS 生命周期拆为独立深化阶段。
  - 跟踪 `bthread_start_*` 到 TaskMeta 初始化、本地/remote 入队、ParkingLot 唤醒、同 tag 窃取、栈切换和结束回收的完整链路。
  - 跟踪 sleep/butex/join/interrupt 的挂起与重新入队机制，并核对 NOSIGNAL/urgent/pthread-stack 语义。
  - 跟踪 IOBuf SmallView/BigView、Block/BlockRef、引用计数、TLS block cache、IOPortal readv、writev 和 protobuf ZeroCopyStream。
  - 跟踪 bthread KeyTable 两级索引、version、析构循环、keytable pool 和 `brpc::thread_local_data()` 的 Server 端复用路径。
  - 将原第 6 节扩展为 11 个 bthread 小节，新增独立 IOBuf 与 TLS 章节和 9 个 PlantUML 图，后续章节顺延为 9-16。
  - 更新源码阅读顺序、术语表和最终结论，使新增三章与原端到端分析形成闭环。
  - 在 rdma_performance 第 11.8 节新增 client lane 调度接力图和 6 步映射，明确 RunTest 退出后由 response-processing bthread 递归续发、NOSIGNAL/flush 和 polling RDMA 隔离规则。
- Files created/modified:
  - `task_plan.md`
  - `progress.md`
  - `findings.md`
  - `docs/cn/brpc_runtime_architecture.md`

### Phase 7: 扩写内容静态校验
- **Status:** complete
- Actions taken:
  - 验证一级章节 0-16 连续，新增/原有标题无编号冲突。
  - 验证 26 个 PlantUML 块各有且仅有一组 start/end，66 个 Markdown fence 成对。
  - 验证全文引用的 32 个 `src/`/`example/` 路径全部存在。
  - 验证 16 个新增关键源码行号锚点全部命中目标符号。
  - 检查文档无尾随空白，`git diff --check` 无输出。
  - 环境仍无 `plantuml` 可执行文件，未实际渲染 PNG/SVG。
- Files created/modified:
  - `docs/cn/brpc_runtime_architecture.md`
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

## Error Log
| Timestamp | Error | Attempt | Resolution |
|-----------|-------|---------|------------|
| 2026-07-14 | 检索了不存在的 bthread 实现文件 | 1 | 使用真实符号所在的 task_control/task_group 文件 |
| 2026-07-14 | 多 hunk 文档补丁上下文匹配失败 | 1 | 拆分为两个局部补丁后成功 |
| 2026-07-15 | TLS 文件检索包含不存在的顶层 `include/` | 1 | 改为当前 checkout 的真实 `src/bthread`、`src/brpc` 路径 |
| 2026-07-15 | 合并更新规划记录的 patch 上下文未匹配 | 1 | 主文档未受影响，拆成按文件小补丁后成功 |
| 2026-07-15 | 微服务章节 heading/path 校验脚本误报 | 1 | awk 初值和范围行号剥离不完整；修正 checker 后重跑，不改正文 |

## Session: 2026-07-15（微服务实践章节）

### Phase 8: 真实微服务使用模式分析
- **Status:** complete
- Actions taken:
  - 恢复已有 7 个阶段和主文档校验状态。
  - 将新需求拆为典型示例/API 核对、实践章节编写和静态校验两个阶段。
  - 核对 `echo_c++`、`cascade_echo_c++`、并发限制、HTTP、组合 Channel、Streaming、bvar 和 builtin services 的当前实现及文档。
  - 明确真实微服务的 Server/Client 双重角色、对象生命周期、同步/异步/fan-out 模式及公开 API 分层。
- Files created/modified:
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

### Phase 9: 新增微服务实践章节并校验
- **Status:** complete
- Actions taken:
  - 新增“真实微服务业务中的 brpc 使用方式”章节，覆盖进程启动、对象生命周期、IDL、Server/Client 骨架、级联调用、治理、IOBuf/TLS、观测和优雅退出。
  - 新增 3 个 PlantUML 图：业务组件摆放、级联调用运行时序、上线与优雅退出。
  - 将原源码阅读顺序、术语和最终结论顺延为第 15/16/17 章，并补充业务术语和最终边界总结。
  - 校验一级章节 0-17 连续、44 个 Markdown 代码块闭合、29 个 PlantUML 块各含一组 start/end。
  - 校验 47 个唯一源码/示例/文档路径全部存在，关键公开 API 在当前头文件和示例中命中。
  - 校验主文档及规划文件无尾随空白，`git diff --check` 无输出；环境无 PlantUML，未实际渲染。
- Files created/modified:
  - `docs/cn/brpc_runtime_architecture.md`
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

## 5-Question Reboot Check
| Question | Answer |
|----------|--------|
| Where am I? | Phase 9 已完成 |
| Where am I going? | 向用户交付新增章节和验证边界 |
| What's the goal? | 产出代码锚定、含 PlantUML 的完整中文 brpc 运行原理资料 |
| What have I learned? | 见 `findings.md` |
| What have I done? | 完成源码分析、资料扩写、26 个 PlantUML 图和静态校验 |

## Session: 2026-07-15（听众问题清单）

### Phase 10: 听众问题覆盖矩阵设计
- **Status:** complete
- Actions taken:
  - 恢复主文档、规划和研究结论。
  - 按主文档 0-17 章建立问题集覆盖范围，并加入 io_uring、故障反例和设计权衡追问。
  - 对照 client/server/IO/LB/HTTP/Streaming/组合 Channel/RDMA/io_uring 文档目录，补齐主链之外的公开能力。
  - 将问题分为基础、源码实现、运行时边界、生产治理和场景挑战五种层次。

### Phase 11: 输出全面问题清单
- **Status:** complete
- Actions taken:
  - 形成按整体架构、Server/Client、协议/I/O、bthread、IOBuf、TLS、治理、RDMA、io_uring、rdma_performance、高级能力和生产实践分组的问题清单。
  - 每组同时包含概念题、代码路径题、执行上下文题、数据变化题和异常/性能追问。
  - 增加听众常用反例和压迫式追问，便于演练时暴露模糊表述。
- Files created/modified:
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

## Session: 2026-07-17（全量运行总图）

### Phase 12: 全量运行总图覆盖设计
- **Status:** complete
- Actions taken:
  - 恢复 11 个已完成阶段和主文档上下文。
  - 将“所有组件”限定为所有核心运行时、公开治理和 transport 组件，避免把辅助类逐一展开导致单图不可读。
  - 计划用颜色区分主请求、响应、bthread、IOBuf、可靠性和生命周期，用编号串联跨子系统流程。
  - 核对核心组件和关键函数入口，完成 9 个分区及跨区流程设计。
  - 新建独立 PlantUML 初稿，覆盖 189 条显式流程关系。
- Files created/modified:
  - `docs/cn/brpc_complete_runtime_flow.puml`
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

### Phase 13: 编写独立 PlantUML 总图
- **Status:** complete
- Actions taken:
  - 完成请求/响应主链、bthread 生命周期、IOBuf 全流程、TCP/epoll 收发、RDMA/io_uring、完成语义和治理观测分区。
  - 完成别名唯一性、箭头端点、花括号和尾随空白初检。
  - 在主文档新增“全量运行总图”章节，提供分区/编号索引、推荐阅读顺序和 SVG 渲染命令。
  - 逐段人工复核数据流与控制流，修正消息解析到派发方向、独立 response pack 节点和 Server Stop/Join 闭环。
- Files created/modified:
  - `docs/cn/brpc_complete_runtime_flow.puml`
  - `docs/cn/brpc_runtime_architecture.md`

### Phase 14: 总图静态校验与交付
- **Status:** complete
- Actions taken:
  - 完成首轮 marker、别名、括号、流程编号、关键节点和文档结构校验。
  - 确认当前环境没有 PlantUML 命令或可用 jar，实际渲染不可执行。
  - 最终验证 1/1 marker、4/4 notes、1/1 legend、10/10 braces，124 个别名无缺失/重复，197 条箭头端点全部有效。
  - 验证 bthread、IOBuf、transport 和 completion 编号连续，关键语义组件全部命中。
  - 验证主文档一级章节 0-18 连续、46 个代码块闭合、引用路径有效且无尾随空白。
- Files created/modified:
  - `docs/cn/brpc_complete_runtime_flow.puml`
  - `docs/cn/brpc_runtime_architecture.md`
  - `task_plan.md`
  - `findings.md`
  - `progress.md`
- Files created/modified:
  - `task_plan.md`
  - `findings.md`
  - `progress.md`
## 2026-07-17：总图纯 TCP/epoll 收敛

- 已确认修改范围为 `docs/cn/brpc_complete_runtime_flow.puml` 和主文档第 15 章读图说明。
- 已删除 RDMA/io_uring 分区、组件、箭头和专属术语，并将后续分区重编号为 6、7。
- IOBuf 输出适配改为 `BlockRefs -> iovec[]`，Socket 工厂只连接 `TcpTransport`，消息批处理说明只保留 TCP consumer 内联与 `bthread_flush()`。
- 总图现有 8 个分区、108 个别名和 168 条箭头；别名定义、箭头端点、brace、note、legend、marker 和编号均通过静态校验。
- 严格残留扫描和尾随空白扫描无结果；主文档仍为 0-18 共 19 个一级编号章节、46 对代码 fence。
- 本机无 `plantuml` 命令或 jar，因此未实际渲染 SVG；本轮未修改任何 brpc 框架源码。
## 2026-07-17：总图简化

- 已确认凌乱的主要来源是 108 个节点和 168 条箭头同时平铺，而不是 TCP 主链本身。
- 已将总图重构为一条端到端主链和四个低耦合子流程，细节通过节点内步骤保留。
- 主链使用 1-13 表达一次请求/响应；其余视图区分别表达 IOBuf、TCP/epoll、bthread 和初始化/可靠性。
- 总图从 387 行降至 218 行，别名从 108 降至 71，可见箭头从 168 降至 62。
- 静态校验通过：69 条箭头端点均有唯一别名定义，brace、note、legend 和 marker 成对，无非 TCP 专属术语和尾随空白。
- 主文档第 15 章已改为 5 个视图区和新的分层阅读顺序；0-18 章及 46 对 Markdown fence 保持完整。
- 本机无 PlantUML 命令或 jar，未实际渲染 SVG；框架源码未修改。
## 2026-07-18：rdma_performance 单请求数据演化图

- 已确定在主文档第 11.7 节嵌入一张专项 PlantUML 图，不修改简化总图。
- 已核对示例 proto、client、server、baidu_std pack/parse、TCP writev/readv、InputMessenger 消息批处理和 bthread 上下文。
- 已明确 1KB 指 1024 字节 attachment，而不是完整 PRPC 帧或单个 TCP segment。
- 已写入专项时序图、12 阶段数据形态表和源码对应关系；正在执行 PlantUML、章节、语义、路径和空白校验。
- 首轮语义校验发现图中只有 EPOLLIN/EPOLLOUT、缺少字面 `epoll_wait`，已补成 client/server `EventDispatcher::epoll_wait -> EPOLLIN`。
- 最终静态校验通过：新图 8 个 participant、2 个 alt、1 个 loop、12 个 note 均配对，12 阶段表格完整，所有参与者引用有定义。
- 主文档现有 30 个 PlantUML block、47 对 Markdown fence、0-18 共 19 个编号章节；关键 TCP/IOBuf/bthread/EndRPC 术语全部命中，无 RDMA 数据面机制、尾随空白或 `git diff --check` 问题。
- 本机仍无 PlantUML 命令或 jar，未实际渲染；简化总图和 brpc 框架源码均未修改。

## Session: 2026-07-20（TCP 运行机制精要版）

### Phase 21: TCP 精要版源码复核
- **Status:** in_progress
- Actions taken:
  - 完整读取 `planning-with-files` 说明，恢复现有 20 个阶段及研究记录。
  - session-catchup 因暂不支持 Codex session 格式而跳过；已直接核对规划文件和工作区状态。
  - 确认当前 HEAD 为 `89b2765ca51659494b2b2f5028fdd87aa3df5992`，与计划基线一致。
  - 确认本轮只新增精要文档和双向链接，不改框架源码或简化总图。
  - 完成示例/baidu_std、IOBuf、bthread、Socket/epoll、Acceptor/Server 的符号级入口扫描，关键路径与既有完整资料一致。
  - 逐段复核请求打包/切帧、IOPortal readv、TaskMeta 创建与 local/remote 入队、TLS 保存恢复和 lazy stack 分配实现。
  - 新建精要文档首稿并在完整资料开头增加入口；首稿结构为 4 章、6 图，但 1019 行超过 900 行目标，进入压缩复核。
  - 压缩图内空行与重复 note 后，文档收敛为 886 行、3905 词；4 个主章节、6 个 PlantUML block 和 28 条 Markdown fence 均满足计划结构。
- Files created/modified:
  - `docs/cn/brpc_tcp_runtime_guide.md`
  - `docs/cn/brpc_runtime_architecture.md`
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

### Phase 22: 编写 TCP 运行机制精要文档
- **Status:** complete
- Actions taken:
  - 使用同一 1KB TCP 场景完成端到端、IOBuf、bthread、连接生命周期四章。
  - 增加 6 张 PlantUML 图：端到端时序、IOBuf 对象结构、复制边界、bthread 生命周期、调度接力、连接全生命周期。
  - 增加 12 阶段数据表、IOBuf 复制矩阵、网络系统调用形态表和源码阅读顺序。
  - 完成完整资料与精要版的双向链接。

### Phase 23: 静态校验与交付
- **Status:** complete
- Actions taken:
  - 最终文档为 887 行、3913 词，满足 650–900 行目标；4 个编号主章节连续。
  - 6 个 PlantUML block 均为单一 start/end，时序图的 alt/loop/end 与状态/类图 brace 全部配对。
  - 28 条 Markdown fence 成对；20 个显式源码/示例引用路径全部存在。
  - request/response 长度公式、BlockA/S/C、IOBuf/iovec、writev/readv、epoll、KeepWrite、NOSIGNAL/flush、version/回收、EndRPC、连接关闭等关键语义均命中。
  - 非 TCP 数据面术语扫描、尾随空白扫描和 `git diff --check` 均无输出。
  - 当前环境没有 `plantuml` 命令或 jar，因此未实际生成或目视检查 SVG。
- Validation results:
  - 文档结构：通过。
  - PlantUML 静态结构：通过。
  - Markdown fence：通过。
  - 引用路径：通过。
  - TCP/epoll 范围：通过。
  - 尾随空白/patch：通过。
  - PlantUML 实际渲染：未执行（工具不可用）。

### Phase 24: 明确端到端图的参考基准
- **Status:** complete
- Actions taken:
  - 在精要版第 1.2 节增加到完整资料第 11.7.1 节的直接链接。
  - 明确图 1 的场景参数、帧长度、复制边界和 bthread 分支以该节为事实基准。
  - 保持 PlantUML 图内容、完整资料正文和简化总图不变。

### Phase 25: 重构端到端图的 Client/Server 前后时序
- **Status:** complete
- Actions taken:
  - 确认当前精要图把 client completion participant 放在 server 之后，导致 client/server 视觉边界不清晰。
  - 读取完整资料 11.7.1 的原图，决定恢复 Client process / TCP / Server process 三段布局和 1–12 步编号。
  - 将 client main/Channel/Socket/input consumer 集中到左侧蓝色 box，将 server Socket/input consumer/Service 集中到右侧绿色 box，TCP 字节流位于中间。
  - 请求链按 client -> TCP -> server 从左到右，响应链按 server -> TCP -> client 从右到左闭环。
  - 保留 writev/KeepWrite、完整帧等待、NOSIGNAL/flush、BlockA/S/C 和 EndRPC/callback 语义。
  - 最终文档 871 行；6 个 PlantUML block 仍完整，图 1 的 2 个 box、3 个 alt/loop 与 end 均配对，无尾随空白。

### Phase 26: 扩写 IOBuf 数据结构与 TLS 交互
- **Status:** complete
- Actions taken:
  - 将修改范围限定为精要文档第 2 章及规划记录，不修改框架源码和其他图。
  - 计划区分 IOBuf 使用的 pthread-local block cache 与 bthread local storage，避免把缓存归属误写成 payload 所有权。
  - 已复核 SmallView/BigView/BlockRef、pthread-local TLSData、share/acquire/release block 和 bthread `tls_bls` 切换实现。
  - 已扩写图 2 数据结构表、复制矩阵 TLS 列、BlockA/S/C 的 TLS 来源、server readv 典型流程、缓存生命周期和两类 TLS 对照表。
  - 最终文档 899 行、4173 词，保持 4 个主章节、6 个 PlantUML block 和 28 条成对 Markdown fence。
  - TLS 关键符号、4 个新增源码路径、PlantUML 控制块、尾随空白和 `git diff --check` 均通过。

### Phase 27: 扩写图 4 的 bthread 生命周期详解
- **Status:** complete
- Actions taken:
  - 将修改范围限定为精要文档 3.2 节及规划记录，不修改 bthread 源码和 PlantUML 图数量。
  - 根据新增详细度要求，将文档上限由 900 行调整为 1000 行。
  - 已复核 API 分流、foreground/background 创建、队列/唤醒、ParkingLot、steal、sched_to、task_runner、sleep/yield/join 和退出回收实现。
  - 在图 4 后新增 16 阶段源码动作表，覆盖创建、身份、队列、唤醒、选取、栈、切换、等待、恢复和回收。
  - 新增 10 条关键细节，解释 queue/signal、READY/RUNNING/SUSPENDED、remained callback、阻塞域、协作式 interrupt 和 join 可见性。
  - 最终文档为 933 行、4521 词；4 个主章节、6 个 PlantUML block、28 条 Markdown fence保持完整，无尾随空白。

### Phase 28: 补充图 4 生命周期整体文字说明
- **Status:** complete
- Actions taken:
  - 确认现有内容以逐项表格为主，缺少从创建到回收的连续文字叙事。
  - 计划在表格前新增生命周期总览，不删除原有源码映射和技术细节。
  - 新增 CREATED -> READY -> RUNNING -> READY/SUSPENDED/END -> RECYCLED 的整体状态主线。
  - 用连续文字解释 TaskMeta/worker 分工、队列与唤醒、lazy stack/TLS 切换、等待恢复和两阶段回收。
  - 增加 `rdma_performance` 中 RunTest 结束后由 client input consumer/callback 接力的实际映射。
  - 最终文档为 951 行、4687 词，4 章、6 图和 30 条 Markdown fence 均完整，无尾随空白。

### Phase 29: 新增第 2 章独立 TLS 机制说明
- **Status:** complete
- Actions taken:
  - 决定把第 2.6 节中的两类 TLS 对照表移入新的独立小节，并补充完整文字说明。
  - 修改范围限定为第 2 章和规划记录，不修改框架源码或 PlantUML 图。
  - 新增 TLS 设计目的、五步缓存生命周期、引用计数边界、bthread 迁移行为和两类 TLS 对照。
  - 将原 2.7/2.8 顺延为 2.8/2.9，源码入口与正文引用保持有效。
  - 最终文档为 962 行、4766 词；4 章、6 图、30 条 Markdown fence完整，无尾随空白。
- Files created/modified:
  - `task_plan.md`
  - `progress.md`
  - `findings.md`

### Phase 30: 简化并整齐化图 4
- **Status:** complete
- Actions taken:
  - 将图 4 的多层嵌套源码节点收敛为 7 个生命周期状态，形成纵向主链。
  - 保留 background/urgent 分流、yield、等待/唤醒、逻辑结束和切栈后物理回收。
  - 把 TaskMeta、队列、NOSIGNAL、ParkingLot、lazy stack、TLS 切换等细节归入 5 个状态注释。
  - 同步把 16 阶段表的“图中节点”更新为新状态名，并在文字状态摘要中加入 SCHEDULING 与 urgent 跳转。
  - 静态检查确认图 4 的 start/end 和 5 组 note/end note 配对，全文 6 个 PlantUML block、30 条 Markdown fence 完整，无尾随空白。
  - 当前环境仍无 PlantUML renderer，因此未实际生成 SVG。
- Files created/modified:
  - `docs/cn/brpc_tcp_runtime_guide.md`
  - `task_plan.md`
  - `progress.md`
  - `findings.md`
