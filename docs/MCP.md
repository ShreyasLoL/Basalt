# BASALT MCP Server and Figma Workflow

## BASALT MCP boundary

The local Model Context Protocol server exposes JSON-RPC 2.0 over stdio by default. Optional loopback SSE may be enabled only when explicitly configured. stdio stdout is reserved for protocol frames; diagnostics go to redacted stderr. The server never exposes shell execution, arbitrary file access, environment dumps, token values or live broker order placement. UI, REST and MCP share validated application facades, not internal mutable objects.

### Nine read-only tools

1. get_system_health — build, process, storage and event-bus state.
2. get_market_watch — bounded instrument quotes with source and quality.
3. get_instrument_snapshot — instrument metadata, snapshot and freshness.
4. get_order_book — explicitly choose internal simulated book or external observed depth.
5. get_recent_trades — simulated executions with model/source context.
6. get_paper_positions — account positions and conservative marks.
7. get_portfolio_summary — cash, equity, P&L and fees.
8. get_strategy_status — config hash, lifecycle, signal and last error.
9. inspect_feed_health — provider/instrument state, gaps, age and counters.

### Five bounded simulation tools

1. run_backtest — bounded dataset/range/config; returns queued run ID and status.
2. run_replay — starts/steps a recorded local session at a capped speed; no source mutation.
3. run_test_suite — runs a named built-in suite from an allowlist; never accepts a command string.
4. run_benchmark — allowlisted workload/implementation and strict operation cap; rate limited to five calls/minute per client.
5. trigger_kill_switch — latches global/account/strategy simulation kill switch with required reason; cannot clear it.

Tool schemas are closed objects with required types, enum/range constraints and per-tool maximum duration. Benchmark/backtest/replay sizes are bounded. Mutations create an audit record with actor, method, request ID, outcome, timestamp and redacted parameter summary in logs/mcp_audit.jsonl. Rate-limit, schema and authorization failures are audit events. Audit file permissions are user-only where supported; rotation preserves bounded storage. Secrets and arbitrary free-form payloads are never logged.

## Protocol behavior

Initialize negotiates supported MCP protocol version and capabilities. tools/list returns the fourteen tools with JSON schemas and simulation annotations. tools/call validates arguments, enforces rate/size/time limits, calls the application facade, and returns structuredContent plus safe text. Errors use protocol error codes without stack traces or secret values. Duplicate request IDs are idempotent for state-changing calls. Server supports cancellation and bounded shutdown. Automated protocol tests cover discovery, happy-path reads, valid calls, malformed schemas, rate limits, audit entries, redaction and denial of shell/file/live execution.

## Figma MCP

Figma MCP is a design-time workflow, not a BASALT runtime integration. Use it to inspect an authorized file's components, variables and layout when the user supplies/authorizes one. Translate tokens to checked-in frontend design tokens. It does not bypass file permissions and does not itself provide browser pixel-diff regression; use the frontend visual verification workflow for that.
