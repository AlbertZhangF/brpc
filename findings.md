# Findings & Decisions

## Requirements
- 读取brpc框架所有代码，理解底层IO机制
- 梳理write/read与epoll/socket的交互流程
- 分析端到端的收发流程（客户端和服务端）
- 绘制模块关系图和时序图
- 补充到brpc_architecture_and_workflow.md文档

## Research Findings

### 已知架构信息
- brpc采用分层架构：butil(基础工具) -> bthread(协程) -> bvar(统计) -> brpc(RPC核心)
- example/rdma_performance示例已分析，包含TCP和RDMA两种模式
- 服务端每次请求创建bthread处理，客户端发送线程复用bthread

### 已发现核心文件
- **EventDispatcher** (`src/brpc/event_dispatcher.h/cpp`): epoll/kqueue的封装类
  - AddConsumer(): 注册fd到epoll，监听EPOLLIN事件
  - RegisterEvent(): 注册EPOLLOUT事件
  - UnregisterEvent(): 取消EPOLLOUT事件
  - 使用IOEventData封装回调函数
  
- **Socket** (`src/brpc/socket.h/cpp`): 核心IO抽象类
  - 管理文件描述符的生命周期
  - Write(): 写入数据，支持零拷贝IOBuf
  - OnInputEvent(): 处理输入事件的静态回调
  - 使用版本化引用计数管理生命周期
  
- **InputMessenger** (`src/brpc/input_messenger.h/cpp`): 输入消息处理器
  - OnNewMessages(): 核心方法，从socket读取数据并处理
  - CutInputMessage(): 切割消息
  - ProcessNewMessage(): 处理新消息
  
- **IOEventData**: IO事件数据封装
  - input_cb: 输入事件回调
  - output_cb: 输出事件回调
  - user_data: 用户数据

### 核心流程已梳理

#### EventDispatcher事件循环机制
- 使用epoll_create创建epoll实例
- Run()函数循环调用epoll_wait等待事件
- 分离处理EPOLLIN和EPOLLOUT事件
- 使用IOEventData封装回调，支持input_cb和output_cb
- 全局EventDispatcher数组，支持多tag和多dispatcher

#### Read流程核心函数调用链
1. epoll_wait返回EPOLLIN事件
2. CallInputEventCallback() -> Socket::OnInputEvent()
3. bthread_start_urgent()创建ProcessEvent bthread
4. ProcessEvent() -> _on_edge_triggered_events() (即InputMessenger::OnNewMessages)
5. Socket::DoRead() -> _read_buf.append_from_file_descriptor()
6. InputMessenger::ProcessNewMessage() -> CutInputMessage()
7. 协议解析 -> 创建bthread处理请求

#### Write流程核心函数调用链
1. Socket::Write() -> StartWrite()
2. 原子操作exchange获取写权限
3. 如果无竞争：直接write一次
4. 如果有竞争或未写完：创建KeepWrite bthread
5. KeepWrite()循环：DoWrite() -> cut_multiple_into_file_descriptor()
6. 如果socket缓冲区满：WaitEpollOut()等待EPOLLOUT事件
7. 写完成后返回成功的WriteRequest

#### 零拷贝机制
- butil::IOBuf: 非连续内存缓冲区，支持零拷贝append/cut
- append_from_file_descriptor(): 直接从fd读取到IOBuf
- cut_into_file_descriptor(): 直接从IOBuf写入fd
- cut_multiple_into_file_descriptor(): 批量写入多个IOBuf

#### WriteRequest队列机制
- 原子链表实现无锁写入队列
- UNCONNECTED标记用于同步
- KeepWrite线程负责批量写入
- 支持pipelined_count实现流水线

### 待探索内容
- acceptor连接建立流程
- 客户端连接建立流程
- RDMA模式的差异

## Technical Decisions
| Decision | Rationale |
|----------|-----------|
|          |           |

## Issues Encountered
| Issue | Resolution |
|-------|------------|
|       |            |

## Resources
- brpc源码位置: /home/zfz/code/brpc/apache-brpc-1.15.0-src/src/
- 示例代码: example/rdma_performance/
- 已有文档: brpc_architecture_and_workflow.md
- CLAUDE.md: 项目概览和构建指南

## Visual/Browser Findings
- 暂无

---
*Update this file after every 2 view/browser/search operations*