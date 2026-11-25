# BASALT MCP and HTTP Threat Model

**Scope:** the local C++ MCP stdio server, the loopback HTTP gateway, and their local persistence paths. This describes the current implementation, not a deployment certification. Revisit it when the gateway is exposed beyond the local machine or any live-broker capability is proposed.

## Assets and security goals

- **Simulation integrity:** order-book state, paper fills, risk decisions, portfolio journals and deterministic replay inputs must not be confused with live market or brokerage state.
- **Emergency controls:** the kill-switch latch and its durable events must remain observable and survive restart when persistence succeeds.
- **Local records:** MCP audit entries, kill-switch frames, dataset catalog metadata, historical files and checkpoints should stay within the user's local data boundary and detect accidental corruption.
- **Credentials and personal data:** provider credentials must not appear in HTTP/MCP responses, logs or audit records. Current MCP and HTTP tools do not submit live broker orders.
- **Availability:** malformed or oversized input should not cause unbounded parsing, simulation work or durable file growth.

## Trust boundaries

1. **MCP client → child process:** the configured client starts `basalt_server --mcp-stdio` and owns its stdin/stdout pipes. The protocol is local stdio, not a network listener. Possession of the pipe grants access to the server's bounded read and simulation tools; there is no additional MCP user authentication.
2. **Browser or local HTTP client → gateway:** the gateway binds to loopback only. A pre-routing policy checks Host, Origin, duplicate security-sensitive headers and mutation content type before serving static content or reading request bodies. Mutation JSON bodies use explicit schemas and a 16-level nesting limit. Unexpected benchmark failures are logged locally with redaction and a bounded diagnostic; callers receive a fixed generic 500 error. CORS is a browser policy and is not authentication for arbitrary local HTTP clients. The compiled Vite UI and the legacy fallback both receive CSP and baseline browser headers; fallback JavaScript and generated CSS are same-origin embedded assets.
3. **Server process → filesystem:** default MCP files are placed under `BASALT_DATA_DIR`, the executable directory, or the OS temporary `basalt-mcp` directory. The protocol cannot supply arbitrary file paths. Permission-setting failures reject a storage candidate; stdio startup and default server construction fail closed when a protected directory cannot be established.
4. **Build and dependency boundary:** CMake dependencies are pinned to immutable commit IDs and frontend dependencies are locked. CI has read-only npm audit, OSV Scanner, Gitleaks and clang-tidy jobs with immutable action and scanner-image references. Scanners have limited ecosystem coverage and do not attest to upstream source integrity.

## Attacker model

In scope are a malicious or compromised MCP caller with access to the configured stdio process; a hostile web page attempting cross-origin or DNS-rebinding requests; a local HTTP client sending malformed or excessive requests; and accidental corruption or truncation of local persistence files. The attacker may control request bytes but does not start with the user's operating-system account privileges.

Out of scope are compromise of the user's account, a malicious administrator, physical access to an unlocked device, and public/remote deployment. Malicious or vulnerable dependencies are in scope for pinning and scanning, but those controls cannot prove upstream source integrity. The gateway has no general user authentication. Binding it to a non-loopback address requires a separately designed authenticated deployment profile; loopback and CORS checks alone do not make remote exposure safe.

## STRIDE review

| Category | Threat | Controls present | Residual risk |
|---|---|---|---|
| **Spoofing** | A hostile web origin or DNS-rebinding hostname attempts to use the HTTP API. An untrusted process may be configured as the MCP client. | HTTP Host must match loopback or the explicitly configured bind IP; Origin must be loopback or an exact `BASALT_CORS_ALLOWED_ORIGINS` entry. Wildcard CORS is disabled. MCP is launched through stdio and does not listen on a port. | Host/Origin checks do not authenticate non-browser local HTTP clients. MCP trusts whoever can access its stdio pipe. Do not expose the gateway remotely without authentication and network controls. |
| **Tampering** | Crafted tool arguments, HTTP bodies or local files attempt to alter simulation state or bypass validation. | MCP tools use closed schemas, typed/range validation, bounded operations, idempotency checks and fixed persistence paths. HTTP mutations require JSON and the body limit is 64 KiB. `.blog` frames use sequence and CRC checks; datasets use SHA-256 verification. | CRC detects accidental corruption, not a malicious rewrite. A process running as the same user can alter local files. There is no signed or remote immutable audit chain. |
| **Repudiation** | A local simulation mutation cannot be reconstructed after the fact. | MCP mutations and failures produce bounded JSONL audit records with safe actor/method/outcome fields and a request-ID digest. Kill-switch state uses a shared versioned event schema; persistence failure is returned explicitly while the in-process latch remains active. | The audit is local, capped at 1 MiB with one bounded backup, and is not cryptographically signed or copied off-host. At-cap persistence rejects further durable writes. |
| **Information disclosure** | Error responses, tools or logs expose credentials, arbitrary local files or free-text kill reasons. | Safe protocol errors omit stack traces and argument values; tools do not expose shell, arbitrary file access, environment dumps or live broker placement. Kill-switch free-text reasons are redacted in durable records. MCP resolves data storage only after owner-only permission setup succeeds where supported; stdio startup fails closed if it cannot establish the directory. | Same-user processes can read the user's files and simulation data. Local files are not encrypted at rest. Current tools intentionally expose bounded simulation state to their caller. |
| **Denial of service** | Oversized frames, excessive calls or expensive inputs exhaust CPU, memory or disk. | MCP frames are capped at 256 KiB; HTTP bodies at 64 KiB. MCP calls have per-tool rate limits, bounded bars/replay/benchmark workloads, 1 MiB audit/log caps and bounded rotation. Market replay input is capped at 1 MiB per line and 256 MiB total, with a 64-column CSV header cap, JSON depth of 32 and one-million-event ceiling. HTTP queue, worker, URI, header and benchmark work are bounded. | Limits are per process/client and do not protect the host from many local processes, OS-level resource exhaustion, slow storage or all worst-case computational costs. |
| **Elevation of privilege** | A request attempts to execute a command, escape a path boundary or gain live-trading authority. | MCP has no shell or arbitrary-path tool. Durable paths are selected by the host process, tool schemas are closed, and the system is simulation-only with no brokerage order API. | The server runs with the launching user's OS privileges. A future live-order or remote-control feature would change this boundary and requires a new security review. |

## Verification and related controls

- HTTP host/origin, CORS preflight, content-type and request-size policy: [Security](SECURITY.md), implemented in `src/server/request_guard.cpp` and `src/server/http_server.cpp`, with real loopback coverage in `tests/test_http_server.cpp`.
- MCP schemas, limits, audit behavior and launch configuration: [MCP server](MCP.md), implemented in `src/server/mcp_server.cpp` and `src/server/main_server.cpp`.
- Parser fuzz targets, corpus seeds and local runtime limitations: [Fuzz findings](../fuzz/FINDINGS.md). The Linux fuzz and TSan CI jobs are configured but have not run from the current local branch.
- Persistence format and recovery limits: [Persistence](PERSISTENCE.md).

This review does not claim WebSocket security testing: the current HTTP transport reports WebSocket streaming unavailable and returns a schema-versioned error for the stream route.

The final local verification on 2026-11-25 passed CTest 177/177 in both Release and ASan/UBSan configurations, frontend typecheck, Vitest 10/10, the frontend production build, actionlint and repo-wide clang-tidy across 36 backend translation units. npm audit reported zero vulnerabilities; OSV reported no findings in the 236 packages from the frontend lockfile; the most recent Gitleaks history scan, run after `11df918`, found no leaks in 53 commits. TSan and libFuzzer were not run locally because this Windows configuration rejects them; the hosted CI workflow was not triggered. See [Security Testing](security/SECURITY_TESTING.md) and the [Security Audit](security/SECURITY_AUDIT.md) for scope and scanner limitations.

## Phase 2 system-wide inventory

The gateway and MCP interface are part of a larger local research application. The complete Phase 2 asset, entry-point and trust-boundary inventory is maintained in [the security audit](security/SECURITY_AUDIT.md). This section records the additional system boundaries relevant to future changes.

### Additional assets

- Order-book state, open order lifecycle, fill quantities and matching priority.
- Cash, positions, fees, reservations, realized/unrealized PnL and account-level exposure.
- Strategy configuration, backtest inputs/results and replay sequence/state.
- Instrument master, provider symbol mappings, market-data provenance and event-bus delivery.
- Build inputs, dependency versions, CI credentials/permissions and generated frontend assets.

### Additional untrusted inputs

- Historical CSV/JSONL lines, headers, numeric fields, timestamps, identifiers and provider labels.
- HTTP query parameters, path captures, duplicate or malformed headers, JSON members and numeric bounds.
- MCP JSON-RPC IDs, versions, method names, argument schemas, arrays and strings.
- Persisted event frames, checkpoints, catalog JSON, dataset files and file paths selected by local configuration.
- Build and package metadata fetched from CMake, npm and GitHub Actions sources.

### Broader trust boundaries

1. Market-data source or import file → CSV/JSONL parser → normalized event pipeline → strategy, risk and simulation consumers.
2. Order intent → risk checks and reservation state → matching/execution → ledger and audit state.
3. Persisted records → bounded recovery/validation → deterministic replay and application state.
4. CI dependency sources and workflow actions → build/test tools → binaries and bundled browser code.

The matching engine and ledger are not intended to be externally reachable as a financial venue. Their controls preserve simulation integrity and bounded research workloads; they do not make the application suitable for live capital or production exchange use. Local administrator privileges, a compromised user account and same-user malware are outside the protection boundary. Dependency pins and scanners improve reproducibility and detection, but cannot protect against malicious upstream source.
