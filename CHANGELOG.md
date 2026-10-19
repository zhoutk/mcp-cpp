# Changelog

All notable changes to this project are documented in this file. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **HTTP concurrency (P0).** `Server::processLine()` is now serialized by
  `dispatchMutex_`. The HTTP transport dispatches from httplib's 64-thread worker
  pool, and unsynchronized `cancellations_`/`logLevel_` were a data race on every
  concurrent deployment. Handlers still run one at a time; re-entrant
  `processLine()` is not supported.
- **Cancellation now reaches a running request.** `notifications/cancelled` is
  handled before the dispatch lock and sets the existing per-request flag instead
  of replacing the map slot, which previously left an in-flight handler looking at
  a stale `false` flag. `Client::cancel()` now includes `params._meta`, so the
  HTTP transport mirrors `MCP-Protocol-Version` and a strict 2026 server accepts
  the notification (it used to answer 400).
- **Idle subscriptions no longer die (P0).** The subscription read timeout was 1 s
  while the server sent an SSE keep-alive only every 15 s, so a silent
  subscription was always torn down. The timeout is now 30 s, the keep-alive
  cadence is 5 s (which also bounds how long a client takes to notice a
  closure), and a dropped stream is re-subscribed (honouring `retry:` clamped to
  100 ms–30 s, suppressing duplicate acknowledgments).
- **Peer-controlled SSE `retry:` values are clamped** to 30 s, so a hostile value
  can no longer overflow `sleep_for` or stall shutdown for weeks.
- **Installed package links (P0).** `mcp::mcp` now propagates `ws2_32` (Windows)
  and `Threads::Threads` (POSIX) as INTERFACE dependencies, and
  `mcpConfig.cmake` resolves `Threads`; a `find_package(mcp)` consumer using the
  HTTP transport links again.
- **Package version (P0).** `CMakeLists.txt` reads `version.txt` for
  `project(VERSION)` instead of the stale `0.1.0` literal, so
  `find_package(mcp 0.2)` succeeds. `mcp_test_version` asserts `kSdkVersion`
  against the CMake value.

### Added

- Concurrency coverage: `ServerConcurrency.*` (16 threads through `processLine`
  plus cancel visibility) and `HttpE2E.ConcurrentPostsAreSerializedSafely`
  (64 concurrent HTTP clients mixing `tools/call` and cancellation), run under
  TSan in CI.
- `HttpE2E.SubscriptionSurvivesIdlePeriod` (5 s of silence, then a delivered
  notification).
- `tests/consumer/` + CI jobs `install-consumer` and `tsan`.

## [0.2.0] - 2026-10-15

### Added

- **Progress notifications.** `Ctx::progress(progress, total, message)` emits
  `notifications/progress` for requests that opt in with `params._meta.progressToken`;
  `Ctx::notify(method, params)` emits arbitrary notifications. The client can pass a
  `progressToken` to `callTool()`/`request()` and receives progress through
  `onNotification()`.
- **`outputSchema` enforcement.** A tool result's `structuredContent` is validated
  against the tool's declared `outputSchema`; non-conforming results fail with
  `InternalError`.
- **Result builders.** `mcp::server::textResult()` and
  `mcp::server::structuredResult()` — the latter also emits the spec-recommended
  serialized-JSON text block for backward compatibility.
- **Client wrappers.** `Client::listResources()`, `Client::listPrompts()`,
  `Client::complete()`.
- **MRTR completed** (basic/patterns/mrtr.md): `Ctx::inputRequired()` builds an
  `InputRequiredResult` and enforces the server requirements (at least one of
  `inputRequests`/`requestState`; only valid MRTR methods; the client must have
  declared the matching capability), `Ctx::elicit()` is the single-elicitation
  convenience, an `input_required` result is rejected on methods that do not
  support it, and `Client::retry()`/`retryToolCall()` echo `requestState`
  verbatim on a fresh request id.
- **Signed `requestState` codec.** `mcp::RequestStateCodec` seals and opens an
  opaque, tamper-evident `requestState` (HMAC-SHA256 over a domain label, the request
  context, and the body, with an expiry timestamp). The key must be at least 32 bytes.
  Verification failures return `InvalidParams` without echoing the reason to the peer.
  SHA-256 and HMAC are implemented in-tree with no new dependency.
- **Server `requestState` binding.** `Builder::requestStateCodec()` enables
  `Ctx::sealRequestState()` / `Ctx::openRequestState()` for `tools/call`,
  `prompts/get` and `resources/read`. A state is bound to the method and target name,
  so it cannot be replayed against another tool.
- **Client MRTR auto-fulfil.** `Client::callToolWithInput(name, args, handler, maxRounds = 10)`
  answers each `input_required` round through a handler and retries with the echoed
  `requestState` and the `inputResponses`. It stops with `InternalError` after
  `maxRounds`, and a handler error ends the loop without a retry.
- **Client conformance fixture.** `examples/conformance_client.cpp` + CI client
  jobs (both spec versions): `Client::initialize()` (legacy handshake that adopts
  the negotiated version), answering server-initiated requests
  (`Client::onServerRequest`), and the transport work below.
  **2026-07-28: 32/32 scenarios (100%), including all 25 `auth/*`; 2025-11-25:
  17/18** (only `elicitation-sep1034-client-defaults`, which the official
  rust-sdk reference fixture also fails here — verified by building it and
  running the same scenario). Two 2025-03-26 legacy auth scenarios pass too.
- **`mcp::oauth` — the OAuth 2.1 client** (`oauth.hpp` / `src/oauth.cpp`).
  Protected-resource metadata discovery (RFC 9728) from the `WWW-Authenticate`
  `resource_metadata` pointer or the well-known candidates; authorization-server
  metadata discovery (RFC 8414 + OIDC) with the issuer validated against the
  identifier the URL was built from; client identity via pre-registration,
  Dynamic Client Registration (RFC 7591) or a Client ID Metadata Document
  (SEP-991); authorization-code + PKCE (S256); the RFC 8707 `resource`
  parameter; RFC 9207 `iss` validation; scope selection from the challenge →
  PRM → AS metadata; scope step-up with a retry cap; SEP-2352
  authorization-server migration; and the 2025-03-26 fallbacks for servers with
  no metadata discovery.
- **`HttpClientTransport::setAuthChallengeHandler`.** A 401/403 carrying
  `WWW-Authenticate` is reported to the caller, which is how the OAuth flow is
  triggered; the transport itself never authenticates.
- **HTTP client transport hardening** (found by the client suite): GET listening
  stream with `Mcp-Session-Id` echo and `Last-Event-ID` reconnect honoring the
  SSE `retry:` field; request POSTs stream responses on background threads so a
  client can answer mid-call elicitations; full SSE field parsing (`event:`,
  `id:`, `retry:`, CRLF terminators); error responses may carry `id: null`
  (JSON-RPC 2.0 allows it; 2025-era peers send it); mirrored `Mcp-Param-*`
  headers with empty values are sent; SEP-2243 client-side annotation
  validation excludes malformed tools.
- **Legacy client API.** `Client::initialize(protocolVersion, clientInfo)` runs
  the pre-2026 `initialize` handshake (params-root metadata, adopting the
  negotiated version) and `Client::onServerRequest(handler)` answers
  server-initiated requests (sampling/elicitation/roots) — the mechanism
  2026-07-28 replaced with MRTR.
- **`ITransport::removeHeader(name)`** and
  `HttpClientTransport::setProtocolVersion(v)`. Removal is now a distinct
  operation: an empty header value is a real value the mirrored-header contract
  requires sending, so `setHeader(name, "")` stores rather than deletes.
- **`mcp_test_oauth`** (CTest suite): 31 cases over a real loopback
  authorization server covering discovery, registration, PKCE, `iss`, scope
  selection, token-endpoint client authentication and SEP-2352 migration.

- **2026-07-28 conformance suite.** `mcp_test_conformance_2026` — 12 named
  scenarios covering discover, stateless `_meta`, version negotiation, capability
  gating, resultType, progress, outputSchema, and a full MRTR round trip over a
  duplex in-memory pipe.
- **CMake package.** `install()` rules, an exported `mcp::mcp` target, and a
  `find_package(mcp CONFIG)` package config (verified with an out-of-tree consumer).
  httplib is now a build-interface-only dependency.
- **Legacy-client compatibility.** In legacy mode the SDK again serves `ping`,
  `logging/setLevel` and `resources/subscribe`/`unsubscribe` (removed in
  2026-07-28), omits `resultType` from pre-2026 results (their schemas reject it),
  and the HTTP transport tolerates a missing `MCP-Protocol-Version` header as
  `2025-03-26` — the backward-compatibility the spec explicitly permits.
- **HTTP notifications during a request.** Notifications a handler emits before
  its result (progress, logging) are streamed back as SSE instead of being
  dropped.
- **Conformance.** `examples/conformance_server.cpp` (the fixture the official
  suite expects) plus `conformance/report.json` and
  `conformance/expected-failures.yaml`. Against suite v0.1.16: **26/30 scored
  server scenarios pass (86.7%)**.
- **Bilingual documentation.** README, PLAN, ROADMAP, CONTRIBUTING, OPTIMIZATION-PLAN,
  ADOPTION-PLAN and CONFORMANCE are available in English (default) and Chinese.
- **Project governance files.** `ROADMAP.md`, `CONTRIBUTING.md` (with dependency
  policy), `SECURITY.md`, `CODE_OF_CONDUCT.md`, `MAINTAINERS.md`, issue/PR templates,
  and CI.

### Changed

- **The legacy `initialize` response is held until its session id is captured.**
  A stateful pre-2026 server issues `Mcp-Session-Id` on the `initialize`
  response and rejects the next request without it. The response message could
  otherwise reach the caller before the worker stored the header, so
  `notifications/initialized` raced ahead and got a 400.
- **`JsonRpcResult::id` is now `std::optional<RequestId>`.** It is absent when a
  legacy peer omits it on a result; the encoder still requires an id, so the
  server's strict wire contract is unchanged.
- **`Client::initialize()` declares the client's configured capabilities.** A
  pre-2026 server gates server-initiated requests on the `capabilities` it
  receives at the params root, so the handshake used to suppress them.
- **Notifications POST synchronously again.** A request response streams on a
  worker thread (it may be a long-lived SSE stream), but a notification has no
  request id to correlate an asynchronous error with, so its POST blocks so the
  failure can be returned.

### Fixed

- **Pure-stdio configure regression.** `-DMCP_DISABLE_HTTP=ON` failed at
  configure time: the runtime-output list named the HTTP-only
  `conformance_client` target unconditionally. The target is now appended only
  when it exists.
- **HTTP client transport data race.** `Mcp-Session-Id` and the echoed protocol
  version were read by the GET-listen thread while the caller wrote them under a
  *different* mutex. They now share one mutex, held without nesting, so no
  lock-order cycle can form with the SSE drain (`reqMutex` -> `listenMutex`).
- **Spurious transport errors.** A request whose response stream stayed open
  after the reply (a session-lifetime SSE stream) hit the read timeout and
  injected a synthetic error for an already-answered request id. Errors are now
  synthesized only when the stream delivered nothing, and the request stream
  uses the same 30 s read timeout as the shared client (it had inherited
  httplib's 5 s default).
- **Over-eager GET-stream reconnect.** Every successful POST forced the
  listening stream to reconnect, churning the GET stream and risking in-flight
  event loss. It is now limited to the case it exists for: a request whose POST
  stream closed without delivering a response (SEP-1699).
- **`id: null` error responses were dropped.** `parseMessage` was relaxed to
  accept them, but the client still ignored them and stalled to its read
  timeout. A sequential client now treats an id-less error as the answer to its
  one outstanding request, mirroring the id-less result path.
- **Notification POST failures were swallowed.** Once request POSTs moved to
  background threads, a failed `notifications/*` POST still returned `Ok()`, so
  `Client::cancel()` could report success against an unreachable peer.
- **SSE `retry:` advice was not honoured by the first reconnect.** A POST stream
  that closed without answering sets the listening stream's forced-reconnect
  flag, but when no GET was open yet the flag was only consumed *after* the next
  connection, so the client reconnected too early (SEP-1699 timing). The flag is
  now honoured before opening the connection too.
- **`sep-2322-client-request-state` was misdiagnosed as a harness flake.** The
  fixture never ran the MRTR flow, so the server-side checks saw "tool was not
  called". The fixture now drives `callToolWithInput` for every listed tool;
  the scenario passes 5/5.
- **Line endings.** 16 tracked text files carried CRLF despite `* text=auto`,
  which made `git stash` fail; they are normalized to LF.

## [0.1.0] - 2026-09-21

Initial feature-complete release (PLAN.md M0–M5): JSON-RPC 2.0 strict codec, five
protocol versions, stdio + Streamable HTTP transports, tools/resources/prompts
registries with capability gating and pagination, resource URI templates, MRTR,
cancellation, `completion/complete`, CacheableResult, a JSON Schema 2020-12
validator, and `x-mcp-header` mirroring. Ten CTest suites, three benchmarks, and the
zorm integration example.

[Unreleased]: https://github.com/zhoutk/mcp-cpp/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/zhoutk/mcp-cpp/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/zhoutk/mcp-cpp/releases/tag/v0.1.0
