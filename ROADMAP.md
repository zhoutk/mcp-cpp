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
| vcpkg port + Conan recipe (`packaging/vcpkg`, `packaging/conan`; overlay/local build, not yet in the registries) | ✅ |

## Milestone B — Conformant (Tier 2)

| Item | Status |
| --- | --- |
| Run the official `modelcontextprotocol/conformance` server suite | ✅ 2026-07-28: **42/42 (100%)** · 2025-11-25: 26/30 |
| Reach 100% of applicable required scenarios | ✅ on 2026-07-28 · 🔄 on 2025-11-25 (4 blocked, see below) |
| Publish `conformance/report.json` + an `--expected-failures` baseline | ✅ |
| Client-mode conformance run (`examples/conformance_client.cpp`) | ✅ 2026-07-28: **32/32 scenarios (100%)**; 2025-11-25: 17/18 (+2 legacy 2025-03-26; upstream fix pending release) |
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
| OAuth 2.1 client + Client ID Metadata Documents (remote HTTP) | ✅ client side (PRM/AS discovery, DCR/CIMD/pre-registration, PKCE, scope step-up, SEP-2352) · ✅ server side: `mcp::oauth::AuthorizationServer` (in-memory core for loopback/dev/test: DCR, code + PKCE S256, RFC 8707 binding, metadata) |
| Typed MRTR / elicitation helpers (`Ctx::elicit`, `Ctx::inputRequired`, client `retry`) | ✅ |
| Signed `requestState` codec (HMAC-SHA256, context-bound, TTL) + server binding + client auto-fulfil | ✅ |
| Logging helper (`Ctx::log`) + `_meta` log-level plumbing (spec gating polished in D6) | ✅ |
| Client request timeout / cancellation API (per-request deadline + watchdog) | ✅ |
| Doxygen API reference; client + HTTP examples | ⏳ |
| Zero-copy parse optimization (re-measure B1) | ⏳ |

## Milestone D — v0.4 spec calibration (2026-10 review runway)

Findings of the full review against the local 2026-07-28 specification snapshot
(conformance already at **100%** on both server and client suites for 2026-07-28;
these are the remaining MUST/SHOULD deviations, all edge-path and locally fixable).

| Item | Class |
| --- | --- |
| D1 · Base64 sentinel re-encoding: a plain-ASCII value matching `=?base64?…?=` MUST be re-encoded before mirroring; decoder handles only true encodings | 🔜 P0 |
| D2 · `MissingRequiredClientCapabilityError.data.requiredCapabilities` emitted as a `ClientCapabilities` object, not a string array | 🔜 P0 |
| D3 · `io.modelcontextprotocol/subscriptionId` type fidelity: stream notifications keep the listen request's JSON-RPC id type (numeric stays numeric — ack/closure already do) | 🔜 P0 |
| D4 · `x-mcp-header` validation tightened to spec: `1*tchar` header names, no `items`/composition reachability, annotations inside composition keywords invalidate the tool, registry-side self-validation | 🔜 P0 |
| D5 · `Client::listTools` excludes tools whose annotations violate D4 (rule moves from the conformance fixture into the library) | 🔜 P0 |
| D6 · `notifications/message` gating: never emit for a request without `_meta.logLevel` (drop the server-default path); reject invalid level values with `-32602` | 🔜 P1 |
| D7 · Optional principal binding for sealed `requestState` (integrity-vs-identity split of the codec) | 🔜 P1 |
| D8 · `serverInfo` in every result `_meta` (configurable) + `initialize` rejections name the supported versions | 🔜 P1 |
| D9 · Builder API to advertise `listChanged`/`subscribe` capability flags (fan-out already works) | 🔜 P1 |
| D10 · Numeric (not string) comparison for integer `Mcp-Param-*` header validation | 🔜 P1 |
| D11 · Token-validation hook on `HttpServerTransport` (resource-server audience checks have no surface today) | 🔜 P1 |
| D12 · Extension negotiation enforcement: fall back to core behavior or reject when an advertised extension is unsupported | 🔜 P1 |

Exit gate: official 2026-07-28 suites stay at 100% after every item; error-surface
pin tests lock the changed wire shapes.

## Milestone E — Reference-fed (v0.5)

Adopted from @modelcontextprotocol official SDKs (python 2.3 / rust rmcp 3.5 /
typescript 2.3), ranked by value; difficulty assessed for C++17, value-semantic,
no async runtime.

| Item | Source |
| --- | --- |
| E1 · Tasks extension (SEP-2663, `io.modelcontextprotocol/tasks`): task records, TTL, pluggable task store, `tasks/get`/`tasks/update`/`tasks/cancel`, `Mcp-Name` mirroring | all three |
| E2 · Client era auto-negotiation: probe `server/discover` → classify (modern error / legacy / timeout) → fall back to `initialize` or pin the version | ts `versionNegotiation`, py `_probe` |
| E3 · Client-side response cache honoring `ttlMs`, respecting `cacheScope`, discover exempt | ts `responseCache.ts` |
| E4 · Extension seam: `Builder::extension()` with `(method, version)` bindings, no shadowing of spec methods (SEP-2133 shape) | py `server/extension.py` |
| E5 · Configurable SSE keep-alive (validation on disable) | ts `sseKeepAlive.ts` |
| E6 · Golden wire fixtures: pinned spec schema twins + error-surface pin tests | ts corpus, py pinned schemas |
| E7 · MRTR hardening: client retry-round cap, key zeroize | py defaults, rust zeroize |
| E8 · Optional: subscription-bus abstraction; middleware chain; legacy `input_required` shim (dual-era handlers) | hard, deferred |

## Beyond Tier 1

| Item | Status |
| --- | --- |
| Tasks extension (`io.modelcontextprotocol/tasks`) | 🔄 planned as E1 (Milestone E) |
| Multi-worker dispatch (compile-time `MCP_WORKERS`) | ⏳ |
| Legacy revision semantics (opt-in) | ✅ dual-era mode: `initialize`, meta-less requests, `ping`, `logging/setLevel`, resource subscriptions |
| MCP Apps (UI extension) | ❌ out of scope |
| Roots / Sampling | ❌ deprecated by SEP-2577 |

## Cadence commitment

- New spec features: targeted **before each spec release** (Tier-1 cadence) once
  Milestone B is complete.
- Issue triage: ≤ 2 business days; critical (P0) bugs: ≤ 7 days.
- Conformance job stays green on `main`.

*Last updated: 2026-10-20.*
