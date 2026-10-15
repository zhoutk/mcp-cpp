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
`examples/conformance_client.cpp`. Run scenarios one at a time (the harness's
parallel suite mode hits a session-routing race in its mock servers that also
affects the official rust-sdk reference client):

```bash
npx @modelcontextprotocol/conformance client \
  --command "bin/conformance_client" \
  --scenario tools_call --spec-version 2026-07-28 \
  --expected-failures conformance/expected-failures.yaml \
  -o conformance/out
```

On Windows the harness spawns the command through `cmd.exe`, so pass the
executable with its extension (`--command "bin\conformance_client.exe"`).

**Client scores (2026-10-15 re-verification, suite 0.2.0-alpha.12, sequential
runs):**

| Scenario | 2025-11-25 | 2026-07-28 |
| --- | --- | --- |
| `initialize` | 1/1 | n/a (legacy-only) |
| `tools_call` | 2/2 | 2/2 |
| `json-schema-2020-12-preservation` | 9/9 | 9/9 |
| `json-schema-ref-no-deref` | n/a | 1/1 |
| `request-metadata` | n/a | 6/6 |
| `sep-2322-client-request-state` | n/a | intermittent* |
| `http-standard-headers` | n/a | 3/3 |
| `http-custom-headers` | n/a | 18/18 |
| `http-invalid-tool-headers` | n/a | 12/12 |
| `sse-retry` | 2/3 (intermittent*) | n/a |
| `elicitation-sep1034-client-defaults` | 0/6 (0/5)* | n/a |
| `auth/*` (≈33 scenarios) | baselined — no OAuth yet | baselined |

\* These three are baselined. The **official rust-sdk reference client fails them
consistently in the same environment** (elicitation 0/5 ×3, sse-retry 0/1 ×2,
sep-2322 0/5 — verified 2026-10-14), which points at the harness mock's session
routing rather than the SDK: the mock receives the client's GET stream but never
sends the elicitation, only emitting keepalives until the scenario times out.
`sse-retry` passes intermittently. Re-verified 2026-10-15 with a pre-change A/B
run reproducing the same result. They are baselined in
[`conformance/expected-failures.yaml`](../conformance/expected-failures.yaml)
with this evidence; CI runs the scenarios sequentially.

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
