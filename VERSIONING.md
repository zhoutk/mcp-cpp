# Versioning Policy

mcp-cpp follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html):
`MAJOR.MINOR.PATCH`, with the version sourced from `version.txt` and mirrored by
`mcp::kSdkVersion` (the two must stay in sync).

`version.txt` is the only value a release edits: `CMakeLists.txt` reads it for
`project(... VERSION ...)`, the installed `mcpConfigVersion.cmake` derives from
that, and `mcp_test_version` asserts `kSdkVersion` against the CMake value in CI.
Editing one without the other fails the build's test run.

## What counts as a breaking (MAJOR) change

- Removing or renaming anything in a public header (`include/mcp/**`).
- Changing the signature or semantics of a public function, method, or handler
  contract (`Ctx`, `Handler`, `ITransport`, result shapes).
- **Dropping support for an MCP protocol revision the SDK previously
  negotiated.** A server that stops answering `2025-11-25` clients it used to
  serve is a MAJOR break, matching the official SDKs' policy.
- Changing the meaning of a wire-visible field the SDK emits (`resultType`,
  `ttlMs`/`cacheScope`, mirrored headers).
- Raising the minimum C++ standard or minimum compiler version.
- Changing the CMake package: target name (`mcp::mcp`), config package name
  (`mcp`), or removing a documented option (`MCP_DISABLE_HTTP`,
  `MCP_ZJSON_DIR`, `MCP_BUILD_TESTS`, `MCP_BUILD_EXAMPLES`, `MCP_INSTALL`).

## What is NOT breaking

- Adding new public API (new builder methods, new `Client` wrappers, new `Ctx`
  helpers).
- Bug fixes that bring behaviour closer to the specification, even when that
  changes wire output that previously contradicted the spec — the change is
  documented in `CHANGELOG.md` and flagged in the release notes when it is
  observable.
- Adding new CMake options, or changing internal build layout (`build/`,
  `bin/`).
- Anything under `src/` not reachable from a public header.

## Deprecation

Deprecated API is marked with a `[[deprecated]]` attribute and a comment naming
the replacement. Deprecated API is removed no earlier than the next MAJOR
release, and the removal is listed in `CHANGELOG.md`.

## Release cadence

- PATCH: as needed for fixes; no schedule.
- MINOR: when a coherent feature set lands; conformance suite must be green at
  the tagged commit and the score recorded in `CHANGELOG.md`.
- MAJOR: for the breaks listed above; a migration note is required in
  `CHANGELOG.md`.

## ABI note

The library is static and consumed by building from source (add_subdirectory,
FetchContent, or an installed CMake package). There is no stable binary ABI
across releases: rebuild consumers when the version changes. The bundled
`thirds/` copies (zjson, cpp-httplib, googletest) are part of the source
distribution and are updated per the [dependency policy](DEPENDENCY_POLICY.md).
