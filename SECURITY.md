# Security Policy

## Supported versions

| Version | Supported |
| --- | --- |
| 0.1.x | ✅ current |
| < 0.1 | ❌ |

Pre-1.0, only the latest release is supported. Security fixes are applied to the
latest release and to `main`.

## Reporting a vulnerability

**Do not open a public issue for a security vulnerability.**

Report privately via GitHub's [private vulnerability reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
on `github.com/zhoutk/mcp-cpp`, or email the maintainer listed in `MAINTAINERS.md`.

Include:

- a description of the issue and its impact,
- steps to reproduce (a minimal server/client snippet if possible),
- affected version/commit and platform,
- any suggested fix.

You will receive an acknowledgement within **2 business days**. Critical issues
(CVSS ≥ 7.0, or core-functionality failures) are targeted for a fix within
**7 days**, consistent with the MCP Tier-1 commitment. We will credit you in the
advisory unless you ask otherwise.

## Scope

In scope:

- The protocol layer (`include/mcp`, `src/`): malformed-input handling, the JSON
  Schema validator's resource bounds, transport parsing, header validation.
- The HTTP transport: origin/DNS-rebinding checks, mirrored-header enforcement.
- Anything that lets a peer cause memory corruption, unbounded resource use, or
  code execution.

Out of scope:

- Vulnerabilities in applications built on mcp-cpp (report to their maintainers).
- Vulnerabilities in bundled third-party components (zjson, cpp-httplib,
  googletest) — report upstream; we will pull the fix and note it in the changelog.
- Denial of service by a *trusted* local client over stdio.

## Hardening notes for users

- The HTTP transport rejects non-localhost `Origin` headers by default (DNS
  rebinding). If you expose a server beyond localhost, terminate auth in front of
  it — mcp-cpp does not yet implement OAuth (see [ROADMAP.md](ROADMAP.md)).
- Tool handlers run with the server's privileges. Treat tool arguments as
  untrusted input; mcp-cpp validates them against `inputSchema`, but your handler
  still owns authorization.
