# Security, Credentials and Market Boundaries

## Local-first defaults

The HTTP API is loopback-only. It binds to `127.0.0.1` by default and also permits the explicit loopback address `::1`. Non-loopback bind addresses are rejected, and `BASALT_ALLOW_REMOTE_BIND=true` fails startup. BASALT does not provide an authenticated remote deployment profile. Enforce payload limits, input validation, timeouts, rate limits and safe error messages. For browser deployments, prefer same-origin serving; the explicit CORS allowlist defaults to empty rather than wildcard.

### HTTP gateway request policy (implemented)

`basalt_server` applies one pre-routing policy (`validate_request_origin` in `src/server/request_guard.cpp`) to every request, including `/`, `/assets/*` and `/api/*`, before static files are served or request bodies are read:

| Check | Rule | Failure |
|---|---|---|
| Host | Must be `127.0.0.1:<port>`, `localhost:<port>` or `[::1]:<port>` (case-insensitive; the port may be omitted only when the server port is 80). Blocks DNS rebinding, because an attacker hostname never matches. | 403 `FORBIDDEN_HOST` |
| Origin (when present) | Must be a loopback HTTP origin for the listening port or an exact `http(s)://host[:port]` entry in `BASALT_CORS_ALLOWED_ORIGINS`. Paths, wildcards, credentials, `null` and unlisted origins are rejected. | 403 `FORBIDDEN_ORIGIN` |
| Content-Type for POST/PUT/PATCH/DELETE | Media type must be `application/json` (parameters ignored). Because this is not a CORS "simple" type, browsers must preflight any cross-origin mutation, and the preflight is refused by the Origin rule. | 415 `UNSUPPORTED_MEDIA_TYPE` |

There is no `Access-Control-Allow-Origin: *`. An accepted Origin is echoed with `Vary: Origin`; `OPTIONS /api/*` returns 204 with `Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS`, `Access-Control-Allow-Headers: Content-Type, X-Request-ID, Idempotency-Key` and a 600 s max-age. The explicit list is configured with comma-separated `BASALT_CORS_ALLOWED_ORIGINS`; empty is the strict default. `POST /api/benchmark` accepts `order_count` 1–10,000 (default 10,000), matching the MCP `run_benchmark` cap; other values return 400. These rules are covered by `basalt_http_tests` (pure policy cases plus real loopback HTTP requests on an ephemeral port).

The C++ server applies `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer` and a restrictive `Permissions-Policy`. The built Vite UI receives a same-origin Content Security Policy. Vite preview uses the same production policy; Vite development uses a separate policy that permits its injected bootstrap code and WebSocket HMR. The checked-in legacy fallback still has inline scripts and a CDN script, so it does not receive CSP. Build the frontend bundle to get CSP protection. HSTS is omitted because the gateway uses HTTP only.

The bundled `cpp-httplib` transport does not expose a WebSocket upgrade hook. The health route reports `websocket_available: false`, and `/api/v1/stream` returns a schema-versioned 501. The frontend skips reconnect attempts when the server reports the stream as unavailable. No WebSocket handshake or streamed frame is claimed as implemented.

## Credentials

Read provider credentials only from named environment variables or OS-protected secret storage. Example variable names are BASALT_UPSTOX_TOKEN and BASALT_FYERS_TOKEN. Never commit .env, tokens or account identifiers. .env.example contains names and dummy placeholders only. Logger redaction is applied before formatting and persists redacted values only. Never expose environment dumps through REST, WS, UI, MCP, crash report or test output. Rotate/disable leaked credentials outside the application and document the event.

## Simulation-only guarantee

No live brokerage order placement is implemented. Market-data adapters have no trading API methods or dependencies. The API, frontend and MCP expose only paper/simulation commands. Tests assert no provider credential can switch execution mode. Any future live-execution proposal requires a separate architecture/security/legal review and is outside this roadmap.

## Data licensing and regulatory boundary

Provider login does not imply redistribution rights. Store source, license tag, retrieval time and permitted use with every dataset. Do not redistribute exchange/provider market data or expose it to third parties unless the account and exchange terms expressly allow it. Personal research and educational use are not assumed to authorize public hosting. Do not describe BASALT as a broker, investment adviser, or compliant live-trading venue. Indian market-data licensing, exchange terms, current SEBI retail-algo requirements, broker terms and tax treatment require current official confirmation before any external-data or deployment use; these rules change. Avoid giving trade recommendations. Keep the application strictly in simulation.

## Data protection and audit

Minimize account identifiers and sensitive logs. Restrict local database/log files to the user where supported. Default MCP audit (`mcp_audit.jsonl`) and kill-switch (`mcp_killswitch.blog`) paths never depend on `cwd`; `McpServer::resolve_data_dir()` resolves the storage directory from `BASALT_DATA_DIR`, else the running executable's directory, else `<os_temp_dir>/basalt-mcp` (with a `stderr` warning), creating the directory with owner-only permissions (`std::filesystem::perms::owner_all`). If a kill-switch activation cannot be persisted to `mcp_killswitch.blog`, `trigger_kill_switch` returns `isError: true` with `KILL_SWITCH_NOT_PERSISTED` while keeping the in-process latch active. Apply path allowlists to CSV imports, prevent traversal, cap file size and never execute imported content. MCP cannot browse arbitrary files or run shell commands. Audits record decisions and safe IDs, not raw secrets or full provider payloads by default. Backups inherit the same local permissions and retention policy.
