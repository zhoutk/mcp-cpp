> 语言 / Language: **中文** | [English](OPTIMIZATION-PLAN.md)
> 日期：2026-10-09 · 基线：mcp-cpp 0.1.0 · 规范目标：MCP **2026-07-28**

# mcp-cpp 优化计划 —— 与官方 MCP SDK 的差距分析

本文档将 mcp-cpp 与十个官方 MCP SDK（TypeScript、Python、C#、Go、Rust、Ruby、
Java、Swift、PHP、Kotlin——参见
[modelcontextprotocol.io/docs/sdk](https://modelcontextprotocol.io/docs/sdk)）
以及 `modelcontextprotocol/conformance` 套件进行对比，并将差异转化为一份按优先级
排列的工作清单。它是 [ADOPTION-PLAN.md](ADOPTION-PLAN.md) 的技术部分；关于成为
官方 SDK 的流程，请阅读那一篇。

目标并非为功能对等而功能对等——而是要跨过 MCP 项目所用的两道客观门槛：**100%
一致性**（Tier 1）以及每个官方 SDK 都满足的**工程基线**（打包、CI、文档、分类
处理、依赖策略）。

---

## 1. mcp-cpp 目前的定位

已对照源码树和 10 个已注册的 CTest 套件核实（在 clang 22.1.8 / MSYS2 clang64 /
Windows 上全部通过）。

### 已达到或高于 Tier-1 对等

| 能力 | 证据 |
| --- | --- |
| JSON-RPC 2.0 严格 MCP 编解码（禁止 null-id、不支持批处理、对象参数、id 保真至 ±2^63） | `src/jsonrpc.cpp`、`tests/test_jsonrpc.cpp` |
| 五种协议版本 + 带 `supported`/`requested` 数据的 `-32022` 协商 | `src/version.cpp`、`tests/test_version.cpp` |
| `server/discover`（supportedVersions / capabilities / ttlMs / cacheScope / serverInfo） | `src/server.cpp` |
| 无状态的每请求 `_meta`（protocolVersion + clientCapabilities 为必需） | `src/types.cpp` |
| stdio 传输（换行分帧、Windows 二进制模式、EOF 关闭） | `src/transport.cpp`、`tests/test_stdio_e2e.cpp` |
| Streamable HTTP（仅 POST、镜像头含 `Mcp-Param-*`、202/404/405、`-32020`） | `src/transport_http.cpp`、`tests/test_http_e2e.cpp` |
| SSE 响应流 + `subscriptions/listen`（ack、按过滤条件扇出、优雅关闭、保活） | `src/transport_http.cpp`、`src/server.cpp` |
| tools / resources / prompts 注册表、能力门控、不透明分页游标 | `src/server.cpp` |
| 资源 URI 模板（RFC 6570 子集）+ 模板回退式 `resources/read` | `src/server.cpp` |
| `notifications/cancelled` + 可查询的取消标志 | `src/server.cpp` |
| MRTR：`input_required` 结果 + `inputResponses` 重试 | `src/server.cpp`、`src/client.cpp` |
| `completion/complete`（服务端处理器 + 客户端封装） | `src/server.cpp`、`src/client.cpp` |
| discover、`*/list`、`resources/read` 上的 CacheableResult（`ttlMs`/`cacheScope`） | `src/server.cpp` |
| JSON Schema 2020-12 校验器（本地 `$ref`、资源边界、无远程 ref） | `src/schema.cpp` |
| `x-mcp-header` 镜像（客户端）+ 服务端校验 | `src/header_params.h`、`src/transport_http.cpp` |
| **进度通知**——服务端发出（`Ctx::progress`）+ 客户端选择性加入（`progressToken`） | *新增于 2026-10-09* |
| **outputSchema 强制校验**——不符合规范的 `structuredContent` 会被拒绝 | *新增于 2026-10-09* |
| **结构化结果**——`structuredResult()` 辅助函数添加规范推荐的文本块 | *新增于 2026-10-09* |
| **CMake 包**——`install()` + `find_package(mcp CONFIG)` 已用一个树外使用者验证 | *新增于 2026-10-09* |
| 双时代服务（旧版 `initialize` + 现代 `_meta`），位于一个选择性启用标志之后 | `src/server.cpp` |

### 有意不做的非目标

| 事项 | 原因 | 状态 |
| --- | --- | --- |
| Roots / Sampling | 已被 SEP-2577 弃用；2026 服务器不主动发起请求 | 未实现；未知方法返回 `-32601` |
| Tasks 扩展（`io.modelcontextprotocol/tasks`） | 官方扩展；任何层级都不要求 | 路线图（v2） |
| MCP Apps（UI 扩展） | 超出协议 SDK 的范围 | 不计划 |

---

## 2. 差距，按类别划分

严重程度图例：**P0** = 阻碍官方采纳；**P1** = Tier 1 / 一致性所必需；
**P2** = 竞争对等 / 开发体验。

### 2.1 打包与分发（P0——最大的单项差距）

官方 SDK 都能用其生态的包管理器一行命令安装（`npm i`、`pip install`、
`cargo add`、`dotnet add package`、`go get`）。mcp-cpp 目前唯一的发布渠道是
"复制仓库"。C++ 没有单一的包管理器，因此一个可信的 C++ SDK 必须在三条常见路径上
都迎合并满足使用者。

| 事项 | 状态 | 行动 |
| --- | --- | --- |
| `find_package(mcp CONFIG)` + 导出的 `mcp::mcp` 目标 | **已完成（2026-10-09）** | — |
| `FetchContent` / `add_subdirectory` | 已可用（自包含源码树） | 编写文档 |
| vcpkg port（`vcpkg.json` + portfile） | 缺失 | P0 |
| Conan recipe（ConanCenter） | 缺失 | P1 |
| pkg-config `.pc` 文件 | 缺失 | P2 |

### 2.2 CI 与质量门控（P0）

Tier 1 要求 2 个工作日的分类处理与 7 天的严重缺陷 SLA——没有自动化就无法证明，
而目前也不存在 `.github/`。

| 事项 | 状态 | 行动 |
| --- | --- | --- |
| GitHub Actions CI（Linux/macOS/Windows × clang/gcc/msvc × Debug/Release） | 缺失 | P0 |
| CI 中的一致性任务 | 缺失 | P0 |
| Sanitizer 任务（ASan/UBSan） | 缺失 | P1 |
| 静态分析（`clang-tidy`） | 仅手动 | P2 |
| 覆盖率报告 | 缺失 | P2 |

### 2.3 协议完整性（P1）

| 事项 | 说明 | 行动 |
| --- | --- | --- |
| OAuth 2.1 / Client ID Metadata Documents | Tier-1 SDK 均提供认证；远程 HTTP 服务器所必需。工作量大。 | P1（v2） |
| ~~强类型 MRTR/elicitation 辅助工具~~ | **已完成（2026-10-09）**：`Ctx::elicit`/`Ctx::inputRequired` 带能力门控，`Client::retry` 精确回显 `requestState`。 | ✅ |
| 日志辅助工具 | `Ctx::notify` 覆盖了原始的 `notifications/message`；一个 `Ctx::log(level, data)` 语法糖 + `_meta` 日志级别打通会更符合 SDK 的人机工效。 | P2 |
| 工具注解的人机工效 | 定义是原始 JSON（透传可用）；强类型辅助会改善开发体验。 | P2 |
| 进度限流辅助 | 规范建议（SHOULD）；目前由调用方负责。 | P2 |
| 旧版修订语义 | 仅有 `initialize` 垫片；官方 SDK 实现全部五个修订。除非声明，一致性测试会排除旧版。 | P2（选择性启用） |
| 客户端 `roots`/`sampling` 垫片 | 已弃用；官方 SDK 保留兼容垫片。 | 不计划 |

### 2.4 性能（P2——竞争性，非阻塞）

诚实的基线（PLAN.md 更新 8）：原始 JSON 解析在 1 KB 时为 serde_json 的 **0.79×**，
在 64 KB 时为 **0.38×**。这不影响一致性，但在"为什么用 C++"的推介中是一个显眼的
数字，因此它要么被弥补，要么被诚实地呈现（目前即是后者）。

| 优化 | 预期效果 | 工作量 |
| --- | --- | --- |
| 零拷贝 `string_view` 解析（zjson arena 已支持借用视图） | 最大的单项收益，尤其是 64 KB | M |
| 信封预扫描快速路径（在完整解码前窥探 `method`/`id`） | 小消息 | M |
| 预编译/缓存工具的 `inputSchema`/`outputSchema`（目前每次调用都校验） | B2 分发 | S |
| 取消表改用 `unordered_map`（单线程分发） | 微小 | S |

### 2.5 文档与社区（P0/P1）

Tier 1 要求"覆盖所有特性、带示例的全面文档"、一份公开的路线图、一份依赖策略，以及
标准化的 issue 标签。

| 事项 | 状态 | 行动 |
| --- | --- | --- |
| 双语 README（默认 EN + zh-CN） | **已完成（2026-10-09）** | — |
| 双语 PLAN / 决策日志 | **已完成（2026-10-09）** | — |
| 公开 ROADMAP | 缺失 | P0 |
| CONTRIBUTING + 依赖/更新策略 | 缺失 | P0 |
| SECURITY.md（漏洞报告） | 缺失 | P0 |
| CODE_OF_CONDUCT.md | 缺失 | P0 |
| CHANGELOG.md（Keep a Changelog） | 缺失 | P1 |
| Issue/PR 模板 + 分类标签 | 缺失 | P0 |
| API 参考（Doxygen） | 缺失 | P1 |
| 客户端示例、HTTP 服务器示例 | 仅有 `minimal_server` + `zorm_server` | P1 |
| 一致性操作指南 | **已完成（2026-10-09）** | — |

---

## 3. 按优先级排列的计划

### 里程碑 A —— "可采纳"（目标：1 周）

使用者或评审者认真对待本项目所需的一切。

- [x] CMake `install()`/`export` + 经验证的 `find_package(mcp CONFIG)`
- [x] 双语 README + PLAN
- [x] 进度通知、outputSchema 强制校验、结构化结果
- [x] 客户端封装（`listResources`、`listPrompts`、`complete`）
- [ ] `ROADMAP.md`、`CONTRIBUTING.md`（+ 依赖策略）、`SECURITY.md`、
      `CODE_OF_CONDUCT.md`、`CHANGELOG.md`
- [ ] `.github/`：issue 模板（bug/enhancement/config）、PR 模板、
      分类标签
- [ ] GitHub Actions CI 矩阵（3 个 OS × 2-3 条工具链）+ 一致性任务
- [ ] vcpkg port

### 里程碑 B —— "符合规范"（目标：2–3 周）

- [x] 运行 `modelcontextprotocol/conformance` 的 `server` 套件 → **26/30（86.7%）**，
      随一份 `--expected-failures` 基线发布在 `conformance/report.json` 中
- [x] 关闭它暴露出的失败：旧版 `ping`/`logging/setLevel`/
      `resources/subscribe`、旧版时代的 `resultType` 遗漏，以及 HTTP 请求期间
      所发出通知的 SSE 流式传输
- [ ] 其余 4 个是服务器发起请求的场景（sampling /
      elicitation）；2026-07-28 已用 MRTR 取代它们——没有 2026 等价项可用来通过
- [ ] 增加一次客户端模式的一致性运行
- [ ] 发布一个稳定的 **1.0.0** 版本

### 里程碑 C —— "具备 Tier-1 就绪条件"（目标：1–2 个月）

- [ ] OAuth 2.1 + Client ID Metadata Documents（远程 HTTP）
- [ ] Doxygen API 参考；客户端 + HTTP 示例
- [ ] 零拷贝解析优化（重新测量 B1）
- [ ] Tasks 扩展（v2）

---

## 4. 本计划刻意*不*做的事

- **以牺牲正确性为代价追逐原始基准数字。** mcp-cpp 的解析比 serde_json 慢，是因为
  它在读入的过程中校验协议语义；这一取舍保持不变。
- **在需要之前就引入异步/并发。** 单线程分发是一种有文档记录的、刻意的简化
  （PLAN.md 更新 1）。多 worker 仍然是一个编译期选项，待某个基准提出要求时再用。
- **为实现已弃用的特性而显得更庞大。** Roots/Sampling 保持不纳入。

---

*维护者：zhoutk · 本文档随里程碑落地而更新；参见
[CHANGELOG.md](../CHANGELOG.md) 和 [PLAN.md](../PLAN.md) 更新 10。*
