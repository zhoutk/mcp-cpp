# Dependency Policy

mcp-cpp deliberately keeps a tiny dependency surface so it can be dropped into
any C++17 project. Adding a runtime dependency is a significant change, not a
convenience.

## Current dependencies

| Dependency | Role | How shipped | Policy |
| --- | --- | --- | --- |
| **zjson** | JSON layer (`mcp::Json` alias) | Bundled single header under `thirds/zjson` | Required; treated as part of the SDK. Public API exposes `mcp::Json`, so its behaviour is covered by the versioning policy. |
| **cpp-httplib** | Streamable HTTP transport | Bundled under `thirds/httplib`; optional via `MCP_DISABLE_HTTP=ON` | Implementation detail. Kept out of the public headers (pimpl + forward declarations) and linked through `$<BUILD_INTERFACE:httplib>` so an installed package needs nothing else. |
| **googletest** | Tests only | Bundled under `thirds/googletest-1.18.0` | Never linked into the library; consumers do not see it. |

## Rules

1. **No new runtime dependency without an approved issue** describing what it
   buys, what it costs, and why it cannot be avoided.
2. New dependencies must be header-only or trivially vendorable, permissively
   licensed, and must not leak into `include/` — the only third-party header
   reachable from a public header is the bundled JSON header.
3. Build-time-only tooling (test frameworks, benchmark helpers) must not be
   linked into the library target.

## Update cadence

- No scheduled bumps. Bundled components are refreshed when a security advisory
  applies, when a bug we hit is fixed upstream, or at least once per MINOR
  release — whichever comes first.
- Every refresh is recorded in `CHANGELOG.md` with the version before → after.
- Refreshes are verified by the full test suite and the conformance run before
  being merged.

## Vendoring, not submodules

`thirds/` holds plain copies, not git submodules, so a fresh clone builds with
no network access. The copies are minimal (only what the build consumes) and the
upstream license is recorded in `NOTICE`.
