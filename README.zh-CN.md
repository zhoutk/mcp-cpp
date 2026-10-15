> 语言 / Language: **中文** | [English](README.md)

# mcp-cpp

[![CI](https://github.com/zhoutk/mcp-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/zhoutk/mcp-cpp/actions/workflows/ci.yml)
[![Conformance](https://img.shields.io/badge/conformance-26%20of%2030-blue)](conformance/report.json)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://en.cppreference.com/w/cpp/17)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)


**一个独立的、以符合规范为导向的 C++17 SDK，用于
[Model Context Protocol](https://modelcontextprotocol.io)。**

mcp-cpp 实现了 MCP **2026-07-28** 规范，具备 Tier-1 SDK 的功能覆盖、值语义的
消息模型，且无需异步运行时。它仅有的依赖是一个随附的单头文件 JSON 库（zjson），
以及——可选的——用于 Streamable HTTP 传输的 cpp-httplib。目前**没有官方 C++
SDK**；本项目即是作为其中之一而提供的（见 [ROADMAP.md](ROADMAP.md)）。

```
CMake ≥ 3.18 · C++17 · MIT · no external deps beyond a bundled JSON header
```

**状态：** v0.1.0，按 [PLAN.md](PLAN.md) §5 功能已完备。针对官方一致性测试套件
（v0.1.16），它通过了 **26/30 个计分的服务端场景（86.7%）**；其余四个需要服务器
在调用中途向客户端*请求*某些东西，而 2026-07-28 已用 MRTR 取代了该机制——参见
[docs/CONFORMANCE.md](docs/CONFORMANCE.md) 和
[`conformance/report.json`](conformance/report.json)。注意该套件目前尚无
2026-07-28 场景。公开的路线图见 [ROADMAP.md](ROADMAP.md)。

---

## 为什么选择 mcp-cpp

- **无需安装任何东西。** 一棵内嵌了 JSON 头文件的 C++17 源码树——用
  `add_subdirectory`、`FetchContent`，或 `find_package(mcp CONFIG)` 即可。无需
  Boost，无需异步框架，无需代码生成。
- **协议层采用值语义且不抛异常。** 消息就是普通的结构体，结果是 `McpResult<T>`
  （当标准库提供 `std::expected` 时即为其别名），解码器从不抛出异常。
- **忠于协议，而非只带点协议味道。** 严格的 JSON-RPC 2.0（禁止 null-id、
  不支持批处理、整数 id 保真至 ±2³），完整的 2026-07-28 无状态模型
  （每请求 `_meta`）、`server/discover`、`subscriptions/listen`、MRTR、进度、
  取消、CacheableResult，以及带 `-32020` 强制校验的镜像 HTTP 头。
- **自我保护。** 一个 JSON Schema 2020-12 校验器（仅本地 `$ref`、资源边界、
  不支持远程 ref）在每次调用时强制执行工具的 `inputSchema`/`outputSchema`。
- **经过验证。** 它是两个完整应用的协议层：
  [zorm-mcp](https://github.com/zhoutk/zorm-mcp)（一个多后端数据库 MCP
  服务器）和 [zorm](https://github.com/zhoutk/zorm)（它所构建于其上的 ORM）。

## 功能矩阵（对比官方 Tier-1 SDK）

| 功能 | mcp-cpp | Tier-1 对等 |
| --- | --- | --- |
| JSON-RPC 2.0 严格编解码（禁止 null-id、不支持批处理、对象参数） | ✅ | ✅ |
| 协议版本 2024-11-05 … 2026-07-28 + `-32022` 协商 | ✅ | ✅ |
| `server/discover`（supportedVersions / capabilities / ttl / cacheScope / serverInfo） | ✅ | ✅ |
| 无状态的每请求 `_meta`（protocolVersion / clientCapabilities / clientInfo） | ✅ | ✅ |
| stdio 传输（换行分帧、Windows 二进制模式、EOF 关闭） | ✅ | ✅ |
| Streamable HTTP（仅 POST、头部镜像、`Mcp-Param-*`、202/404/405、`-32020`） | ✅ | ✅ |
| SSE 响应流 + `subscriptions/listen`（ack、按过滤条件扇出、优雅关闭、保活） | ✅ | ✅ |
| tools / resources / prompts 注册表、能力门控、分页游标 | ✅ | ✅ |
| 资源 URI 模板（RFC 6570 子集）+ 模板回退式 `resources/read` | ✅ | ✅ |
| `notifications/cancelled` + 可查询的取消标志 | ✅ | ✅ |
| MRTR：`input_required` 结果 + `inputResponses` 重试、`requestState` 回显、带能力门控的 `Ctx::elicit`/`Ctx::inputRequired` | ✅ | ✅ |
| 签名 `requestState`（`RequestStateCodec`：HMAC-SHA256、绑定上下文、TTL）+ `Client::callToolWithInput` 自动履行循环 | ✅ | ✅ |
| 官方一致性套件——**服务端模式**（2026-07-28：42/42 项检查） | ✅ | ✅ |
| 官方一致性套件——**客户端模式**（2026-07-28：32/32 场景，含全部 25 个 `auth/*`） | ✅ | ✅ |
| OAuth 2.1 客户端：PRM + AS 元数据发现、DCR / CIMD / 预注册、PKCE（S256）、RFC 8707 `resource`、RFC 9207 `iss`、scope 升级、SEP-2352 迁移 | ✅ | ✅ |
| 进度通知（`Ctx::progress`，客户端通过 `progressToken` 选择性加入） | ✅ | ✅ |
| `completion/complete`（服务端 + 客户端） | ✅ | ✅ |
| discover、`*/list`、`resources/read` 上的 CacheableResult（`ttlMs`/`cacheScope`） | ✅ | ✅ |
| JSON Schema 2020-12 校验器；强制执行工具的 `inputSchema` **与** `outputSchema` | ✅ | ✅ |
| `x-mcp-header` 镜像（客户端）+ 服务端校验 | ✅ | ✅ |
| 结构化结果（`structuredResult()` + 规范推荐的文本块） | ✅ | ✅ |
| 2026-07-28 一致性测试套件（仓库内，`mcp_test_conformance_2026`） | ✅ | n/a |
| CMake 包（`find_package(mcp CONFIG)`，导出 `mcp::mcp`） | ✅ | n/a |
| Roots / Sampling（已被 SEP-2577 弃用） | ❌ 有意不支持 | Tier-1 保留兼容垫片 |
| Tasks 扩展（`io.modelcontextprotocol/tasks`） | ⏳ 路线图 | 扩展 |

值语义的消息模型、不抛异常的协议层、带 `Ok`/`Err` 工厂的 `[[nodiscard]]`
结果类型——无论 `McpResult<T>` 是 `std::expected` 的别名还是使用内置回退，
其读法都完全一致。

## 基准测试

`RelWithDebInfo`，独立构建，clang 22.1.8（MSYS2 clang64，windows-gnu），
两者使用同一台机器和相同的消息形态：

| 基准 | mcp-cpp | serde_json（rust-sdk 的解析器） |
| --- | --- | --- |
| B1 解析约 1 KB 请求 | 714,000 msg/s (1.40 µs) | 907,000 msg/s (1.10 µs) |
| B1 解析约 64 KB 请求 | 35,900 msg/s (27.8 µs) | 136,800 msg/s (7.31 µs) |
| B2 `tools/call` 分发往返（门控 + schema 校验 + 编码） | 298,500 msg/s (3.35 µs) | n/a（需要完整的 tokio 栈） |
| B3 stdio 端到端往返（真实管道、子进程服务器） | 24,700 msg/s (40.4 µs) | n/a |

**诚实的解读**（PLAN.md 更新 8）：原始 JSON 解析**并未**胜过 serde_json——
zjson 的解析即拷贝模型加上严格的语义校验，其开销高于 serde 的零拷贝借用式解析，
在 64 KB 时尤为明显。优化路径记录在
[ROADMAP.md](ROADMAP.md)。B2/B3 的数字是全栈的
（包含校验）并反映了应用实际体验到的性能。

## 安装

### CMake `find_package`

```bash
cmake --preset clang-rlz
cmake --build build -j 8
cmake --install build --prefix /your/prefix
```

```cmake
find_package(mcp CONFIG REQUIRED)
target_link_libraries(my_server PRIVATE mcp::mcp)
```

### FetchContent / add_subdirectory

```cmake
add_subdirectory(third_party/mcp-cpp)
target_link_libraries(my_server PRIVATE mcp::mcp)
```

独立配置需要在 `thirds/zjson` 下有一份 zjson 副本，或传入
`-DMCP_ZJSON_DIR=<dir>`。选项：`MCP_BUILD_TESTS`（ON）、`MCP_BUILD_EXAMPLES`（ON）、
`MCP_DISABLE_HTTP`（OFF——纯 stdio 构建）、`MCP_INSTALL`（ON）。

> vcpkg port 和 Conan recipe 已在路线图上（[ROADMAP.md](ROADMAP.md)）。

## 快速开始

### 服务端

```cpp
#include <mcp/mcp.hpp>

int main() {
  mcp::server::Builder b;
  b.name("my-server").version("1.0");

  b.tool(mcp::Json(R"({
        "name": "add",
        "description": "adds two integers",
        "inputSchema": {
          "type": "object",
          "properties": { "a": {"type":"integer"}, "b": {"type":"integer"} },
          "required": ["a","b"]
        }
      })"),
      [](mcp::server::Ctx& c) -> mcp::McpResult<mcp::Json> {
        const auto& args = c.params["arguments"];
        c.progress(1, 1, "computing");          // no-op unless the client opted in
        return mcp::Ok(mcp::server::structuredResult(mcp::Json(R"({"sum":0})")));
      });

  // A parametric resource: one template serves every db/table URI, including
  // tables created after the server started.
  b.resourceTemplate(
      mcp::Json(R"({"uriTemplate":"db://schema/{db}/{table}","name":"table schema"})"),
      [](mcp::server::Ctx& c) -> mcp::McpResult<mcp::Json> {
        std::vector<std::pair<std::string, std::string>> vars;
        mcp::server::matchUriTemplate("db://schema/{db}/{table}",
                                      c.params["uri"].toString(), &vars);
        return mcp::Ok(mcp::server::textResult("...columns for " + vars[1].second));
      });

  auto server = b.build();
  mcp::StdioTransport transport;
  return server->run(transport).has_value() ? 0 : 1;
}
```

对于 Streamable HTTP，改为使用 `mcp::http::HttpServerTransport` 构建——同样的
`Server`，不同的传输。

### 客户端

```cpp
mcp::client::Client client("probe", "1.0");
client.connect(transport);                       // any ITransport
client.onNotification([](const mcp::JsonRpcNotification& n) { /* progress, ... */ });

auto tools = client.listTools();
auto r = client.callTool("add", mcp::Json(R"({"a":2,"b":3})"), mcp::Json(nullptr),
                         mcp::RequestId(std::string("progress-1")));
```

要与 **pre-2026** 服务器通信，改用旧版握手——它采用协商出的版本，并以
`notifications/initialized` 收尾：

```cpp
client.onServerRequest([](const std::string& method, const mcp::Json& params,
                          const mcp::RequestId&) -> mcp::McpResult<mcp::Json> {
  if (method == "elicitation/create") return mcp::Ok(/* ... */);
  return mcp::Err(mcp::McpErrorCode::MethodNotFound, "unsupported");
});
client.initialize("2025-11-25", mcp::Implementation{"probe", "", "1.0", "", "", mcp::Json(nullptr)});
```

对于受 OAuth 保护的 HTTP 服务器，`mcp::oauth::OAuthClient` 可完成整套
授权码 + PKCE 流程（发现、DCR/CIMD、`resource`、`iss`）：

```cpp
mcp::oauth::Config cfg;
cfg.resourceUrl = "https://host/mcp";
cfg.redirectUri = "http://localhost:3000/callback";
cfg.clientMetadataUrl = "https://my.app/client-metadata.json";  // CIMD
mcp::oauth::OAuthClient auth(cfg);
auth.discover(challengeFrom401);                    // WWW-Authenticate
auto url = auth.authorizationUrl({});                // scopes，或 {} 表示推导
auth.authorizeAndExchange(*url);                     // GET -> Location -> token
transport.setHeader("Authorization", "Bearer " + auth.accessToken());
```

## 生态——用 mcp-cpp 构建

- **[zorm-mcp](https://github.com/zhoutk/zorm-mcp)**——一个生产级 MCP 服务器，
  通过一套无方言差异的工具接口，在五种后端（sqlite3 / jsonfile /
  mysql / postgres / dm8）之上暴露 13 个结构化数据库工具。
- **[zorm](https://github.com/zhoutk/zorm)**——zorm-mcp 所构建于其上的 C++ ORM
  库（`zormlib`）；mcp-cpp 的第一个、也是要求最严苛的使用者。

两者都是完整的应用：mcp-cpp 提供协议层，它们提供领域。

## 文档

| 文档 | 内容 |
| --- | --- |
| [PLAN.md](PLAN.md) | 设计与决策日志（M0–M9，含诚实的基准记录） |
| [ROADMAP.md](ROADMAP.md) | 迈向 Tier 1 的公开路线图 |
| [docs/ARCHITECTURE.zh-CN.md](docs/ARCHITECTURE.zh-CN.md) | 分层、线程模型与必须守住的不变量 |
| [docs/CONFORMANCE.zh-CN.md](docs/CONFORMANCE.zh-CN.md) | 运行官方 conformance 套件（双规范版本） |
| [docs/CONFORMANCE.md](docs/CONFORMANCE.md) | 运行官方一致性测试套件 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 构建、测试与依赖策略 |
| [CHANGELOG.md](CHANGELOG.md) | 发布历史 |

## 目录布局

```
include/mcp/   public headers (mcp.hpp aggregates everything)
src/           compiled implementation (oauth.cpp is the OAuth 2.1 client)
tests/         googletest unit tests (13 CTest registrations, incl. real
               subprocess stdio e2e and loopback HTTP/SSE e2e)
bench/         B1 parse / B2 dispatch / B3 stdio e2e benchmarks
examples/      minimal_server (Inspector-ready) + 两个官方一致性 fixture
               （conformance_server、conformance_client）
cmake/         package config for find_package(mcp CONFIG)
docs/          design, optimization, adoption and conformance docs
```

## 参与贡献

参见 [CONTRIBUTING.md](CONTRIBUTING.md)。安全问题报告：[SECURITY.md](SECURITY.md)。
参与行为受[行为准则](CODE_OF_CONDUCT.md)约束。

## 许可证

MIT——参见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE) 了解随附的第三方组件
（zjson、cpp-httplib、googletest）。
