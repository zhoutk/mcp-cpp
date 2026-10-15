# Changelog

All notable changes to this project are documented in this file. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
  (`Client::onServerRequest`), and the transport work below. Passing client
  scenarios: initialize, tools_call, json-schema-2020-12-preservation (9/9),
  json-schema-ref-no-deref, request-metadata (6/6), http-standard-headers (3/3),
  http-custom-headers (18/18), http-invalid-tool-headers (12/12). Baseline
  (with evidence): auth/* (no OAuth yet) and three scenarios the official
  rust-sdk reference client also fails in this environment.
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
- **Line endings.** 16 tracked text files carried CRLF despite `* text=auto`,
  which made `git stash` fail; they are normalized to LF.

## [0.1.0] - 2026-09-21

Initial feature-complete release (PLAN.md M0–M5): JSON-RPC 2.0 strict codec, five
protocol versions, stdio + Streamable HTTP transports, tools/resources/prompts
registries with capability gating and pagination, resource URI templates, MRTR,
cancellation, `completion/complete`, CacheableResult, a JSON Schema 2020-12
validator, and `x-mcp-header` mirroring. Ten CTest suites, three benchmarks, and the
zorm integration example.

[Unreleased]: https://github.com/zhoutk/mcp-cpp/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/zhoutk/mcp-cpp/releases/tag/v0.1.0
