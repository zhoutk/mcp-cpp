> Language: **English** | [中文](ARCHITECTURE.zh-CN.md)

# Architecture

How mcp-cpp is layered, what runs on which thread, and the invariants a
contributor must not break. For the history of *why*, see [PLAN.md](../PLAN.md)
(the decision log).

## Layering

Each layer depends only on the ones before it:

```
error → version → json → jsonrpc → types → transport → server / client → schema / transport_http
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
| `include/mcp/client.hpp` | blocking `Client` over any `ITransport` |
| `include/mcp/schema.hpp` | JSON Schema 2020-12 validator (local `$ref` only, resource bounds) |
| `include/mcp/transport_http.hpp` | Streamable HTTP + SSE, both sides (pimpl keeps httplib out of the header) |

**No project header includes another project header** — cross-module use happens
only inside `.cpp` files, which is what keeps the flat `include/` directory
viable. A meta-test enforces the include direction.

## Threading model

- **Single-threaded dispatch.** `Server::run()` reads one message, processes it,
  writes the reply. Handlers are synchronous; `Server` members are plain except
  `subscriptions_`, which is mutex-guarded because notification fan-out may be
  published from another thread (HTTP SSE pumps).
- **The server never initiates requests.** Server-to-client interaction travels
  inside an `input_required` result (MRTR). A response arriving at a server is
  ignored.
- A blocking handler blocks the whole connection. For long work, poll
  `Ctx::isCancelled` and emit `Ctx::progress` so the client can watch.

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
- **`version.txt` and `kSdkVersion` stay in sync.**
- **`thirds/` headers never appear in public headers** (only `mcp::Json`
  reaches the API surface), and httplib stays behind the pimpl wall.
- Conformance suites (official, both spec versions + the in-repo 2026-07-28
  suite) must stay green; the scores are recorded in `CHANGELOG.md`.

## Deliberate simplifications

- Single-threaded, no async runtime. Multi-worker remains a possible
  compile-time option if a workload demands it.
- Long-lived server instance (per-request state is the handler's business;
  MRTR state is carried by `requestState`). Horizontal scaling therefore
  requires stateful routing, unlike the official SDKs' per-request factory
  model — a documented trade-off, revisit if a workload demands it.
- No OAuth yet (see `ROADMAP.md`); terminate auth in front of an HTTP server.
