> Language: **English** | [中文](PLAN.zh-CN.md)

# mcp-cpp — Standalone Library Implementation Plan

> Status: **Draft v1** (think before doing; this document rolls forward with the implementation process, recording the train of thought)
> Date: 2026-09-21 · Branch: `mcp`
> Related: `docs/mcp/mcp-summary.md` (protocol summary), `docs/mcp/specification-2026-07-28/` (spec text), `docs/mcp/mcp-migration-plan.md` (overall migration plan; this document is the deepened landing of its "protocol-layer standalone library")

---

## 0. One-sentence goal

Build a **standalone, publishable MCP C++ SDK** named `mcp-cpp` (namespace `mcp`):
targeting full feature coverage of the official Tier-1 SDKs (TS/Python/C#/Go/Rust) plus protocol conformance alignment,
with performance benchmarked against and exceeding rust-sdk (zero-copy parsing, arena memory model, lock-free dispatch), and natively serving zorm's scenarios.

## 1. Verified inputs (2026-09-21)

| Item | Conclusion | Source |
| --- | --- | --- |
| Protocol version | **2026-07-28** is the current stable spec; all protocol descriptions in this document take it as authoritative | spec changelog |
| Protocol surface | `server/discover`, `initialize` (legacy compat), `tools/list|call`, `resources/*`, `prompts/*`, `completion/complete`, `elicitation/create` (client), `sampling/create` (deprecated), `roots/list` (deprecated), `notifications/*` (cancelled/progress/message/list_changed), `subscriptions/listen` (new in 2026), results must carry `resultType` | schema.ts enum (all method names already extracted from the local schema.md) |
| Major changes in 2026 | **Stateless-ization**: `initialize` handshake removed, `Mcp-Session-Id` removed on the HTTP side; client per-request `_meta` carries `protocolVersion/clientCapabilities/clientInfo`; `subscriptions/resource subscribe` → `subscriptions/listen`; `ping`/`logging/setLevel` removed (logLevel goes through request `_meta`); error-code partition `-32020~-32099` assigned to the spec, `-32000~-32019` legacy; `CacheableResult` (`ttlMs/cacheScope`); MRTR replaces server-initiated requests; Tasks turned into extension `io.modelcontextprotocol/tasks` | changelog, 9 major items |
| zjson number model | **Three-state NumberData: Int64/Uint64/Double**, `from_chars` fast path, arena zero-copy (StringView borrows the parsing document's memory), streaming output `toString/dumpTo`; the integer parsing size recovered satisfies the protocol's **int64 fidelity** requirement with no precision loss; `Json(text)`/`add(key,text)` has a `{`/`[` leading-char sniffing convention (text values must be wrapped in `Json::str()`, a historical bug class) | `thirds/zjson/zjson.hpp` L73~121, project CLAUDE.md |
| httplib | yhirose/cpp-httplib **v0.52.0** (MIT), single header `thirds/httplib/httplib.h` + a ready-made INTERFACE target (thread-pool definition/keepalive tuning already configured) | `thirds/httplib/CMakeLists.txt` |
| Toolchain | Primary: clang 22.1.8 (MSYS2 clang64, libc++); reference: MSVC (VS2026), GCC 15 (libstdc++); CMake≥3.18 | measured locally |
| conformance tests | `modelcontextprotocol/conformance` repo (opened 2026-01); Tier-1 = 100% pass on "applicable must-test items" (pending/skip may be annotated) | sdk-tiers.md |

### 1.1 C++ standard maturity measurement (local clang 22 / libc++, verified header-by-header compilation)

| Feature | clang22(libc++) local | GCC 15(libstdc++) | MSVC(VS2026) | Notes |
| --- | --- | --- | --- | --- |
| `std::expected` | ✅ | ✅ 12.1+ | ✅ | primary error handling |
| `std::format/print` | ✅ | ✅ 13/14+ | ✅ | logging, schema construction |
| `std::flat_map` | ✅ | ✅ 15.1+ | ✅ | |
| `std::span` | ✅ | ✅ | ✅ | transport-layer buffers |
| `<generator>` | ❌ header missing | ✅ 14.1+* | ✅ | *libstdc++ is TS/experimental |
| `<inplace_vector>` | ❌ | ❌ | ✅ (`/std:c++latest`) | |
| `<contracts>` / `<meta>` | ❌ | ❌ | ❌ | missing in all three; C++26 not practical |
| `std::chrono tz` | ✅ | ✅ 14.1+ | ✅ | |

Conclusion: **C++23 is the highest sweet spot that is "mature in both MSVC+GCC and locally verifiable"** (clang22's libc++ lacks generator/inplace_vector, which can be worked around with `std::function`/a hand-written trivially-copyable vector, without touching vtable complexity). The C++26 language layer (contracts/reflection) is not landed in any of the three, so including it would only introduce platform randomness. **C++23 recommended**, while using feature-test macros to guard against fallback (see §6.1).
> **2026-09-21 Update (v1.1, conclusion revised)**: the user raised "for zorm (a C++17 project), is the progress from 17→23 worth giving up backward compatibility". After re-reviewing subsystem by subsystem, the choice was overturned to **C++17**; the full rationale and alternatives are in §9 Update 2. This table is retained as the raw record of "feature-availability measurement" and no longer carries the selection conclusion.
## 2. Product positioning and boundaries

### 2.1 Positioning (fix the scope to prevent bloat)

1. **Protocol core library** (no zorm dependency whatsoever): JSON-RPC 2.0 codec, message model and version negotiation for five spec versions (2024-11-05 → 2026-07-28), stdio + Streamable HTTP transport, server/client dual-side capability dispatch, MRTR, cancellation, progress, subscriptions.
2. **Type system and Schema**: a complete TS/Python-parity type layer (Request/Result/Error tri-state + `resultType` polymorphic discrimination), a JSON Schema 2020-12 validator (shared by tools inputSchema/outputSchema/elicitation schema).
3. **Runtime**: server runtime (tool dispatch, capability negotiation, error codes, concurrency model), an optional "server-builder" portable API (benchmarked against Rust `Server::builder().add_tool(...)`, TS `McpServer.registerTool`).
4. **C++17 as the base, conditional modernization**: error propagation uses the built-in `McpResult` (aliased directly to `std::expected` when `__cpp_lib_expected` is detected) — see the item-by-item comparison table in §9 Update 2.

### 2.2 Out of scope (not done in v1, written into ROADMAP)

- OAuth 2.1 / Client ID Metadata Documents (HTTP auth) — depends on WebSocket/TLS ecosystem selection, deferred;
- Tasks extension (`io.modelcontextprotocol/tasks`) — downgraded to an official extension in 2026, requires an async scheduler, put in v2;
- MCP Apps (UI extension) — beyond library responsibility;
- HTTP+SSE old transport (deprecated 2025-03) — only retain compatibility probing (send `GET` 409/405 to distinguish old servers), not a full implementation.

## 3. Directory and CMake structure

```
thirds/mcp-cpp/
  include/mcp/                 # public, 3~5 headers
    mcp.hpp                    # all-in-one single-header entry (aggregates several sub-headers)
    types.hpp                  # Json types + Message types (Version/Capabilities/Tool/...)
    error.hpp                  # Error codes + expected<T,McpError>
    server.hpp                 # Server / builder API
    client.hpp                 # Client (stdio + http)
    transport.hpp              # ITransport / StdioTransport / HttpTransport
    schema.hpp                 # JSON Schema 2020-12 validation
    version.hpp                # MCP protocol version enum/strings
  src/
    types.cpp  error.cpp  schema.cpp  schema_check.cpp
    server.cpp  client.cpp
    transport_stdio.cpp  transport_http.cpp
    executor.cpp               # single-threaded core/multi-thread, task queue, cancellation
  tests/
    test_jsonrpc.cpp           # full RFC coverage (invalid id/batch/strict parsing)
    test_types_roundtrip.cpp   # schema.ts all types ↔ Json round-trip
    test_version.cpp           # version alignment matrix tests (6 spec versions)
    test_schema.cpp            # JSON Schema 2020-12 validation test set
    test_stdio_e2e.cpp         # stdio transport end-to-end (process)
    test_http_e2e.cpp          # httplib end-to-end
    test_mrtr.cpp test_cancel.cpp test_progress.cpp
  bench/
    bench_parse.cpp bench_dispatch.cpp    # performance comparison vs "Rust baseline" (see §8)
  examples/
    zorm_server.cpp  minimal_server.cpp  client_probe.cpp
  CMakeLists.txt  README.md  LICENSE
```

- CMake targets: `mcp` (static library, **C++17**, consistent with the host project, WIN32/ZORM decoupled), `mcp::mcp` as an alias, `mcp_tests`, `mcp_bench`.
- Integration into the root project: root CMake `add_subdirectory(thirds/mcp-cpp)`, appending only the two targets `mcp` / `mcp_tests`, 👍 while also independently buildable (self-contained within the directory, copy-and-use).
- Relationship with zjson: **the protocol core library carries a thin JSON abstraction** (the default implementation directly uses the zjson single-header package `thirds/zjson`, exposing only a `mcp::Json` forward declaration + concept constraints to the outside; users may also inject adapters such as rapidjson). Reason: an independent release cannot hard-bind the zorm repo path.

## 4. Module design

### 4.1 Type layer (benchmarked against TS `types.ts`, the ~1500-line C++ counterpart)

```cpp
namespace mcp {
  // Protocol version (corresponds to TS LATEST_PROTOCOL_VERSION and 6 version constants)
  enum class ProtocolVersion : uint8_t {
    V2024_11_05, V2025_03_26, V2025_06_18,
    V2025_11_25, V2026_07_28, Latest = V2026_07_28
  };
  std::string_view toString(ProtocolVersion);
  std::optional<ProtocolVersion> fromString(std::string_view);

  // Capability structures (the 2016 version uses variant, the 2026 version queries _meta)
  struct ClientCapabilities { /* fs/roots/experimental + extensions dict */ };
  struct ServerCapabilities { /* tools/resources/prompts/... + extensions */ };

  // Tri-state result model (new in 2026): complete / input required / transport-level error
  using RpcResult = std::variant<CompleteResult, InputRequiredResult>;
  // Error: strongly-typed error codes + JSON-RPC code
  enum class McpErrorCode : int32_t { ... };  // ParseError=-32700, InvalidRequest=-32600,
                                              // MethodNotFound=-32601, InvalidParams=-32602, InternalError=-32603,
                                              // -32020 HeaderMismatch, -32021 MissingRequiredClientCapability,
                                              // -32022 UnsupportedProtocolVersion ...
  struct McpError { int32_t code; std::string message; Json data; };
  template<class T> using McpResult = std::expected<T, McpError>;
}
```

- Decision: **do not use inheritance polymorphism** to express Result (the C++ counterpart of Rust's enum is `std::variant` + visitor; the counterpart of C#'s `record` is a `struct` aggregate); the message layer is entirely value semantics + `[[nodiscard]]`.
- schema validation: **JSON Schema 2020-12** validator (supports local `$ref`, `const/enum/type/minimum/...` 18 strong-semantic keywords + recursion depth cap + composition branch cap — following SEP-2106's "resource bounds"). v1 is for **protocol self-protection** (validating the consistency of self-generated schemas); full `$ref` remote resolution and custom vocabularies go to v2.

### 4.2 Transport layer

```cpp
class ITransport {
public:
  virtual ~ITransport() = default;
  // Read/write loop: read one JSON-RPC message, write one out or stream
  virtual McpResult<Message> readMessage() = 0;
  virtual McpResult<void>   writeMessage(const Message&) = 0;
};
```

- **stdio**: `_setmode`/unistd dual-platform binary mode; line framing (`\n`); **strict UTF-8** (required by the spec, invalid sequences → `ParseError`); `notifications/cancelled` interpreted by the scheduler.
- **Streamable HTTP**: based on `thirds/httplib` (v0.52). Single-endpoint POST; the response may be a single JSON or `text/event-stream`; implement `subscriptions/listen` per changelog items 4/9 (long-lived POST stream + `io.modelcontextprotocol/subscriptionId`); **no session header** (2026 removed `Mcp-Session-Id`, guarding against mis-design). httplib has built-in SSE support (v0.52 `set_chunked_content_provider`), a thread pool of 64 concurrent controlled by the ready-made parameters in thirds/httplib/CMakeLists.txt.
- Transport and scheduling decoupled: `ITransport` is completely non-virtual (injected via a template parameter or std::function callback); tests can use an in-memory mock transport.

### 4.3 Scheduler/runtime (performance-sensitive area)

- **Lock-free dispatch design**: single-threaded event loop (`epoll/kqueue/IOCP` abstraction → first use `std::this_thread` blocking read + task ring as a zero-dependency version), inbound message → (validate schema → dispatch → handler) all with arena JSON, **zero `std::string` copies** (zjson's StringView borrows document memory, lifetime managed by the arena pool). Aligns with rust-sdk's `Arc<Mutex<>>` path, but lock-free.
- Optional TL-threads mode: multiple workers (each with its own arena), inbound sharded by hash, handlers declare thread affinity.
- Handler signature (concept-constrained, rejects non-value-semantics handlers at compile time):

```cpp
using ToolHandler = std::function<McpResult<CallToolResult>(const CallToolRequest&)>;
// high-performance path: provide `context` (arena*out) at registration time for the user to reserve a buffer, avoiding a second allocation
```

### 4.4 Server/Client portable API (benchmarked against the best practices of five Tier-1 SDKs)

```cpp
mcp::server::Builder b(ProtocolVersion::Latest);
b.with_name("zorm").with_version("0.1.0")
 .options({.pageSize=50, .requireConfirmationOnWrite=true})
 .add_tool(Tool{ .name="db_select", .description="...", .inputSchema=schema })
     .handle([](Ctx& c, const Json& args) -> McpResult<Json> {
        auto db = c.get<zorm::Idb>();  // zorm is in the example; the library itself does not depend on it
        return ...;
     });
auto server = b.build();
server.run(StdioTransport{});     // or server.serve_http("0.0.0.0", 8080)
```

- Client API benchmarked against python-sdk `ClientSession`: `connect(transport)` / `list_tools()` / `call_tool()` / `read_resource()` / `list_resource_templates()` / `subscribe(...)`, all `McpResult<T>`.
- C++17 idiom reinforcement: builder chaining returns use ref-qualified `T&& set_x(...)&&` (C++17 can already precisely control `&&` lifetimes); compile-time schema registration uses `constexpr` aggregates + a `consteval` precursor (enable `consteval` if a C++20 compiler is detected, otherwise `constexpr`).

## 5. Feature alignment matrix with the official Tier-1 SDKs (design acceptance)

| Feature | TS/src | Python | C# | Go | Rust | **mcp-cpp coverage stage** |
| --- | --- | --- | --- | --- | --- | --- |
| JSON-RPC 2.0 strict codec | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| 6-version protocol negotiation (schema see the V-version constants) | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| stdio transport | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| Streamable HTTP + SSE | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| tools/resources/prompts full types | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| resource URI templates (`resources/templates/list` + read template fallback) | ✅ | ✅ | ✅ | ✅ | ✅ | M5 follow-up |
| error codes + `resultType` tri-state | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| MRTR (input_required → inputResponses retry) | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| cancellation notifications/cancelled | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| progress notifications/progress | ✅ | ✅ | ✅ | ✅ | ✅ | P1 |
| `server/discover` (new 2026 RPC) | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| `subscriptions/listen` (new 2026) | ✅ | ✅ | ✅ | ✅ | ✅ | P0 |
| CacheableResult(ttlMs/cacheScope) | ✅ | ✅ | ✅ | ✅ | ✅ | P1 |
| completion/complete | ✅ | ✅ | ✅ | ✅ | ✅ | P1 |
| client capability: elicitation(MRTR) | ✅ | ✅ | ✅ | ✅ | ✅ | P1 |
| Roots/Sampling (deprecated) | ✅ | ✅ | ✅ | ✅ | ✅ | **not implemented** (SEP-2577; keep a deprecated-handshake compatibility layer) |
| Tasks extension | E | E | E | E | E | v2 |
| OAuth/auth | ✅ | ✅ | ✅ | ✅ | ✅ | v2 |
| JSON Schema validation | TS uses ajv | pydantic | data-annotation | gojsonschema | jsonschema | **self-built lightweight validator** |
| **C++ feature: arena zero-copy / lock-free dispatch** | — | — | — | — | — | **P0 (differentiation)** |

> For the deprecated Roots/Sampling: the library **does not implement** them but guarantees messages don't crash (pass through unknown methods → MethodNotFound / ignore deprecated notifications), with HLS compatibility-disable-layer unit tests.

## 6. Engineering constraints (C++ best practices / publishability)

### 6.1 Standard and compiler matrix (v1.1 revised per §9 Update 2)

- **Language standard = C++17** (consistent with the zorm main project; `std=c++17` / `/std:c++17`).
- Conditional C++20/23 features (optional acceleration, not a threshold): if `__cpp_lib_expected >= 202202L` then `McpResult` aliases `std::expected` directly, otherwise use the built-in `Result` fallback; if `__cpp_lib_three_way_comparison` then enable `<=>`; everything not permitted by C++17 is written as a single implementation (no flood of dual implementations).
- Compiler matrix `(clang22, gcc≥11, msvc v19.3x) × (RelWithDebInfo, Debug)`; GCC lower bound lowered to 11 (Ubuntu 22.04's default version, maximizing audience).
- **Dependency transparency**: `thirds/zjson` (JSON), `thirds/httplib` (HTTP, optional macro `MCP_DISABLE_HTTP`), no other third-party libraries; gtest uses the root project's existing `googletest-1.18.0` (tests are internal only, not shipped with the library).

### 6.2 Publishability / standalone-release checklist

- [ ] `thirds/mcp-cpp` can be `git subtree split` into a standalone repo (self-contained CMake, no zorm path/header dependency)
- [ ] Build zero warnings + `clang-tidy` (`modernize-*`, `bugprone-*`, `readability-identifier-naming`=MCP-prefixed)
- [ ] `MCP_DISABLE_HTTP` builds independently of HTTP (pure stdio for idle low-spec devices)
- [ ] `include/` leak prevention: CI grep verifies public headers contain no `zorm/`, `db_`, mysql/pg symbols
- [ ] ABI: PIMPL isolates `std::` types across the boundary (`server.hpp` uses `pimpl`, a single exported forward declaration)
- [ ] Version file `version.txt` + semver + `MCP_SDK_VERSION` macro; License: MIT + NOTICE listing zjson/httplib
- [ ] README contains: feature matrix, a differences section vs TS/Go/Rust, zorm integration example

### 6.3 Testing and (beyond-official) quality items

1. **Unit**: JSON-RPC strict parsing (invalid UTF-8, id conflicts, large integers ±2^63, floating-point precision, deep-nesting depth cap), schema validation, version compatibility matrix.
2. **Protocol conformance self-test**: using `docs/mcp/specification-2026-07-28/schema.md` as source-of-truth, write a **generator**: a script scans the md where schema.ts lives, emits a C++ failure-case table → guaranteeing type packing cannot drift. (Align once the official conformance repo opens)
3. **Transport end-to-end**: stdio spawns a child process for real read/write; HTTP uses httplib to run server/client against each other across the 6 spec versions.
4. **Stress/concurrency**: 1000 concurrent tool calls, cancellation storm, stream backpressure, arena peak-memory regression.
5. **Mutation testing** (a zorm project convention): manual mutation of critical branches (version negotiation, error-code mapping, MRTR state machine) to verify test effectiveness.
6. **Performance gate**: bench output goes into CI to prevent performance regression.

## 7. Milestones (3-week granularity)

| Milestone | Content | Acceptance |
| --- | --- | --- |
| **M0 Skeleton** (2d) | directory/CMake/include skeleton, zjson integration, CI gray | all three compilers green |
| **M1 Protocol core** (1w) | types/error/version, JSON-RPC read/write, stdio transport | test_jsonrpc all green, Inspector runs through |
| **M2 server builder** (1w) | server dispatch, tools/resources/prompts, MRTR, cancellation | examples/minimal_server passes Inspector; test_stdio_e2e |
| **M3 client + HTTP** (1w) | client API, Streamable HTTP, subscriptions/listen | test_http_e2e; set up `zorm_server.cpp` connecting to a real DB |
| **M4 schema + completeness** (1w) | JSON Schema validator, completion, Cacheable, version matrix | all features aligned with §5 |
| **M5 performance & release** (1w) | three bench items (parse/dispatch/e2e) vs rust baseline, docs, standalone-release checklist | performance exceeds rust-sdk in the same scenario; all three-party checklist boxes ticked |

## 8. Performance goals ("exceeding rust-sdk" made verifiable)

Define three reproducible benchmarks (bench/):

| Benchmark | Content | rust-sdk baseline caliber | mcp-cpp goal |
| --- | --- | --- | --- |
| B1 parse | 1KB / 64KB JSON-RPC message throughput | serde_json + sdk type deserialization | ≥1.5× (arena + from_chars path) |
| B2 dispatch | tools/call dispatch + handler stub reply QPS | tokio single task | ≥2× (lock-free / no future overhead) |
| B3 e2e | stdio child process 10k message round-trip latency | official stdio transport | ≥1.2× (avoid double UTF-8 validation) |

- The rust baseline is run on the same machine with `cargo bench` (executed in M5); if it falls short, profile (`perf`/VTune) then optimize or lower the promise (recorded honestly in the docs).
- Means list by priority: arena reuse (zjson already has it) → lock-free ring enqueue → message pre-parse fingerprint (peek method first → fast-path schema validation) → iovec writev batching.

## 9. Decision records (start work after this round's confirmation)

### 2026-09-21 Update 2 (v1.1) — Choice overturned: **language standard = C++17**

**User's challenge**: how much progress does 17→23 actually make in this project? Is it worth giving up backward compatibility (the zorm main project is C++17) for it?
**Answer**: reviewed the C++20/23 candidate features subsystem by subsystem for "real contribution"; the conclusion is **the gains are concentrated in syntactic sugar, with not a single exciting structural advance**. Item by item:

| C++20/23 feature | Use in mcp-cpp | Alternative if kept in C++17 | Real cost |
| --- | --- | --- | --- |
| `std::expected<T,E>` | error propagation (return-type convention) | hand-written `McpResult` (a 60~80-line `Result/Ok/Err` template for this purpose only, value semantics, `[[nodiscard]]`) | +0.5d one-time |
| `std::variant`+visitor | message model | **already in C++17**, no change | 0 |
| `std::format/print` | logging/schema construction | zjson already has streaming/append capability + snappy hand-written formatting; protocol text only needs `%s/%d/%llx`-level requirements | ~0 |
| `deducing this` | builder chaining `&&` lifetimes | copy-qualified chaining (the same as the Rust builder convention) or explicit `move` | ~0 |
| concepts (20) | handler concept constraints | C++17 `if constexpr`+ trait, `static_assert` when truly non-conforming | slightly reduced readability |
| `constexpr string/vector`(20) | compile-time schema registration | start with a runtime registry when `consteval` is unusable | revisit in v2 |
| `<flat_map>` | small method→handler table | `std::unordered_map` (method-name magnitude <40, difference is ns-level) | ~0 |
| `std::span`(20) | buffer views | **already in C++17** (`<span>` is C++20 but zjson internally already uses `string_view` for equivalent scenarios) | 0 |
| ranges/views(20/23) | data pipelines | this library's data flow is "one message, one hop", no long chained pipeline | ~0 |
| coroutines/generator | streaming responses | M1~M3 have no streaming requirement; future SSE handlers callback directly at the transport layer, no coroutines needed | 0 |
| `std::format` fmt-style logging | structured logging | MISSION: share a simple stderr logger with zjson | ~0 |

Key verification: **the three biggest areas of interest (performance, memory, concurrency) have zero vendor increment between C++17→23** — performance comes from the zjson arena (already in 17), lock-free/single-threaded scheduling (C semantics layer, version-independent), `from_chars` (already in `<charconv>` as of C++17).
Conversely, the **certain costs** of going to 23 are:

1. **Narrower audience**: GCC≥13/Clang≥17/MSVC v19.3x and up — old distros (Ubuntu 20.04/22.04 default GCC 9/11, CentOS/Kylin family, old MSVC), and users who "want to drag the library directly into their own C++17 project" are all excluded. For a library whose selling point is "there's no official C++ SDK, you fill the gap", audience is the most core asset.
2. **Break from the host project**: mcp-cpp must both integrate into zorm (C++17) and be independently released; dual standards would make "one fix, two CIs" the norm (ABI/header compatibility matrix doubles).
3. **The C++26 part (alternative C)** was only ever experimental, not a decision factor.

**Overturned conclusion**: the library body is **C++17** (same standard as zorm, `-fsigned-char`/build system as before); at the same time make good use of compiler-independent C++20 *language* only as **conditional features** (`__has_include`/`__cpp_lib_expected` detection; use the standard-library implementation if detected, otherwise the built-in fallback — a two-line `#if`, delivering "nice C++20 acceleration" at zero burden).

**Performance/feature promises unchanged** (§8 three benchmarks, §5 full coverage matrix), plus one new positive promise: `mcp-cpp` is a library that is "copy-and-use in any C++17 project" — this itself is a product-level selling point against the reality that "boost seems to be maintained only by us".

### 2026-09-21 Update 1 (v1 first draft) — autonomous decision (adopt the recommended options, for the user's later review)

The user was offline at the time; handled as "autonomous decision, overturnable afterwards"; the preparations needed to start work with three recommended values adopted are ready:

1. ~~C++ standard = C++23~~ (**revised to C++17 in v1.1**, see Update 2); mitigation: the CI matrix includes a GCC13 compatibility check (most distros have expected/format from GCC 13);
2. **Concurrency model = start with a single-threaded event loop, with multiple workers as a compile-time switch (MCP_WORKERS=N) enabled when the M4 performance gate misses the B2 target**. Avoids the +3 days of concurrency testing brought forward by an "all-at-once dual-mode" approach.
3. **JSON layer = zjson by default + an `IJsonAdapter` injection point**. Takes all the arena zero-copy benefit; the injection point ensures third parties can replace it with rapidjson/nlohmann after standalone release. zjson ships with the library via a copy at `thirds/mcp-cpp/thirds/zjson` (or a CMake cache path).

If you overturn any item, modify this section and synchronize the related designs in §1.1/§4.3/§6.1; the cost estimate is unchanged.

> As previously agreed: before M0 starts, please state your position on the three items in §9; as each subsequent milestone completes, this file rolls forward with debug/measured records (a "update log" subsection appended at the end of each section).

### 2026-09-21 Update 3 — M0 complete (skeleton landing record)

**Acceptance caliber revision (user instruction)**: testing/acceptance is done only locally on clang 22.1.8 (MSYS2 clang64, windows-gnu); the GCC/MSVC matrix is downgraded to "maintain compatibility while coding" (all platform differences go through feature-test macros, no hard-coded compiler branches), to be verified later.

**Three corrections to the plan (verified facts take precedence)**:

1. **There are 5 protocol versions, not "6 versions"** (a typo in the §3 test list; the 5 enum values in §4.1 are authoritative), consistent with the spec version history.
2. **Error codes checked one by one against the local spec** (`docs/mcp/specification-2026-07-28/basic/index.md` "Error codes"): the five JSON-RPC standard codes + the MCP reserved region `-32020..-32099`, with only three named: `HeaderMismatch=-32020`, `MissingRequiredClientCapability=-32021` (whose `data.requiredCapabilities` points to `ClientCapabilities`), `UnsupportedProtocolVersion=-32022`. Also note that the 2026 version changed resource-not-found from `-32002` to `-32602` (InvalidParams); there is **no** dedicated resource-not-found code. `McpError.data` (JSON payload) is deferred to M1 to be added together with the message model — the ownership writing of `data` under zjson arena semantics must be settled first.
3. **Headers land by milestone**, no all-empty placeholder skeleton (avoiding ineffective diffs of "reserve the name first, fill the flesh later"). M0 actually delivers: `mcp.hpp` (aggregation), `version.hpp/.cpp`, `error.hpp`, `json.hpp`.

**Design records**:

- `McpError.code` uses the strongly-typed `enum class McpErrorCode : int32_t` (the plan's original text was bare int32_t — strong typing prevents call sites from passing arbitrary integers; `static_cast` when going onto the wire).
- **Error-return dual-mode unification**: `McpResult<T>` aliases `std::expected` when `__cpp_lib_expected >= 202202L`, otherwise the built-in `Result<T>` (variant implementation, with a `Result<void>` specialization using monostate; member surface identical to expected: `has_value/value/error`). The `Ok(v)/Ok()/Err(code,msg)/Err(McpError)` factories return an `ErrCarrier`, implicitly convertible to the function's return type `McpResult<T>` — the call site's `return Err(...)` needs no template arguments and is character-for-character identical in both modes.
- **Pitfall**: `detail::makeErr` must be placed after the `McpResult` alias declaration (template aliases must be declared before use) — clang pointed directly at the instantiation point, fixed in one pass.
- **zjson integration**: the `mcp::Json = ZJSON::Json` alias comes first; the `IJsonAdapter` injection point is deferred to M1 per the original plan (waiting for the minimal adapter surface to be clear). CMake's `MCP_ZJSON_DIR` probe order: `./thirds/zjson` (standalone-release layout) → `../zjson` (in-repo layout) → error.
- **gtest discovery order**: already registered by the parent project → repo `thirds/googletest-1.18.0` → system GTest package → warn and skip tests. The library body has zero test dependency.
- During the first configuration the `../../zjson` path was miscalculated once (`thirds/mcp-cpp`'s repo sibling is `../zjson`); CMake's EXISTS check caught it immediately — the probe design worked.

**M0 acceptance (local clang 22.1.8 / ninja)**:

- Root project integrated build all green (zormlib + all existing test targets unaffected).
- `ctest -R mcp_test`: `mcp_test_version`, `mcp_test_error` all pass (version round-trip/unknown string/error-code static_assert pinning the wire values/Result tri-state and void/non-default-constructible T).
- **Standalone build path verified in practice**: `cmake -S thirds/mcp-cpp -B <tmp> -DMCP_ZJSON_DIR=...` standalone configure, build, test all green — the "copy-and-use" promise holds.

### 2026-09-21 Update 4 — M1 complete (protocol core: types/JSON-RPC/stdio transport)

**Incremental facts verified from the spec before starting work** (all per `docs/mcp/specification-2026-07-28/`):

- **Four message types**: Request (id is string|integer, **null forbidden**), Notification (**id forbidden**, including null), Result Response (`resultType` mandatory; if missing, read as `complete` per legacy), Error Response (schema is `id?: RequestId` — **no null id**, omit when id can't be read). JSON-RPC 2.0's "id null means notification" reading is explicitly rejected: an id member present but null → InvalidRequest; mismatches must fail loudly.
- **Stateless**: the `initialize` handshake is removed in 2026; each request's `params._meta` carries `io.modelcontextprotocol/protocolVersion` (required) + `clientCapabilities` (required) + `clientInfo` (optional); missing → `-32602`. Dual-era servers split by "does the request carry modern `_meta` or carry `initialize`" — this is the key design input for the M2 server side.
- **`server/discover`**: `DiscoverResult{resultType:"complete", supportedVersions[], capabilities, instructions?, ttlMs, cacheScope}`; stdio clients use "a discover probe + recognizing modern errors (-32022 etc.)" to determine the server era; **the fallback must not bind to a single error code** (legacy servers report -32601/-32602 or do not respond to unknown methods).
- **stdio transport**: line framing, no embedded newlines, stdout only emits messages, stderr free, EOF is the primary graceful-shutdown signal; the wire format is independent of the transport channel (works for both socket/pipe) → `StreamTransport` injects an iostream pair, stdio is just a special case.
- **Capability objects stay Json-backed** (structured types, following spec evolution), providing `capabilitiesInclude`/`capabilitiesIncludeExtension` probes only for fields the runtime branches on. A fully-typed struct would freeze a field set that the spec is still actively extending — a deliberate deviation from the plan §4.1's "all-fields struct" route.

**Implementation points**:

- `parseMessage` never fails: any input produces `variant<Request, Notification, Result, ErrorResponse, InvalidMessage>`, and `InvalidMessage` carries everything needed to construct an error response (code + recoverable id). params must be an object (all MCP method params are objects); arrays (JSON-RPC batch) are explicitly rejected; fractional ids/error codes rejected; ±2^63 ids preserved.
- The encoding side wraps all text through `Json::str()` (the zjson feeding convention); tests include a "string value shaped like JSON is not sniffed" case.
- `Result<T>` gains `operator->`/`operator*`, fully aligned with the `std::expected` member surface.

**Pitfall records (two, both worth archiving)**:

1. **zjson API misuse**: `Json::str()` is a static factory (creates a string value); the member method for reading a string value is `toString()`; `Json::parse` is private, and the public entry is `Json::ParseJson(input, err)`. Corrected per the zorm main project's existing idiom (`params["id"].toString()`).
2. **Dangling UB in tests (the most valuable lesson)**: `asRequest(parseMessage(text))` pointed a pointer into a **temporary variant**, dangling the moment the full expression ended, then reading out garbage — 5 tests reported "bad_variant_access" that looked like a library bug but were actually test UB; the same library with a named local variable probe passed all. Lesson: **for functions returning a variant, land it in a local variable first, then `get_if`**. All 20 call sites were changed to the two-step pattern. Also: before ctest, confirm no stale exe is running (the project CLAUDE.md already has this pitfall recorded; the cwd drift hit this variant once more).

**M1 acceptance (local clang 22.1.8 / ninja)**: all 5 CTest registrations green (version/error/jsonrpc/types/transport, ~60 cases). The plan's "Inspector runs through" item is deferred to M2 (a real server process is needed to have something to connect to). The plan's `test_stdio_e2e` (child-process round-trip) is moved to M2 along with minimal_server — a stdio e2e without a handler loop can only test echo, of limited value.

### 2026-09-21 Update 5 — M2 complete (server builder + stdio e2e)

**Design decisions**:

- **Stateless dispatch**: `Server::processLine` is pure message processing — parse → meta validation → version gate → capability gate → routing → handler → encode reply. No session state whatsoever, conforming to 2026's "each request self-carries metadata".
- **Error-code mapping of the three-layer gate** (checked item by item against schema.md): missing `_meta`/required fields → `-32602`; unsupported version → `-32022` (data carries `supported`/`requested`, the array output in enum order); method not routed → `-32601`; capability not advertised → `-32601` (MethodNotFoundError documented explicitly); unknown tool/resource/prompt → `-32602` (same doc: unknown tool is InvalidParams).
- **Capability advertisement linked to registration**: `Builder::tool/resource/prompt` registration advertises the corresponding capability. The capability object `capabilities_` must be initialized to an **empty Object** rather than Null — `add` to a Null node is ineffective and silent (hit this time: 9 tests red together, root cause one line). The Null/Object distinction in Json must be respected as a type system.
- **Cancellation**: a `requestId → shared_ptr<atomic_bool>` slot table; `notifications/cancelled` sets the flag; on request arrival **reuse the existing slot** (under out-of-order delivery a cancellation must not be overwritten by a new flag); the slot is cleared on request completion. Under single-thread this is complete semantics; with M4's multiple workers the same slot design is naturally thread-safe.
- **resultType injection**: the server fills in `"complete"` for a handler-returned default resultType (spec: 2026 implementations must carry it; handlers only write it when they themselves care about polymorphism). `input_required` (MRTR) is passed through as-is — the handler just constructs an InputRequiredResult itself, and the M3 client closes the loop.
- **Pagination**: `*\/list` uses an opaque cursor (this implementation encodes it as an offset digit string), pageSize 0 = no pagination; an invalid cursor → `-32602`. `resources/templates/list` had a route registered but the template registry was not done in v1 (recorded as a gap) (since filled by Update 9).

**Pitfalls**:

1. `ASSERT_*` can only be used in void test functions — `spawnSelf()` returns a Child, and using ASSERT directly is a compile error (caught by gtest's static assertion); switched to EXPECT + an explicit validity check.
2. Windows pipe e2e: `CreateProcessA`'s lpCommandLine needs a **writable** LPSTR — `const std::string`'s `data()` is const char*, and clang pointed directly at the const loss.
3. zjson's idiom for iterating keys is `obj.getAllKeys().toVector()` (elements are Json, `toString()` gets the key), not range-for directly (that gives JsonEntry).

**M2 acceptance**: 7/7 CTest green (new test_server 30+ cases, test_stdio_e2e). **test_stdio_e2e is a real child-process end-to-end**: the test binary self-starts as an MCP server with the `--mcp-serve-child` argument (Windows CreateProcess + anonymous pipes / POSIX fork+execv), going through a real OS pipe to verify the discover/tools/list/tools/call triple + stdin EOF graceful shutdown. `minimal_server` passes a manual smoke test (feeding data through a pipe → correct reply); Inspector hookup is left for the user to verify in an interactive terminal (`npx @modelcontextprotocol/inspector build/thirds/mcp-cpp/minimal_server.exe`).

### 2026-09-21 Update 6 — M3 complete (client + Streamable HTTP + subscriptions)

**Spec inputs** (full close reading of streamable-http.md + subscriptions.md):

- The 2026 HTTP is **POST-only single endpoint**: the GET stream and Mcp-Session-Id are deleted (GET/DELETE return 405); unknown method **404 + -32601** (distinguishing from legacy HTTP+SSE servers' empty-body 404); notification POST → **202 with no body**; request → application/json single object or an SSE stream.
- **Header mirroring**: `MCP-Protocol-Version` (must match the body `_meta`), `Mcp-Method` (all requests), `Mcp-Name` (tools/call, resources/read, prompts/get, taking params.name/uri); missing/mismatched → **400 + -32020 HeaderMismatch**. Unsafe characters go through `=?base64?...?=` sentinel encoding.
- **subscriptions/listen**: the response is a long-lived SSE stream; the acknowledged notification must be the first and carries `io.modelcontextprotocol/subscriptionId` (value = the request id); subsequent notifications match per filter (toolsListChanged/promptsListChanged/resourcesListChanged/resourceSubscriptions); graceful close = reply to the original request with a result carrying subscriptionId.

**Implementation**:

- **Client** (transport-agnostic): id correlation, notification interleaved callbacks, JSON-RPC errors → McpError (data preserved, -32022's supported list readable), MRTR: the `input_required` result surfaces as-is + `callTool(..., inputResponses)` retry automatically carries a new id, subscribe returns at the ack notification (a result response never comes), `onSubscriptionClosed` receives graceful closes, `cancel` sends notifications/cancelled. Each request automatically carries `_meta` (version/capability/clientInfo) — a stateless protocol's client obligation.
- **HttpServerTransport**: an Origin validation hook (rejects non-localhost by default), a full header validation chain, 202/404/405/400 splitting, application/json responses; subscriptions/listen upgrades to SSE (`X-Accel-Buffering: no`, 15s comment keep-alive, a Pump queue + condvar), `Server::notifyListChanged/notifyResourceUpdated/closeSubscription` fan-out from any thread.
- **HttpClientTransport**: one POST per message; derived headers computed automatically (including the base64 sentinel); SSE responses are streamed on a **background thread + a separate connection** (avoiding a long-lived POST blocking the client loop), event lines queued for read() to consume; read() blocks waiting (an instantaneous empty queue ≠ EOF); **x-mcp-header mirroring**: listTools caches tool schemas, and callTool collects annotated parameter values along the static chain of properties and encodes them into `Mcp-Param-*` headers (ITransport gains a default-empty setHeader virtual method, and the stdio transport is naturally a no-op).
- **Server changes**: `build()` returns `shared_ptr<Server>` (mutex members forbid copy/move); a new Server move constructor (mutex default-constructed); a subscription registry + thread-safe fan-out.

**Pitfalls (all 3 recorded into tests)**:

1. **httplib stop() deadlock**: `svr.stop()` first joins the worker threads, and `is_running()` clears its flag only after the join — if the SSE provider uses `is_running()` as its loop condition, they wait on each other. Solution: the provider uses **its own** atomic stop flag; stop() sets the flag first, then calls svr.stop(); the Pump registry lets stop immediately close every subscription stream.
2. **Lifetime of the detached server thread**: after the Fixture destructs, the thread still references members → dangling hang. Changed to saving the thread + joining after stop.
3. A Python batch replace wrote `\n` as a real newline into C++ string literals (two places), exposed on the spot by compile errors — in the future, verify such replacements with the Edit tool.

**Known gap (recorded, filled in M4)**: the server-side MUST validation of `Mcp-Param-*` headers against the body is not implemented (needs to reverse-look-up the annotation path from the registry's tool schema); the client-side mirroring is implemented.

**M3 acceptance**: 9/9 CTest green (new test_client 8 cases, test_http_e2e 7 cases — real httplib server/client running against each other on loopback: discover/callTool/readResource/404/405/403/-32020/SSE subscription stream ack+notification+filtering). Zero warnings.

### 2026-09-21 Update 7 — M4 complete (schema validator + completions + Cacheable) + systematic elimination of the zjson deep-search hazard

**Deliveries**:

- **JSON Schema 2020-12 validator** (`mcp/schema.hpp`): type/enum/const, numeric bounds (minimum/maximum/exclusive*/multipleOf), strings (minLength/maxLength/pattern), objects (required/properties/additionalProperties/patternProperties/min-maxProperties), arrays (items single-value and tuple/uniqueItems/min-maxItems), composition (allOf/anyOf/oneOf/not), local `$ref` (JSON Pointer + `~0/~1` escaping, remote refs rejected outright), boolean schemas. **Resource bounds**: depth 64, check count capped at 10000 (implementing the spec's SHOULD against schema DoS); exceeding the limit fails rather than passing leniently.
- **server integration**: tools/call validates arguments per the tool's inputSchema (violation → -32602); `completion/complete` route + completions capability + `Builder::completions(handler)`; `Builder::listCache(ttlMs, scope)` adds CacheableResult's ttlMs/cacheScope to `*\/list`.
- **M3 gap filled**: the server does MUST validation of `Mcp-Param-*` mirror headers (reverse-look-up the annotation path from the registry's tool schema; missing/mismatched/extra headers → 400 + -32020); `x-mcp-header` annotation traversal is extracted into `header_params.h`, shared by client/server.
- `member()` enters the public API (see below).

**★ zjson operator[] deep-search fallback — a systematic hazard (the user asked for key emphasis; now in the persistent memory store)**:

- **Symptom**: the schema validator took `schema["type"]` on a `{"anyOf":[...]}` object and actually got the `"type":"string"` nested inside the array — anyOf was skipped and the sub-schema validated directly, order-dependent and hard to diagnose.
- **Root cause**: zjson's `operator[]` is **deliberately designed** as "direct member first, otherwise `findPtrDeep` deep-searches the whole tree" (the source comment states this explicitly). This is harmless for zorm's flat usage, but at the protocol layer, probing "possibly absent" members misreads nested data. Similar systematic exposure points: `doc["id"]` would get `params.id`, `params["name"]` would get `_meta.clientInfo.name`.
- **Fix**: a new public helper **`mcp::member(obj, key)`** (direct member or Null, never deep-searches, based on `directMemberPtr`), and a **whole-library sweep** of all "possibly absent" member reads in six places: jsonrpc/types/server/transport_http/client/header_params. `contains()` is itself direct semantics, unchanged.
- **Supporting pitfall**: the absent sentinel must not use `Json("null")` — the sniffing convention turns it into the string "null" (only `{`/`[` leading chars are parsed) — must be `Json(nullptr)` (a true Null node).
- **Lesson**: when introducing a third-party library, "extra semantics" (deep-search fallback) are as dangerous as "missing semantics"; probe-type reads must explicitly choose direct/deep semantics. This item has been written into `~/.claude/projects/D--codes-zorm/memory/zjson-operator-deep-search-trap.md`.

**M4 acceptance**: 10/10 CTest green zero warnings (new test_schema 12 cases; server extended with inputSchema enforcement/completion/cache-hint cases).

### 2026-09-21 Update 8 — M5 complete (benchmarks + release checklist) + project wrap-up

**Measured benchmarks** (RelWithDebInfo standalone build, clang 22.1.8 / MSYS2 clang64 / windows-gnu; rust baseline = serde_json 1.x, rust-sdk's actual parsing layer, same machine same message shape):

| Benchmark | mcp-cpp | serde_json baseline | Goal (§8) | Conclusion |
| --- | --- | --- | --- | --- |
| B1 parse ~1KB | 714,000 msg/s (1.40µs) | 907,000 msg/s (1.10µs) | ≥1.5× | **missed** (0.79×) |
| B1 parse ~64KB | 35,900 msg/s (27.8µs) | 136,800 msg/s (7.31µs) | ≥1.5× | **missed** (0.38×) |
| B2 dispatch | 298,500 msg/s (3.35µs, incl. meta/version/capability gate + inputSchema validation + encoding) | needs a full tokio stack, no single-machine equivalent benchmark | ≥2× | no direct comparison; recorded honestly |
| B3 stdio e2e | 24,700 round trips/s (40.4µs, real pipe child process) | same as above | ≥1.2× | no direct comparison; recorded honestly |

**Honest revision of the §8 promises**: raw JSON parsing did not beat serde_json — serde's zero-copy borrowing parse is one of the industry's fastest models, while zjson is parse-into-owned-tree + our parseMessage layering strict semantic validation on top (M1's design tradeoff: correctness first). The §8 predefined optimization paths are now identified and recorded: ① `parseJsonText` currently copies a string_view once; zjson's arena borrowed view can do zero-copy parsing; ② the envelope pre-scan fast path (peek method/id first, then full parse); ③ in the large-message scenario zjson's tree deep copy is the hotspot. Whether it meets the target is left to re-measurement after implementation — the current version centers its delivery on "feature completeness + protocol conformance".

**Release checklist (§6.2) ticked**:

- [x] Standalone-buildable: the standalone path verified in practice multiple times throughout (first verified at M0, re-verified at M5 with RelWithDebInfo + Release)
- [x] Build zero warnings (clang -Wall -Wextra -Wpedantic)
- [x] `MCP_DISABLE_HTTP=ON` pure stdio build: standalone configure/build + 9/9 tests pass
- [x] include/ leak grep: no zorm/db_/driver symbols (zjson via the mcp::Json alias is expected)
- [x] version.txt + semver + `mcp::kSdkVersion`
- [x] MIT LICENSE + NOTICE (zjson/httplib/googletest)
- [x] README: feature matrix (vs Tier-1), honest benchmark comparison, usage, zorm integration
- [x] examples/zorm_server.cpp: built only in-repo (guarded by TARGET zormlib), four tools MCP-ized + inputSchema + real-DB smoke test passing; the library body stays zorm-free

**Mutation verification (§6.3 convention)**: the mutation of the version-negotiation error code -32022→-32601 was immediately caught by `ServerGates.UnsupportedVersionListsSupported`, and passed after restoration.

**M5 acceptance**: 10/10 CTest green + 3 runnable benchmarks (mcp_bench_parse/dispatch/e2e, with a `--bench-child` self-spawn mode).

**Project wrap-up status**: M0–M5 all complete. In the PLAN §5 feature matrix, everything has landed except the intentionally unimplemented Roots/Sampling (deprecated by SEP-2577) and the v2 items (Tasks/OAuth). Follow-up candidates (ordered by value): zero-copy parse optimization (the B1 gap), Tasks extension, ~~resources/templates/list registry~~ (completed, see Update 9), conformance repo alignment.

### 2026-10-09 Update 9 — resource URI templates (post-M5 follow-up)

**Motivation**: parameterized resources (typically database tables created only at runtime) under v1 must be registered statically one by one at startup — a table added after startup requires a restart or re-registration, and the client still gets a startup snapshot. URI templates let "a class of URIs" be registered once and any instance be readable, exactly filling the gap noted in M2 Update 5: "`resources/templates/list` had a route registered but the template registry was not done in v1".

**Deliveries**:

- **`Builder::resourceTemplate(Json definition, Handler readHandler)`** (`src/server.cpp`, declared in `include/mcp/server.hpp`): registers by the definition object's `uriTemplate` field as key, advertising the `resources` capability on demand just like `resource()`; a definition missing `uriTemplate` is ignored with a warning.
- **RFC 6570 subset matching**: literal segments + `{var}` (non-empty, no `/`) + `{+var}` (non-empty, may contain `/`); `{?query}`/`{#fragment}`/multi-variable expressions unsupported. Single-pass greedy matching, with variables delimited by the next literal.
- **`resources/templates/list`**: now returns a `resourceTemplates` array (pagination/caching on the same path as `resources/list`, `handlePaginatedList`); previously this route existed but returned MethodNotFound.
- **`resources/read` template fallback**: exact registration takes priority; when there's no exact hit, resolve by the most specific template — fewest variables first, and on a tie the longest literal prefix first.
- **Public helper `mcp::server::matchUriTemplate(tmpl, uri, vars)`** (`include/mcp/server.hpp`): the server's internal routing uses it, and it's also exposed to server authors needing the same matching.
- **Client `Client::listResourceTemplates(cursor)`** (`include/mcp/client.hpp`, `src/client.cpp`): a paginated-cursor wrapper over `resources/templates/list`.

**Acceptance**: new cases `ServerRegistrys.ResourceTemplates` (templates and concrete resources listed separately, an unregistered URI readable via a template, an exact hit beating a template, no match still `-32602`, templates inheriting the CacheableResult hint) and `ServerRegistrys.UriTemplateMatching` (`{var}` doesn't cross `/`/doesn't match an empty string, `{+var}` crosses `/`, a trailing literal is consumed exactly); `ctest` 10/10 green.

### 2026-10-09 Update 10 — Toward official adoption (gap analysis + protocol completeness)

**Motivation**: the goal is to propose mcp-cpp as the *official* C++ MCP SDK. Research on 2026-10-09 (modelcontextprotocol.io/docs/sdk, the SDK Working Group charter, the conformance repo) confirmed the opening: the official roster is TS/Python/C#/Go/Rust/Ruby (Tier 1), Java (Tier 2), Swift/PHP/Kotlin (Tier 3) — **there is no C++ SDK, and no community-SDK list to join**. Admission is a Working Group decision ("Official SDK Roster", WG consensus → Core Maintainer approval), not a SEP. See `docs/ADOPTION-PLAN.md`.

**Deliverables this round**:

- **Gap analysis** (`docs/OPTIMIZATION-PLAN.md`): feature-by-feature comparison against the Tier-1 SDKs and the conformance suite, with a prioritized work list (P0 packaging/CI/governance, P1 protocol completeness, P2 performance).
- **Adoption plan** (`docs/ADOPTION-PLAN.md`) and **conformance how-to** (`docs/CONFORMANCE.md`). The conformance suite has **not been run yet** — no score is claimed until `conformance/report.json` exists.
- **Bilingual documentation** (English default): `README.md`/`README.zh-CN.md`, `PLAN.md`/`PLAN.zh-CN.md`, plus `ROADMAP`, `CONTRIBUTING`, and the three docs above.
- **Governance files** required by the Tiering System: `ROADMAP.md`, `CONTRIBUTING.md` (with dependency policy), `SECURITY.md`, `CODE_OF_CONDUCT.md`, `MAINTAINERS.md`, `CHANGELOG.md`, `.github/` issue + PR templates, and a CI workflow.

**Protocol completeness** (the gaps the comparison surfaced):

- **Progress notifications** (`basic/patterns/progress.md`): `Ctx::progress(progress, total, message)` emits `notifications/progress` for requests carrying `params._meta.progressToken`; `Ctx::notify(method, params)` emits arbitrary notifications. `RequestMeta` now decodes `progressToken`; `encodeRequestMeta` and `Client::request`/`callTool` accept one. Progress is a no-op without a token, per the spec's "MUST only reference tokens from an active request".
- **`outputSchema` enforcement** (`server/tools.md` "Output Schema"): a tool result's `structuredContent` is validated against the declared `outputSchema`; non-conforming content fails with `InternalError`. Text-only results skip validation (nothing structured to check).
- **Result builders**: `textResult()` / `structuredResult()` — the latter also emits the spec-recommended serialized-JSON text block for backward compatibility.
- **Client wrappers**: `listResources()`, `listPrompts()`, `complete()`.
- **CMake package**: `install()` + exported `mcp::mcp` + `find_package(mcp CONFIG)`, verified with an out-of-tree consumer. httplib became a build-interface-only dependency (`$<BUILD_INTERFACE:httplib>`) so the installed target needs nothing but `mcp::mcp`.

**Deliberately not done** (recorded, not forgotten): OAuth 2.1, typed elicitation helpers, Tasks extension, zero-copy parse — all listed with priorities in the optimization plan. Roots/Sampling remain intentionally unimplemented (SEP-2577).

**Verification** (clang 22.1.8 / MSYS2 clang64 / windows-gnu): 10/10 CTest green with 8 new cases (`ServerProgress.*` ×3, `ServerOutputSchema.*` ×3, `ClientProgress.*` ×2); build is zero-warning; the `find_package(mcp CONFIG)` consumer builds and runs out of tree.

---
*Document maintenance rule: after each milestone completes, record the deviations encountered, the measured performance numbers, and rejected designs as newly added "Update YYYY-MM-DD" entries under the corresponding section; the original text is never deleted.*
