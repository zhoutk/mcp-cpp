> Language: **English** | [中文](CONTRIBUTING.zh-CN.md)

# Contributing to mcp-cpp

Thanks for helping build the C++ MCP SDK. This document covers how to build, test,
and submit changes, and the dependency policy the project commits to.

## Getting set up

Requirements: CMake ≥ 3.18 and a C++17 compiler. The verified primary toolchain is
clang 22 (MSYS2 clang64) with Ninja; GCC ≥ 11 and MSVC v19.3x are supported targets.

```bash
git clone https://github.com/zhoutk/mcp-cpp
cd mcp-cpp
cmake --preset clang-rlz          # Release; clang-dbg for Debug
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

On a machine without the preset's hard-coded MSYS2 paths, configure manually:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

Standalone configuration needs a zjson single header under `thirds/zjson`, or pass
`-DMCP_ZJSON_DIR=<dir>`.

### Useful targets

```bash
ctest --test-dir build -R mcp_test_server -V        # one suite
bin/mcp_test_server.exe --gtest_filter=ServerProgress.*   # one case
bin/mcp_bench_parse.exe                              # benchmarks (not in CTest)
```

Executables land in `bin/`, intermediates in `build/`.

## Making a change

1. **Open an issue first** for anything non-trivial, so we can agree on the shape
   before you write code. Use the issue templates.
2. **Branch** from `main`; keep commits focused.
3. **Match the surrounding code** — this codebase has strong conventions:
   - Protocol reads go through `mcp::member(obj, key)`, **never** `obj[key]` — zjson's
     `operator[]` deep-searches missing keys and misreads nested protocol data.
   - Wrap text values in `Json::str(...)` — zjson sniffs a leading `{`/`[`.
   - Test presence with `isNull()`/`isError()`/`hasChild`, never `toString().empty()`.
   - Keep new protocol wording in `guide.cpp`/the relevant header comment so it stays
     coherent.
4. **Add tests.** New server behaviour goes in `tests/test_server.cpp`; new protocol
   types in the matching `tests/test_*.cpp`. Tests drive the server through the
   in-memory `MemTransport`, so they need no real IO.
5. **Keep the build warning-free** (`-Wall -Wextra -Wpedantic`; `/W4` on MSVC).
6. **Run the full suite** before opening a PR: `ctest --test-dir build --output-on-failure`.

## Pull requests

- One logical change per PR; describe *why*, not just *what*.
- Reference the issue it closes.
- CI must be green (build matrix + tests + conformance).
- A maintainer reviews; expect feedback on protocol fidelity first, style second.

## Dependency policy

mcp-cpp deliberately keeps a tiny dependency surface so it can be dropped into any
C++17 project.

| Dependency | Role | Policy |
| --- | --- | --- |
| **zjson** | JSON layer (`mcp::Json` alias) | Bundled single header. Required. Treated as part of the SDK. |
| **cpp-httplib** | Streamable HTTP transport | Optional (`MCP_DISABLE_HTTP=ON` removes it). Header-only; kept out of public headers via pimpl. |
| **googletest** | Tests only | Never linked into the library. |

Rules:

- **No new runtime dependency without an RFC-style issue** and maintainer approval.
- New dependencies must be header-only or trivially vendorable, permissively
  licensed, and must not leak into the public headers (`include/` stays
  dependency-clean except for the `mcp::Json` alias).
- **Update cadence:** bundled third-party components are refreshed when a security
  advisory applies or at least once per minor release; the change is noted in
  `CHANGELOG.md`.
- Public headers must not include any third-party header except the bundled JSON
  header. CI enforces this with a grep check.

## Releasing

1. Update `version.txt`, `include/mcp/version.hpp` (`kSdkVersion`) and `CHANGELOG.md`.
   `CMakeLists.txt` reads `version.txt`, so no CMake edit is needed; the version
   test fails if `kSdkVersion` drifts.
2. Tag `vX.Y.Z`; the CI release job builds and attaches artifacts.
3. Run the conformance suite and record the score in the changelog.

## Code of conduct

By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md). Report
security issues per [SECURITY.md](SECURITY.md), not in a public issue.

## Questions

Open a GitHub Discussion, or an issue using the `question` template.
