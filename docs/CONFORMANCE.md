> Language: **English** | [中文](CONFORMANCE.zh-CN.md)
> Date: 2026-10-09

# Conformance

MCP SDKs are scored by the official
[`modelcontextprotocol/conformance`](https://github.com/modelcontextprotocol/conformance)
suite. Tier 1 requires **100%** of applicable required scenarios; Tier 2 requires
**80%**. This document explains how mcp-cpp is wired to that suite.

> **Status (2026-10-13), both spec versions:**
>
> - **2026-07-28: 42/42 checks, 20/20 scenarios — 100%** of the scored (active)
>   suite, with `@modelcontextprotocol/conformance@0.2.0-alpha.12` — see
>   [`conformance/report-2026-07-28.json`](../conformance/report-2026-07-28.json).
> - **2025-11-25: 26/30** with v0.1.16 — see
>   [`conformance/report.json`](../conformance/report.json). The four failures
>   all require the **server to send a request to the client** mid-call
>   (`sampling/createMessage` / `elicitation/create`); 2026-07-28 replaced that
>   mechanism with MRTR, which this SDK implements. Recorded in
>   [`conformance/expected-failures.yaml`](../conformance/expected-failures.yaml).
>
> The unscored *pending* 2026 scenarios additionally run with `--suite all`:
> `sep-2164-resource-not-found` passes; `server-stateless` needs the fixture's
> `--strict` flag (15/28); the `input-required-*` scenarios need the fixture to
> expose MRTR tools — tracked as follow-up work in the 2026 report.

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

Server mode takes a `--url`; there is no stdio option. `examples/conformance_server.cpp`
is the fixture the suite expects — it exposes the required tool/resource/prompt
surface and runs in legacy-client mode so the 2025-era handshake and missing
mirrored headers are accepted:

```bash
./bin/conformance_server.exe --port 8099 &
npx @modelcontextprotocol/conformance server --url http://127.0.0.1:8099/mcp \
  -o conformance/out
```

Useful flags: `--scenario <name>` (one scenario), `--suite active|all|pending`,
`--spec-version <v>` (only 2025-xx are valid), `--expected-failures <file>.yaml`,
`--verbose`. There is no `--output json` on `server` in v0.1.16; the per-scenario
`checks.json` files under `-o` are the raw results.

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

**Client scores (2026-10-15, suite 0.2.0-alpha.12):**

| Revision | Scenarios | Checks |
| --- | --- | --- |
| **2026-07-28** | **32/32 (100%)** — every required scenario, incl. all 25 `auth/*` | **399/399** |
| 2025-11-25 | 17/18 (only `elicitation-sep1034-client-defaults`) | 231/231 scored |
| 2025-03-26 (extra) | 2/2 legacy auth scenarios | 15/15 |

The single baselined failure is the harness mock's, not the SDK's: for
`elicitation-sep1034-client-defaults` the mock never delivers the server→client
elicitation (its GET stream answers keepalives only), so the tool call it is
meant to drive never completes. The **official rust-sdk reference client fails it
identically here** — verified 2026-10-15 by building `refer/rust-sdk` and running
the same scenario (0/5; it also crashes with `0xC0000005`). mcp-cpp's client
answers server-initiated elicitation correctly (unit-tested, and green on the
auth scenarios that exercise the same mechanism); it simply never receives the
request. It is baselined in
[`conformance/expected-failures.yaml`](../conformance/expected-failures.yaml)
with this evidence.

### Tier self-assessment

```bash
npx @modelcontextprotocol/conformance tier-check \
  --repo zhoutk/mcp-cpp \
  --conformance-server-url http://127.0.0.1:8099/mcp
```

## 3. CI integration

The suite ships a GitHub Action (v0.1.11+). It runs in server mode against a
locally built fixture server, and in client mode against the scenario server:

```yaml
- name: Conformance (server)
  uses: modelcontextprotocol/conformance@v0.1.16
  with:
    mode: server
    url: http://127.0.0.1:8099/mcp
```

The `.github/workflows/ci.yml` conformance job starts `conformance_server`,
invokes the action with `--expected-failures conformance/expected-failures.yaml`,
and uploads the report as an artifact. A non-zero exit (an unexpected failure)
fails the job, so regressions are caught while the known four stay baselined.

## 3b. The in-repo 2026-07-28 suite

Because the official suite has no 2026-07-28 scenarios, the repository carries
its own: **`tests/test_conformance_2026.cpp`** (`mcp_test_conformance_2026`,
registered with CTest and run by CI). It is written as named scenarios, one per
spec requirement, so the 2026 surface is covered the way the official suite will
cover it once it catches up.

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
- `conformance/report.json` is the machine-readable summary committed with each
  release; `CHANGELOG.md` records the score.
- Attach it to the SDK Working Group proposal when one is submitted.

---

*Related: [ROADMAP.md](../ROADMAP.md), [ARCHITECTURE.md](ARCHITECTURE.md).*
