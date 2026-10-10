# WG proposal draft — C++ on the official MCP SDK roster

> Language: **English** | [中文](WG-PROPOSAL.zh-CN.md)

**Status: draft, not yet posted.** This is the text to post in the
`modelcontextprotocol/modelcontextprotocol` GitHub Discussions (and to raise in
the `#general-sdk-dev` Discord channel), per the SDK Working Group charter's
"Official SDK Roster" scope. Post it only after the repository is public and CI
is green. See [ADOPTION-PLAN.md](ADOPTION-PLAN.md) Phase 4.

Fill in the bracketed placeholders before posting.

---

## Title

**Proposal: add C++ to the official MCP SDK roster (`mcp-cpp`)**

## Body

### Summary

There is no official C++ SDK for MCP. I would like to propose
[`zhoutk/mcp-cpp`](https://github.com/zhoutk/mcp-cpp) for the official SDK roster,
and I am offering to maintain it under the project's governance.

C++ is where a large part of the world's performance-sensitive infrastructure
lives — databases, game engines, embedded and edge devices, trading systems, HPC.
Those systems are increasingly the ones exposing tools and data to agents, and
today they have to either bind to a non-C++ SDK or hand-roll the protocol.

### What it is

- A C++17 SDK targeting the **2026-07-28** specification. Value-semantic message
  model, exception-free protocol layer, no async runtime required.
- Dependencies: a bundled single-header JSON library and, optionally, cpp-httplib
  for the Streamable HTTP transport. Nothing else.
- Implements `server/discover`, stateless per-request `_meta`, `tools/*`,
  `resources/*` (including URI templates), `prompts/*`, `completion/complete`,
  `subscriptions/listen`, MRTR, cancellation, progress, and CacheableResult, on
  both stdio and Streamable HTTP (with mirrored-header enforcement).
- Validates tool `inputSchema` and `outputSchema` with a bundled JSON Schema
  2020-12 validator on every call.

### Evidence

| Claim | Artifact |
| --- | --- |
| Passes the official conformance suite | [`conformance/report.json`](../conformance/report.json) — **26/30 scored server scenarios (86.7%)**, suite v0.1.16 |
| Known gaps are documented, not hidden | [`conformance/expected-failures.yaml`](../conformance/expected-failures.yaml) |
| 2026-07-28 behaviour is tested | `tests/test_conformance_2026.cpp` — 12 named scenarios, including a full MRTR round trip |
| Real-world use | [zorm-mcp](https://github.com/zhoutk/zorm-mcp) (13 database tools over five backends) and [zorm](https://github.com/zhoutk/zorm) |
| Engineering baseline | CMake package (`find_package(mcp CONFIG)`), CI matrix, published roadmap, dependency policy, issue/PR templates, `SECURITY.md` |
| Tier self-assessment | [ROADMAP.md](../ROADMAP.md) |

### The four remaining conformance failures — and why

All four (`tools-call-sampling`, `tools-call-elicitation`,
`elicitation-sep1034-defaults`, `elicitation-sep1330-enums`) require the **server
to send a request to the client mid-call**. The 2026-07-28 specification replaced
that mechanism with MRTR, which this SDK implements and tests. Those scenarios
are tagged `[2025-06-18, 2025-11-25]` only; there is no 2026 equivalent to pass.

### One finding worth the working group's attention

Suite **v0.1.16 has no 2026-07-28 scenarios**. Its accepted spec versions are
`2025-03-26`, `2025-06-18`, `2025-11-25`, `draft` and `extension`. An SDK that
targets the current specification revision therefore cannot be scored against it
today — it can only be scored on its 2025-era backwards compatibility. If C++
joins the roster, I would want to contribute 2026-07-28 server scenarios
(including MRTR) upstream so the gap closes for every SDK, not just this one.

### Maintenance commitment

- I will be the maintainer, and I accept the Working Group's tier requirements.
- Realistically: **Tier 2 at admission, with a published Tier-1 roadmap**, moving
  to Tier 1 once the conformance job and issue triage automation have a track
  record. Promising Tier 1's 2-business-day triage on day one would be a promise
  I could not yet evidence.
- I am willing to transfer the repository to the `modelcontextprotocol`
  organisation if that is the expected form.

### Questions for the working group

1. Is a C++ SDK in scope for the roster, and is there a preferred proposal format
   beyond this discussion?
2. Is there an existing effort I should be coordinating with rather than
   duplicating? (I am aware of IBM's TSAR-MCP and `itcv-GmbH/cpp-mcp-sdk`; neither
   is governed by the project or passes the conformance suite.)
3. Would the working group want the 2026-07-28 conformance scenarios contributed
   before, or alongside, the roster decision?
4. What does "official" mean concretely for a repo — org transfer, a
   `MAINTAINERS.md` entry, listing on `modelcontextprotocol.io/docs/sdk`?

---

## Posting checklist

- [ ] Repository public, CI green, badges resolving.
- [ ] `conformance/report.json` committed and linked from the README.
- [ ] A stable release tagged.
- [ ] Announcement posted first (see [LAUNCH.md](LAUNCH.md)), so there is visible
      demand to cite.
- [ ] Fill in every `[bracketed]` placeholder above.
- [ ] Post in GitHub Discussions, then raise it in `#general-sdk-dev`.

*Related: [ADOPTION-PLAN.md](ADOPTION-PLAN.md), [LAUNCH.md](LAUNCH.md),
[CONFORMANCE.md](CONFORMANCE.md).*
