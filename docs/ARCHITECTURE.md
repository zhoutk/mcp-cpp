> Language: **English** | [中文](ARCHITECTURE.zh-CN.md)

# Architecture

How mcp-cpp is layered, what runs on which thread, and the invariants a
contributor must not break. For the history of *why*, see [PLAN.md](../PLAN.md)
(the decision log).

## Layering

Each layer depends only on the ones before it:

```
error → version → json → jsonrpc → types → transport → server / client → schema / request_state / transport_http / oauth
```

`include/mcp/mcp.hpp` is the single aggregate include; applications include only
that one.

| File | Role |
| --- | --- |
| `include/mcp/error.hpp` | `McpErrorCode` (JSON-RPC codes + the MCP-reserved `-32020…-32099` band), `McpResult<T>` (aliases `std::expected` when available, else a built-in `Result`), `Ok()`/`Err()` factories |
| `include/mcp/version.hpp` | `ProtocolVersion` enum ↔ wire date strings — the single source of truth for protocol revisions |
| `include/mcp/json.hpp` | `mcp::Json` = alias of the bundled zjson `Json` |
| `include/mcp/jsonrpc.hpp` | strict JSON-RPC 2.0 codec with the MCP deltas (null-id ban, no batching, object params) |
| `include/mcp/types.hpp` | `RequestId`, `RequestMeta`, capability probes, `member()`, MRTR helpers |
| `include/mcp/transport.hpp` | `ITransport`, `StreamTransport`, `StdioTransport` |
| `include/mcp/server.hpp` | `Server` dispatch loop + `Builder` registration API |
| `include/mcp/request_state.hpp` | `RequestStateCodec` — HMAC-sealed `requestState` (context-bound, TTL) for MRTR; `src/sha256.*` is the dependency-free SHA-256/HMAC primitive |
| `include/mcp/client.hpp` | blocking `Client` over any `ITransport`; modern `request()` (per-request `_meta`) plus the legacy `initialize()` handshake and `onServerRequest()` for pre-2026 server-initiated requests |
| `include/mcp/schema.hpp` | JSON Schema 2020-12 validator (local `$ref` only, resource bounds) |
| `include/mcp/transport_http.hpp` | Streamable HTTP + SSE, both sides (pimpl keeps httplib out of the header) |
| `include/mcp/oauth.hpp` | `mcp::oauth::OAuthClient` — the OAuth 2.1 client for remote HTTP servers: PRM + AS metadata discovery, DCR/CIMD/pre-registration, PKCE, RFC 8707 `resource`, RFC 9207 `iss`, scope step-up, SEP-2352 migration (HTTP-only, like `transport_http`) |

**No project header includes another project header** — cross-module use happens
only inside `.cpp` files, which is what keeps the flat `include/` directory
viable. The direction is a review convention (there is no automated check yet).

## Threading model

- **Dispatch slots (`MCP_WORKERS`).** `Server::run()` reads one message,
  processes it and writes the reply. `processLine()` waits for one of
  `MCP_WORKERS` slots (a counting semaphore built from mutex + cv); the default
  of 1 reproduces the historical serialized model exactly. With `N > 1`, up to
  N handlers run concurrently and handler code must be thread-safe.
- **The HTTP server transport dispatches from httplib's pool** (64 threads by
  default). Concurrency therefore changes latency, not handler ordering; a
  handler must not re-enter `processLine()` (it would wait on a slot it already
  holds with one worker, and nest with more).
- **Cancellation bypasses the dispatch lock.** `notifications/cancelled` flips a
  per-request `shared_ptr<atomic_bool>` under `cancellationsMutex_`, so it takes
  effect while a long handler still holds the dispatch lock. This is required
  for correctness, not just latency: the request being cancelled is the one
  holding the lock, so a cancel that queued behind dispatch would deadlock.
  A cancellation that arrives before its request parks a pre-set flag for the
  request to adopt.
- `subscriptions_` has its own mutex (`subscriptionsMutex_`) because notification
  fan-out may be published from another thread (HTTP SSE pumps).
- **The server never initiates requests.** Server-to-client interaction travels
  inside an `input_required` result (MRTR). A response arriving at a server is
  ignored.
- **A pre-2026 server may.** The client keeps answering when it does: a request
  the peer initiated (sampling/elicitation/roots) is handed to
  `Client::onServerRequest`, and the answer goes back as a fresh message; the
  client loop then keeps waiting for its own response.
- With one dispatch slot a blocking handler blocks every connection; with
  `MCP_WORKERS > 1` it occupies one slot. For long work, poll
  `Ctx::isCancelled` and emit `Ctx::progress` so the client can watch (and so a
  cancel can take effect).
- **The HTTP client transport runs its own threads.** Every request POST streams
  its response on a worker thread (a response may be a long-lived SSE stream, so
  a synchronous read would block the client loop from answering a mid-call
  elicitation), and the GET listening stream owns a reconnect loop. The only
  channel back to `read()` is the message queue. Three handles stop them -
  `closeSubscriptions()`, `closeRequestStreams()`, `closeListenStreams()` (the
  last two destructor-only) - and the destructor joins them so no thread
  outlives the queue.
- **The legacy `initialize` response is held back until its session id is
  read.** A stateful pre-2026 server issues `Mcp-Session-Id` on that response and
  rejects the next request without it; queueing the message before the worker
  stored the header let `notifications/initialized` race ahead of it.
- **`HttpClientTransport::setAuthChallengeHandler` reports 401/403
  `WWW-Authenticate` challenges**; the transport never authenticates itself.
  That is the hook `mcp::oauth` (and the conformance fixture) drives.
- **A subscription is a durable stream, not one HTTP request.** The server emits
  an SSE keep-alive comment every 5 s, so the subscription stream's read timeout
  is 30 s (a shorter timeout tears down an idle subscription between
  keep-alives). After a dropped stream the client re-subscribes, honours the
  server's `retry:` advice clamped to 100 ms–30 s, and suppresses duplicate
  acknowledgments from the reconnect, so the caller sees exactly one
  acknowledgment per subscribe. A **successful non-SSE answer** — a `200` body
  that is not an event stream, usually a JSON-RPC error — is queued as one
  message and terminates the stream thread: re-subscribing after a refusal
  would produce a reconnect storm.
- **`read()` has no idle timeout.** `nullopt` is orderly teardown (the
  destructor), and an error is a lost connection (the GET listen loop could not
  reconnect). A quiet connection is alive, so a slow request must not look like
  EOF. There is no per-request timeout API; a caller that needs one wraps the
  call. `closeSubscriptions()` does not wake a reader blocked in `read()`.

## Request lifecycle (2026-07-28)

1. Parse into a `variant<Request, Notification, Result, ErrorResponse,
   InvalidMessage>` — parsing never fails, it classifies.
2. Validate `params._meta`: `protocolVersion` + `clientCapabilities` are
   required; missing/ill-typed → `-32602`. In legacy mode a partial `_meta` is
   served with defaults instead.
3. Version gate: unsupported → `-32022` with the supported list in `error.data`.
4. Capability gate via the route table: unadvertised feature → `-32601`; unknown
   tool/resource/prompt → `-32602`.
5. Handler runs; the server injects `resultType:"complete"` when absent
   (modern requests only — pre-2026 schemas reject the field), validates tool
   `inputSchema`/`outputSchema`, and streams any notifications the handler
   emitted (progress, logging) before the result — over stdio as lines, over
   HTTP as an SSE body when there is more than one message.

## Invariants a contributor must not break

- **Protocol reads go through `mcp::member(obj, key)`, never `obj[key]`** —
  zjson's `operator[]` deep-searches nested layers for missing keys and misreads
  nested protocol data.
- **Text values are wrapped in `Json::str(...)`** — zjson sniffs a leading `{`/`[`
  and parses it as JSON.
- **Probe presence with `isNull()`/`isError()`/`isObject()`**, never
  `toString().empty()` — Null renders as the literal string `"null"`.
- **An empty mirrored `Mcp-Param-*` value is a value.** A present-but-empty
  argument must be sent as a header (and validated as such server-side);
  `setHeader(name, "")` stores, and only `removeHeader(name)` deletes. This is
  why the two are distinct operations.
- **`version.txt` and `kSdkVersion` stay in sync.** `version.txt` is the single
  source: `CMakeLists.txt` reads it for `project(VERSION)`, and `mcp_test_version`
  asserts `kSdkVersion` against the value CMake was configured with.
- **The installed package carries its platform link dependencies.** httplib is a
  `BUILD_INTERFACE` link (so it stays out of the export), but its platform
  libraries are propagated explicitly as `INTERFACE` (`ws2_32` on Windows,
  `Threads::Threads` on POSIX). Do not drop them: a `find_package(mcp)` consumer
  would fail to link the HTTP transport.
- **`thirds/` headers never appear in public headers** (only `mcp::Json`
  reaches the API surface), and httplib stays behind the pimpl wall.
- Conformance suites (official, both spec versions + the in-repo 2026-07-28
  suite) must stay green; the scores are recorded in `CHANGELOG.md`.

## Deliberate simplifications

- Dispatch through at most `MCP_WORKERS` slots (1 = serialized), no async
  runtime. `processLine()` gates on the slots, so the HTTP worker pool adds
  transport concurrency and `MCP_WORKERS` bounds how much handler concurrency
  results. A per-request `Server` instance model (like the official SDK
  factories) remains a possible future option if a workload demands isolation
  as well as concurrency.
- Long-lived server instance (per-request state is the handler's business;
  MRTR state is carried by `requestState`, which `RequestStateCodec` seals with
  HMAC-SHA256 bound to the method and target, so an echoed state cannot be
  replayed against another tool and survives a server restart when the key does).
  Horizontal scaling therefore requires stateful routing, unlike the official
  SDKs' per-request factory model — a documented trade-off, revisit if a
  workload demands it.
- OAuth is **client-side only** (`mcp::oauth`, used by the conformance fixture).
  A server still terminates authentication in front of the HTTP endpoint; the
  SDK does not validate bearer tokens.
