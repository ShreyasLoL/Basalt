# Security, Credentials and Market Boundaries

## Local-first defaults

Bind API and WebSocket listeners to 127.0.0.1. Do not bind 0.0.0.0 unless an explicit secure deployment profile with authentication and network controls is implemented. Enforce payload and subscription limits, input validation, timeouts, rate limits and safe error messages. For browser deployments, prefer same-origin serving; CORS allowlist defaults to the local UI origin rather than wildcard in the production workstation.

## Credentials

Read provider credentials only from named environment variables or OS-protected secret storage. Example variable names are BASALT_UPSTOX_TOKEN and BASALT_FYERS_TOKEN. Never commit .env, tokens or account identifiers. .env.example contains names and dummy placeholders only. Logger redaction is applied before formatting and persists redacted values only. Never expose environment dumps through REST, WS, UI, MCP, crash report or test output. Rotate/disable leaked credentials outside the application and document the event.

## Simulation-only guarantee

No live brokerage order placement is implemented. Market-data adapters have no trading API methods or dependencies. The API, frontend and MCP expose only paper/simulation commands. Tests assert no provider credential can switch execution mode. Any future live-execution proposal requires a separate architecture/security/legal review and is outside this roadmap.

## Data licensing and regulatory boundary

Provider login does not imply redistribution rights. Store source, license tag, retrieval time and permitted use with every dataset. Do not redistribute exchange/provider market data or expose it to third parties unless the account and exchange terms expressly allow it. Personal research and educational use are not assumed to authorize public hosting. Do not describe BASALT as a broker, investment adviser, or compliant live-trading venue. Indian market-data licensing, exchange terms, current SEBI retail-algo requirements, broker terms and tax treatment require current official confirmation before any external-data or deployment use; these rules change. Avoid giving trade recommendations. Keep the application strictly in simulation.

## Data protection and audit

Minimize account identifiers and sensitive logs. Restrict local database/log files to the user where supported. Apply path allowlists to CSV imports, prevent traversal, cap file size and never execute imported content. MCP cannot browse arbitrary files or run shell commands. Audits record decisions and safe IDs, not raw secrets or full provider payloads by default. Backups inherit the same local permissions and retention policy.
