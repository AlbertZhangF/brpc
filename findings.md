# 研究发现：TCP连接问题诊断

## 错误日志分析

### 客户端错误分类
| 错误码 | 含义 | 出现场景 |
|--------|------|----------|
| E110 | Connection timed out | TCP连接超时，服务端未响应SYN+ACK |
| E101 | Network is unreachable | 本地端口耗尽或路由不可达 |
| E112 | Not connected yet | Socket未建立连接就尝试发送请求 |

### 服务端错误
- `Connection reset by peer` - 客户端在连接建立前放弃，发送RST

## 根本原因分析

### R1: 连接风暴 (CRITICAL)
- 64线程 × 64 Channel = 4096个Channel
- 每个Channel在pooled模式下首次RPC时触发连接建立
- RunTest循环立即开始发送，所有线程同时发起连接
- 服务端listen backlog（默认128）无法容纳如此多SYN请求
- 超出backlog的SYN被丢弃→客户端超时

### R2: 缺少连接预热 (HIGH)
- Init()仅对Channel[0]做1次同步RPC
- 其余63个Channel的连接从未建立
- RunTest开始后，所有线程同时尝试建立连接
- 连接建立需要时间（TCP三次握手+brpc内部初始化）

### R3: Channel数量过多 (HIGH)
- 当前设计：每线程创建num_channels个Channel
- 64线程 × 64 Channel = 4096个Channel对象
- 每个Channel独立连接池，独立Socket管理
- 过多Channel导致资源浪费和连接管理复杂

### R4: bthread_concurrency配置问题 (MEDIUM)
- 运行命令中`--bthread_concurrency=160`但代码中`perf_bthread_concurrency=0`
- ApplyCommonFlags中0表示不设置，实际bthread_concurrency=8+2=10
- 10个工作pthread无法支撑64个发送bthread+回调处理

## brpc连接机制分析

### pooled连接建立流程
1. Channel::CallMethod() → 获取Socket
2. SocketPool::GetSocket() → 从池中取空闲Socket
3. 池空 → 创建新Socket → TCP connect()
4. 连接成功 → Socket::IsAvailable()=true
5. 连接未完成 → IsAvailable()=false → E112错误

### 关键发现
- Channel.Init()**不会**建立TCP连接，仅创建Socket元数据
- TCP连接在**首次RPC时**按需建立
- pooled模式下，连接池初始为空
- max_connection_pool_size控制池中最大空闲连接数，不是最大连接数

## 解决方案

### S1: 连接预热
- Init()中对所有Channel进行同步ping
- 确保所有Channel的连接在压测前建立完成
- 分批预热，避免瞬时大量连接

### S2: Channel共享优化
- 减少Channel数量，多线程共享Channel
- Channel是线程安全的，可被多bthread共享
- 使用较少Channel + pooled连接池实现高并发

### S3: bthread_concurrency正确设置
- 默认值设为CPU核数
- 确保ApplyCommonFlags正确生效

### S4: 渐进式启动
- 分批启动发送bthread
- 每批启动后短暂等待连接建立
- 避免所有线程同时发起连接
