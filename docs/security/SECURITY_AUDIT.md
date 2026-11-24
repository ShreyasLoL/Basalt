# BASALT Phase 2 Security Audit

**Baseline date:** 2026-11-25
**Repository:** `C:\Users\Shreyas\Documents\Projects\Basalt`
**Baseline revision:** `e1086d2` (`main`)
**Scope:** source, build declarations, C++ and frontend dependencies, HTTP/MCP boundaries, persistence, market-data import, financial-state invariants, browser delivery, CI, and the working-tree state present at audit start.

This is a source-based assessment, not a security certification. Findings are limited to evidence observed in this checkout. Items marked potential or unresolved are not presented as confirmed exploitable vulnerabilities.

## Baseline and repository state

- `git status --short` at audit start showed a user modification to `.gitignore` adding `docs/private/` and untracked `docs/.mcp.json`. Both were preserved and excluded from this audit's commits. The MCP config's purpose is local server configuration; inspection found no literal secret-like values and one environment-variable reference. Values were not printed.
- C++ dependencies declared with CMake FetchContent: GoogleTest `v1.14.0`, cpp-httplib `v0.15.3`, and nlohmann/json `v3.11.3`. All are release tags, not immutable commit pins.
- Frontend direct dependencies are listed in `frontend/package.json`; the checked-in npm lockfile resolves the installed tree. At audit start `npm audit` and `npm audit --omit=dev` reported zero advisories when run with registry access. The sandboxed attempt could not resolve `registry.npmjs.org`; the successful audit was run with network access.
- Stage B candidate pins (fetched in a fresh build): GoogleTest `v1.14.0` at `f8d7d77c06936315286eb55f8de22cd23c188571`; cpp-httplib `v0.58.0` at `4f3f9ef19be83ae97a5d9a059432dc00e445b7ab`; nlohmann/json `v3.11.3` at `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03`. All three official tag refs resolved to these commit IDs, and a fresh CMake configure fetched those exact commits. Frontend lockfile updates were not justified by the zero-advisory audit.
- Baseline checks: `cmake --build build-release --parallel` passed (Ninja reported no work); CTest passed **164/164** after enabling loopback access. In the restricted sandbox, the same suite passed 154/164; all 10 failures were HTTP integration cases unable to connect to their loopback fixtures. Frontend typecheck passed, Vitest passed **10/10 across 3 files**, and the production build passed with 42 modules transformed.
- The existing repository already has useful security controls: a 64 KiB HTTP body cap, exact Host/Origin policy, no wildcard CORS, JSON content-type checks on mutation methods, bounded benchmark order count, bounded MCP frames/tool inputs/audit files, strict market-data line and event counts, CRC-checked bounded event-log frames, checked financial arithmetic, regression/property tests, ASan/UBSan and fuzz harness configuration. The fuzz campaigns and TSan are configured in Linux CI but have not been executed in this Windows session.

## Assets

| Asset | Security goal |
|---|---|
| Order-book and simulation order state | Preserve lifecycle, matching priority and quantity conservation. |
| Cash, positions, reservations, fills and realized PnL | Prevent overflow, duplicate application and ledger divergence. |
| Risk configuration and kill-switch state | Reject unsafe orders and preserve/report emergency-stop state. |
| Strategy and backtest configuration/results | Keep workloads bounded and distinguish simulated output from live results. |
| Market-data history, instrument catalog and normalized events | Reject malformed or ambiguous data and preserve provenance. |
| Event logs, checkpoints, dataset files and MCP audit records | Bound parsing/writes, detect accidental corruption and constrain paths. |
| Local HTTP server, UI, MCP stdio interface | Prevent hostile input from bypassing validation or exhausting the process. |
| Provider credentials and local configuration | Keep secrets out of source, bundles, responses, logs and audit records. |

## Entry points and untrusted input

| Entry point | Input | Main controls observed |
|---|---|---|
| HTTP gateway | Request line, method, URI, Host/Origin/Content-Type and body | Host/Origin/content-type guard, 64 KiB body cap, route-level JSON checks, request/benchmark limits. |
| Browser UI | Backend JSON, instrument labels and error text | React text rendering; no `dangerouslySetInnerHTML` or direct `innerHTML` use found. |
| MCP stdio | JSON-RPC lines and tool arguments from the configured caller | 256 KiB frame cap, JSON-RPC checks, closed tool schemas, bounded per-tool inputs/rates, no shell or arbitrary-file tool. |
| CSV / JSONL replay | User-selected stream contents, rows and fields | 1 MiB line cap, one-million-event cap, typed parsing and market-data validation. |
| Market-data provider interface | Raw quotes/bars/trades, timestamps, sequence and symbol mapping | Instrument mapping, fixed-point normalization, quality/sequence checks; real broker adapters are not connected. |
| Persistence recovery | Local `.blog`, checkpoint, JSON and CSV files | Bounded event frame length, sequence and CRC validation, prefix recovery, dataset digest checks. |
| Environment/configuration | Bind address, port, queue sizes, CORS allowlist, data directory and provider settings | Typed parsing/range checks; non-loopback binds are rejected and remote-bind opt-in fails closed. |
| Build and package sources | FetchContent tags, npm lockfile and GitHub Actions references | Release pins and lockfile exist; C++ pins/actions are not immutable and automated supply-chain scanning is incomplete. |

## HTTP route inventory

The route list below is from `src/server/http_server.cpp`. All routes pass the Host/Origin guard; POST/PUT/PATCH/DELETE require JSON, and the HTTP body cap is enforced by the transport.

| Route | Method | Access | Input and work | Risk | Protection observed after request-boundary hardening |
|---|---|---|---|---|---|
| `/` and `/assets/*` | GET/HEAD (static handler) | Read | Built UI and local bundled files | Browser content and static-file parsing | Loopback Host/Origin policy; baseline response headers; strict CSP for the built Vite bundle. The legacy inline-script fallback does not receive CSP and remains a tracked limitation. |
| `/api/v1/health` | GET | Read | No body | Minimal process status disclosure | Loopback Host/Origin; fixed response schema. |
| `/api/v1/market/watch` | GET | Read | No body | Returns bounded instrument list | Loopback Host/Origin; in-memory platform list. |
| `/api/v1/instruments/{id}` | GET | Read | Numeric instrument path | Lookup and response serialization | Loopback Host/Origin; ID parsed as an integer. |
| `/api/v1/instruments/{id}/snapshot?depth=` | GET | Read | Instrument ID and depth 1–200 | Bounded snapshot work | Loopback Host/Origin; bounded depth and fixed read model. |
| `/api/v1/portfolio/{account_id}` | GET | Read | Account path | Local paper-account disclosure | Loopback Host/Origin; fixed account read model. |
| `/api/v1/risk/{account_id}` | GET | Read | Account path | Local risk-state disclosure | Loopback Host/Origin; fixed account read model. |
| `/api/v1/trades?limit=` | GET | Read | Limit 1–200 | Historical result allocation | Loopback Host/Origin; bounded result count. |
| `/api/v1/system/metrics` | GET | Read | No body | Operational-state disclosure | Loopback Host/Origin; fixed bounded counters. |
| `/api/v1/stream` | GET | Read/unavailable | No body | None while unavailable | Returns schema-versioned 501; no WebSocket upgrade is implemented. |
| `/api/state?depth=` | GET | Read | Depth parsed and capped by legacy handler | Book snapshot disclosure and allocation | Loopback Host/Origin; bounded depth/read models. |
| `/api/orders` | POST | Write | JSON order ID, side, type, price and quantity | Simulation state and risk engine | Loopback Host/Origin; strict object schema; integer price/quantity bounds; unknown keys and deep JSON rejected before state changes. |
| `/api/orders/{id}` | DELETE | Write | Numeric order ID path; no body | Cancel simulation order | Loopback Host/Origin; JSON content type; numeric ID parsing; non-empty bodies rejected; matching lifecycle checks. |
| `/api/sim/step` | POST | Write/expensive | Optional JSON integer count, default 1 | CPU and simulated order volume | Loopback Host/Origin; strict object schema; count 1–500 or 400; unknown keys and deep JSON rejected. |
| `/api/reset` | POST | Write/destructive | Optional JSON `seed_demo` boolean | Clears simulation state | Loopback Host/Origin; strict object schema; boolean only; unknown keys rejected. |
| `/api/benchmark` | POST | Expensive benchmark | Optional JSON profile, order count and baseline comparison | CPU consumption | Loopback Host/Origin; strict object schema; order count 1–10,000; supported profile validation before admission; at most 5 requests per rolling minute; 8 workers and 32 queued requests. |
| `/api/v1/*` fallback | GET/POST/PUT/PATCH/DELETE | Unavailable | Route path and bounded body | Method confusion or unintended API surface | Explicit schema-versioned 501 fallback. Other unsupported methods are not registered. |
| `/api/*` preflight | OPTIONS | Read/policy | Origin and requested method/headers | Browser cross-origin boundary | Host/Origin guard; exact configured allowlist; no wildcard CORS. |

The route inventory describes current code; it does not imply every v1 route is implemented. Implemented legacy mutation routes accept only JSON objects with explicit per-route fields, at most 16 levels of JSON nesting, and correctly typed bounded scalar values. Malformed JSON returns fixed client-safe messages. Unexpected runtime exception detail remains a separate follow-up finding.

## Trust boundaries and threat model

1. Browser or local HTTP client → loopback HTTP gateway.
2. HTTP/MCP request handler → C++ simulation, risk, accounting and benchmark code.
3. MCP client → child process over stdin/stdout; possession of the pipe grants the tool surface, with no separate authentication.
4. Provider/replay file → parser → instrument normalization → event bus → strategy and portfolio consumers.
5. Persistence file → bounded record parser/recovery → replay/accounting state.
6. Frontend state → backend API results; the UI must not invent success when the backend reports rejection or unavailability.
7. CMake/npm/CI dependency sources → compiled server, tests and frontend assets.

In-scope actors include a hostile web page attempting DNS rebinding or cross-origin access, a local process sending malformed HTTP/MCP input, a malicious replay file, corrupted local records, and accidental resource exhaustion. The user's OS account is trusted to choose local paths and start processes. The application cannot protect its files or in-memory state from a fully compromised host, an administrator with equivalent privileges, physical access to an unlocked session, or malicious code already running as the same user. Loopback and CORS do not authenticate local clients. BASALT has no live brokerage order placement; provider integrations in this build are synthetic/replay or diagnostic-only.

The existing [threat model](../THREAT_MODEL.md) records additional STRIDE scenarios and controls. Its baseline assumptions remain: no remote deployment, no live execution and no cryptographic tamper-evident audit chain.

## Findings

Severity uses Critical / High / Medium / Low / Informational. A finding is confirmed only when the source/configuration demonstrates the condition; exploitability notes are specific to BASALT's current local-only intent.

### SEC-DEP-001 — cpp-httplib is pinned to a vulnerable historical release

**Finding ID:** SEC-DEP-001
**Title:** HTTP parser dependency v0.15.3 predates multiple upstream security fixes
**Severity:** High
**Affected component:** HTTP transport / dependency supply chain
**Affected file:** `CMakeLists.txt` (FetchContent declaration), compiled header `build/_deps/httplib-src/httplib.h`
**Evidence:** CMake pins `v0.15.3`; the compiled header reports `CPPHTTPLIB_VERSION "0.15.3"`. The official upstream advisories mark releases at or below v0.22.0 affected by trailer/header smuggling (patched v0.23.0), at or below v0.38.0 affected by request smuggling from unconsumed GET bodies (patched v0.40.0), at or below v0.43.3 affected by negative chunk-size handling (patched v0.43.4), and at or below v0.43.4 affected by percent-decoded request header values/CRLF injection (patched v0.44.0). The server serves mounted static assets and accepts untrusted HTTP/1.1 framing. The release is therefore within those affected ranges.
**Attack scenario:** A local HTTP client sends malformed chunked/framed requests or crafted header values into the bundled parser; a hostile local caller may trigger request desynchronization, header confusion or denial of service. Current local-only binding limits remote reachability but does not remove local parser exposure.
**Potential impact:** Server state confusion, unsafe header interpretation, or process availability loss.
**Recommended remediation:** Upgrade to a stable upstream release beyond all applicable fixes, pin its immutable commit, and add raw-protocol regression coverage.
**Regression tests:** HTTP integration tests for malformed framing, chunked bodies, encoded header bytes, duplicate Host handling and connection shutdown.
**Status:** Fixed and verified in `0a19e47`. The pinned v0.58.0 is beyond the applicable patched versions; a fresh Release build passed and CTest passed 165/165, including the live percent-encoded Origin regression. Additional raw-protocol tests confirm this parser accepts duplicate `Host` fields, so BASALT now rejects repeated Host, Origin, Content-Type, Content-Length and Transfer-Encoding fields before routing. Advisory scope was checked against the upstream [security advisories](https://github.com/yhirose/cpp-httplib/security) and [release history](https://github.com/yhirose/cpp-httplib/releases/). Specific conditional advisories: ContentReader decompression affects only streaming handlers not used by BASALT; trusted-proxy `X-Forwarded-For` handling is not configured; TLS/client advisories do not match the current server-only, non-TLS build. The upstream v0.58.0 release history was also checked for request smuggling, malformed chunk framing, regex path stack exhaustion, multipart parsing and recent protocol fixes.
**Associated commit:** `0a19e47`.

### SEC-NET-001 — Environment configuration can enable unauthenticated remote binding

**Finding ID:** SEC-NET-001
**Title:** Remote HTTP binding can be enabled without an authentication architecture
**Severity:** High
**Affected component:** Network exposure and HTTP API
**Affected file:** `include/basalt/common/config.hpp`, `src/server/http_server.cpp`, `src/server/request_guard.cpp`
**Evidence:** `BASALT_ALLOW_REMOTE_BIND=true` permits non-loopback `BASALT_BIND_ADDRESS`; the request guard then adds that remote IP authority. The HTTP API has no user authentication.
**Attack scenario:** A user or deployment environment sets the opt-in, intentionally or accidentally, and exposes the unauthenticated simulation API on a network interface.
**Potential impact:** Remote mutation of simulated state, unauthenticated inspection, and remote resource exhaustion. No live trading endpoint exists.
**Recommended remediation:** Remove the remote-binding path and fail clearly on non-loopback bind configuration.
**Regression tests:** Configuration tests for all environment combinations; startup and live-bind tests prove only IPv4/IPv6 loopback can be bound.
**Status:** Fix implemented and verified. `BASALT_ALLOW_REMOTE_BIND=true` now fails explicitly, all other non-loopback bind addresses are rejected by configuration validation, the request guard no longer accepts caller-supplied remote addresses, and directly constructed `HttpServer` configs are validated before listening. CTest passed 166/166 including configuration and direct-construction regression coverage.
**Associated commit:** `91c9804`.

### SEC-HTTP-001 — HTTP work queue and expensive endpoint lack application-level admission control

**Finding ID:** SEC-HTTP-001
**Title:** The server uses an unbounded accepted-work queue and benchmark rate is not limited
**Severity:** Medium
**Affected component:** HTTP availability
**Affected file:** `src/server/http_server.cpp`; cpp-httplib default queue construction in `httplib.h`
**Evidence:** BASALT sets a 64 KiB body cap but does not replace cpp-httplib's default `ThreadPool`, constructed with an omitted queue cap (`max_queued_requests = 0`, meaning unlimited). `/api/benchmark` caps a single run at 10,000 orders but has no admission/rate control.
**Attack scenario:** A local client submits requests faster than the worker pool can finish, including repeated synchronous benchmark runs.
**Potential impact:** Queue growth, excess memory use and CPU starvation of the local UI/simulation.
**Recommended remediation:** Bound worker and queued-request counts; add a small explicit cooldown/rate budget to the expensive benchmark route and explicit read/write/keepalive timeouts.
**Regression tests:** Queue saturation, rate-limit response, timeout and normal benchmark compatibility tests.
**Status:** Fix implemented and verified. The server now uses a fixed 8-worker pool with at most 32 queued jobs, an explicit 64 KiB body cap, 4 KiB URI/header line caps, 64-header cap, 5-second read/write timeouts, a 2-second keep-alive timeout, at most five requests per keep-alive connection, and a five-per-minute benchmark admission limit after input validation. The existing 10,000-order cap remains. BASALT also rejects repeated Host, Origin, Content-Type, Content-Length or Transfer-Encoding headers before routing. Release build passed; full CTest passed 169/169 with loopback, including queue saturation, oversized URI/header, ambiguous-host/framing and benchmark rate-limit regressions.
**Associated commit:** `5217dee`.

### SEC-HTTP-002 — Mutation JSON schemas are permissive and malformed media types are accepted

**Finding ID:** SEC-HTTP-002
**Title:** Legacy mutation handlers default/coerce request values instead of validating explicit schemas
**Severity:** Medium
**Affected component:** HTTP input validation and simulation state
**Affected file:** `src/server/http_server.cpp`, `src/server/request_guard.cpp`
**Evidence:** Before remediation, mutation handlers used `json.value(...)`, which accepted unknown keys, defaulted missing values and could coerce JSON scalars; `/api/sim/step` silently clamped out-of-range values. The content-type check compared only the media type before `;` and accepted malformed or unsupported parameters.
**Attack scenario:** A malformed, ambiguous or deeply nested local request reaches mutation logic with a defaulted/coerced value, or causes unnecessary parser work.
**Potential impact:** Unexpected simulation actions, inconsistent client/server interpretation, or process resource pressure.
**Recommended remediation:** Enforce top-level object schemas, required/optional keys, exact JSON scalar types and numeric ranges; bound nesting; reject unknown fields and malformed content-type parameters before mutation.
**Regression tests:** Invalid JSON, arrays, unknown/missing fields, wrong types, nulls, numeric boundaries, deep nesting, ignored DELETE bodies, and valid legacy frontend payloads.
**Status:** Fix implemented and verified. Mutation requests now use explicit schemas for order, step, reset and benchmark; JSON nesting is capped at 16 before parsing; unknown fields, invalid scalar types and out-of-range step counts are rejected with stable 400 responses. Order quantity and price are bounded by matching-engine limits. DELETE bodies are rejected. Only application/json with an optional single UTF-8 charset parameter is accepted. Focused HTTP/input tests passed 12/12; full CTest passed 173/173 with loopback.
**Associated commit:** Pending.

### SEC-HTTP-003 — Benchmark exceptions can expose internal exception text

**Finding ID:** SEC-HTTP-003
**Title:** Unexpected benchmark exceptions are reflected to the HTTP client
**Severity:** Medium
**Affected component:** HTTP error handling
**Affected file:** `src/server/http_server.cpp`
**Evidence:** The `/api/benchmark` `std::exception` handler currently serializes `error.what()` into a 500 response.
**Attack scenario:** An internal failure message includes implementation details or a local path and is returned to the requester.
**Potential impact:** Disclosure of internal state that can help a local caller target other weaknesses.
**Recommended remediation:** Return a stable generic error code/message, log a redacted diagnostic privately, and preserve the error signal.
**Regression tests:** Force an internal benchmark failure and assert the client receives no exception/path detail while the failure is logged.
**Status:** Open; remediation is a separate follow-up feature.
**Associated commit:** None.

### SEC-FE-001 — Browser security headers are not explicitly configured

**Finding ID:** SEC-FE-001
**Title:** HTTP responses do not apply a Content Security Policy or baseline browser hardening headers
**Severity:** Medium
**Affected component:** Browser security / frontend delivery
**Affected file:** `src/server/http_server.cpp`, `web/index.html`, `frontend/vite.config.ts`
**Evidence:** At baseline, no Content-Security-Policy, X-Content-Type-Options, Referrer-Policy, frame restriction or Permissions-Policy was configured. The compiled Vite UI has same-origin script/style assets and no external runtime dependency; `web/index.html` remains a separate legacy fallback with inline scripts and a Tailwind CDN dependency.
**Attack scenario:** If a future rendering bug or compromised asset introduces script injection, the browser has no CSP defense-in-depth; framing and MIME sniffing are also not explicitly restricted.
**Potential impact:** Increased impact of a separate XSS/content-injection defect. No current raw-HTML sink was found in the React source.
**Recommended remediation:** Apply a policy compatible with the built UI and Vite development/preview modes, then migrate or remove the inline-script legacy fallback before enforcing CSP on it. Do not send HSTS for the current HTTP-only loopback server.
**Regression tests:** HTTP response-header assertions for UI and API; frontend production build, typecheck and tests.
**Status:** Partially remediated by `8cac104`. BASALT applies `nosniff`, frame denial, a no-referrer policy and a restrictive Permissions-Policy to all C++ responses. The compiled Vite bundle receives `default-src 'self'` CSP from the C++ server, and Vite preview sends the same production policy. The Vite development server has a development-only CSP that permits the inline module/bootstrap code Vite injects and WebSocket HMR. HSTS is omitted because the application is HTTP-only. Residual: the checked-in legacy fallback contains inline application scripts and loads Tailwind from a CDN, so the C++ server intentionally omits CSP for that fallback to preserve its current behavior. It should be migrated or removed before this finding can close.
**Associated commit:** `8cac104`.


### SEC-CI-001 — Static and secret scanning are not integrated into CI

**Finding ID:** SEC-CI-001
**Title:** CI lacks an automated secret scanner and dedicated static/dependency security scans
**Severity:** Low
**Affected component:** Build and supply-chain security
**Affected file:** `.github/workflows/ci.yml`, dependency declarations
**Evidence:** The workflow runs C++/frontend regression, sanitizers, TSan and fuzz smoke jobs, but no CodeQL/clang-tidy/OSV/Gitleaks or npm audit step. `clang-tidy` is installed locally but is not configured in the workflow; Gitleaks and OSV Scanner are not installed on this host.
**Attack scenario:** A vulnerable dependency or accidentally committed credential may enter the repository without an automated warning.
**Potential impact:** Delayed detection of dependency or source-level problems and credential exposure.
**Recommended remediation:** Add a minimal read-only CI security job for dependency and secret scans and run a practical C++ static analysis configuration.
**Regression tests:** Verify workflows parse and the scanner executes on the current repository without requiring write permissions or secrets.
**Status:** Open at baseline.
**Associated commit:** None at baseline.

### SEC-SUP-001 — CMake dependency tags are mutable references

**Finding ID:** SEC-SUP-001
**Title:** C++ FetchContent dependencies use tags instead of immutable commit IDs
**Severity:** Low
**Affected component:** Reproducibility / supply chain
**Affected file:** `CMakeLists.txt`
**Evidence:** GoogleTest, cpp-httplib and nlohmann/json are fetched by named tags with shallow Git clones. A tag is less reproducible than a verified commit hash or content-addressed archive.
**Attack scenario:** An upstream tag is moved or a compromised upstream account changes the tagged object; a clean build resolves different source than the audited build.
**Potential impact:** Non-reproducible or compromised builds.
**Recommended remediation:** Pin exact upstream commit hashes (or immutable archive hashes) and verify the fetched source matches the recorded release.
**Regression tests:** Clean configure from empty FetchContent cache and compare reported versions/commit IDs.
**Status:** CMake dependencies now use the immutable commit IDs recorded above; fresh configure verified the resolved commits.
**Associated commit:** `0a19e47`.

## Existing protections verified

- HTTP policy is applied to UI/assets/API routes; loopback Host checks, exact Origin allowlist, no wildcard CORS and JSON media type requirements are covered by real HTTP tests.
- Frontend source has no direct unsafe HTML insertion. The production asset build succeeds.
- Market-data CSV/JSONL readers enforce a 1 MiB per-line and one-million event cap; parser and normalizer tests cover malformed rows, instrument mapping, sequence gaps, stale timestamps and quality state.
- MCP input is line-framed and bounded, tool argument schemas reject unknown fields, tools expose no shell or arbitrary path access, and kill-switch/audit writes have explicit caps and error reporting.
- EventLog validates magic/version/length/sequence/CRC before allocating payloads, retains valid prefixes and reports/quarantines corrupt tails. CRC is not cryptographic tamper protection.
- Financial core uses checked integer arithmetic, risk reservations/idempotency and ledger reconciliation; existing tests cover order lifecycle, partial fills, kill-switch and deterministic accounting. These tests support the specific invariants but do not prove all concurrency paths race-free.
- CI already contains ASan/UBSan, TSan and four bounded libFuzzer jobs. Local TSan and fuzz campaigns were not run on this Windows toolchain; no crash-free fuzz result is claimed.

## Baseline limitations

- HTTP has no user authentication. Origin/Host validation is browser-boundary defense and does not authenticate arbitrary local clients.
- Persistence uses checksums/digests for corruption detection, not signed or cryptographic tamper evidence for all records.
- Same-user and administrator processes can inspect or modify BASALT's local data. Localhost cannot defend a compromised host.
- No live brokerage orders are implemented. The system remains a research simulator, not production exchange or broker infrastructure.
- Current local environment could not run TSan/libFuzzer campaigns; configured CI execution is still required before claiming those dynamic analyses passed.
- npm dependency advisories were clean at baseline, but C++ dependency advisory coverage was not machine-reported by a scanner; the cpp-httplib findings above were verified against upstream advisory ranges.

## Change and verification log

This section is updated after every dedicated security commit. A finding is closed only after code, regression tests, build/test results and commit hash are recorded.

| Date | Commit | Feature / findings | Verification |
|---|---|---|---|
| 2026-11-25 | `fad0c43` | Phase 0 inventory and threat model; SEC-DEP-001, SEC-NET-001, SEC-HTTP-001, SEC-FE-001, SEC-CI-001, SEC-SUP-001 recorded open at baseline | Release build; CTest 164/164 with loopback; typecheck; Vitest 10/10; frontend build; npm audit 0 |
| 2026-11-25 | `0a19e47` | `security(deps): pin patched cpp-httplib release` — SEC-DEP-001 remediated; SEC-SUP-001 improved | Fresh CMake fetch by immutable commit; Release build 85/85; CTest 165/165; encoded-Origin HTTP regression passed; npm audit and `npm audit --omit=dev` each reported zero advisories |
| 2026-11-25 | `91c9804` | `security(network): disable remote HTTP binding` — SEC-NET-001 remediated | Release build; CTest 166/166 with loopback |
| 2026-11-25 | `5217dee` | `security(http): bound request admission and benchmark work` — SEC-HTTP-001 fix implemented; duplicate sensitive headers rejected | Release build; targeted HTTP tests 21/21; full CTest 169/169 with loopback |
| 2026-11-25 | `8cac104` | `security(frontend): add browser security headers` — SEC-FE-001 partially remediated; legacy fallback remains unresolved | C++ build; HTTP security-header test; typecheck; Vitest 10/10; production build; Vite preview returned CSP/security headers |
| 2026-11-25 | Pending | `security(http): validate mutation JSON schemas` — SEC-HTTP-002 fix implemented | C++ build; focused HTTP/input tests 12/12; full CTest 173/173 with loopback |
