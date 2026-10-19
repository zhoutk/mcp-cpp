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
| vcpkg port | ⏳ |

## 里程碑 B —— 符合规范（Tier 2）

| 事项 | 状态 |
| --- | --- |
| 运行官方 `modelcontextprotocol/conformance` 服务端套件 | ✅ 2026-07-28：**42/42（100%）** · 2025-11-25：26/30 |
| 达到所有适用必需场景的 100% | ✅ 2026-07-28 · 🔄 2025-11-25（4 个受阻，见下文） |
| 发布 `conformance/report.json` + 一份 `--expected-failures` 基线 | ✅ |
| 客户端模式的一致性运行（`examples/conformance_client.cpp`） | ✅ 2026-07-28：**32/32 场景（100%）**；2025-11-25：17/18（另加两个 2025-03-26 legacy 场景） |
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
| OAuth 2.1 客户端 + Client ID Metadata Documents（远程 HTTP） | ✅ 客户端侧（PRM/AS 发现、DCR/CIMD/预注册、PKCE、scope 升级、SEP-2352）· ⏳ 服务端侧 |
| 强类型 MRTR / elicitation 辅助工具（`Ctx::elicit`、`Ctx::inputRequired`、客户端 `retry`） | ✅ |
| 签名 `requestState` 编解码（HMAC-SHA256、绑定上下文、TTL）+ 服务端绑定 + 客户端自动履行 | ✅ |
| 日志辅助工具（`Ctx::log`）+ `_meta` 日志级别打通 | ⏳ |
| 客户端请求超时 / 可取消 API | ⏳ |
| Doxygen API 参考；客户端 + HTTP 示例 | ⏳ |
| 零拷贝解析优化（重新测量 B1） | ⏳ |
| Conan recipe | ⏳ |

## 超越 Tier 1

| 事项 | 状态 |
| --- | --- |
| Tasks 扩展（`io.modelcontextprotocol/tasks`） | ⏳ |
| 多 worker 分发（编译期 `MCP_WORKERS`） | ⏳ |
| 旧版修订语义（选择性启用） | ✅ 双时代模式：`initialize`、无 `_meta` 请求、`ping`、`logging/setLevel`、资源订阅 |
| MCP Apps（UI 扩展） | ❌ 超出范围 |
| Roots / Sampling | ❌ 已被 SEP-2577 弃用 |

## 节奏承诺

- 新的规范特性：在里程碑 B 完成后，于**每次规范发布之前**完成（Tier-1 节奏）。
- Issue 分类：≤ 2 个工作日；严重（P0）缺陷：≤ 7 天。
- 一致性任务在 `main` 上保持绿色。

*最后更新：2026-10-19。*
