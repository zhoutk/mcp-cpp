> 语言 / Language: **中文** | [English](ARCHITECTURE.md)

# 架构

mcp-cpp 的分层方式、线程模型，以及贡献者不可破坏的不变量。「为什么这样设计」见 [PLAN.md](../PLAN.md)（决策日志）。

## 分层

每层只依赖它之前的层：

```
error → version → json → jsonrpc → types → transport → server / client → schema / request_state / transport_http / oauth
```

`include/mcp/mcp.hpp` 是唯一的聚合入口；应用只包含这一个头。

| 文件 | 职责 |
| --- | --- |
| `include/mcp/error.hpp` | `McpErrorCode`（JSON-RPC 码 + MCP 保留区 `-32020…-32099`）、`McpResult<T>`（可用时别名 `std::expected`，否则内置 `Result`）、`Ok()`/`Err()` 工厂 |
| `include/mcp/version.hpp` | `ProtocolVersion` 枚举 ↔ wire 日期串——协议版本的唯一事实来源 |
| `include/mcp/json.hpp` | `mcp::Json` = 内置 zjson `Json` 的别名 |
| `include/mcp/jsonrpc.hpp` | 严格 JSON-RPC 2.0 编解码，含 MCP 增量（禁 null id、禁批量、params 必须为对象） |
| `include/mcp/types.hpp` | `RequestId`、`RequestMeta`、能力探针、`member()`、MRTR 辅助函数 |
| `include/mcp/transport.hpp` | `ITransport`、`StreamTransport`、`StdioTransport` |
| `include/mcp/server.hpp` | `Server` 派发循环 + `Builder` 注册 API |
| `include/mcp/request_state.hpp` | `RequestStateCodec`——HMAC 签名的 MRTR `requestState`（绑定上下文、带 TTL）；`src/sha256.*` 是零依赖的 SHA-256/HMAC 原语 |
| `include/mcp/client.hpp` | 阻塞式 `Client`，可运行于任意 `ITransport`；含 modern `request()`（每请求 `_meta`）、legacy `initialize()` 握手，以及应答 pre-2026 服务端发起请求的 `onServerRequest()` |
| `include/mcp/schema.hpp` | JSON Schema 2020-12 校验器（仅本地 `$ref`，带资源上限） |
| `include/mcp/transport_http.hpp` | Streamable HTTP + SSE 双侧实现（pimpl 把 httplib 挡在头文件外） |
| `include/mcp/oauth.hpp` | `mcp::oauth::OAuthClient`——面向远程 HTTP 服务器的 OAuth 2.1 客户端：PRM/AS 元数据发现、DCR/CIMD/预注册、PKCE、RFC 8707 `resource`、RFC 9207 `iss`、scope 升级、SEP-2352 迁移（与 `transport_http` 一样仅 HTTP 构建） |

**项目头文件之间互不包含**——跨模块使用只发生在 `.cpp` 内，这正是扁平 `include/` 目录能维持的原因。该包含方向属于评审约定（暂未自动化检查）。

## 线程模型

- **派发槽位（`MCP_WORKERS`）**。`Server::run()` 读一条消息、处理、回写。`processLine()` 等待 `MCP_WORKERS` 个槽位之一（用 mutex + 条件变量实现的计数信号量）；默认值 1 与历史上的串行模型完全一致。`N > 1` 时最多 N 个 handler 并发执行，handler 代码必须线程安全。
- **HTTP 服务端传输从 httplib 线程池派发**（默认 64 线程）。并发改变的是延迟，而非 handler 的顺序语义；handler 不得重入 `processLine()`（互斥量不可重入）。
- **取消通知绕过派发锁。** `notifications/cancelled` 在 `cancellationsMutex_` 下置位对应请求的 `shared_ptr<atomic_bool>`，因此长 handler 仍持有派发锁时取消也能生效。这是正确性要求而非时延优化：被取消的请求正是持锁者，排在派发锁之后的取消会死锁。若取消早于请求到达，则留下已置位的标志供请求接管。
- `subscriptions_` 有自己的互斥量（`subscriptionsMutex_`），因为通知扇出可能来自其他线程（HTTP SSE pump）。
- **服务器从不主动发起请求。** 服务端到客户端的交互只通过 `input_required` 结果（MRTR）承载；到达服务器的响应会被忽略。
- **但 pre-2026 服务器会。** 客户端对此仍能应答：对端发起的请求（sampling/elicitation/roots）交给 `Client::onServerRequest`，应答作为一条新消息发回；客户端循环随后继续等待自己的响应。
- 只有一个派发槽位时，阻塞的 handler 会阻塞所有连接；`MCP_WORKERS > 1` 时它只占用一个槽位。长任务应轮询 `Ctx::isCancelled` 并用 `Ctx::progress` 上报进度（取消也才能生效）。
- **HTTP 客户端传输自带线程。** 每个请求 POST 在 worker 线程上流式读取响应（响应可能是长寿命 SSE 流，同步读取会让客户端循环无法应答调用中途的 elicitation），GET 监听流还拥有自己的重连循环。回到 `read()` 的唯一通道是消息队列。三个句柄负责停止它们——`closeSubscriptions()`、`closeRequestStreams()`、`closeListenStreams()`（后两者仅析构）——析构函数会 join，确保没有线程比队列活得更久。
- **旧版 `initialize` 响应会先被扣住，直到读出其 session id。** 有状态的 pre-2026 服务器在该响应上发放 `Mcp-Session-Id`，没有它就会拒绝下一个请求；若在 worker 存下该头之前就把消息入队，`notifications/initialized` 会抢先发出。
- **`HttpClientTransport::setAuthChallengeHandler` 上报 401/403 的 `WWW-Authenticate` 质询**；传输层自身不做认证。这正是 `mcp::oauth`（以及一致性 fixture）所驱动的入口。
- **订阅是一条持久流，而不是一次 HTTP 请求。** 服务器每 5 秒发一次 SSE 注释 keep-alive
  （`HttpServerTransport::setSseKeepAlive` 可调整或关闭），因此订阅流的读超时为 30 秒（更短会在两次 keep-alive 之间拆掉空闲订阅）。流断开后客户端会重新订阅，遵循服务器 `retry:` 建议（钳制在 100 ms–30 s），并抑制重连带来的重复 ack，使调用方每次订阅只看到一次确认。**成功的非 SSE 应答**（`200` 但不是事件流，通常是 JSON-RPC 错误）会作为一条消息入队并终止流线程：对端拒绝后继续重订阅只会形成重连风暴。
- **`read()` 没有空闲超时。** `nullopt` 表示有序拆除（析构），错误表示连接丢失（GET 监听流无法重连）。静默连接是活的，因此慢请求不能被当成 EOF。目前没有每请求超时 API；需要上限的调用方应在外层自行控制。`closeSubscriptions()` 不会唤醒阻塞在 `read()` 中的线程。

## 请求生命周期（2026-07-28）

1. 解析为 `variant<Request, Notification, Result, ErrorResponse, InvalidMessage>`——解析从不失败，只分类。
2. 校验 `params._meta`：`protocolVersion` + `clientCapabilities` 必填；缺失或类型错误 → `-32602`。legacy 模式下，不完整的 `_meta` 用默认值服务。
3. 版本门：不支持 → `-32022`，`error.data` 携带支持列表。
4. 能力门（路由表）：未广播的能力 → `-32601`；未知的 tool/resource/prompt → `-32602`。
5. handler 执行；服务器在缺失时注入 `resultType:"complete"`（仅 modern 请求——pre-2026 schema 拒绝该字段），校验工具的 `inputSchema`/`outputSchema`，并把 handler 期间发出的通知（progress、logging）在结果之前流式送出——stdio 按行，HTTP 在多于一条消息时按 SSE 体。

## 贡献者不可破坏的不变量

- **协议读取一律用 `mcp::member(obj, key)`，绝不用 `obj[key]`**——zjson 的 `operator[]` 对缺失键会深搜嵌套层，误读嵌套协议数据。
- **文本值必须用 `Json::str(...)` 包裹**——zjson 会嗅探开头的 `{`/`[` 并按 JSON 解析。
- **用 `isNull()`/`isError()`/`isObject()` 探测存在性**，绝不用 `toString().empty()`——Null 渲染为字面字符串 `"null"`。
- **空的 `Mcp-Param-*` 镜像值也是值。** 存在但为空的参数必须作为头发送（服务端也据此校验）；`setHeader(name, "")` 是存储，只有 `removeHeader(name)` 才是删除。这正是两者必须分开的原因。
- **`version.txt` 与 `kSdkVersion` 保持同步。** `version.txt` 是唯一事实来源：`CMakeLists.txt` 读取它作为 `project(VERSION)`，`mcp_test_version` 断言 `kSdkVersion` 与 CMake 配置值一致。
- **安装包必须携带平台链接依赖。** httplib 以 `BUILD_INTERFACE` 链接（不进入导出），但它的平台库需以 `INTERFACE` 显式传递（Windows 上 `ws2_32`，POSIX 上 `Threads::Threads`）。不要删掉：否则 `find_package(mcp)` 的消费者链接 HTTP 传输会失败。
- **`thirds/` 的头文件不出现在公共头里**（只有 `mcp::Json` 到达 API 面），httplib 始终在 pimpl 之后。
- 各一致性套件（官方双版本 + 仓库内 2026-07-28 套件）必须保持绿色；分数记录在 `CHANGELOG.md`。

## 有意为之的简化

- 最多 `MCP_WORKERS` 个派发槽位（1 = 串行），无异步运行时。`processLine()` 在槽位上排队，因此 HTTP worker 池带来传输层并发，而 `MCP_WORKERS` 限定 handler 并发的上限。若将来需要每请求独立 `Server`（对齐官方 SDK 的工厂模型，兼顾隔离与并发），仍可作为后续选项。
- 长寿命服务器实例（每请求状态归 handler；MRTR 状态由 `requestState` 承载，`RequestStateCodec` 用 HMAC-SHA256 绑定方法与目标进行签名，回显的状态既不能重放到其他工具，也能在服务器重启后（密钥不变时）存活）。因此水平扩展需要有状态路由，与官方 SDK 的每请求工厂模型不同——这是已记录的权衡，有真实负载需求时再改。
- OAuth 仅在**客户端侧**实现（`mcp::oauth`，供一致性 fixture 使用）。服务器仍需在 HTTP 端点之前完成认证；SDK 不校验 bearer token。
