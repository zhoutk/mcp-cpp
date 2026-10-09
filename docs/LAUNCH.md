# Launch kit — announcing mcp-cpp

> Language: **English** | [中文](LAUNCH.zh-CN.md)

Drafts and channel notes for announcing the SDK. **Nothing here has been posted.**
Posting is outward-facing: review the text, then post it yourself (or say so and
have it posted), and only after the repository is public.

The goal is demand: the SDK Working Group is easier to convince when the C++ SDK
already has users. See [ADOPTION-PLAN.md](ADOPTION-PLAN.md) Phase 3.

---

## 1. Positioning (one line, reused everywhere)

> **There is no official C++ SDK for MCP. mcp-cpp is a C++17 one that passes the
> official conformance suite — and it's already running two real applications.**

Longer version (for the blog post and the WG proposal):

> The official MCP SDK roster is TypeScript, Python, C#, Go, Rust, Ruby, Java,
> Swift, PHP and Kotlin. C++ — where databases, engines, embedded systems and
> trading systems live — has no official SDK, and the community C++ options do
> not pass the conformance suite. mcp-cpp is a C++17 SDK with no dependencies
> beyond a bundled JSON header and optional cpp-httplib. It implements the
> 2026-07-28 specification (stateless per-request metadata, `server/discover`,
> MRTR, `subscriptions/listen`, progress, cancellation, CacheableResult, mirrored
> HTTP headers), validates tool schemas on every call, and passes 26/30 scored
> scenarios of the official conformance suite. The four it does not pass all
> require the server to *request* something from the client mid-call — a
> mechanism 2026-07-28 replaced with MRTR, which mcp-cpp implements instead.

## 2. Blog post draft

**Title:** A C++17 SDK for MCP, and what the conformance suite taught us

**Hook:** C++ is missing from MCP's official SDK roster. Here's a C++17
implementation of the 2026-07-28 spec — plus the five real bugs that running the
official conformance suite found in it.

**Body outline**

1. *Why C++* — embedding MCP in an existing C++ service; edge and embedded;
   latency-sensitive database/engine integrations. Name the two real users:
   [zorm-mcp](https://github.com/zhoutk/zorm-mcp) (a multi-backend database MCP
   server) and [zorm](https://github.com/zhoutk/zorm) (the ORM behind it).
2. *Design* — C++17, value-semantic messages, exception-free protocol layer,
   `McpResult<T>` aliased to `std::expected` when available, single-threaded
   stateless dispatch, `Ctx::progress` / `Ctx::notify` / `Ctx::elicit`.
3. *The interesting part: what conformance caught.* Five concrete bugs, each
   with the spec clause:
   - the HTTP transport dropped notifications emitted during a request (progress
     and logging were silently lost) — now streamed as SSE;
   - `resultType` was injected on pre-2026 responses, which the older schemas
     reject;
   - the legacy metadata path never decoded `progressToken`;
   - `ping` / `logging/setLevel` / `resources/subscribe` were gone for old clients;
   - a missing `MCP-Protocol-Version` header was rejected, when the spec says a
     server MAY treat it as `2025-03-26`.
4. *The awkward finding* — the official suite (v0.1.16) has **no 2026-07-28
   scenarios**; its newest is 2025-11-25. So a 2026-07-28 SDK cannot be scored
   against the current spec revision yet. What we did instead: an in-repo
   2026-07-28 suite (`tests/test_conformance_2026.cpp`, 12 named scenarios
   including a full MRTR round trip).
5. *Honest numbers* — raw JSON parse is slower than serde_json (0.79× at 1 KB,
   0.38× at 64 KB) because it validates protocol semantics on the way in; the
   full-stack dispatch and stdio round-trip numbers are in the README.
6. *Ask* — try it, file issues, and if C++ belongs on the official roster, say so
   in the MCP SDK Working Group discussions.

## 3. Channel checklist

| Channel | What to post | Notes |
| --- | --- | --- |
| Hacker News | "Show HN: A C++17 SDK for MCP that passes the conformance suite" | Link the repo; be present in comments; lead with the conformance story, not the feature list |
| r/mcp | The blog post | This is the audience that cares about MCP specifically |
| r/cpp | The blog post, framed as C++ design (value semantics, exception-free, CMake packaging) | Lead with the C++ angle, not the AI angle |
| MCP Discord `#general-sdk-dev` | Short intro + repo link + the 2026-07-28-scenarios gap | The WG members read this channel |
| MCP GitHub Discussions | Proposal to add C++ to the official roster | See ADOPTION-PLAN.md Phase 4 |
| Awesome-MCP lists / Smithery / Pulse MCP | SDK entry | So "C++" stops being blank in SDK directories |
| vcpkg / ConanCenter | Port PRs | Reach C++ users where they already are |

## 4. Posting rules

- **Post the blog post before the WG proposal**, so the proposal can cite real
  traffic and issues.
- **Do not claim a 2026-07-28 conformance score.** The only defensible number is
  "26/30 on suite v0.1.16, which covers 2025-xx revisions".
- **Answer the two objections up front** (in the post, not just in comments):
  "why C++ for servers" and "two community C++ SDKs already exist".
- Keep the tone factual. The bugs the suite found are a feature of the story, not
  an embarrassment — they are exactly why conformance testing matters.

## 5. Assets to prepare before posting

- [ ] Repository public, CI green, README badges resolving.
- [ ] A short screen recording or animated GIF: `minimal_server` + MCP Inspector.
- [ ] The conformance report committed and linked.
- [ ] `v0.1.0` tagged (or `v1.0.0` once the release is cut).
- [ ] A pinned issue listing the known gaps (the four MRTR-era scenarios, the
      absent 2026 scenarios) so the first comments are not a surprise.

---

*Related: [ADOPTION-PLAN.md](ADOPTION-PLAN.md), [OPTIMIZATION-PLAN.md](OPTIMIZATION-PLAN.md),
[CONFORMANCE.md](CONFORMANCE.md).*
