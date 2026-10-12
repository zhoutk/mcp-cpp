# 推广与准入计划（详实版）

> 目标：让 mcp-cpp 进入 `modelcontextprotocol` 官方 SDK 名单，成为 **C++ 官方实现**。
> 语言：中文（内部执行文档）。对外材料（博客/提案）用英文，见 `LAUNCH.md` / `WG-PROPOSAL.md`。
> 日期：2026-10-12 · 依据：rust-sdk 与 typescript-sdk 深读结论（见 `SESSION-REVIEW.md` §六）
> 状态：**未开始执行**。本文是执行序列，每一步都有「完成判据」，做完才进下一步。

---

## 0. 核心洞察（决定整个计划）

1. **官方 conformance 套件已有 2026-07-28 场景**（rust-sdk CI 用 `@modelcontextprotocol/conformance@0.2.0-alpha.12` 双版本跑）。我们此前只跑了 v0.1.16 的 2025 部分（26/30）。**拿到官方 2026 分数是整个计划的第一块基石**——没有它，一切推广都是空头支票。
2. **rust-sdk 提供了完整的「准入手册」**：它的 `conformance/results/2026-02-25-*.md` 是 Tier 3 → Tier 1 的完整整改记录，`ROADMAP.md` 是 SEP-1730 审计格式。我们的路径可以直接对表抄写。
3. **差距是硬化，不是协议形状**：requestState HMAC、client fixture、wire 黄金测试、治理文档。这些全是 1~2 周量级的工程，不是数月。
4. **准入是工作组决定，不是 RFC**：SDK WG 共识 → Core Maintainer 批准（charter「Official SDK Roster」）。所以计划的本质是：**攒证据 → 造需求 → 递交**。

---

## 1. 阶段 A：拿到证据（1~2 周）—— 没有这一步，后面全是空话

### A1. 跑官方 2026-07-28 套件（最高优先级）
- 动作：升级到 `@modelcontextprotocol/conformance@0.2.0-alpha.12`；对 `conformance_server` 跑 `--spec-version 2026-07-28`；对 `minimal_server --http` 同样跑一遍。
- 完成判据：`conformance/report-2026-07-28.json` 入库，分数如实记录（无论多少）。
- 风险预案：我们的 server fixture 是为 2025 场景写的（legacy 模式 + 全套 fixture 工具）。2026 场景会要求 modern 模式行为（resultType 必须在、镜像头必须强制）——**预期会暴露一批真实差异，逐个修掉**，这正是套件的价值。

### A2. client fixture + client 模式套件
- 动作：写 `examples/conformance_client.cpp`（对齐 rust `conformance/src/bin/client.rs` 的职责），接入 `npx ... client --command`。
- 前置依赖：客户端 MRTR 自动履行循环（B1，可先做最小版）。
- 完成判据：CI 出现 client job，双 spec 版本都跑。

### A3. requestState HMAC+TTL 完整性
- 动作：SDK 提供 `RequestStateCodec`（HMAC-SHA256 + 关联数据绑定 + TTL），server 在 handler 之前校验，失败回 `-32602`。依赖：pacman clang64 的 OpenSSL 或自写 HMAC（~150 行）。
- 依据：rust 与 TS 都做进了 SDK；规范 mrtr.md 服务器要求 4 是 MUST。
- 完成判据：单测覆盖「篡改拒绝 / 过期拒绝 / 跨用户拒绝」三例。

### A4. wire-shape 黄金测试
- 动作：pin 精确 JSON —— `resultType` 注入规则、`ttlMs/cacheScope` 字段、镜像头 `-32020` 行为、legacy 剥离 resultType。参考 rust `test_result_type_wire.rs`。
- 完成判据：新增 gtest 文件，每个 golden 用例断言**完整序列化字符串**。

### A5. 治理文档三件套
- `VERSIONING.md`（语义化版本 + 「什么是破坏性变更」清单 + 「丢弃已支持的协议版本 = MAJOR」）。
- `DEPENDENCY_POLICY.md`（升级为独立文件，扩充 CONTRIBUTING 里的小节）。
- `ROADMAP.md` 重写为 SEP-1730 审计格式：requirement / status / evidence 表 + 双套件分数表。
- 完成判据：rust `ROADMAP.md` 里的每一行审计项，我们的表里都有对应行（✅ 或带日期的 ⏳）。

### 阶段 A 完成判据（汇总）
- [ ] 官方 2026 分数入库
- [ ] client-mode 套件进 CI
- [ ] HMAC codec 有三例攻击测试
- [ ] 黄金 wire 测试 ≥8 例
- [ ] 三份治理文档入库

---

## 2. 阶段 B：API 补齐（2~4 周，与 A 尾部并行）

| # | 事项 | 判据 |
| --- | --- | --- |
| B1 | 客户端 MRTR 自动履行循环（本地 handler 响应 sampling/elicitation/roots，默认上限 10 轮，可手动模式） | 单测覆盖多轮 + 轮数耗尽错误带 `lastResult` |
| B2 | 本地 SDK 错误词表（字符串码、不序列化）：`NOT_CONNECTED`/`REQUEST_TIMEOUT`/`CAPABILITY_NOT_SUPPORTED`/`UNSUPPORTED_RESULT_TYPE`/`INPUT_REQUIRED_ROUNDS_EXCEEDED`/`ERA_NEGOTIATION_FAILED` | 客户端各错误路径返回强类型 |
| B3 | era×方法门控：对 2025 对等端发 `server/discover` 本地快速失败，不发请求 | 单测 |
| B4 | 边界元测试：CTest 脚本 grep 头文件 include 方向 | 进 CI |
| B5 | 工具错误审计：handler 异常必须落 result 级 `isError` | 新增负路径测试 |
| B6 | 分层 cache hint（注册级覆盖服务器级） | 单测 |
| B7 | 3 个示例：`mrtr`、`dual-era`、`http+listen`，各配 README，CMake 构建 | 示例可独立运行 |

---

## 3. 阶段 C：真实推广（A 完成后启动，4~6 周）

> 原则：**先有分数和用户，再谈官方**。WG 最容易被「已经有人在用」说服。

### C1. 发布准备（1 周）
- [ ] 版本标签：切 `v0.2.0`（含本轮全部工作），CHANGELOG 收口
- [ ] README：conformance 徽章改为**双版本真实分数**（替换含括号的 URL）
- [ ] `docs/` 面向用户的入口整理：快速上手 / 示例索引 / conformance 报告链接
- [ ] CI 全绿三个 job（矩阵、sanitizer、conformance 双版本）

### C2. 内容发布（第 1~2 周）
- [ ] 英文博客：《A C++17 SDK for MCP — and what the official conformance suite taught us》（`LAUNCH.md` 有骨架；加入 2026 分数与 A 阶段修掉的差异清单）
- [ ] 渠道：Hacker News（Show HN）、r/mcp、r/cpp、MCP Discord `#general-sdk-dev`
- [ ] 中文渠道：知乎/掘金技术社区（用 `LAUNCH.zh-CN.md` 改写）
- [ ] 每个渠道准备回答两个必然问题：「为什么 C++ 做服务器」「已有两个社区 C++ SDK，为什么还要一个」

### C3. 生态占位（第 2~3 周）
- [ ] vcpkg port 提交（`mcp-cpp` 名称）
- [ ] ConanCenter recipe PR
- [ ] Awesome-MCP 列表 / Smithery / Pulse MCP 的 SDK 目录条目
- [ ] 给 zorm-mcp 发 release，作为「生产用户」证言

### C4. 上游贡献（第 3~4 周）—— 隐形的加分项
- [ ] 把我们自建的 2026-07-28 场景（含 MRTR 往返）整理成 PR 贡献回 `modelcontextprotocol/conformance`
- [ ] 这一步的价值：**在 WG 讨论开始前，我们就已经是「贡献者」而不是「申请者」**；同时和 conformance 维护者建立联系
- 完成判据：PR 合并或进入评审（即使被要求改）

### C5. 需求积累（持续）
- [ ] GitHub Discussions 开起来，issue 用标准化标签（`bug`/`enhancement`/`question` + `ready for work` 等）
- [ ] 记录 triage 响应时间（Tier 1 要求 ≤2 个工作日——从现在就开始按这个标准做，申请时才有数据）
- [ ] 争取 2~3 个外部贡献者（good first issue 标签）

---

## 4. 阶段 D：官方准入（C 有数据后，2~4 周流程）

### D1. 自评（1 周）
- 按 rust-sdk 的 `ROADMAP.md` 审计表逐行自评，产出一份 `conformance/results/2026-XX-XX-mcp-cpp-assessment.md` 入库（直接模仿它的格式）。
- 现实目标：**以 Tier 2 进入，附 Tier 1 路线图**。Tier 1 的 2 天 triage 与「随 spec 发布交付」需要自动化支撑才承诺。

### D2. 递交（第 1 周）
- [ ] `modelcontextprotocol/modelcontextprotocol` 开 Discussions：**Proposal: add C++ to the official SDK roster**（`WG-PROPOSAL.md` 草稿，填入 A 阶段的真实分数）
- [ ] Discord `#general-sdk-dev` 同步，申请一次双周会演示席位
- [ ] 递交前确认：博客已发（有可见需求）、conformance PR 已开（已是贡献者）

### D3. 谈判要点（提前备好答案）
| 对方问题 | 我们的答案 |
| --- | --- |
| 为什么 C++ 需要 SDK？ | 数据库/引擎/嵌入式/交易系统的宿主语言；zorm-mcp 是真实生产用户 |
| 已有两个社区 C++ SDK？ | 都未过 conformance、不在治理范围内；官方 SDK 的意义就是整合 |
| 单人维护 bus factor？ | triage 数据 + CI 全自动 + 愿意移交仓库到 modelcontextprotocol org + 招共同维护者 |
| Tier 1 承诺？ | 先 Tier 2 + 路线图；自动化跑通后再升 Tier 1（rust-sdk 也是 Tier 3→1 走过来的） |
| 2026 场景缺失？ | 我们已贡献回上游（C4） |

### D4. 完成判据
- [ ] WG 共识 → Core Maintainer 批准
- [ ] 仓库进入 modelcontextprotocol org（或按其要求的形式）
- [ ] `modelcontextprotocol.io/docs/sdk` 出现 C++ 行

---

## 5. 时间线总览

```
周 1-2   A 证据（2026 分数 / client fixture / HMAC / 黄金测试 / 治理文档）
周 3-6   B API 补齐（与 A 尾部并行）＋ C1 发布准备
周 5-8   C 推广（博客 → 生态占位 → 上游 PR）
周 9-12  D 准入（自评 → 递交 → WG 流程）
```

关键路径：**A1（2026 分数）→ C2（博客）→ D2（提案）**。其余可并行、可调整。

---

## 6. 风险与对策

| 风险 | 对策 |
| --- | --- |
| 2026 套件暴露大量差异，A1 拖期 | 差异本身就是资产：修一个记一个，成为博客素材；必要时分两批跑 |
| HMAC 依赖选型（OpenSSL vs 自写） | clang64 有 OpenSSL；若想保持零依赖，自写 HMAC-SHA256（~150 行，有公开测试向量） |
| WG 对「又一个 C++ SDK」冷淡 | 先上游贡献（C4）+ 真实用户（zorm-mcp）+ 分数，避免空手提案 |
| 单人维护质疑 | 从 C5 开始就积累 triage 数据；提案时附 |
| 官方 SDK 迭代太快，我们的 2026 支持过时 | rust/TS 都已稳定在 2026-07-28；订阅 spec repo 的 SEP 通知，Tier-1 节奏跟进 |

---

## 7. 立即行动清单（下一步执行顺序）

1. 升级 conformance 套件版本，跑 2026-07-28 server 套件 → 拿到第一个官方 2026 分数（半天）
2. 按差异清单修复（1~3 天，视差距而定）
3. HMAC requestState codec（1 天）
4. wire 黄金测试（半天）
5. VERSIONING/DEPENDENCY_POLICY/ROADMAP 重写（1 天）

完成 1~5 后，阶段 C 的发布动作就全部有了弹药。

---

*依据：`SESSION-REVIEW.md` §六（rust-sdk / typescript-sdk 深读）。执行记录随进展更新到本文与 `ROADMAP.md`。*
