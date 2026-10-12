# Session 评审报告与下一步计划

> 评审对象：mcp-cpp 本轮工作（2026-10-09 ~ 10-12，提交 `6a11bf0`…`dd25f75`，共 10 个提交）
> 评审日期：2026-10-12 · 评审方式：实际构建 + 全量测试 + 源码复核 + 承诺兑现比对
> 关联：`mcp-cpp-review.md`（上轮独立评审）、`OPTIMIZATION-PLAN.md`、`ADOPTION-PLAN.md`、`CONFORMANCE.md`

---

## 一、结论先行

**本地完成度高、可验证、诚实**：本轮把项目从「功能完备的 0.1.0」推进到「有官方一致性测试数据、有 CMake 包、有治理文件、MRTR 补全、有 2026-07-28 自建套件」的状态，10/11 个 CTest 套件全绿（新增 13+12 用例），父工程 zorm-mcp 不受影响。

**但有一个 P0 阻塞项：GitHub 推送尚未发生。** `zhoutk/mcp-cpp` 仓库已创建但仍为空（`git ls-remote` 返回 0 个引用），本地 10 个提交一个都没上去。所有推广动作（badges、CI、博客、WG 提案）都被这一步挡住。

```
git push github master
```

---

## 二、本轮产出盘点（逐项核实）

| 项 | 内容 | 验证方式 |
| --- | --- | --- |
| 提交 | 10 个，按主题分阶段，每个都单独用 worktree 构建验证过 | `git log` + 逐提交 worktree 构建 |
| 测试 | 11 个 CTest 注册全绿（`mcp_test_conformance_2026` 为新增，12 场景） | `ctest --test-dir build` 复跑通过 |
| 官方一致性 | 26/30（86.7%），套件 v0.1.16，4 项基线化 | `conformance/report.json` + 复跑退出码 0 |
| MRTR | `Ctx::inputRequired`/`Ctx::elicit`（能力门控）、`Client::retry`（`requestState` 精确回显）、不支持方法上的 `input_required` 被拒 | 12 场景中 3 个 MRTR 场景覆盖 |
| 打包 | `install()`/`find_package(mcp CONFIG)`，httplib 改为 build-interface-only | 仓库外消费者构建运行成功 |
| 文档 | 20 个新 .md，双语（EN 默认 + `.zh-CN`），含优化方案/推广计划/conformance 说明/launch kit/WG 提案 | 逐对抽查，中英同步深度一致 |
| 治理 | ROADMAP、CONTRIBUTING（含依赖策略）、SECURITY、CoC、MAINTAINERS、CHANGELOG、issue/PR 模板、CI 矩阵 | 文件存在且被 README 引用 |
| 父工程 | zorm-mcp 重建 + 2/2 e2e 通过 | 复跑通过 |
| 版本同步 | `version.txt` = `kSdkVersion` = 0.1.0 | 已核对 |

### 运行官方套件发现并修复的 5 个真实缺陷

| # | 缺陷 | 影响 | 修复 |
| --- | --- | --- | --- |
| 1 | HTTP 传输把请求期间的通知**丢弃**（sink 只保留最后一行） | progress/logging 在 HTTP 下静默失效 | 收集全部行，多于一行时按 SSE 流回传 |
| 2 | 给 legacy 响应注入 `resultType` | 2025 schema 拒绝未知字段，ping/logging/subscribe 全挂 | 仅对 modern 请求注入 |
| 3 | relaxed `_meta` 解码器不读 `progressToken` | legacy 请求的进度通知失效 | 补解码 |
| 4 | 缺 `ping`/`logging/setLevel`/`resources/subscribe|unsubscribe` | 老客户端完全不可用 | legacy 模式下重新路由 |
| 5 | 镜像头缺失一律 `-32020` | 违反规范的向后兼容 MAY | 仅 strict 模式拒绝 |

另修：`HttpServerTransport` 无法绑定固定端口；`resultTypeOf()` 改用 `member()`（原来走 `operator[]`，会命中 zjson 深搜回退——正是本项目反复警告的 bug 类）。

---

## 三、逐项审查发现（按严重度）

### P0 — 阻塞一切对外动作

**1. GitHub 推送未发生。** 仓库存在但为空。以下动作全部依赖它：CI 跑绿、badges 生效、vcpkg/Conan 提交、博客可引用、WG 提案有仓库可指。**这是下一步的第一件事，一条命令。**

### P1 — 发布前应修

**2. CI 就绪探测循环有缺陷**（`.github/workflows/ci.yml` L86-88）：

```yaml
curl -sf -o /dev/null http://127.0.0.1:8099/mcp || sleep 0.5
```

服务器对 GET 返回 **405**，`curl -f` 遇 4xx 必然失败，所以循环退化为「固定睡 0.5s × 20 = 10 秒」。能跑，但有误导性且浪费。应改为接受 405 即视为就绪，或直接 `sleep 2`。

**3. legacy `resources/subscribe` 是空实现。** 接受请求并回 `{}`，但不追踪订阅、不发 `notifications/resources/updated`。规范要求 unsubscribe 后停止通知——目前无通知可停，等于「假订阅」。conformance 只验证「接受」，所以过测，但真实老客户端得不到更新推送。应在文档标注为已知缺口，或在 SDK 内做真实的 per-URI 注册表。

**4. `logging/setLevel` 只记录级别、不过滤通知。** 规范要求按级别过滤后续通知。conformance 只设 `debug` 后数通知条数，所以过测。`Server::logLevel_` 已存但无人消费。

**5. `Builder::supports()` 的语义陷阱。** 首次显式调用会**替换**默认的 Latest-only，而不是追加——本轮实测踩到（`-32022`，supported 列表里没有 2026-07-28）。已在头文件补了显式警告，但这属于「文档化了一个陷阱」而非「消除陷阱」。建议 v0.2 改为追加语义 + 显式 `resetVersions()`。

**6. `WG-PROPOSAL.md` 缺 `.zh-CN` 兄弟。** 本轮建立的双语约定（EN 默认 + 中文兄弟）在 7 份核心文档上成立，唯独 WG 提案没有中文版——而它恰恰是最需要中文评审的（用户是中文使用者）。

### P2 — 值得做但非阻塞

**7. `requestState` 的完整性保护缺失。** 规范（mrtr.md 服务器要求 4）明文：server **MUST** 把 `requestState` 当攻击者可控输入，若影响授权/业务逻辑 **MUST** 做 HMAC/AEAD 完整性保护。SDK 目前把 `requestState` 当不透明字符串透传（正确），但没提供任何签名 helper——意味着下游应用（如 zorm-mcp 用明文 JSON）大概率不做保护。SDK 应提供 `signRequestState()/verifyRequestState()`（HMAC-SHA256）。

**8. conformance badge 的 URL 含括号**（`26%2F30%20(2025--era%20suite)`），shields.io 对括号的解析不稳定，可能渲染异常。建议改为无括号的 `26%20of%2030`。

**9. 4 个基线失败的取舍已记录但值得复审。** 全部因「服务端中途向客户端发请求」而起，2026 用 MRTR 取代。不做 legacy 服务端请求是正确决定（与规范演进方向一致），但代价是：**老客户端（2025-11-25 及更早）永远无法获得 sampling/elicitation**。若实际有此类客户端需求，需重新评估。

### 过程复盘（如实记录，供下次改进）

- **测试草稿两次返工**：`test_conformance_2026.cpp` 首版留有脚手架垃圾；fixture 两处 bug（zjson Null 渲染成 `"null"` 的既有陷阱、server 线程未 join 导致 terminate）靠运行才暴露。教训：**写新测试时先过一遍 zjson 陷阱清单**。
- **提交顺序错误**：`build` 提交先于 `examples` 提交，导致 `add_executable(conformance_server ...)` 引用尚不存在的源文件。第一次修复时 `git commit --amend` **忘了 `git add`**，改动根本没进 amend——用逐提交 worktree 构建才在第二轮发现。教训：**重排历史后必须逐提交验证构建，且 amend 前确认 staged 内容**。
- **`/tmp` 使用**：早期把 install/consumer 测试放 `/tmp`，与用户后来的「输出尽量在本项目目录内」指令冲突，之后改到 `build/install`、`build/consumer`。
- **打包范围偏差**：用户在多选题里**未勾选**「打包与 CI」，但本轮做了 CMake install/export（并证明它是官方准入的硬前提）。事后看正确，但属于「替用户做了决定」，应在当时点明而不是事后报告。
- **遗留文件**：`build/COMMIT_MSG.txt`（gitignored，无害，可删）。

---

## 四、下一步计划

### 第 0 步（立即，5 分钟）

```bash
git push github master          # 或在输入框: ! git push github master
```

推送后到 GitHub Actions 看 CI 是否跑绿（三个 job：矩阵构建、ASan/UBSan、conformance）。

### 第 1 阶段 — 发布就绪（本周）

| # | 事项 | 量级 |
| --- | --- | --- |
| 1 | 修 CI 就绪探测循环（405 处理） | S |
| 2 | 补 `WG-PROPOSAL.zh-CN.md` | S |
| 3 | 在 README/CONFORMANCE 标注 legacy 订阅与 logLevel 过滤为已知缺口 | S |
| 4 | 决策版本号：打 `v0.1.0` 标签，或按 CHANGELOG [Unreleased] 内容直接切 `v0.2.0` | S |
| 5 | `client_probe` 示例 + 官方套件 client 模式跑通（ROADMAP 仍 ⏳ 的一项） | M |
| 6 | vcpkg port | M |

### 第 2 阶段 — Tier-1 差距（2~4 周）

| # | 事项 | 量级 |
| --- | --- | --- |
| 7 | `requestState` HMAC 完整性 helper（规范 MUST 项的 SDK 化） | M |
| 8 | 真实的 legacy 资源订阅注册表（补 P1-3） | M |
| 9 | OAuth 2.1 + Client ID Metadata（远程 HTTP 部署需要） | L |
| 10 | `Ctx::log` + `_meta` logLevel 消费（补 P1-4） | S |
| 11 | 零拷贝解析优化（B1 目前 0.79×/0.38×） | M |

### 第 3 阶段 — 推广与准入（按 ADOPTION-PLAN Phase 3-4 执行）

1. 发博客（`docs/LAUNCH.md` 草稿已备，先于 WG 提案）。
2. 提交到 Awesome-MCP / Smithery / Pulse MCP 的 SDK 目录，让「C++」不再空白。
3. 向 `modelcontextprotocol/modelcontextprotocol` 提 Discussions 提案（`docs/WG-PROPOSAL.md` 草稿已备），同时把 2026-07-28 场景（含 MRTR）贡献回官方 `conformance` 仓库——这是把「自建套件」变成「官方认可」的关键一步。
4. 招募共同维护者，回应单人维护的 bus factor 质疑。

### 明确不做（已记录）

- legacy 服务端→客户端请求（sampling/elicitation 的 2025 机制）——与 2026 演进方向相反。
- Roots/Sampling —— SEP-2577 已弃用。
- MCP Apps —— 超出协议 SDK 职责。

---

## 五、复核记录

| 步骤 | 做法 | 结果 |
| --- | --- | --- |
| 提交完整性 | `git log` + `git show --stat dd25f75` | 10 提交，工作树干净，无 Claude 声明 |
| 测试 | `ctest --test-dir build` | 11/11 全绿（1.26s） |
| 版本同步 | `version.txt` vs `kSdkVersion` | 均为 0.1.0 |
| 双语同步 | 逐对 grep 比对 | README/PLAN/ROADMAP/CONTRIBUTING/LAUNCH 同步；**WG-PROPOSAL 缺中文版** |
| 推送状态 | `git ls-remote github` | **仓库空，未推送** |
| CI 脚本 | 逐行读 ci.yml | 发现就绪探测缺陷 |

---

## 六、参考实现对比分析（2026-10-12 追加）

对 `refer/rust-sdk`（rmcp）与 `refer/typescript-sdk`（v2 monorepo，2.0.0-alpha）做了定向深读。**这一节推翻了第三节的一个隐含假设，并大幅更新了下一步计划。**

### 6.1 最重要的发现：我们不在协议形状上落后

两个官方 SDK 都已完整实现 **2026-07-28**：

| 协议能力 | rust-sdk | typescript-sdk | mcp-cpp |
| --- | --- | --- | --- |
| 无状态 per-request `_meta` | ✅ | ✅（按 era 分两套 wire codec） | ✅ |
| `server/discover` | ✅ | ✅ | ✅ |
| MRTR（input_required/inputResponses/requestState） | ✅ 一等公民，客户端**自动履行**最多 10 轮 | ✅（inputRequiredEngine 自动履行） | ✅ 手动 retry |
| `subscriptions/listen` | ✅ | ✅ | ✅ |
| CacheableResult | ✅ | ✅（分层 hint：操作级>注册级>服务器级） | ✅（仅服务器级） |
| **requestState HMAC+TTL 完整性** | ✅ `RequestStateCodec` | ✅ `createRequestStateCodec`，在 handler 之上校验、篡改直接 `-32602` | ❌ **缺失** |
| 官方 conformance 双版本（2025+2026） | ✅ **server 30/30、client 39/39** | ✅ 每次 push 都跑 client+server | ⚠️ 仅 v0.1.16 的 2025 套件 26/30 |

结论：**差距不在协议形状，而在硬化与证据。** 这比「我们落后」好得多，也比「我们领先」更清醒——官方 SDK 把我们缺的三件事都做完了。

### 6.2 关键事实：官方套件已有 2026-07-28 场景

rust-sdk 的 CI 用 `npx @modelcontextprotocol/conformance@0.2.0-alpha.12` **同时跑 `--spec-version 2025-11-25` 和 `2026-07-28`**。我们此前测的 v0.1.16 只有 2025 场景——**升级套件版本后，我们可以拿到官方 2026 分数**，这直接改写 ADOPTION-PLAN 的前提。

### 6.3 值得照搬的具体做法（按价值排序，已并入 §七 计划）

| # | 做法 | 来源 | 对我们的意义 |
| --- | --- | --- | --- |
| 1 | **两个 conformance fixture 二进制**（server 1786 行 + client 1267 行）接入官方 npm 套件，server 走 `--url`、client 走 `--command`，双 spec 版本 × server/client 四象限 | rust `conformance/` | 我们只有 server fixture；client 模式是 ROADMAP 上一直 ⏳ 的缺口 |
| 2 | **双向严格 expected-failures**：未列出的失败挂 CI，**已列出但开始通过的场景也挂 CI（stale entry）** | rust `expected-failures-extensions.yaml` | 防止基线腐烂，直接照抄 |
| 3 | **conformance 审计报告入库作为证据链** | rust `conformance/results/2026-02-25-*.md`（Tier 3→Tier 1 的完整整改记录） | 这就是我们 tier 申请的「supporting evidence」模板 |
| 4 | **行为冻结的 wire-shape 测试 + 规范 fixture 语料库** | TS `wire/rev2026-07-28/buildSchemas.ts` + `test/corpus/`（含 `fetch:schema-twins` 同步脚本） | 我们没有任何 pin 精确 JSON 的黄金测试；`test_result_type_wire.rs` 同类做法也值得抄 |
| 5 | **requestState HMAC+TTL 编解码**，校验在 handler 之前、失败回 `-32602` | rust `model/request_state.rs`、TS `requestStateCodec.ts` | §三 P2-7 升级为 P1：两个官方 SDK 都把它做进 SDK，说明这是准入预期 |
| 6 | **SEP-1730 格式的 ROADMAP**（requirement/status/evidence 表 + 双套件分数表） | rust `ROADMAP.md` | 我们的 ROADMAP 是任务清单，不是审计证据格式 |
| 7 | **VERSIONING.md + DEPENDENCY_POLICY.md**（含「丢弃已支持的协议版本 = MAJOR 破坏」这类硬规则） | 两个仓库都有 | Tier 1/2 的硬性要求，我们只有 CONTRIBUTING 里的依赖小节 |
| 8 | **era 门控的方法可用性**：客户端对 2025 对等端发 `server/discover` 时本地快速失败（不发请求） | TS `protocolEras.ts` + `METHOD_NOT_SUPPORTED_BY_PROTOCOL_VERSION` | 我们的 `kRoutes` 只做能力门控，没有 era×方法表 |
| 9 | **本地 SDK 错误词表**（字符串码、永不序列化、带结构化 data）与协议错误分离 | TS `sdkErrors.ts`（如 `INPUT_REQUIRED_ROUNDS_EXCEEDED` 携带 `lastResult`） | 我们的错误只有 wire 侧 `McpErrorCode`；客户端侧需要本地错误词汇 |
| 10 | **边界元测试**：机械检查 include 方向 | TS `coreBoundary.test.ts` | 我们「头文件互不包含」的规则只靠评审，可写成 CTest |
| 11 | **例子即文档**：30+ 可运行、CI 类型检查的示例 | TS `examples/` | 我们只有 minimal_server + conformance_server + zorm_server |
| 12 | **分层 cache hint**：操作级 > 注册级 > 服务器级 | TS `resultCacheHints.ts` | 小改动，提升 API 表达力 |

**明确不抄**：rust 的 proc-macro 工具路由（C++17 无对应物，硬模仿会造出怪异模板）；TS 的 factory-per-request 服务器构造（我们的长寿命 `App` 是有意取舍，但须把 `PendingOp` 表标注为有状态权衡）。

### 6.4 修正后的差距结论

| 维度 | 此前评估 | 修正后 |
| --- | --- | --- |
| 协议完备性 | 「与 Tier-1 对齐」 | ✅ 确认对齐（两官方 SDK 同架构） |
| conformance | 「26/30，4 项被 MRTR 阻塞」 | 需升级套件版本重测；2026 套件存在，rust/TS 均 100%——**我们的天花板是 100%，不是 26/30** |
| 客户端 | 「封装齐全」 | 缺 MRTR 自动履行循环 + client fixture，是 client-mode conformance 的硬前提 |
| 安全 | 「requestState 透传即可」 | ❌ 不足，HMAC+TTL 是官方标配 |
| 工程门槛 | 「打包+CI+治理文件」 | 还需 VERSIONING/DEPENDENCY_POLICY、SEP-1730 格式 ROADMAP、证据链入库 |

---

## 七、下一步计划（v2，按参考实现修正）

### 第 0 步 ✅ 已完成（2026-10-12）

`git push github master` —— 11 个提交已上 `github.com/zhoutk/mcp-cpp`。

### 第 1 阶段 — 证据与硬化（1~2 周，最高优先级）

| # | 事项 | 来源启发 | 量级 |
| --- | --- | --- | --- |
| 1 | 升级官方套件到 0.2.0-alpha.12，跑 **2026-07-28 server 套件**，拿到第一个官方 2026 分数 | rust CI | S |
| 2 | 修 CI 就绪探测（405）、badge 括号 | §三 P1-2/P2-8 | S |
| 3 | `requestState` HMAC-SHA256+TTL 编解码 + handler 前校验 | rust/TS 各自实现 | M |
| 4 | client fixture（`conformance_client`）+ client-mode 官方套件接入 CI | rust `conformance/src/bin/client.rs` | M |
| 5 | 双向严格 expected-failures（stale entry 也挂 CI） | rust | S |
| 6 | wire-shape 黄金测试：pin `resultType`/缓存字段/镜像头的精确 JSON | rust `test_result_type_wire.rs`、TS corpus | S~M |
| 7 | VERSIONING.md + DEPENDENCY_POLICY.md | 两个参考仓库 | S |
| 8 | ROADMAP.md 重写为 SEP-1730 证据表格式 | rust `ROADMAP.md` | S |

### 第 2 阶段 — API 与工程补齐（2~4 周）

| # | 事项 | 量级 |
| --- | --- | --- |
| 9 | 客户端 MRTR 自动履行循环（本地 handler 响应 sampling/elicitation/roots，轮数上限） | M |
| 10 | 本地 SDK 错误词表（`INPUT_REQUIRED_ROUNDS_EXCEEDED` 等，带结构化 data） | S |
| 11 | era×方法门控表（客户端本地快速失败） | S |
| 12 | 边界元测试（CTest 检查 include 方向） | S |
| 13 | 3~4 个可运行示例（mrtr / dual-era / http+listen）作为文档 | M |
| 14 | 工具错误审计：handler 抛错必须走 result 级 `isError`，绝不变成 JSON-RPC error | S |
| 15 | 分层 cache hint（注册级覆盖） | S |

### 第 3 阶段 — 推广与准入（见 `docs/PROMOTION-PLAN.md`）

推广计划已独立成文（详实版）：`docs/PROMOTION-PLAN.md`。核心顺序：**先拿到官方 2026 分数 → 补齐治理文件 → 发博客造势 → 贡献 2026 场景回上游 → WG 提案**。

### 明确不做（维持）

- rust 的 macro 工具路由；TS 的 per-request 服务器工厂。
- legacy 服务端→客户端请求；Roots/Sampling；MCP Apps。

---

*评审人：AI 辅助评审。初版 2026-10-12；§六/§七 于参考实现分析后追加。*
