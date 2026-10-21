> Language: **English** | [中文](CONFORMANCE.zh-CN.md)
> Date: 2026-10-21

# Conformance

MCP SDKs are scored by the official
[`modelcontextprotocol/conformance`](https://github.com/modelcontextprotocol/conformance)
suite. Tier 1 requires **100%** of applicable required scenarios; Tier 2 requires
**80%**. This document explains how mcp-cpp is wired to that suite.

> **Status (2026-10-21, suite `@modelcontextprotocol/conformance@0.2.0-alpha.12`):**
>
> - **2026-07-28: 193/193 checks — 100%**, run as
>   `server --requirements 2026-07-28` against the fixture in `--strict` mode
>   (a modern-only server: per-request `_meta` required, removed methods answer
>   404). This scores the tasks extension, the whole MRTR family, stateless
>   per-request metadata, header mirroring and custom-header validation.
> - **2025-11-25: 55 passed + 4 baselined** against the dual-era fixture. The
>   four (`tools-call-sampling`, `tools-call-elicitation`,
>   `elicitation-sep1034-defaults`, `elicitation-sep1330-enums`) need the
>   **server to send a request to the client** mid-call; 2026-07-28 replaced
>   that mechanism with MRTR, which this SDK implements. Recorded in
>   [`conformance/expected-failures.yaml`](../conformance/expected-failures.yaml).
> - **Client mode 2026-07-28: 32/32 scenarios, 399/399 checks**, including all
>   25 `auth/*` OAuth 2.1 + CIMD scenarios.
>
> **Why two fixture modes.** A modern-only server must answer 404 for the RPCs
> 2026-07-28 removed, and must reject requests without per-request `_meta`; the
> 2025-era scenarios expect the opposite (the `initialize` handshake, no
> mirrored headers). The fixture therefore has `--strict` and dual-era modes and
> CI runs the suite once per mode. See section 2.

---

## 1. What the suite does

- **Server mode** drives the SDK's server as a *client*: it spawns or connects to
  the server, issues spec scenarios, and checks every exchange — plus a
  wire-schema validation of every message.
- **Client mode** points the SDK's client at a scenario test server.
- **Scoring** (`--requirements <revision>`) counts only what a spec revision
  requires, frozen at release. Reports look like
  `Server 67/67 required scenarios (100%)`, with a "Not scored" section
  (`extension`, `added-after-release`, `pending`) and a "Tier 1 Blockers" list.
  `--output json` returns `passed`/`failed`/`total`/`pass_rate`/`not_scored`.
  Exit code reflects required scenarios only.

## 2. Running it locally

Requires Node (the CLI ships as `@modelcontextprotocol/conformance`). Build the
fixture server first:

```bash
cmake --preset clang-rlz && cmake --build build -j 8
```

### Server mode (Streamable HTTP)

Server mode takes a `--url`; there is no stdio option.
`examples/conformance_server.cpp` is the fixture the suite expects — it exposes
the tool / resource / prompt surface, the tasks extension, the MRTR tools, an
`x-mcp-header` tool, and two era modes.

**2026-07-28 (scored, modern-only fixture):**

```bash
./bin/conformance_server.exe --port 8099 --strict &
npx @modelcontextprotocol/conformance@0.2.0-alpha.12 server \
  --url http://127.0.0.1:8099/mcp --requirements 2026-07-28 -o conformance/out-strict
```

`--requirements <revision>` runs exactly the scenarios that revision requires
(frozen at release) and is the number quoted in the status block above.

**2025-11-25 (dual-era fixture):**

```bash
./bin/conformance_server.exe --port 8099 &
npx @modelcontextprotocol/conformance@0.2.0-alpha.12 server \
  --url http://127.0.0.1:8099/mcp --spec-version 2025-11-25 \
  --expected-failures conformance/expected-failures.yaml -o conformance/out
```

Useful flags: `--scenario <name>` (one scenario), `--suite active|all|pending|draft`,
`--spec-version <v>` (only 2025-xx are valid), `--requirements <revision>`,
`--expected-failures <file>.yaml`, `--verbose`. The per-scenario `checks.json`
files under `-o` are the raw results.

### Client mode

The harness starts its own scenario servers and drives OUR client through
`examples/conformance_client.cpp`, which implements every required client
scenario including the 25 `auth/*` OAuth 2.1 + CIMD ones (via `mcp::oauth`).

```bash
npx @modelcontextprotocol/conformance client \
  --command "bin/conformance_client" \
  --suite all --spec-version 2026-07-28 \
  --expected-failures conformance/expected-failures.yaml \
  -o conformance/out
```

On Windows the harness spawns the command through `cmd.exe`, so pass the
executable with its extension (`--command "bin\conformance_client.exe"`).

**Client scores (2026-10-15 baseline, suite 0.2.0-alpha.12):**

| Revision | Scenarios | Checks |
| --- | --- | --- |
| **2026-07-28** | **32/32 (100%)** — every required scenario, incl. all 25 `auth/*` | **399/399** |
| 2025-11-25 | 17/18 (only `elicitation-sep1034-client-defaults`) | 231/231 scored |
| 2025-03-26 (extra) | 2/2 legacy auth scenarios | 15/15 |

The single baselined failure is the harness mock's, not the SDK's: for
`elicitation-sep1034-client-defaults` the mock never delivers the server→client
elicitation (its GET stream answers keepalives only), so the tool call it drives
never completes. The **official rust-sdk reference client fails it identically**
(0/5, plus a `0xC0000005` crash). Reported upstream as
[modelcontextprotocol/conformance#557][issue-557]; **fixed by [PR #561][pr-561]**
(`server.request()` is now tied to the originating response stream), verified
2026-10-20 against the PR preview build: 5/5 checks, and the full 2025-11-25
suite passes 236/236. The baseline entry stays until CI pins a *release* that
contains #561 (the pinned `0.2.0-alpha.12` still has the defect), then delete
it and re-baseline.

[issue-557]: https://github.com/modelcontextprotocol/conformance/issues/557
[pr-561]: https://github.com/modelcontextprotocol/conformance/pull/561

### Tier self-assessment

```bash
npx @modelcontextprotocol/conformance tier-check \
  --repo zhoutk/mcp-cpp \
  --conformance-server-url http://127.0.0.1:8099/mcp
```

## 3. CI integration

The `.github/workflows/ci.yml` `conformance` job runs the CLI twice, once per
fixture mode, and uploads both reports as an artifact:

1. `conformance_server --strict` + `server --requirements 2026-07-28`
   (fails the job on any unexpected result: this run has no baseline);
2. `conformance_server` (dual-era) + `server --spec-version 2025-11-25
   --expected-failures conformance/expected-failures.yaml`.

A non-zero exit (an unexpected failure) fails the job, so regressions are caught
while the four known legacy scenarios stay baselined.

## 3b. The in-repo 2026-07-28 suite

The official suite now scores the 2026-07-28 surface, but the repository still
carries its own focused suite:
**`tests/test_conformance_2026.cpp`** (`mcp_test_conformance_2026`,
registered with CTest and run by CI). It is written as named scenarios, one per
spec requirement, so a regression points at the clause it breaks instead of at a
suite scenario name.

| Scenario | Spec section |
| --- | --- |
| `DiscoverAdvertisesVersionsCapabilitiesAndCacheHints` | `server/discover`, caching |
| `StatelessMetaIsRequiredOnEveryRequest` | `basic/index.md` statelessness |
| `UnsupportedVersionReportsSupportedList` | `basic/versioning.md` (`-32022`) |
| `CapabilityGatingReturnsMethodNotFound` | `-32601` for unadvertised features |
| `UnknownToolIsInvalidParams` | `-32602` for an unknown tool |
| `ResultTypeIsCompleteOnEveryResult` | `basic/index.md` responses |
| `ProgressOnlyForOptedInRequests` | `basic/patterns/progress.md` |
| `OutputSchemaIsEnforced` | `server/tools.md` output schema |
| `MrtrRejectsUndeclaredClientCapability` | `basic/patterns/mrtr.md` server req. 7 |
| `MrtrNotAllowedOnUnsupportedMethods` | `basic/patterns/mrtr.md` supported requests |
| `MrtrElicitationRoundTrip` | full MRTR flow over a duplex in-memory pipe |
| `MrtrClientEchoesRequestStateExactly` | MRTR client requirement 2 |

```bash
ctest --test-dir build -R mcp_test_conformance_2026 -V
```

## 4. Handling failures

- **Real failure** → fix it in the SDK. These are the scenarios worth the most.
- **Genuinely pending / disputed** → add it to
  `conformance/expected-failures.yaml` with a comment explaining why, and cite
  the spec section. Keep this file small and shrinking; it is reviewed at each
  release.
- **Legacy scenarios** → excluded unless the SDK claims legacy support
  (mcp-cpp's legacy mode is opt-in, so they are not claimed).

## 5. Reporting

- The pass rate and the suite version are published in `README.md` and
  `ROADMAP.md`.
- `conformance/report-2026-07-28.json` holds the 2026-07-28
  `--requirements` run and `conformance/report.json` the 2025-11-25 run; both are
  regenerated for each release, and `CHANGELOG.md` records the score.
- Attach them to the SDK Working Group proposal when one is submitted.

---

*Related: [ROADMAP.md](../ROADMAP.md), [ARCHITECTURE.md](ARCHITECTURE.md).*
