# AGENTS.md

This file provides guidance to AI coding agents (Claude Code, Codex, and others) working with code in
this repository. It is the canonical copy: `CLAUDE.md` is a one-line import of this file, so edit
guidance here and both tools will pick it up.

## What this is

`mcp-cpp` is an **independent, standalone MCP (Model Context Protocol) C++17 SDK** — a header-first
library (`include/mcp/` + `src/`), not an application. It targets the **2026-07-28** specification with
Tier-1 SDK feature parity (TS/Python/C#/Go/Rust) and is value-semantic / exception-free at the protocol
layer (no async runtime required). It is one of the two submodules `zorm-mcp` links; zorm consumes it via
`examples/zorm_server.cpp` (built only inside the zorm repo), but **the library itself has zero zorm/db
symbols and must stay that way** — do not add application-specific code here.

This is a real git repo with its own remote (`origin` → internal GitLab). Commit and push changes *here*;
the parent `zorm-mcp` repo records the new revision via its `sync-thirds.sh`.

`README.md` holds the feature-parity matrix, benchmark numbers, and usage snippets. **`PLAN.md` is the
historical design/decision log** — every milestone deviation is recorded there (Updates 1–10); read the
relevant Update before changing protocol behaviour. Its `docs/mcp/...` paths refer to the original zorm
workspace layout and no longer resolve here. Docs are **bilingual, English default**: each has a
`.zh-CN.md` sibling (README, PLAN, ROADMAP, CONTRIBUTING, ARCHITECTURE, CONFORMANCE). `docs/` holds
`ARCHITECTURE.md` (layering/threading/invariants — read before changing protocol behaviour) and
`CONFORMANCE.md` (running the official conformance suite, both spec versions). Project governance files
live at the repo root (`ROADMAP.md`, `VERSIONING.md`, `DEPENDENCY_POLICY.md`, `CONTRIBUTING.md`,
`SECURITY.md`, `CODE_OF_CONDUCT.md`, `MAINTAINERS.md`, `CHANGELOG.md`) plus `.github/` templates and
CI. Strategy/process documents (adoption, promotion, session reviews) live in `docs/internal-docs/`
(gitignored — never commit them or link to them from tracked files).

**Packaging:** `CMakeLists.txt` has `install()`/export rules behind `MCP_INSTALL` (ON); `cmake/mcpConfig.cmake.in`
produces `find_package(mcp CONFIG)` with the `mcp::mcp` target. httplib is linked via
`$<BUILD_INTERFACE:httplib>` so it stays out of the installed export — do not "simplify" it back to a
plain PUBLIC link or the installed target will reference a target consumers don't have. Because
`BUILD_INTERFACE` also swallows httplib's own interface dependencies, the platform libraries the compiled
HTTP code needs are propagated explicitly as `INTERFACE` (`ws2_32` on Windows, `Threads::Threads` on
POSIX); `cmake/mcpConfig.cmake.in` resolves `Threads` for consumers. `tests/consumer/` is the CI smoke
test for exactly that.

## Build & test

Standalone (the primary mode; `thirds/` already bundles zjson, httplib, googletest):

```bash
cmake --preset clang-rlz          # MSYS2 clang64 + Ninja (clang-dbg for Debug)
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

The presets hardcode `D:/msys64/clang64/bin/{clang++,ninja}` — on another machine configure manually
with `-G Ninja` and the clang64 toolchain. Plain `cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo`
also works. Executables land in `bin/` (`RUNTIME_OUTPUT_DIRECTORY`), intermediates in `build/`.

CMake options: `MCP_BUILD_TESTS` (ON), `MCP_BUILD_EXAMPLES` (ON), `MCP_DISABLE_HTTP` (OFF — pure-stdio
build), `MCP_ZJSON_DIR` (override zjson location; defaults to `thirds/zjson`, then the zorm sibling dir).

Single suite / single gtest case:

```bash
ctest --test-dir build -R mcp_test_server -V
bin/mcp_test_server.exe --gtest_filter=ServerTest.DiscoverAdvertisesCapabilities
```

The `mcp_test_*` names are exactly the CTest registration names (`mcp_test_version`, `_error`, `_jsonrpc`,
`_types`, `_transport`, `_server`, `_client`, `_schema`, `_request_state`, `_stdio_e2e`, `_http_e2e`,
`_oauth`, `_conformance_2026`).
Benchmarks
(`mcp_bench_parse|dispatch|e2e`) are **not** registered with CTest — run them manually from `bin/`.

When embedded in the zorm root project, mcp-cpp's tests are picked up by the root build; run them with
`ctest -R mcp_test` from the repo root.

`version.txt` is the only place the release version is written: `CMakeLists.txt` reads it for
`project(... VERSION ...)`, `mcp_test_version` asserts `kSdkVersion` against that value, and CI builds
`tests/consumer` against an installed package to prove `find_package(mcp 0.2 CONFIG)` + HTTP linking work
(`ws2_32` / `Threads::Threads` are propagated as INTERFACE).

## Architecture

Layering (each layer depends only on those before it): `error` → `version` → `json` → `jsonrpc` → `types`
→ `transport` → `server` / `client` → `schema` / `transport_http`. `include/mcp/mcp.hpp` is the single
aggregate include; use it in examples/tests.

| File | Role |
| --- | --- |
| `include/mcp/error.hpp` | `McpErrorCode` (+ MCP-reserved `-32020…-32099`), `McpResult<T>`, `Ok()`/`Err()` |
| `include/mcp/version.hpp` / `src/version.cpp` | `ProtocolVersion` enum ↔ wire date strings (single source of truth) |
| `include/mcp/json.hpp` | `mcp::Json` = alias of the bundled zjson `Json` |
| `include/mcp/jsonrpc.hpp` / `src/jsonrpc.cpp` | strict JSON-RPC 2.0 codec (MCP deltas) |
| `include/mcp/types.hpp` / `src/types.cpp` | `RequestId`, `RequestMeta`, capability probes, `member()` |
| `include/mcp/transport.hpp` / `src/transport.cpp` | `ITransport`, `StreamTransport`, `StdioTransport` |
| `include/mcp/server.hpp` / `src/server.cpp` | `Server` dispatch loop + `Builder` registration API |
| `include/mcp/request_state.hpp` / `src/request_state.cpp` | `RequestStateCodec` — HMAC-sealed MRTR `requestState` (`src/sha256.*` is the in-tree primitive); enabled per-server via `Builder::requestStateCodec()`, surfaced to handlers as `Ctx::sealRequestState`/`Ctx::openRequestState` |
| `include/mcp/client.hpp` / `src/client.cpp` | blocking `Client` over any `ITransport` |
| `include/mcp/schema.hpp` / `src/schema.cpp` | JSON Schema 2020-12 validator (local `$ref` only) |
| `include/mcp/transport_http.hpp` / `src/transport_http.cpp` | Streamable HTTP + SSE (httplib), both sides |
| `include/mcp/oauth.hpp` / `src/oauth.cpp` | `mcp::oauth::OAuthClient` — OAuth 2.1 client for remote HTTP servers: PRM/AS metadata discovery, DCR/CIMD/pre-registration, PKCE (S256), RFC 8707 `resource`, RFC 9207 `iss`, scope step-up, SEP-2352 migration. HTTP-only, like `transport_http` |

Key runtime facts:

- **Serialized dispatch.** `Server::run()` reads one line, calls `processLine()`, writes the reply.
  `processLine()` holds `dispatchMutex_` for the whole dispatch, so the HTTP transport's httplib
  worker pool (64 threads) is safe and handlers still run one at a time. Handlers must not re-enter
  `processLine()` (the mutex is not recursive). Other members are plain except `subscriptions_`
  (`subscriptionsMutex_`, notification fan-out from any thread) and `cancellations_`
  (`cancellationsMutex_`; keys are type-tagged so the number `1` and the string `"1"` differ, and
  pre-registered "parked" flags are capped at 1024 with FIFO eviction).
- **Stateless protocol.** Every request carries `params._meta` with `protocolVersion` +
  `clientCapabilities` (required). `decodeRequestMeta` (strict) rejects missing/ill-typed `_meta` with
  `-32602`; `decodeRequestMetaRelaxed` is the legacy path. Unknown/unsupported version → `-32022` with
  the supported list in `error.data`.
- **Capability gating via a route table** (`kRoutes` in `src/server.cpp`): calling a method whose
  capability the server never advertised (no tool/resource/prompt registered) is `MethodNotFound`
  (`-32601`); an unknown *tool name* is `InvalidParams` (`-32602`). Keep new methods in `kRoutes`.
- **The server never initiates requests.** Server-to-client interaction travels only inside an
  `input_required` result (MRTR). A response arriving at the server is ignored. `notifications/cancelled`
  sets a queryable per-id flag (exposed to handlers as `Ctx::isCancelled`). Cancellation is handled
  **before** the dispatch lock and flips an existing flag in place, so a cancel reaches a running
  handler over HTTP; a cancel that arrives first parks a set flag for the request to adopt.
- **`resultType` is injected.** `processLine` adds `"complete"` to any object result that omits it; a
  handler returning `input_required` (MRTR) must set it itself.
- **CacheableResult is on by default.** `*/list`, `resources/read`, and `server/discover` emit `ttlMs` +
  `cacheScope`; `listTtlMs_ = 0` (immediately stale) is the conformant default — strict modern clients
  reject results *without* these fields. `Builder::listCache(-1)` opts out entirely.
- **Legacy dual-era mode.** `Builder::acceptLegacyClients()` serves the old `initialize` handshake and
  requests lacking `_meta`. Off by default; only enable to support pre-2026 clients.
- **URI templates** (RFC 6570 subset: literals, `{var}`, `{+var}`) are matched by `matchUriTemplate`;
  `resources/read` prefers an exact `resource()` match, else the most specific template (fewest vars,
  then longest literal prefix). Registered via `Builder::resourceTemplate()`.
- **HTTP transport** (`transport_http.cpp`) keeps httplib out of the public header (pimpl). Single POST
  endpoint; GET/DELETE → 405, unknown method → 404, notifications → 202; validates mirrored headers
  (`MCP-Protocol-Version`, `Mcp-Method`, `Mcp-Name`, `Mcp-Param-*`) → `-32020` on mismatch. A present
  argument with an empty string value still mirrors a header (`setHeader` stores; `removeHeader` deletes —
  they are distinct). Subscriptions upgrade the response to an SSE stream pumped on its own thread;
  `closeSubscriptions()` must be called (or the destructor) to terminate those threads — see the P0 fix in
  `docs/CODE-REVIEW-2026-10.md`.
- **A client subscription is a durable stream.** The server sends an SSE keep-alive comment every 5 s, so
  the subscription stream read timeout is 30 s (a shorter one kills an idle subscription between keep-alives).
  After a drop the client re-subscribes, applies the peer's `retry:` advice clamped to 100 ms–30 s, and
  suppresses duplicate acknowledgments. Peer-controlled `retry:` values are always clamped. A **successful
  non-SSE answer** (a `200` body that is not an event stream, typically a JSON-RPC error) is queued as one
  message and ends the thread — do not re-subscribe after it, or a refusing peer produces a reconnect storm.
- **`HttpClientTransport::read()` has no idle timeout.** `nullopt` means orderly transport teardown and an
  error means a lost connection (the GET listen loop could not reconnect). A quiet connection is alive, so a
  request that legitimately takes minutes must not look like EOF. There is no per-request timeout API yet;
  wrap `Client` calls externally if a bound is required, and note that `closeSubscriptions()` does not wake a
  blocked `read()`.
- **`HttpClientTransport` runs background threads.** Each request POST streams its response on a worker
  (a response may be a long-lived SSE stream, and the client must stay free to answer a mid-call
  elicitation), and the GET listening stream owns a reconnect loop; `read()` gets messages only through
  the queue. The destructor joins all three thread groups (`closeSubscriptions`, `closeRequestStreams`,
  `closeListenStreams`). `sessionId`/`protocolVersion` live under `sharedMutex` — held alone, never nested
  with `reqMutex`/`listenMutex`, so no lock-order cycle forms with the SSE drain (`reqMutex` → `listenMutex`).
- **Legacy client surface.** `Client::initialize()` sends the pre-2026 handshake (metadata at the params
  root, `capabilities` from the client's own config, adopting the negotiated version) and
  `Client::onServerRequest()` answers server-initiated requests. `JsonRpcResult::id` is optional (absent on
  legacy id-less results); the server never consumes results, so the encoder keeps the strict contract.
- **The legacy `initialize` response is deferred** until its `Mcp-Session-Id` has been captured: a
  stateful server rejects the next request without it, and queueing the message earlier let
  `notifications/initialized` race the header. Relatedly, the SSE `retry:` advice is honoured *before*
  the listening stream's first reconnect, not only after a connection.
- **`HttpClientTransport::setAuthChallengeHandler`** forwards 401/403 `WWW-Authenticate` challenges to the
  caller; the transport itself never authenticates. `mcp::oauth` (used by the conformance fixture) is what
  drives the flow on top of it.

## zjson gotchas (this library's JSON layer)

`mcp::Json` is the bundled zjson `Json`; its quirks are load-bearing here (see `json.hpp` and the
`member()` note in `types.hpp`):

- **Use `mcp::member(obj, key)` for every protocol read, never `obj[key]`.** zjson's `operator[]`
  *deep-searches* nested layers when the key is absent at the top level — that misreads nested protocol
  data (e.g. `params.id` vs. a request id). `member()` is a direct-member lookup returning Null.
- **Text values must be wrapped in `Json::str(...)`.** `Json("...")` / `add(key, "...")` sniff the leading
  char: a string starting with `{` or `[` is parsed as JSON, not stored as a string. This is a recurring
  bug class.
- Build arrays/objects from `Json("[]")` / `Json(R"({})")`, then `push_back` / `add`. Null renders as the
  literal `"null"`; probe with `isNull()` / `isError()` / `isObject()`, never `toString().empty()`.

## Testing conventions

- Unit tests use googletest and an **in-memory `ITransport`** (`tests/MemTransport.h`) to drive
  `Server::processLine` without real IO. Add new server behaviour tests in `tests/test_server.cpp`.
- Concurrency coverage lives in `tests/test_server.cpp` (`ServerConcurrency.*`: 16 threads through
  `processLine`, plus cancel-visibility) and `tests/test_http_e2e.cpp` (64 concurrent HTTP clients mixing
  `tools/call` and `notifications/cancelled`). CI runs both under TSan; keep them race-free.
- `tests/test_stdio_e2e.cpp` owns its `main()` and doubles as the child server: it re-execs itself with
  `--mcp-serve-child` and talks newline-framed JSON-RPC over OS pipes (Windows + POSIX branches).
- `tests/test_http_e2e.cpp` runs a real httplib server + client on a loopback ephemeral port (only built
  when `MCP_HAS_HTTP`).
- `examples/conformance_client.cpp` is the official suite's client fixture (`docs/CONFORMANCE.md`); CI runs
  it with `conformance-client` (`--suite all`) for both spec revisions. It links only when the HTTP
  transport is on, so a pure-stdio configure must not name it unconditionally.
- `tests/test_oauth.cpp` (`mcp_test_oauth`, HTTP-only) runs `mcp::oauth` against a real loopback
  authorization server: discovery, registration, PKCE, `iss`, scope selection, token auth and migration.
- `bench/bench_e2e.cpp` uses the same self-spawn trick with `--bench-child`.
- `include/mcp/version.hpp`'s `kSdkVersion` and the repo-root `version.txt` must stay in sync.
