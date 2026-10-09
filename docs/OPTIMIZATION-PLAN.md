> Language: **English** | [中文](OPTIMIZATION-PLAN.zh-CN.md)
> Date: 2026-10-09 · Baseline: mcp-cpp 0.1.0 · Spec target: MCP **2026-07-28**

# mcp-cpp Optimization Plan — Gap Analysis vs the Official MCP SDKs

This document compares mcp-cpp against the ten official MCP SDKs (TypeScript,
Python, C#, Go, Rust, Ruby, Java, Swift, PHP, Kotlin — see
[modelcontextprotocol.io/docs/sdk](https://modelcontextprotocol.io/docs/sdk))
and against the `modelcontextprotocol/conformance` suite, and turns the
differences into a prioritized work list. It is the technical half of
[ADOPTION-PLAN.md](ADOPTION-PLAN.md); read that one for the process of becoming
an official SDK.

The goal is not feature parity for its own sake — it is to clear the two
objective bars the MCP project uses: **100% conformance** (Tier 1) and the
**engineering baseline** (packaging, CI, docs, triage, dependency policy) that
every official SDK meets.

---

## 1. Where mcp-cpp stands today

Verified against the source tree and the 10 registered CTest suites (all green
on clang 22.1.8 / MSYS2 clang64 / Windows).

### Already at or above Tier-1 parity

| Capability | Evidence |
| --- | --- |
| JSON-RPC 2.0 strict MCP codec (null-id ban, no batching, object params, id fidelity to ±2^63) | `src/jsonrpc.cpp`, `tests/test_jsonrpc.cpp` |
| Five protocol versions + `-32022` negotiation with `supported`/`requested` data | `src/version.cpp`, `tests/test_version.cpp` |
| `server/discover` (supportedVersions / capabilities / ttlMs / cacheScope / serverInfo) | `src/server.cpp` |
| Stateless per-request `_meta` (protocolVersion + clientCapabilities required) | `src/types.cpp` |
| stdio transport (newline framing, Windows binary mode, EOF shutdown) | `src/transport.cpp`, `tests/test_stdio_e2e.cpp` |
| Streamable HTTP (POST-only, mirrored headers incl. `Mcp-Param-*`, 202/404/405, `-32020`) | `src/transport_http.cpp`, `tests/test_http_e2e.cpp` |
| SSE response streams + `subscriptions/listen` (ack, filter fan-out, graceful closure, keep-alive) | `src/transport_http.cpp`, `src/server.cpp` |
| tools / resources / prompts registries, capability gating, opaque pagination cursors | `src/server.cpp` |
| Resource URI templates (RFC 6570 subset) + template-fallback `resources/read` | `src/server.cpp` |
| `notifications/cancelled` + queryable cancellation flag | `src/server.cpp` |
| MRTR: `input_required` result + `inputResponses` retry | `src/server.cpp`, `src/client.cpp` |
| `completion/complete` (server handler + client wrapper) | `src/server.cpp`, `src/client.cpp` |
| CacheableResult (`ttlMs`/`cacheScope`) on discover, `*/list`, `resources/read` | `src/server.cpp` |
| JSON Schema 2020-12 validator (local `$ref`, resource bounds, no remote refs) | `src/schema.cpp` |
| `x-mcp-header` mirroring (client) + server-side validation | `src/header_params.h`, `src/transport_http.cpp` |
| **Progress notifications** — server emit (`Ctx::progress`) + client opt-in (`progressToken`) | *added 2026-10-09* |
| **outputSchema enforcement** — non-conforming `structuredContent` is rejected | *added 2026-10-09* |
| **Structured results** — `structuredResult()` helper adds the spec-recommended text block | *added 2026-10-09* |
| **CMake package** — `install()` + `find_package(mcp CONFIG)` verified with an out-of-tree consumer | *added 2026-10-09* |
| Dual-era serving (legacy `initialize` + modern `_meta`) behind an opt-in flag | `src/server.cpp` |

### Intentional non-goals

| Item | Why | Status |
| --- | --- | --- |
| Roots / Sampling | Deprecated by SEP-2577; 2026 servers do not initiate requests | Not implemented; unknown methods return `-32601` |
| Tasks extension (`io.modelcontextprotocol/tasks`) | Official extension; not required for any tier | Roadmap (v2) |
| MCP Apps (UI extension) | Out of a protocol SDK's scope | Not planned |

---

## 2. Gaps, by category

Severity key: **P0** = blocks official adoption; **P1** = needed for Tier 1 /
conformance; **P2** = competitive parity / DX.

### 2.1 Packaging & distribution (P0 — the biggest single gap)

Official SDKs are installable from their ecosystem's package manager in one
line (`npm i`, `pip install`, `cargo add`, `dotnet add package`, `go get`).
mcp-cpp's only distribution channel today is "copy the repo". C++ has no single
package manager, so a credible C++ SDK must meet consumers on all three common
paths.

| Item | State | Action |
| --- | --- | --- |
| `find_package(mcp CONFIG)` + exported `mcp::mcp` target | **Done (2026-10-09)** | — |
| `FetchContent` / `add_subdirectory` | Already works (self-contained tree) | Document |
| vcpkg port (`vcpkg.json` + portfile) | Missing | P0 |
| Conan recipe (ConanCenter) | Missing | P1 |
| pkg-config `.pc` file | Missing | P2 |

### 2.2 CI & quality gates (P0)

Tier 1 requires a 2-business-day triage and 7-day critical-bug SLA — impossible
to demonstrate without automation, and no `.github/` exists today.

| Item | State | Action |
| --- | --- | --- |
| GitHub Actions CI (Linux/macOS/Windows × clang/gcc/msvc × Debug/Release) | Missing | P0 |
| Conformance job in CI | Missing | P0 |
| Sanitizer job (ASan/UBSan) | Missing | P1 |
| Static analysis (`clang-tidy`) | Manual only | P2 |
| Coverage reporting | Missing | P2 |

### 2.3 Protocol completeness (P1)

| Item | Notes | Action |
| --- | --- | --- |
| OAuth 2.1 / Client ID Metadata Documents | Tier-1 SDKs ship auth; required for remote HTTP servers. Large. | P1 (v2) |
| ~~Typed MRTR/elicitation helpers~~ | **Done (2026-10-09)**: `Ctx::elicit`/`Ctx::inputRequired` with capability gating, `Client::retry` with exact `requestState` echo. | ✅ |
| Logging helper | `Ctx::notify` covers raw `notifications/message`; a `Ctx::log(level, data)` sugar + `_meta` log-level plumbing would match SDK ergonomics. | P2 |
| Tool annotations ergonomics | Definitions are raw JSON (pass-through works); typed helper would improve DX. | P2 |
| Progress rate-limiting helper | Spec SHOULD; currently the caller's responsibility. | P2 |
| Legacy revision semantics | Only the `initialize` shim; official SDKs implement all five revisions. Conformance excludes legacy unless claimed. | P2 (opt-in) |
| Client `roots`/`sampling` shims | Deprecated; official SDKs keep compat shims. | Not planned |

### 2.4 Performance (P2 — competitive, not blocking)

The honest baseline (PLAN.md Update 8): raw JSON parse is **0.79×** serde_json
at 1 KB and **0.38×** at 64 KB. This does not affect conformance, but it is a
visible number in a "why C++" pitch, so it should either be closed or framed
honestly (it currently is).

| Optimization | Expected effect | Effort |
| --- | --- | --- |
| Zero-copy `string_view` parse (zjson arena already supports borrowed views) | Largest single win, esp. 64 KB | M |
| Envelope pre-scan fast path (peek `method`/`id` before full decode) | Small messages | M |
| Pre-compile/cache tool `inputSchema`/`outputSchema` (currently validated per call) | B2 dispatch | S |
| `unordered_map` for the cancellation table (single-threaded dispatch) | Micro | S |

### 2.5 Documentation & community (P0/P1)

Tier 1 requires "comprehensive documentation with examples for all features",
a published roadmap, a dependency policy, and standardized issue labels.

| Item | State | Action |
| --- | --- | --- |
| Bilingual README (EN default + zh-CN) | **Done (2026-10-09)** | — |
| Bilingual PLAN / decision log | **Done (2026-10-09)** | — |
| Public ROADMAP | Missing | P0 |
| CONTRIBUTING + dependency/update policy | Missing | P0 |
| SECURITY.md (vulnerability reporting) | Missing | P0 |
| CODE_OF_CONDUCT.md | Missing | P0 |
| CHANGELOG.md (Keep a Changelog) | Missing | P1 |
| Issue/PR templates + triage labels | Missing | P0 |
| API reference (Doxygen) | Missing | P1 |
| Client example, HTTP server example | Only `minimal_server` + `zorm_server` | P1 |
| Conformance how-to | **Done (2026-10-09)** | — |

---

## 3. Prioritized plan

### Milestone A — "Adoptable" (target: 1 week)

Everything a consumer or reviewer needs to take the project seriously.

- [x] CMake `install()`/`export` + verified `find_package(mcp CONFIG)`
- [x] Bilingual README + PLAN
- [x] Progress notifications, outputSchema enforcement, structured results
- [x] Client wrappers (`listResources`, `listPrompts`, `complete`)
- [ ] `ROADMAP.md`, `CONTRIBUTING.md` (+ dependency policy), `SECURITY.md`,
      `CODE_OF_CONDUCT.md`, `CHANGELOG.md`
- [ ] `.github/`: issue templates (bug/enhancement/config), PR template,
      triage labels
- [ ] GitHub Actions CI matrix (3 OS × 2-3 toolchains) + conformance job
- [ ] vcpkg port

### Milestone B — "Conformant" (target: 2–3 weeks)

- [x] Run `modelcontextprotocol/conformance` `server` suite → **26/30 (86.7%)**,
      published in `conformance/report.json` with an `--expected-failures` baseline
- [x] Close the failures it surfaced: legacy `ping`/`logging/setLevel`/
      `resources/subscribe`, legacy-era `resultType` omission, and SSE streaming
      of notifications emitted during an HTTP request
- [ ] The remaining 4 are server-initiated-request scenarios (sampling /
      elicitation); 2026-07-28 replaced them with MRTR — no 2026 equivalent to pass
- [ ] Add a client-mode conformance run
- [ ] Cut a stable **1.0.0** release

### Milestone C — "Tier-1 ready" (target: 1–2 months)

- [ ] OAuth 2.1 + Client ID Metadata Documents (remote HTTP)
- [ ] Doxygen API reference; client + HTTP examples
- [ ] Zero-copy parse optimization (re-measure B1)
- [ ] Tasks extension (v2)

---

## 4. What this plan deliberately does *not* do

- **Chase raw benchmark numbers at the cost of correctness.** mcp-cpp's parse is
  slower than serde_json because it validates protocol semantics on the way in;
  that trade-off stays.
- **Add async/concurrency before it is needed.** Single-threaded dispatch is a
  documented, deliberate simplification (PLAN.md Update 1). Multi-worker remains
  a compile-time option for when a benchmark demands it.
- **Implement deprecated features to look bigger.** Roots/Sampling stay out.

---

*Maintainer: zhoutk · This document is updated as milestones land; see
[CHANGELOG.md](../CHANGELOG.md) and [PLAN.md](../PLAN.md) Update 10.*
