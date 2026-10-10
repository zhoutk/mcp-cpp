> Language: **English** | [中文](ADOPTION-PLAN.zh-CN.md)
> Date: 2026-10-09 · Owner: zhoutk · Target repo: `github.com/zhoutk/mcp-cpp`

# mcp-cpp Adoption Plan — Becoming the Official C++ MCP SDK

This is the process half of the plan; the technical half is
[OPTIMIZATION-PLAN.md](OPTIMIZATION-PLAN.md). Read both before starting.

---

## 1. The opportunity, stated precisely

Verified against [modelcontextprotocol.io/docs/sdk](https://modelcontextprotocol.io/docs/sdk)
and the [SDK Working Group charter](https://modelcontextprotocol.io/community/sdk/charter)
on 2026-10-09:

- The official SDK roster is **TypeScript, Python, C#, Go, Rust, Ruby** (Tier 1),
  **Java** (Tier 2), **Swift, PHP, Kotlin** (Tier 3). **There is no C++ SDK — not
  official, not community-listed, not tiered.**
- The official docs page has **no community-SDK section**, so there is no
  lower-effort "get listed as community" path. The only path is admission to the
  official roster.
- Community C++ efforts exist but are not sanctioned — e.g. IBM's TSAR-MCP (C/C++,
  AIX/IBM i focus) and itcv-GmbH/cpp-mcp-sdk (2025-11-25 target). Neither is
  governed by the MCP project; the WG charter explicitly scopes
  *"third-party and community SDKs"* out.

**The gap is real and unclaimed.** A mature, conformance-passing C++17 SDK with a
credible maintainer is the missing entry, and C++ is the language most of the
world's performance-sensitive infrastructure (databases, embedded, game engines,
trading, HPC) is written in.

---

## 2. The admission path (there is no RFC — it is a WG decision)

The only governance document that covers SDK admission is the **SDK Working Group
charter**. Direct quotes:

- Scope (In): *"**Official SDK Roster**: Evaluating proposals to add new official
  SDKs or retire existing ones."*
- Decision authority: *"Adding or retiring an official SDK | WG consensus → Core
  Maintainer approval."*
- Membership: *"Maintainers of any official SDK are WG Members by default."*
- Operations: biweekly 45-minute session; `#general-sdk-dev` on Discord; GitHub
  Discussions.

So the path is:

```
Propose to the SDK WG  →  WG consensus  →  Core Maintainer approval  →  listed
        ▲                                        ▲
        └──────────── conformance score + tier self-assessment ───────┘
```

Tier advancement (a separate, later step) is a public process: self-assess,
[open an issue in modelcontextprotocol/modelcontextprotocol](https://github.com/modelcontextprotocol/modelcontextprotocol)
with evidence, pass automated conformance, receive WG approval.

---

## 3. The bar

Tier requirements (from the [SDK Tiering System](https://modelcontextprotocol.io/community/sdk-tiers)):

| Requirement | Tier 1 | Tier 2 | Tier 3 |
| --- | --- | --- | --- |
| Conformance tests | **100%** | 80% | none |
| New protocol features | before spec release | within 6 months | none |
| Issue triage | ≤ 2 business days | ≤ 1 month | none |
| Critical bug resolution | ≤ 7 days | ≤ 2 weeks | none |
| Stable release | required, clear versioning | ≥ 1 stable release | none |
| Documentation | comprehensive, all features | core features | none |
| Dependency policy | published | published | none |
| Roadmap | published | published or explained | none |

**Realistic first target: Tier 2 on admission, with a published Tier-1 roadmap.**
Tier 1 is achievable but the 2-day triage and "before spec release" cadence are
sustained commitments, not one-off tasks — promise them only once the CI and
issue automation exist (Milestone A/B in the optimization plan).

---

## 4. Phased plan

### Phase 0 — Publish (this week)

- [ ] Create `github.com/zhoutk/mcp-cpp` (public, MIT). Push the standalone tree
      (`git subtree split` out of the zorm repo, or push the submodule repo
      directly — it is already self-contained).
- [ ] Confirm the standalone build from a clean clone on all three OSes.
- [ ] Add CI badges to the README.

### Phase 1 — Be adoptable (weeks 1–2)

Do **Milestone A** of the optimization plan in full. A reviewer's first five
minutes decide everything; packaging, CI, ROADMAP, CONTRIBUTING, SECURITY and
issue templates are what they look for. Then:

- [ ] Tag **v0.1.0** with a CHANGELOG, then drive to **v1.0.0** once conformance
      is green (a stable release is a Tier-2 requirement).

### Phase 2 — Prove conformance (weeks 2–4)

- [x] Run the official suite → **26/30 scored server scenarios (86.7%)**, suite
      v0.1.16. `examples/conformance_server.cpp` is the fixture; the report is
      committed at `conformance/report.json`.
- [x] Fix the failures it surfaced (legacy `ping`/`logging`/`resources/subscribe`,
      legacy `resultType`, SSE streaming of mid-request notifications).
- [x] Publish `conformance/report.json` and `conformance/expected-failures.yaml`
      (the 4 server-initiated-request scenarios).
- [ ] **Finding for the WG:** suite v0.1.16 has **no 2026-07-28 scenarios**; its
      valid versions are 2025-03-26/2025-06-18/2025-11-25/draft/extension. A
      2026-07-28 score cannot be produced until the suite adds them — raise this
      in the proposal.
- [ ] Wire the GitHub Action into CI and run `tier-check` for the self-assessment.

See [CONFORMANCE.md](CONFORMANCE.md) for the concrete wiring.

### Phase 3 — Announce and build demand (weeks 3–6, overlapping)

Adoption is easier to argue when there are already users. Concrete channels:

- [ ] Blog post: "A conformance-passing C++17 MCP SDK" (link the conformance
      report; be honest about the serde_json parse gap and why it exists).
- [ ] Post to `r/mcp`, Hacker News, `r/cpp`, the `#general-sdk-dev` MCP Discord,
      and MCP GitHub Discussions.
- [ ] Submit to community directories (Awesome-MCP lists, Smithery/Pulse MCP SDK
      pages) so "C++" is no longer blank.
- [ ] Ship two flagship consumers as proof:
      **[zorm-mcp](https://github.com/zhoutk/zorm-mcp)** (multi-backend database
      MCP server) and **zorm** (the ORM it builds on) — both are complete
      applications built on mcp-cpp.
- [ ] Land the vcpkg port and open a ConanCenter PR.

### Phase 4 — Engage the Working Group (weeks 4–8)

- [ ] Read the current [SDK WG charter](https://modelcontextprotocol.io/community/sdk/charter)
      and the WG's recent meeting notes for the exact proposal format.
- [ ] Open a GitHub Discussion in `modelcontextprotocol/modelcontextprotocol`
      proposing C++ as an official SDK, linking: the repo, the conformance
      report, the tier self-assessment, and the flagship consumers. Draft text:
      [WG-PROPOSAL.md](WG-PROPOSAL.md).
- [ ] Ask for a slot in a biweekly WG session to present.
- [ ] Expect the maintainership question. Be ready to:
  - transfer the repo to the `modelcontextprotocol` org, and
  - serve as its maintainer (WG Members are the SDK maintainers), and
  - commit to the triage/cadence SLAs you can actually meet.
- [ ] Address the "two community C++ SDKs already exist" objection head-on:
      neither passes the conformance suite; mcp-cpp does, and it is the one
      asking to be governed.

### Phase 5 — Sustain (ongoing)

- [ ] Publish the ROADMAP and update it every release.
- [ ] Keep the conformance job green (relegation: any failure for 4 weeks drops
      Tier 1 → Tier 2).
- [ ] Track the next spec revision before it ships (Tier-1 cadence).

---

## 5. Positioning: the one-paragraph pitch

> There is no official C++ SDK for MCP. C++ is where performance-critical
> infrastructure lives, and the current community C++ options do not pass the
> conformance suite. mcp-cpp is a C++17, zero-dependency (beyond a bundled JSON
> header and optional cpp-httplib) SDK that implements the 2026-07-28
> specification, passes the official conformance suite, is already exercised by
> two real applications (zorm-mcp, zorm), and is offered to the MCP project under
> its governance.

---

## 6. Risks and honest counters

| Risk | Counter |
| --- | --- |
| "Why not TS/Python for servers?" | C++ matters for embedding in existing C++ services, edge/embedded, and latency-sensitive DB/engine integrations — exactly zorm-mcp's use case. |
| "Two C++ SDKs already exist." | Neither is conformance-passing or governed; consolidation is the point of an official SDK. |
| "Single-maintainer bus factor." | Publish the governance and roadmap; recruit co-maintainers from the C++ MCP community; the CI/conformance job makes contribution low-risk. |
| "Tier-1 SLA is a trap." | Enter as Tier 2 with a Tier-1 roadmap; promise Tier 1 only once triage automation exists. |
| "Repo is on internal GitLab." | Mirror to GitHub first (Phase 0); the GitHub repo is the one proposed. |

---

*Maintainer: zhoutk. Facts in §1–§3 were verified against
modelcontextprotocol.io on 2026-10-09; re-verify before proposing, as the roster
and tier assignments change.*
