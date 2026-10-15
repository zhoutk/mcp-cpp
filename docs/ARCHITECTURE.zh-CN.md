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

**项目头文件之间互不包含**——跨模块使用只发生在 `.cpp` 内，这正是扁平 `include/` 目录能维持的原因。该包含方向有元测试机械检查。

## 线程模型

- **单线程派发**。`Server::run()` 读一条消息、处理、回写。handler 同步执行；`Server` 成员是普通成员，唯独 `subscriptions_` 有互斥锁，因为通知扇出可能来自其他线程（HTTP SSE pump）。
- **服务器从不主动发起请求。** 服务端到客户端的交互只通过 `input_required` 结果（MRTR）承载；到达服务器的响应会被忽略。
- **但 pre-2026 服务器会。** 客户端对此仍能应答：对端发起的请求（sampling/elicitation/roots）交给 `Client::onServerRequest`，应答作为一条新消息发回；客户端循环随后继续等待自己的响应。
- 阻塞的 handler 会阻塞整条连接。长任务应轮询 `Ctx::isCancelled` 并用 `Ctx::progress` 上报进度。
- **HTTP 客户端传输自带线程。** 每个请求 POST 在 worker 线程上流式读取响应（响应可能是长寿命 SSE 流，同步读取会让客户端循环无法应答调用中途的 elicitation），GET 监听流还拥有自己的重连循环。回到 `read()` 的唯一通道是消息队列。三个句柄负责停止它们——`closeSubscriptions()`、`closeRequestStreams()`、`closeListenStreams()`（后两者仅析构）——析构函数会 join，确保没有线程比队列活得更久。
- **旧版 `initialize` 响应会先被扣住，直到读出其 session id。** 有状态的 pre-2026 服务器在该响应上发放 `Mcp-Session-Id`，没有它就会拒绝下一个请求；若在 worker 存下该头之前就把消息入队，`notifications/initialized` 会抢先发出。
- **`HttpClientTransport::setAuthChallengeHandler` 上报 401/403 的 `WWW-Authenticate` 质询**；传输层自身不做认证。这正是 `mcp::oauth`（以及一致性 fixture）所驱动的入口。

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
- **`version.txt` 与 `kSdkVersion` 保持同步。**
- **`thirds/` 的头文件不出现在公共头里**（只有 `mcp::Json` 到达 API 面），httplib 始终在 pimpl 之后。
- 各一致性套件（官方双版本 + 仓库内 2026-07-28 套件）必须保持绿色；分数记录在 `CHANGELOG.md`。

## 有意为之的简化

- 单线程、无异步运行时。多 worker 保留为将来可能的编译期选项。
- 长寿命服务器实例（每请求状态归 handler；MRTR 状态由 `requestState` 承载，`RequestStateCodec` 用 HMAC-SHA256 绑定方法与目标进行签名，回显的状态既不能重放到其他工具，也能在服务器重启后（密钥不变时）存活）。因此水平扩展需要有状态路由，与官方 SDK 的每请求工厂模型不同——这是已记录的权衡，有真实负载需求时再改。
- 暂无 OAuth（见 `ROADMAP.md`）；HTTP 服务器对外时请在其前面做认证。
