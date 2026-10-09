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

## [0.1.0] - 2026-09-21

Initial feature-complete release (PLAN.md M0–M5): JSON-RPC 2.0 strict codec, five
protocol versions, stdio + Streamable HTTP transports, tools/resources/prompts
registries with capability gating and pagination, resource URI templates, MRTR,
cancellation, `completion/complete`, CacheableResult, a JSON Schema 2020-12
validator, and `x-mcp-header` mirroring. Ten CTest suites, three benchmarks, and the
zorm integration example.

[Unreleased]: https://github.com/zhoutk/mcp-cpp/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/zhoutk/mcp-cpp/releases/tag/v0.1.0
