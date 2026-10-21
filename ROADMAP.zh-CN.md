> 语言 / Language: **中文** | [English](ROADMAP.md)

# 路线图

这是 [MCP SDK Tiering System](https://modelcontextprotocol.io/community/sdk-tiers)
所要求的公开路线图。它记录了达成 **Tier 1**（100% 一致性）所需的具体工作。
如何运行各一致性套件见 [docs/CONFORMANCE.zh-CN.md](docs/CONFORMANCE.zh-CN.md)；
本路线图所演进的架构见 [docs/ARCHITECTURE.zh-CN.md](docs/ARCHITECTURE.zh-CN.md)。

图例：✅ 已完成 · 🔄 进行中 · ⏳ 计划中 · ❌ 不计划

## 里程碑 A —— 可采纳

使用者或评审者认真对待本项目所需的一切。

| 事项 | 状态 |
| --- | --- |
| CMake `install()` / `export` + `find_package(mcp CONFIG)` | ✅ |
| 双语 README + PLAN | ✅ |
| 进度通知、`outputSchema` 强制校验、结构化结果 | ✅ |
| 客户端封装（`listResources`、`listPrompts`、`complete`） | ✅ |
| `ROADMAP.md`、`CONTRIBUTING.md`、`SECURITY.md`、`CODE_OF_CONDUCT.md`、`CHANGELOG.md` | ✅ |
| `.github/` issue + PR 模板、分类标签 | ✅ |
| GitHub Actions CI 矩阵（Linux/macOS/Windows × clang/gcc/msvc），另含 TSan、消毒器、install-consumer、C++23 与纯 stdio 任务 | ✅ |
| vcpkg port + Conan recipe (`packaging/vcpkg`, `packaging/conan`; overlay/local build, not yet in the registries) | ✅ |

## 里程碑 B —— 符合规范（Tier 2）

| 事项 | 状态 |
| --- | --- |
| 运行官方 `modelcontextprotocol/conformance` 服务端套件 | ✅ 2026-07-28：**42/42（100%）** · 2025-11-25：26/30 |
| 达到所有适用必需场景的 100% | ✅ 2026-07-28 · 🔄 2025-11-25（4 个受阻，见下文） |
| 发布 `conformance/report.json` + 一份 `--expected-failures` 基线 | ✅ |
| 客户端模式的一致性运行（`examples/conformance_client.cpp`） | ✅ 2026-07-28：**32/32 场景（100%）**；2025-11-25：17/18（另加两个 2025-03-26 legacy 场景；上游修复待正式发布） |
| 客户端模式 `--expected-failures` 基线 | ✅ 仅 1 个套件 mock 缺陷场景，附 rust-sdk A/B 证据 |
| 仓库内 **2026-07-28** 一致性测试套件（`mcp_test_conformance_2026`，12 个场景，含完整的 MRTR 往返） | ✅ |
| 稳定的 **v1.0.0** 发布 | ⏳ |

其余四个服务端失败（`tools-call-sampling`、
`tools-call-elicitation`、`elicitation-sep1034-defaults`、
`elicitation-sep1330-enums`）都需要服务器在调用中途向客户端发送请求。
2026-07-28 规范已用 MRTR 取代该机制，mcp-cpp 实现了 MRTR；该套件没有 2026
场景可用来替代计分。

## 里程碑 C —— 具备 Tier-1 就绪条件

| 事项 | 状态 |
| --- | --- |
| OAuth 2.1 客户端 + Client ID Metadata Documents（远程 HTTP） | ✅ 客户端侧（PRM/AS 发现、DCR/CIMD/预注册、PKCE、scope 升级、SEP-2352）· ✅ 服务端侧：`mcp::oauth::AuthorizationServer`（面向本地回环/开发/测试的内存核心：DCR、授权码 + PKCE S256、RFC 8707 绑定、元数据） |
| 强类型 MRTR / elicitation 辅助工具（`Ctx::elicit`、`Ctx::inputRequired`、客户端 `retry`） | ✅ |
| 签名 `requestState` 编解码（HMAC-SHA256、绑定上下文、TTL）+ 服务端绑定 + 客户端自动履行 | ✅ |
| 日志辅助工具（`Ctx::log`）+ `_meta` 日志级别打通（语义门控收口在 D6） | ✅ |
| 客户端请求超时 / 可取消 API（按请求期限 + 看门狗） | ✅ |
| Doxygen API 参考；客户端 + HTTP 示例 | ⏳ |
| 零拷贝解析优化（重新测量 B1） | ⏳ |

## 里程碑 D —— v0.4 规范校准（2026-10 评审跑道）

针对本地 2026-07-28 规范快照的全面评审结论（官方一致性套件 2026-07-28
服务端/客户端均已 **100%**；以下为剩余 MUST/SHOULD 偏差，均属边缘路径、可就地修复）。

| 事项 | 级别 |
| --- | --- |
| D1 · Base64 哨兵值再编码：形如 `=?base64?…?=` 的纯 ASCII 值 MUST 先再编码再镜像；解码器只接受真编码值 | ✅ P0 |
| D2 · `MissingRequiredClientCapabilityError.data.requiredCapabilities` 改按 schema 输出 `ClientCapabilities` 对象（而非字符串数组） | ✅ P0 |
| D3 · `io.modelcontextprotocol/subscriptionId` 类型保真：流通知保留 listen 请求 JSON-RPC id 的类型（数字保持数字——ack/关闭回包已经如此） | ✅ P0 |
| D4 · `x-mcp-header` 校验收紧至规范：头名限 `1*tchar`，禁止经 `items`/组合关键字可达，组合关键字内注解视为无效工具，注册侧自校验 | ✅ P0 |
| D5 · `Client::listTools` 剔除违反 D4 的工具（规则从一致性夹具下沉进库） | ✅ P0 |
| D6 · `notifications/message` 门控：请求缺 `_meta.logLevel` 一律不发送（移除服务端默认级别路径）；非法级别值以 `-32602` 拒绝 | ✅ P1 |
| D7 · 签名 `requestState` 可选 principal 绑定（编解码器区分完整性与身份） | ✅ P1 |
| D8 · 每个 result 的 `_meta` 附 `serverInfo`（可配置）；拒绝 `initialize` 时附 `supported` 版本列表 | ✅ P1 |
| D9 · Builder API 将 `listChanged`/`subscribe` 能力标志广告进 capabilities（扇出已就绪） | ✅ P1 |
| D10 · 整数 `Mcp-Param-*` 头校验改为数值比较（非字符串比较） | ✅ P1 |
| D11 · `HttpServerTransport` 的令牌校验挂钩（资源服务器侧 audience 校验目前无落点） | ✅ P1 |
| D12 · 扩展协商强制：不支持已广告的扩展时回退核心行为或拒绝请求 | ✅ P1 |

出口门槛：每一项完成后官方 2026-07-28 套件保持 100%；错误面 pin 测试锁定
变更的 wire 形状。**状态：十二项全部落地并复验（单测 14/14；2026-07-28 客户端
399/399；服务端 55 项计分检查 + 4 个基线 legacy 场景；2025-11-25 客户端套件
多次并行运行全绿）。**

## 里程碑 F —— 评审加固（v1.0 前）

复核里程碑 D 时发现的后续项。均不阻塞合规，各自关闭一个易用性或纵深防御缺口。

| 事项 | 级别 |
| --- | --- |
| F1 · 把已认证 principal 暴露给 MRTR：`Builder::requestStatePrincipal(fn)` / `Ctx::principal`，让 D7 的绑定成为默认路径而非仅 codec API | ✅ P1 |
| F2 · 将 `Client` 的 x-mcp-header 过滤限定在真正镜像头的传输上（stdio MAY ignore；一律过滤会隐藏可用工具） | ✅ P2 |
| F3 · legacy GET 监听流携带配置头（含 `Authorization`）——目前仅 POST 携带 | ✅ P1 |
| F4 · `requiredCapabilities` 一次报告全部缺失能力 | ✅ P2 |
| F5 · 修复 sse-retry 偶发：强制重连不得被判为 peer gone；无 `retry:` 建议时不得提前重连 | ✅ P1 |

## 里程碑 E —— 参考实现供养（v0.5）

学习 @modelcontextprotocol 官方 SDK（python 2.3 / rust rmcp 3.5 / typescript 2.3），
按价值排序；难度按 C++17、值语义、无异步运行时评估。

| 事项 | 来源 |
| --- | --- |
| E1 · Tasks 扩展（SEP-2663，`io.modelcontextprotocol/tasks`）：任务记录、TTL、可插拔 TaskStore、`tasks/get`/`tasks/update`/`tasks/cancel`、`Mcp-Name` 镜像 | ✅ 三家皆有 |
| E2 · 客户端时代自动协商：探测 `server/discover` → 分类（现代错误/legacy/超时）→ 回退 `initialize` 或锁定版本 | ✅ ts `versionNegotiation`、py `_probe` |
| E3 · 客户端响应缓存：遵守 `ttlMs`、尊重 `cacheScope`、discover 单条豁免 | ✅ ts `responseCache.ts` |
| E4 · 扩展接缝：`Builder::extension()` 以 `(method, version)` 绑定、禁遮蔽规范方法（SEP-2133 形状） | ✅ py `server/extension.py` |
| E5 · SSE keep-alive 可配置（禁用值校验） | ✅ ts `sseKeepAlive.ts` |
| E6 · 黄金线缆夹具：钉住的规范 schema twins + 错误面 pin 测试 | ✅ ts corpus、py pinned schema |
| E7 · MRTR 加固：客户端重试轮次上限、密钥 zeroize | ✅ py 默认值、rust zeroize |
| E8 · 可选：订阅总线抽象；中间件链；legacy `input_required` shim（双时代 handler） | 困难，延后 |

## 超越 Tier 1

| 事项 | 状态 |
| --- | --- |
| Tasks 扩展（`io.modelcontextprotocol/tasks`） | ✅ `Builder::tasks()` + `mcp/tasks.hpp` |
| 多 worker 分发（编译期 `MCP_WORKERS`） | ✅ `MCP_WORKERS=N` 派发槽位（默认 1） |
| 旧版修订语义（选择性启用） | ✅ 双时代模式：`initialize`、无 `_meta` 请求、`ping`、`logging/setLevel`、资源订阅 |
| MCP Apps（UI 扩展） | ❌ 超出范围 |
| Roots / Sampling | ❌ 已被 SEP-2577 弃用 |

## 节奏承诺

- 新的规范特性：在里程碑 B 完成后，于**每次规范发布之前**完成（Tier-1 节奏）。
- Issue 分类：≤ 2 个工作日；严重（P0）缺陷：≤ 7 天。
- 一致性任务在 `main` 上保持绿色。

*最后更新：2026-10-20。*
