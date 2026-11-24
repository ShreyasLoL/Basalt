# BASALT Security Posture

**Assessment date:** 2026-11-25
**Scope:** current checked source and default configuration

BASALT's security posture is designed for a local-first research and exchange simulator. The Phase 2 work adds concrete protections at the HTTP, browser, market-data, MCP, dependency and CI boundaries. It does not make BASALT a production exchange, brokerage platform or authenticated multi-user service.

## Active controls

- The HTTP server is loopback-only (`127.0.0.1` or `::1`); the former remote-bind opt-in fails closed.
- Host and Origin values are checked against exact loopback authorities and the configured origin allowlist. CORS is not wildcarded. These checks help defend browser access and DNS-rebinding paths; they are not user authentication.
- The HTTP transport has bounded request bodies, workers/queue admission, timeouts and bounded benchmark execution. Mutation endpoints enforce closed JSON schemas and safe errors.
- Responses include browser security headers and a restrictive CSP for the compiled UI and the same-origin legacy fallback assets.
- Market-data replay parsing bounds line and file size, CSV columns, JSON nesting, accepted properties and event counts.
- MCP runs over local stdio, bounds frames and tool inputs, validates schemas, records local tool actions and exposes no arbitrary shell, file or credential operation. Data storage setup fails closed when the intended permission controls cannot be established.
- C++ dependencies are pinned to immutable commits, npm dependencies are locked, and CI defines read-only dependency, secret and static-analysis jobs plus sanitizer and fuzz jobs.

## Access and connectivity

- **Authentication:** none for the HTTP server. Any local process able to connect to the loopback port can issue requests allowed by the route policy. MCP has no separate authentication; access to its stdio pipe is the capability boundary.
- **Remote access:** not supported. Binding to non-loopback interfaces is rejected. Do not expose or tunnel the HTTP port as a remote service.
- **Market data:** the checked provider adapters are synthetic, replay or diagnostic-only. Upstox and FYERS adapters do not establish live authenticated network sessions.
- **Trading:** no live brokerage orders are placed. Orders and executions are simulated.
- **Persistence integrity:** framing, lengths, sequence, CRC and dataset digests help detect corruption and accidental mismatch. There is no general cryptographic signature or authenticated tamper-evident audit chain.

## Limits

Loopback does not protect against a compromised host, malware or another process running with the same user permissions, administrator access, or someone using an unlocked session. Same-user processes may be able to inspect or modify local files and invoke the unauthenticated HTTP service. Operating-system account separation and trusted local process execution remain assumptions.

Host and Origin validation make browser-based cross-site access harder; they do not prove who sent a native local request. CSP is defense in depth and does not replace safe rendering or validation. CRC is a corruption check, not a cryptographic authenticity check. Tests and scanners cover selected failure classes and cannot prove the absence of vulnerabilities.

Local verification passed the full CTest suite **177/177** in both Release and ASan/UBSan builds, frontend typecheck, Vitest **10/10**, production build, actionlint and repo-wide clang-tidy across 36 backend translation units. npm audit reported zero vulnerabilities; OSV found no issues in 236 frontend lockfile packages; Gitleaks found no leaks in 51 commits. A working-tree Gitleaks scan found eight redacted upstream example/test matches only in generated FetchContent files under `build-phase2/_deps/httplib-src`; it found no matches elsewhere. The OSV result does not cover CMake FetchContent commits, which were reviewed separately. The hosted CI workflow itself has not been triggered, and Docker-based pinned image invocations remain untested locally. TSan and libFuzzer were not run because the CMake configuration rejects them on Windows. See [Security Testing](SECURITY_TESTING.md) and [Security Audit](SECURITY_AUDIT.md) for evidence and finding status.
