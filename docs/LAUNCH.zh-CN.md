> 语言 / Language: **中文** | [English](LAUNCH.md)

# 发布套件 — 对外宣布 mcp-cpp

用于对外宣布本 SDK 的草稿与渠道说明。**这里的内容尚未发布任何一处。**
发布是对外行为：请先审阅文本，然后由你自己发布（或明确表示同意由他人发布），
并且只在仓库公开之后进行。

目标是获取需求（demand）：当 C++ SDK 已经拥有真实用户时，说服 SDK 工作组会更容易。
参见 [ADOPTION-PLAN.md](ADOPTION-PLAN.md) 的第 3 阶段。

---

## 1. 定位（一句话，处处复用）

> **MCP 没有官方 C++ SDK。mcp-cpp 是一个 C++17 实现，通过了官方一致性测试套件——而且它已经支撑着两个真实应用在运行。**
>
> **（There is no official C++ SDK for MCP. mcp-cpp is a C++17 one that passes the official conformance suite — and it's already running two real applications.）**

更长的版本（用于博客文章和工作组提案）：

> **MCP 官方的 SDK 阵容是 TypeScript、Python、C#、Go、Rust、Ruby、Java、Swift、PHP 和 Kotlin。C++——数据库、引擎、嵌入式系统和交易系统所在之处——没有官方 SDK，而社区的 C++ 方案都无法通过一致性测试套件。mcp-cpp 是一个 C++17 SDK，除了一份内置的 JSON 头文件和可选的 cpp-httplib 之外没有任何依赖。它实现了 2026-07-28 规范（每请求无状态的元数据、`server/discover`、MRTR、`subscriptions/listen`、进度、取消、CacheableResult、镜像 HTTP 头），在每次调用时校验工具 schema，并通过了官方一致性测试套件中 26/30 个计分场景。它未能通过的那四个场景，都要求服务器在调用过程中向客户端*发起请求*——2026-07-28 已用 MRTR 取代了这一机制，而 mcp-cpp 实现的正是 MRTR。**
>
> **（The official MCP SDK roster is TypeScript, Python, C#, Go, Rust, Ruby, Java, Swift, PHP and Kotlin. C++ — where databases, engines, embedded systems and trading systems live — has no official SDK, and the community C++ options do not pass the conformance suite. mcp-cpp is a C++17 SDK with no dependencies beyond a bundled JSON header and optional cpp-httplib. It implements the 2026-07-28 specification (stateless per-request metadata, `server/discover`, MRTR, `subscriptions/listen`, progress, cancellation, CacheableResult, mirrored HTTP headers), validates tool schemas on every call, and passes 26/30 scored scenarios of the official conformance suite. The four it does not pass all require the server to *request* something from the client mid-call — a mechanism 2026-07-28 replaced with MRTR, which mcp-cpp implements instead.）**

## 2. 博客文章草稿

**标题：** 一个面向 MCP 的 C++17 SDK，以及一致性测试套件教会我们的东西

**引子（Hook）：** MCP 官方 SDK 阵容中缺少 C++。这是一个 2026-07-28 规范的 C++17 实现——外加运行官方一致性测试套件在其中发现的五个真实 bug。

**正文提纲**

1. *为什么是 C++*——将 MCP 嵌入现有的 C++ 服务；边缘与嵌入式；对延迟敏感的数据库/引擎集成。点名两个真实用户：
   [zorm-mcp](https://github.com/zhoutk/zorm-mcp)（一个多后端数据库 MCP 服务器）和 [zorm](https://github.com/zhoutk/zorm)（其背后的 ORM）。
2. *设计*——C++17、值语义消息、无异常（exception-free）的协议层、
   `McpResult<T>` 在可用时别名为 `std::expected`、单线程无状态调度、`Ctx::progress` / `Ctx::notify` / `Ctx::elicit`。
3. *最精彩的部分：一致性测试发现了什么。* 五个具体的 bug，各自附带规范条款：
   - HTTP 传输会丢弃请求过程中发出的通知（进度和日志被静默丢失）——现在以 SSE 流式发送；
   - `resultType` 被注入到了 2026 之前的响应上，而更早的 schema 会拒绝它；
   - 旧版元数据路径从不解析 `progressToken`；
   - `ping` / `logging/setLevel` / `resources/subscribe` 对老客户端消失了；
   - 缺失的 `MCP-Protocol-Version` 头会被拒绝，而规范说服务器可以将它视作 `2025-03-26`。
4. *尴尬的发现*——官方套件（v0.1.16）**没有 2026-07-28 场景**；它最新的场景是 2025-11-25。因此一个 2026-07-28 的 SDK 目前还无法针对当前规范修订版计分。我们的替代做法：仓库内的一个 2026-07-28 套件（`tests/test_conformance_2026.cpp`，包含 12 个具名场景，其中包括一次完整的 MRTR 往返）。
5. *诚实的数字*——原始 JSON 解析比 serde_json 慢（1 KB 时 0.79×，64 KB 时 0.38×），因为它在解析时校验协议语义；全栈调度和 stdio 往返的数字在 README 中。
6. *请求（Ask）*——试用它、提交 issue，如果 C++ 应当进入官方阵容，请在 MCP SDK 工作组的讨论中说出来。

## 3. 渠道清单

| 渠道 | 发布什么 | 备注 |
| --- | --- | --- |
| Hacker News | "Show HN: A C++17 SDK for MCP that passes the conformance suite" | 链接仓库；在评论中积极出现；以一致性测试的故事开头，而不是功能列表 |
| r/mcp | 博客文章 | 这是特别关心 MCP 的受众 |
| r/cpp | 博客文章，以 C++ 设计的角度切入（值语义、无异常、CMake 打包） | 以 C++ 角度开头，而不是 AI 角度 |
| MCP Discord `#general-sdk-dev` | 简短介绍 + 仓库链接 + 2026-07-28 场景缺口 | 工作组成员会读这个频道 |
| MCP GitHub Discussions | 提议将 C++ 加入官方阵容 | 参见 ADOPTION-PLAN.md 第 4 阶段 |
| Awesome-MCP lists / Smithery / Pulse MCP | SDK 条目 | 让 "C++" 在 SDK 目录中不再是空白 |
| vcpkg / ConanCenter | 移植 PR | 在 C++ 用户已经在的地方触达他们 |

## 4. 发布规则

- **先发博客文章，再发工作组提案**，这样提案才能引用真实的流量和 issue。
- **不要声称拥有 2026-07-28 的一致性分数。** 唯一站得住脚的数字是
  "在套件 v0.1.16 上 26/30，该套件覆盖 2025-xx 修订版"。
- **预先回答两个反对意见**（写在文章里，而不仅仅在评论里）：
  "服务器为什么用 C++" 以及 "社区已有两个 C++ SDK"。
- 保持语气客观。套件发现的 bug 是这段故事的一个亮点，而非尴尬——它们恰恰是一致性测试之所以重要的原因。

## 5. 发布前需要准备的素材

- [ ] 仓库已公开，CI 全绿，README 徽章可正常显示。
- [ ] 一段简短的屏幕录制或动画 GIF：`minimal_server` + MCP Inspector。
- [ ] 一致性报告已提交并附上链接。
- [ ] 已打上 `v0.1.0` 标签（或者一旦发布版定稿就改为 `v1.0.0`）。
- [ ] 一个置顶 issue，列出已知缺口（四个 MRTR 时代的场景、缺失的 2026 场景），这样最初的评论就不会是意外。

---

*相关：[ADOPTION-PLAN.md](ADOPTION-PLAN.md)、[OPTIMIZATION-PLAN.md](OPTIMIZATION-PLAN.md)、
[CONFORMANCE.md](CONFORMANCE.md)。*
