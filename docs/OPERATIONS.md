# Operations and Twenty Recovery Runbooks

All runbooks preserve source events and ledger history. Operators use the local UI or typed CLI/API commands; no runbook invokes a broker order. Display a visible system state and record each recovery action.

| # | Failure / detection | Containment and UI state | Recovery and validation |
|---:|---|---|---|
| 1 | Provider disconnect / heartbeat timeout | Mark DISCONNECTED; block new strategy intents | Reconnect with bounded backoff; reconcile sequence and fresh snapshot |
| 2 | Feed stale | Mark STALE with age; show warning | Wait for valid fresh event; verify clock and provider status |
| 3 | Duplicate events | Count and quarantine duplicate | Reconnect/replay; verify no duplicate execution IDs |
| 4 | Out-of-order timestamps | Mark DEGRADED, preserve original time | Check clock skew and stable sequence; replay ordered source |
| 5 | Provider sequence gap | Emit gap event, pause affected strategy | Request snapshot/replay gap if available; resume only after reconciliation |
| 6 | Malformed/oversized payload | Reject frame, count decode failures | Verify schema/version and size bounds; keep valid prior state labeled stale |
| 7 | Instrument mapping missing/stale | Reject affected event/order | Refresh signed/versioned instrument master and verify symbol mapping |
| 8 | Tick size/decimal mismatch | Reject value before mutation | Correct mapping/parser; replay fixture and check exact tick alignment |
| 9 | Durable queue saturation | Mark DEGRADED; stop new strategies | Reduce input or drain persistence; verify gap-free durable sequence |
| 10 | UI queue/slow WS consumer | Coalesce old quotes; count replacements | Reconnect and refresh snapshot; verify non-coalesced executions recovered |
| 11 | SQLite busy/locked | Pause writes and mutations; expose storage warning | Retry bounded transaction; inspect WAL and resume after checkpoint |
| 12 | Database corruption | Enter read-only/fail-stop; preserve file | Restore backup and replay validated .blog prefix; compare state hash |
| 13 | .blog CRC/truncated tail | Stop at last valid frame; mark recovery state | Quarantine tail, keep original copy, resume only from verified checkpoint |
| 14 | Disk full | Stop authoritative mutation and event intake | Free/expand storage, flush, verify hashes and restart at last commit |
| 15 | Strategy exception | Isolate and mark FAILED | Inspect redacted reason, validate config, restart as a new run |
| 16 | Risk limit breach | Reject intent with rule ID | Inspect exposure/mark freshness; change limits only with audit record |
| 17 | Kill switch activation | Latch switch; cancel simulated open orders | Review reason and health; explicit authorized reset; confirm latch audit |
| 18 | Ledger reconciliation mismatch | Freeze account orders; mark portfolio degraded | Rebuild from immutable execution journal; compare balanced postings |
| 19 | Backtest/replay mismatch | Mark run invalid; preserve manifest and outputs | Compare dataset/config hashes and event sequence; rerun in clean state |
| 20 | API/MCP crash or invalid requests | Keep domain read-only if possible; bound/reject clients | Restart local adapter, verify health, tool schema, rate limit and redaction |

## Startup and shutdown

Startup validates config, schema migrations, file permissions, database/log consistency, instrument catalog and kill-switch latches before opening listeners. If storage or risk state cannot be trusted, start read-only or fail closed. Shutdown stops accepting mutation requests, pauses providers/strategies, finishes or rejects in-flight work deterministically, flushes log/database, writes a checkpoint, closes listeners and records final sequence/hash. A forced shutdown is detected on next startup and recovers from the last valid durable point.

## Backups and retention

Back up SQLite with a transactionally consistent snapshot plus event-log range and checksums. Retain immutable source data only under its licensing rules. Verify restoration periodically in an isolated directory. Never overwrite the only copy during repair. Retention/export reports include dataset license tag and hash.
