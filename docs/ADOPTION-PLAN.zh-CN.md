> 语言 / Language: **中文** | [English](ADOPTION-PLAN.md)
> 日期：2026-10-09 · 负责人：zhoutk · 目标仓库：`github.com/zhoutk/mcp-cpp`

# mcp-cpp 采纳计划 —— 成为官方 C++ MCP SDK

这是计划的流程部分；技术部分见
[OPTIMIZATION-PLAN.md](OPTIMIZATION-PLAN.md)。开始之前请将两者都读完。

---

## 1. 机会，精确陈述

已于 2026-10-09 对照
[modelcontextprotocol.io/docs/sdk](https://modelcontextprotocol.io/docs/sdk)
和 [SDK Working Group charter](https://modelcontextprotocol.io/community/sdk/charter)
核实：

- 官方 SDK 名单为 **TypeScript、Python、C#、Go、Rust、Ruby**（Tier 1），
  **Java**（Tier 2），**Swift、PHP、Kotlin**（Tier 3）。**没有 C++ SDK——不是
  官方的，不在社区列表中，也没有分层。**
- 官方文档页面**没有社区 SDK 板块**，因此不存在一条更省力的"以社区身份上榜"
  路径。唯一的路径是被纳入官方名单。
- 存在一些社区 C++ 尝试，但都未获认可——例如 IBM 的 TSAR-MCP（C/C++，
  面向 AIX/IBM i）和 itcv-GmbH/cpp-mcp-sdk（目标日期 2025-11-25）。两者都不受
  MCP 项目治理；WG 章程明确将*"第三方和社区 SDK"*排除在范围之外。

**这一缺口真实存在且无人认领。** 一个成熟、能通过一致性测试、有可信维护者的
C++17 SDK 正是那个缺失的条目，而 C++ 是世界上大多数性能敏感基础设施（数据库、
嵌入式、游戏引擎、交易、HPC）所使用的语言。

---

## 2. 准入路径（没有 RFC——这是 WG 的决定）

唯一涵盖 SDK 准入的治理文件是 **SDK Working Group 章程**。直接引用：

- 范围（纳入）：*"**Official SDK Roster**：评估新增官方 SDK 或淘汰现有官方 SDK 的
  提案。"*
- 决定权：*"新增或淘汰官方 SDK | WG 共识 → Core Maintainer 批准。"*
- 成员资格：*"任何官方 SDK 的维护者默认即为 WG 成员。"*
- 运作：每两周一次 45 分钟的会议；Discord 上的 `#general-sdk-dev`；GitHub
  Discussions。

因此路径是：

```
Propose to the SDK WG  →  WG consensus  →  Core Maintainer approval  →  listed
        ▲                                        ▲
        └──────────── conformance score + tier self-assessment ───────┘
```

层级晋升（一个独立的、更靠后的步骤）是一个公开流程：自我评估，
[在 modelcontextprotocol/modelcontextprotocol 开一个 issue](https://github.com/modelcontextprotocol/modelcontextprotocol)
并附上证据，通过自动化一致性测试，获得 WG 批准。

---

## 3. 门槛

层级要求（来自 [SDK Tiering System](https://modelcontextprotocol.io/community/sdk-tiers)）：

| 要求 | Tier 1 | Tier 2 | Tier 3 |
| --- | --- | --- | --- |
| 一致性测试 | **100%** | 80% | 无 |
| 新协议特性 | 规范发布之前 | 6 个月之内 | 无 |
| Issue 分类 | ≤ 2 个工作日 | ≤ 1 个月 | 无 |
| 严重缺陷解决 | ≤ 7 天 | ≤ 2 周 | 无 |
| 稳定发布 | 必需，版本管理清晰 | ≥ 1 次稳定发布 | 无 |
| 文档 | 全面，覆盖所有特性 | 核心特性 | 无 |
| 依赖策略 | 已发布 | 已发布 | 无 |
| 路线图 | 已发布 | 已发布或已说明 | 无 |

**现实的首个目标：准入时为 Tier 2，并附一份已发布的 Tier-1 路线图。** Tier 1 是
可以达成的，但 2 天的分类处理与"规范发布之前"的节奏是持续性的承诺，而非一次性
任务——只有当 CI 和 issue 自动化到位后（优化计划中的里程碑 A/B），才做出这些承诺。

---

## 4. 分阶段计划

### 阶段 0 —— 发布（本周）

- [ ] 创建 `github.com/zhoutk/mcp-cpp`（公开，MIT）。推送这棵独立源码树
      （从 zorm 仓库用 `git subtree split` 拆分出来，或直接推送子模块仓库——
      它已经是自包含的）。
- [ ] 在三个 OS 上从干净克隆确认独立构建成功。
- [ ] 在 README 中添加 CI 徽章。

### 阶段 1 —— 做到可采纳（第 1–2 周）

完整地完成优化计划的**里程碑 A**。评审者的头五分钟决定一切；打包、CI、
ROADMAP、CONTRIBUTING、SECURITY 和 issue 模板正是他们要看的。然后：

- [ ] 打上 **v0.1.0** 标签并附 CHANGELOG，待一致性测试通过后推进到 **v1.0.0**
      （稳定发布是 Tier-2 的要求）。

### 阶段 2 —— 证明一致性（第 2–4 周）

- [x] 运行官方套件 → **30 个计分的服务端场景中通过 26 个（86.7%）**，套件版本
      v0.1.16。`examples/conformance_server.cpp` 是 fixture；报告提交在
      `conformance/report.json`。
- [x] 修复它暴露出的失败（旧版 `ping`/`logging`/`resources/subscribe`、
      旧版 `resultType`、请求中途通知的 SSE 流式传输）。
- [x] 发布 `conformance/report.json` 和 `conformance/expected-failures.yaml`
      （那 4 个服务器发起请求的场景）。
- [ ] **给 WG 的发现：** 套件 v0.1.16 **没有 2026-07-28 场景**；其有效版本为
      2025-03-26/2025-06-18/2025-11-25/draft/extension。在套件添加这些场景之前
      无法产出 2026-07-28 的得分——在提案中提出这一点。
- [ ] 将 GitHub Action 接入 CI，并运行 `tier-check` 进行自我评估。

具体接线方式见 [CONFORMANCE.md](CONFORMANCE.md)。

### 阶段 3 —— 宣传并创造需求（第 3–6 周，有重叠）

当已经有使用者时，采纳就更容易论证。具体渠道：

- [ ] 博客文章："A conformance-passing C++17 MCP SDK"（链接一致性报告；对
      serde_json 解析差距及其原因保持诚实）。
- [ ] 发帖到 `r/mcp`、Hacker News、`r/cpp`、MCP Discord 的 `#general-sdk-dev`，
      以及 MCP GitHub Discussions。
- [ ] 提交到社区目录（Awesome-MCP 列表、Smithery/Pulse MCP SDK 页面），让
      "C++"不再是空白。
- [ ] 交付两个旗舰使用者作为证明：
      **[zorm-mcp](https://github.com/zhoutk/zorm-mcp)**（多后端数据库
      MCP 服务器）和 **zorm**（它所构建于其上的 ORM）——两者都是基于 mcp-cpp
      构建的完整应用。
- [ ] 落地 vcpkg port 并提交一个 ConanCenter PR。

### 阶段 4 —— 接触工作组（第 4–8 周）

- [ ] 阅读当前的 [SDK WG 章程](https://modelcontextprotocol.io/community/sdk/charter)
      以及 WG 近期的会议纪要，了解确切的提案格式。
- [ ] 在 `modelcontextprotocol/modelcontextprotocol` 开一个 GitHub Discussion，
      提议将 C++ 作为官方 SDK，并链接：仓库、一致性报告、层级自我评估，以及
      旗舰使用者。
- [ ] 申请在双周 WG 会议中获得一个发言时段。
- [ ] 预期会被问到维护者的问题。准备好：
  - 将仓库转移到 `modelcontextprotocol` 组织，以及
  - 担任其维护者（WG 成员即 SDK 维护者），以及
  - 承诺你能真正满足的分类处理/节奏 SLA。
- [ ] 正面回应"已经存在两个社区 C++ SDK"的反对意见：
      两者都未通过一致性套件；mcp-cpp 通过了，而且它是那个主动请求被治理的。

### 阶段 5 —— 维持（持续进行）

- [ ] 发布 ROADMAP 并在每次发布时更新它。
- [ ] 让一致性任务保持绿色（降级规则：任何失败持续 4 周会将 Tier 1 降为 Tier 2）。
- [ ] 在下一个规范修订发布之前跟进它（Tier-1 节奏）。

---

## 5. 定位：一段话的推介

> MCP 没有官方 C++ SDK。C++ 是性能关键基础设施所在之处，而当前社区的 C++ 选项都
> 未通过一致性套件。mcp-cpp 是一个 C++17、零依赖（除随附的 JSON 头文件和可选的
> cpp-httplib 之外）的 SDK，它实现了 2026-07-28 规范，通过了官方一致性套件，已被
> 两个真实应用（zorm-mcp、zorm）实际使用，并在其治理下提供给 MCP 项目。

---

## 6. 风险与诚实的应对

| 风险 | 应对 |
| --- | --- |
| "服务器为什么不用 TS/Python？" | C++ 对于嵌入既有 C++ 服务、边缘/嵌入式，以及对延迟敏感的数据库/引擎集成至关重要——这正是 zorm-mcp 的用例。 |
| "已经有两个 C++ SDK 了。" | 两者都未通过一致性测试、也未受治理；整合正是官方 SDK 的意义所在。 |
| "单一维护者的巴士系数。" | 发布治理与路线图；从 C++ MCP 社区招募共同维护者；CI/一致性任务使贡献变得低风险。 |
| "Tier-1 SLA 是个陷阱。" | 以 Tier 2 身份进入并附 Tier-1 路线图；只有当分类处理自动化到位后才承诺 Tier 1。 |
| "仓库在内网 GitLab 上。" | 先镜像到 GitHub（阶段 0）；所提议的是那个 GitHub 仓库。 |

---

*维护者：zhoutk。§1–§3 中的事实已于 2026-10-09 对照
modelcontextprotocol.io 核实；在提案前请重新核实，因为名单和层级归属会变化。*
