> Language: **English** | [中文](README.zh-CN.md)

# mcp-cpp

[![CI](https://github.com/zhoutk/mcp-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/zhoutk/mcp-cpp/actions/workflows/ci.yml)
[![Conformance](https://img.shields.io/badge/conformance-26%20of%2030-blue)](conformance/report.json)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://en.cppreference.com/w/cpp/17)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)


**An independent, conformance-oriented C++17 SDK for the
[Model Context Protocol](https://modelcontextprotocol.io).**

mcp-cpp implements the MCP **2026-07-28** specification with Tier-1 SDK feature
coverage, a value-semantic message model, and no async runtime required. Its only
dependencies are a bundled single-header JSON library (zjson) and — optionally —
cpp-httplib for the Streamable HTTP transport. There is currently **no official
C++ SDK**; this project is offered as one (see [ROADMAP.md](ROADMAP.md)).

```
CMake ≥ 3.18 · C++17 · MIT · no external deps beyond a bundled JSON header
```

**Status:** v0.1.0, feature-complete per [PLAN.md](PLAN.md) §5. Against the
official conformance suite (v0.2.0-alpha.12) it passes **100% of the scored
2026-07-28 server scenarios (42/42 checks)** — see
[`conformance/report-2026-07-28.json`](conformance/report-2026-07-28.json) and
[docs/CONFORMANCE.md](docs/CONFORMANCE.md). On the 2025-11-25 suite it scores
26/30; the four remaining need the server to *request* something from the client
mid-call, which 2026-07-28 replaced with MRTR. The public roadmap is
[ROADMAP.md](ROADMAP.md).

---

## Why mcp-cpp

- **Nothing to install.** A C++17 tree with the JSON header vendored — `add_subdirectory`,
  `FetchContent`, or `find_package(mcp CONFIG)`. No Boost, no async framework, no codegen.
- **Value-semantic and exception-free at the protocol layer.** Messages are plain
  structs, results are `McpResult<T>` (aliased to `std::expected` when the standard
  library provides it), and decoders never throw.
- **Protocol-faithful, not protocol-flavoured.** Strict JSON-RPC 2.0 (null-id ban,
  no batching, integer-id fidelity to ±2³), the full 2026-07-28 stateless model
  (per-request `_meta`), `server/discover`, `subscriptions/listen`, MRTR, progress,
  cancellation, CacheableResult, mirrored HTTP headers with `-32020` enforcement.
- **Self-defending.** A JSON Schema 2020-12 validator (local `$ref`, resource
  bounds, no remote refs) enforces tool `inputSchema`/`outputSchema` on every call.
- **Proven.** It is the protocol layer behind two complete applications:
  [zorm-mcp](https://github.com/zhoutk/zorm-mcp) (a multi-backend database MCP
  server) and [zorm](https://github.com/zhoutk/zorm) (the ORM it builds on).

## Feature matrix (vs the official Tier-1 SDKs)

| Feature | mcp-cpp | Tier-1 parity |
| --- | --- | --- |
| JSON-RPC 2.0 strict codec (null-id ban, no batching, object params) | ✅ | ✅ |
| Protocol versions 2024-11-05 … 2026-07-28 + `-32022` negotiation | ✅ | ✅ |
| `server/discover` (supportedVersions / capabilities / ttl / cacheScope / serverInfo) | ✅ | ✅ |
| Stateless per-request `_meta` (protocolVersion / clientCapabilities / clientInfo) | ✅ | ✅ |
| stdio transport (newline framing, Windows binary mode, EOF shutdown) | ✅ | ✅ |
| Streamable HTTP (POST-only, header mirroring, `Mcp-Param-*`, 202/404/405, `-32020`) | ✅ | ✅ |
| SSE response streams + `subscriptions/listen` (ack, filter fan-out, graceful closure, keep-alive) | ✅ | ✅ |
| tools / resources / prompts registries, capability gating, pagination cursors | ✅ | ✅ |
| Resource URI templates (RFC 6570 subset) + template-fallback `resources/read` | ✅ | ✅ |
| `notifications/cancelled` + queryable cancellation flag | ✅ | ✅ |
| MRTR: `input_required` results + `inputResponses` retry, `requestState` echo, capability-gated `Ctx::elicit`/`Ctx::inputRequired` | ✅ | ✅ |
| Signed `requestState` (`RequestStateCodec`: HMAC-SHA256, context-bound, TTL) + `Client::callToolWithInput` auto-fulfil loop | ✅ | ✅ |
| Official conformance suite — **client mode** (8 scenarios green, both spec versions) | ✅ | ✅ |
| Progress notifications (`Ctx::progress`, client `progressToken` opt-in) | ✅ | ✅ |
| `completion/complete` (server + client) | ✅ | ✅ |
| CacheableResult (`ttlMs`/`cacheScope`) on discover, `*/list`, `resources/read` | ✅ | ✅ |
| JSON Schema 2020-12 validator; tool `inputSchema` **and** `outputSchema` enforced | ✅ | ✅ |
| `x-mcp-header` mirroring (client) + server-side validation | ✅ | ✅ |
| Structured results (`structuredResult()` + spec-recommended text block) | ✅ | ✅ |
| 2026-07-28 conformance suite (in-repo, `mcp_test_conformance_2026`) | ✅ | n/a |
| CMake package (`find_package(mcp CONFIG)`, exported `mcp::mcp`) | ✅ | n/a |
| Roots / Sampling (deprecated by SEP-2577) | ❌ intentionally | Tier-1 keeps compat shims |
| Tasks extension (`io.modelcontextprotocol/tasks`) | ⏳ roadmap | extension |
| OAuth 2.1 / Client ID Metadata | ⏳ roadmap | ✅ |

Value-semantic message model, exception-free protocol layer, `[[nodiscard]]` result
types with `Ok`/`Err` factories that read identically whether `McpResult<T>` aliases
`std::expected` or uses the built-in fallback.

## Benchmarks

`RelWithDebInfo`, standalone build, clang 22.1.8 (MSYS2 clang64, windows-gnu), same
machine and message shapes for both:

| Bench | mcp-cpp | serde_json (rust-sdk's parser) |
| --- | --- | --- |
| B1 parse ~1 KB request | 714,000 msg/s (1.40 µs) | 907,000 msg/s (1.10 µs) |
| B1 parse ~64 KB request | 35,900 msg/s (27.8 µs) | 136,800 msg/s (7.31 µs) |
| B2 `tools/call` dispatch round trip (gates + schema validation + encode) | 298,500 msg/s (3.35 µs) | n/a (needs full tokio stack) |
| B3 stdio end-to-end round trip (real pipes, child server) | 24,700 msg/s (40.4 µs) | n/a |

**Honest reading** (PLAN.md Update 8): raw JSON parse does **not** beat serde_json —
zjson's copy-on-parse model plus strict semantic validation costs more than serde's
zero-copy borrow parse, especially at 64 KB. The optimization paths are tracked in
[ROADMAP.md](ROADMAP.md). The B2/B3 numbers are
full-stack (validation included) and are the ones applications actually experience.

## Install

### CMake `find_package`

```bash
cmake --preset clang-rlz
cmake --build build -j 8
cmake --install build --prefix /your/prefix
```

```cmake
find_package(mcp CONFIG REQUIRED)
target_link_libraries(my_server PRIVATE mcp::mcp)
```

### FetchContent / add_subdirectory

```cmake
add_subdirectory(third_party/mcp-cpp)
target_link_libraries(my_server PRIVATE mcp::mcp)
```

Standalone configuration needs a zjson copy under `thirds/zjson`, or pass
`-DMCP_ZJSON_DIR=<dir>`. Options: `MCP_BUILD_TESTS` (ON), `MCP_BUILD_EXAMPLES` (ON),
`MCP_DISABLE_HTTP` (OFF — pure-stdio build), `MCP_INSTALL` (ON).

> A vcpkg port and a Conan recipe are on the roadmap ([ROADMAP.md](ROADMAP.md)).

## Quick start

### Server

```cpp
#include <mcp/mcp.hpp>

int main() {
  mcp::server::Builder b;
  b.name("my-server").version("1.0");

  b.tool(mcp::Json(R"({
        "name": "add",
        "description": "adds two integers",
        "inputSchema": {
          "type": "object",
          "properties": { "a": {"type":"integer"}, "b": {"type":"integer"} },
          "required": ["a","b"]
        }
      })"),
      [](mcp::server::Ctx& c) -> mcp::McpResult<mcp::Json> {
        const auto& args = c.params["arguments"];
        c.progress(1, 1, "computing");          // no-op unless the client opted in
        return mcp::Ok(mcp::server::structuredResult(mcp::Json(R"({"sum":0})")));
      });

  // A parametric resource: one template serves every db/table URI, including
  // tables created after the server started.
  b.resourceTemplate(
      mcp::Json(R"({"uriTemplate":"db://schema/{db}/{table}","name":"table schema"})"),
      [](mcp::server::Ctx& c) -> mcp::McpResult<mcp::Json> {
        std::vector<std::pair<std::string, std::string>> vars;
        mcp::server::matchUriTemplate("db://schema/{db}/{table}",
                                      c.params["uri"].toString(), &vars);
        return mcp::Ok(mcp::server::textResult("...columns for " + vars[1].second));
      });

  auto server = b.build();
  mcp::StdioTransport transport;
  return server->run(transport).has_value() ? 0 : 1;
}
```

For Streamable HTTP, build with `mcp::http::HttpServerTransport` instead — same
`Server`, different transport.

### Client

```cpp
mcp::client::Client client("probe", "1.0");
client.connect(transport);                       // any ITransport
client.onNotification([](const mcp::JsonRpcNotification& n) { /* progress, ... */ });

auto tools = client.listTools();
auto r = client.callTool("add", mcp::Json(R"({"a":2,"b":3})"), mcp::Json(nullptr),
                         mcp::RequestId(std::string("progress-1")));
```

## Ecosystem — built with mcp-cpp

- **[zorm-mcp](https://github.com/zhoutk/zorm-mcp)** — a production MCP server
  exposing 13 structured database tools over five backends (sqlite3 / jsonfile /
  mysql / postgres / dm8) through one dialect-free tool surface.
- **[zorm](https://github.com/zhoutk/zorm)** — the C++ ORM library (`zormlib`) that
  zorm-mcp builds on; mcp-cpp's first and most demanding consumer.

Both are complete applications: mcp-cpp supplies the protocol layer, they supply the
domain.

## Documentation

| Document | What it covers |
| --- | --- |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layering, threading model, and the invariants that must hold |
| [PLAN.md](PLAN.md) | Design + decision log (the "why" behind every deviation) |
| [ROADMAP.md](ROADMAP.md) | Public roadmap toward Tier 1 |
| [docs/CONFORMANCE.md](docs/CONFORMANCE.md) | Running the official conformance suite, both spec versions |
| [VERSIONING.md](VERSIONING.md) | SemVer policy and what counts as breaking |
| [DEPENDENCY_POLICY.md](DEPENDENCY_POLICY.md) | The dependency surface and update rules |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Build, test, and contribution workflow |
| [CHANGELOG.md](CHANGELOG.md) | Release history |

## Layout

```
include/mcp/   public headers (mcp.hpp aggregates everything)
src/           compiled implementation
tests/         googletest unit tests (10 CTest registrations, incl. real
               subprocess stdio e2e and loopback HTTP/SSE e2e)
bench/         B1 parse / B2 dispatch / B3 stdio e2e benchmarks
examples/      minimal_server (Inspector-ready) + zorm_server
cmake/         package config for find_package(mcp CONFIG)
docs/          design, optimization, adoption and conformance docs
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Security reports: [SECURITY.md](SECURITY.md).
Participation is governed by the [Code of Conduct](CODE_OF_CONDUCT.md).

## License

MIT — see [LICENSE](LICENSE) and [NOTICE](NOTICE) for bundled third-party
components (zjson, cpp-httplib, googletest).
