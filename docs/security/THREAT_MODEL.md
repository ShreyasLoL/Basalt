# BASALT Threat Model

**Scope date:** 2026-11-25
**Applies to:** current checked source and default configuration
**Related assessment:** [Security audit](SECURITY_AUDIT.md)

This is a practical source-based threat model for the local simulation product. It records controls observed in the repository and their limits; it is not a security certification.

## Security objectives and assets

| Asset | Security objective |
|---|---|
| Order book and simulation orders | Preserve order lifecycle, matching priority and quantity conservation. |
| Cash, positions, reservations, fills and PnL | Prevent overflow, double application and ledger divergence. |
| Risk settings and kill-switch state | Reject unsafe simulated actions and preserve/report emergency-stop state. |
| Strategies, backtests and replay | Keep work bounded and label results as simulated or historical. |
| Market data and instrument catalog | Reject malformed or ambiguous input and preserve normalization provenance. |
| Event logs, checkpoints and datasets | Bound recovery work, detect corruption and constrain file use. |
| HTTP UI, local server and MCP tools | Reject hostile input, limit resource use and expose only the intended simulator surface. |
| Configuration and credentials | Keep secrets out of source, browser bundles, responses, logs and audit records. |

## Entry points and trust boundaries

| Boundary | Untrusted input or capability | Controls observed |
|---|---|---|
| Browser / local HTTP client → loopback HTTP server | Method, URI, headers, origin, content type, JSON body | Loopback-only bind; exact Host/Origin checks; bounded request body and admission; explicit routes and mutation schemas; generic client-safe failures; browser security headers and CSP. |
| HTTP handlers → simulation and benchmark code | Order parameters, simulation steps, reset and benchmark options | Closed JSON schemas, scalar type/range checks, queue and rate limits, bounded benchmark order count. |
| Local parent process → MCP child over stdio | JSON-RPC frames, tool names and arguments | Bounded frame/tool inputs, closed schemas, limited operations, audit records, no shell/arbitrary-file/credential tool. Possession of the process pipe grants the local tool surface; there is no separate authentication. |
| Replay file / provider interface → normalizer and event bus | CSV/JSONL rows, timestamps, symbols, prices and quantities | Bounded line and aggregate input, schema/depth/column/event caps, instrument mapping and data-quality checks. Upstox/FYERS adapters are diagnostic-only in this build. |
| Persistence files → recovery and replay | Event records, checkpoints, catalog and dataset metadata | Length/version/sequence/CRC checks before payload allocation, bounded record parsing, valid-prefix recovery and corruption reporting. CRC detects accidental damage, not malicious tampering. |
| Build and package sources → executable/assets | CMake dependencies, npm lockfile and GitHub Actions | C++ dependencies are pinned to immutable commit IDs; npm uses a lockfile; CI uses read-only scanner jobs and immutable action/scanner references. Scanner jobs still require a CI run for observed results. |

## Threat actors and scenarios

| Scenario | Current defenses | Residual risk |
|---|---|---|
| A malicious website tries to reach the browser's localhost session, including through DNS rebinding or cross-origin requests. | HTTP binds only to loopback; Host and Origin accept exact local authorities/allowlisted origins; no wildcard CORS; mutation requests require valid JSON; CSP and response headers reduce browser content risk. | These controls are not authentication. A native local client can issue requests, and browser protections depend on the browser enforcing the same-origin policy. |
| A local process sends malformed, oversized, slow or expensive HTTP requests. | Patched cpp-httplib is pinned; request body, queue, worker and benchmark/rate limits are configured; JSON mutation schemas are closed; errors do not return internal exception text. | HTTP has no user authentication. A process running as the user can still consume the local service's bounded resources or invoke allowed simulation actions. |
| A selected market-data file is malicious, huge, deeply nested or structurally ambiguous. | CSV/JSONL readers enforce a 1 MiB line cap, 256 MiB aggregate cap, 64-column header cap, 32 JSON nesting levels, closed JSON properties and event limits. | Parsing limits do not establish the truth or provenance of user-provided prices or timestamps. Imported data remains research input. |
| A local event log or checkpoint is truncated or corrupted. | Parsers validate framing and checksums before allocation, retain valid prefixes and report/quarantine corrupt tails. | Records do not have general cryptographic signatures or authenticated tamper evidence. A same-user process can rewrite local data and recompute checksums. |
| A dependency source or build action is compromised or has a known vulnerability. | CMake dependencies use fixed commits; npm has a lockfile; CI scanner images and actions are pinned; npm audit, OSV Scanner, Gitleaks and clang-tidy are configured and were run locally. | Hosted CI results remain unobserved; OSV's local scan covered the npm lockfile rather than the CMake FetchContent commits; scanners have coverage and false-positive limits and do not prove the build is safe. |
| Invalid numbers, arithmetic overflow, repeated operations or concurrency break financial simulation state. | Financial values use checked fixed-point arithmetic and ledger/risk invariants; regression tests cover lifecycle and idempotency. CI has sanitizer, TSan and bounded fuzz jobs. | Local TSan/fuzz campaigns were not run on the Windows host. Tests cover selected invariants, not every interleaving or possible state transition. |
| MCP input attempts arbitrary execution or uncontrolled file access. | Tool names and arguments are schema-validated; tools expose bounded inspection/simulation capabilities and no shell or arbitrary path access; persistence setup fails closed when permissions cannot be established. | Any process with access to the stdio pipe can exercise the permitted local simulation tools. This interface is not an authenticated multi-user service. |

## Assumptions and out of scope

- The host operating system, user account, local filesystem and process launcher are trusted.
- BASALT cannot defend its memory or files from administrator-equivalent access, a fully compromised host, malware running with the same user permissions, or an unlocked physical session.
- The supported deployment is local-only. Remote exposure would require a separate design for authentication, authorization, TLS, deployment boundaries and operations; loopback controls are not a substitute.
- No live brokerage connection or live order placement is implemented. Results are for simulation, research or historical analysis, not production exchange infrastructure.
- Market-data provider adapters in this checkout are synthetic, replay or diagnostic-only; no active credentials or real provider connection are required.
