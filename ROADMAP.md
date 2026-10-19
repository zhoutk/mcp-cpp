> Language: **English** | [中文](ROADMAP.zh-CN.md)

# Roadmap

This is the public roadmap required by the [MCP SDK Tiering System](https://modelcontextprotocol.io/community/sdk-tiers).
It tracks the concrete work needed to reach **Tier 1** (100% conformance).
How to run the conformance suites is in [docs/CONFORMANCE.md](docs/CONFORMANCE.md);
the architecture this roadmap evolves is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

Legend: ✅ done · 🔄 in progress · ⏳ planned · ❌ not planned

## Milestone A — Adoptable

Everything a consumer or reviewer needs to take the project seriously.

| Item | Status |
| --- | --- |
| CMake `install()` / `export` + `find_package(mcp CONFIG)` | ✅ |
| Bilingual README + PLAN | ✅ |
| Progress notifications, `outputSchema` enforcement, structured results | ✅ |
| Client wrappers (`listResources`, `listPrompts`, `complete`) | ✅ |
| `ROADMAP.md`, `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md`, `CHANGELOG.md` | ✅ |
| `.github/` issue + PR templates, triage labels | ✅ |
| GitHub Actions CI matrix (Linux/macOS/Windows × clang/gcc/msvc), plus TSan, sanitizer, install-consumer, C++23 and stdio-only jobs | ✅ |
| vcpkg port | ⏳ |

## Milestone B — Conformant (Tier 2)

| Item | Status |
| --- | --- |
| Run the official `modelcontextprotocol/conformance` server suite | ✅ 2026-07-28: **42/42 (100%)** · 2025-11-25: 26/30 |
| Reach 100% of applicable required scenarios | ✅ on 2026-07-28 · 🔄 on 2025-11-25 (4 blocked, see below) |
| Publish `conformance/report.json` + an `--expected-failures` baseline | ✅ |
| Client-mode conformance run (`examples/conformance_client.cpp`) | ✅ 2026-07-28: **32/32 scenarios (100%)**; 2025-11-25: 17/18 (+2 legacy 2025-03-26) |
| Client-mode `--expected-failures` baseline | ✅ one harness-mock scenario, with rust-sdk A/B evidence |
| In-repo **2026-07-28** conformance suite (`mcp_test_conformance_2026`, 12 scenarios incl. the full MRTR round trip) | ✅ |
| Stable **v1.0.0** release | ⏳ |

The four remaining server failures (`tools-call-sampling`,
`tools-call-elicitation`, `elicitation-sep1034-defaults`,
`elicitation-sep1330-enums`) all require the server to send a request to the
client mid-call. The 2026-07-28 spec replaced that with MRTR, which mcp-cpp
implements; the suite has no 2026 scenario to score instead.

## Milestone C — Tier-1 ready

| Item | Status |
| --- | --- |
| OAuth 2.1 client + Client ID Metadata Documents (remote HTTP) | ✅ client side (PRM/AS discovery, DCR/CIMD/pre-registration, PKCE, scope step-up, SEP-2352) · ⏳ server side |
| Typed MRTR / elicitation helpers (`Ctx::elicit`, `Ctx::inputRequired`, client `retry`) | ✅ |
| Signed `requestState` codec (HMAC-SHA256, context-bound, TTL) + server binding + client auto-fulfil | ✅ |
| Logging helper (`Ctx::log`) + `_meta` log-level plumbing | ⏳ |
| Doxygen API reference; client + HTTP examples | ⏳ |
| Zero-copy parse optimization (re-measure B1) | ⏳ |
| Conan recipe | ⏳ |

## Beyond Tier 1

| Item | Status |
| --- | --- |
| Tasks extension (`io.modelcontextprotocol/tasks`) | ⏳ |
| Multi-worker dispatch (compile-time `MCP_WORKERS`) | ⏳ |
| Legacy revision semantics (opt-in) | ⏳ |
| MCP Apps (UI extension) | ❌ out of scope |
| Roots / Sampling | ❌ deprecated by SEP-2577 |

## Cadence commitment

- New spec features: targeted **before each spec release** (Tier-1 cadence) once
  Milestone B is complete.
- Issue triage: ≤ 2 business days; critical (P0) bugs: ≤ 7 days.
- Conformance job stays green on `main`.

*Last updated: 2026-10-15.*
