> 语言 / Language: **中文** | [English](CONFORMANCE.md)
> 日期：2026-10-09

# 一致性

MCP SDK 由官方
[`modelcontextprotocol/conformance`](https://github.com/modelcontextprotocol/conformance)
套件评分。Tier 1 要求所有适用必需场景的 **100%**；Tier 2 要求 **80%**。本文档
说明 mcp-cpp 是如何接入该套件的。

> **状态（2026-10-13），双规范版本：**
>
> - **2026-07-28：42/42 检查项、20/20 场景——计分（active）套件 100%**，使用
>   `@modelcontextprotocol/conformance@0.2.0-alpha.12`——参见
>   [`conformance/report-2026-07-28.json`](../conformance/report-2026-07-28.json)。
> - **2025-11-25：26/30**（v0.1.16）——参见
>   [`conformance/report.json`](../conformance/report.json)。四个失败都需要
>   **服务器在调用中途向客户端发送请求**（`sampling/createMessage` /
>   `elicitation/create`）；2026-07-28 已用 MRTR 取代该机制，而本 SDK 已实现
>   MRTR。记录在
>   [`conformance/expected-failures.yaml`](../conformance/expected-failures.yaml) 中。
>
> 未计分的 *pending* 2026 场景可用 `--suite all` 额外运行：
> `sep-2164-resource-not-found` 通过；`server-stateless` 需要 fixture 的
> `--strict` 旗标（15/28）；`input-required-*` 系列需要 fixture 暴露 MRTR
> 工具——已作为后续工作记录在 2026 报告中。

---

## 1. 该套件做什么

- **服务端模式**以*客户端*身份驱动 SDK 的服务端：它启动或连接到服务器，发出规范
  场景，检查每一次交换——外加对每条消息做线上 schema 校验。
- **客户端模式**将 SDK 的客户端指向一个场景测试服务器。
- **评分**（`--requirements <revision>`）只统计某个规范修订所要求的内容，且在发布
  时冻结。报告形如
  `Server 67/67 required scenarios (100%)`，并带有一个"Not scored"部分
  （`extension`、`added-after-release`、`pending`）和一个"Tier 1 Blockers"列表。
  `--output json` 返回 `passed`/`failed`/`total`/`pass_rate`/`not_scored`。
  退出码只反映必需场景。

## 2. 在本地运行

需要 Node（CLI 以 `@modelcontextprotocol/conformance` 形式发布）。先构建
fixture 服务器：

```bash
cmake --preset clang-rlz && cmake --build build -j 8
```

### 服务端模式（Streamable HTTP）

服务端模式接收一个 `--url`；没有 stdio 选项。`examples/conformance_server.cpp`
是该套件所期望的 fixture——它暴露了所需的 tool/resource/prompt 接口面，并以
旧版客户端模式运行，以便接受 2025 时代的握手和缺失的镜像头：

```bash
./bin/conformance_server.exe --port 8099 &
npx @modelcontextprotocol/conformance server --url http://127.0.0.1:8099/mcp \
  -o conformance/out
```

有用的标志：`--scenario <name>`（单个场景）、`--suite active|all|pending`、
`--spec-version <v>`（只有 2025-xx 有效）、`--expected-failures <file>.yaml`、
`--verbose`。在 v0.1.16 中，`server` 上没有 `--output json`；`-o` 下每个场景的
`checks.json` 文件就是原始结果。

### 客户端模式

套件会启动自己的场景服务器，通过 `examples/conformance_client.cpp` 驱动我们的
客户端。请逐个场景运行（套件的并行套件模式会触发其 mock 服务器的会话路由竞态，
官方 rust-sdk 参考客户端同样受影响）：

```bash
npx @modelcontextprotocol/conformance client \
  --command "bin/conformance_client" \
  --scenario tools_call --spec-version 2026-07-28 \
  --expected-failures conformance/expected-failures.yaml \
  -o conformance/out
```

在 Windows 上套件通过 `cmd.exe` 启动命令，因此请带上可执行文件扩展名
（`--command "bin\conformance_client.exe"`）。

**客户端得分（2026-10-15 复验，套件 0.2.0-alpha.12，串行运行）：**

| 场景 | 2025-11-25 | 2026-07-28 |
| --- | --- | --- |
| `initialize` | 1/1 | n/a（仅 legacy） |
| `tools_call` | 2/2 | 2/2 |
| `json-schema-2020-12-preservation` | 9/9 | 9/9 |
| `json-schema-ref-no-deref` | n/a | 1/1 |
| `request-metadata` | n/a | 6/6 |
| `sep-2322-client-request-state` | n/a | 间歇* |
| `http-standard-headers` | n/a | 3/3 |
| `http-custom-headers` | n/a | 18/18 |
| `http-invalid-tool-headers` | n/a | 12/12 |
| `sse-retry` | 2/3（间歇*） | n/a |
| `elicitation-sep1034-client-defaults` | 0/6（0/5）* | n/a |
| `auth/*`（约 33 个场景） | 已基线——暂无 OAuth | 已基线 |

\* 这三个场景已基线化。**官方 rust-sdk 参考客户端在相同环境下持续失败**
（elicitation 0/5 ×3、sse-retry 0/1 ×2、sep-2322 0/5——2026-10-14 验证），
说明问题出在套件 mock 的会话路由而非 SDK：mock 收到了客户端的 GET 流，却始终
不发送 elicitation，只发 keepalive 直到场景超时。`sse-retry` 可间歇通过。
2026-10-15 用改动前的代码做 A/B 复跑，结果一致。它们已连同该证据一并基线化在
[`conformance/expected-failures.yaml`](../conformance/expected-failures.yaml)
中；CI 按场景串行运行。

### 层级自我评估

```bash
npx @modelcontextprotocol/conformance tier-check \
  --repo zhoutk/mcp-cpp \
  --conformance-server-url http://127.0.0.1:8099/mcp
```

## 3. CI 集成

该套件附带一个 GitHub Action（v0.1.11+）。它以服务端模式针对本地构建的 fixture
服务器运行，以客户端模式针对场景服务器运行：

```yaml
- name: Conformance (server)
  uses: modelcontextprotocol/conformance@v0.1.16
  with:
    mode: server
    url: http://127.0.0.1:8099/mcp
```

`.github/workflows/ci.yml` 中的一致性任务会启动 `conformance_server`，以
`--expected-failures conformance/expected-failures.yaml` 调用该 action，并将报告
作为产物上传。非零退出码（即一次意外失败）会使该任务失败，因此在已知的四个失败
保持基线化的同时，回归也能被捕获。

## 3b. 仓库内的 2026-07-28 套件

由于官方套件没有 2026-07-28 场景，本仓库自带了一套：
**`tests/test_conformance_2026.cpp`**（`mcp_test_conformance_2026`，已注册到
CTest 并由 CI 运行）。它以具名场景的形式编写，每个场景对应一条规范要求，因此
2026 接口面能像官方套件将来补齐后那样得到覆盖。

| 场景 | 规范章节 |
| --- | --- |
| `DiscoverAdvertisesVersionsCapabilitiesAndCacheHints` | `server/discover`、缓存 |
| `StatelessMetaIsRequiredOnEveryRequest` | `basic/index.md` 无状态性 |
| `UnsupportedVersionReportsSupportedList` | `basic/versioning.md`（`-32022`） |
| `CapabilityGatingReturnsMethodNotFound` | 未声明特性的 `-32601` |
| `UnknownToolIsInvalidParams` | 未知工具的 `-32602` |
| `ResultTypeIsCompleteOnEveryResult` | `basic/index.md` 响应 |
| `ProgressOnlyForOptedInRequests` | `basic/patterns/progress.md` |
| `OutputSchemaIsEnforced` | `server/tools.md` 输出 schema |
| `MrtrRejectsUndeclaredClientCapability` | `basic/patterns/mrtr.md` 服务端要求 7 |
| `MrtrNotAllowedOnUnsupportedMethods` | `basic/patterns/mrtr.md` 支持的请求 |
| `MrtrElicitationRoundTrip` | 通过双工内存管道的完整 MRTR 流程 |
| `MrtrClientEchoesRequestStateExactly` | MRTR 客户端要求 2 |

```bash
ctest --test-dir build -R mcp_test_conformance_2026 -V
```

## 4. 处理失败

- **真正的失败** → 在 SDK 中修复它。这些是最有价值的场景。
- **确实待定 / 有争议** → 将其加入
  `conformance/expected-failures.yaml` 并附一条说明原因的注释，并引用
  规范章节。让这个文件保持小且不断缩减；它在每次发布时都会被审查。
- **旧版场景** → 除非 SDK 声明支持旧版，否则排除在外
  （mcp-cpp 的旧版模式是选择性启用的，因此未声明支持）。

## 5. 报告

- 通过率和套件版本发布在 `README.md` 和 `ROADMAP.md` 中。
- `conformance/report.json` 是随每次发布提交的机器可读摘要；`CHANGELOG.md`
  记录得分。
- 提交 SDK 工作组提案时将其一并附上。

---

*相关：[ROADMAP.md](../ROADMAP.md)、[ARCHITECTURE.zh-CN.md](ARCHITECTURE.zh-CN.md)。*
