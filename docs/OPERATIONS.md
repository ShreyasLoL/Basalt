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
| 11 | Catalog or journal write failure | Pause mutations; preserve the last valid files and expose a storage warning | Check data-directory permissions and disk space; reopen only after JSON/CSV hashes and the valid `.blog` prefix have been verified |
| 12 | Catalog, history or checkpoint corruption | Mark affected datasets/state unavailable; preserve source files | Restore a verified backup or quarantine the damaged file; compare catalog/history hashes and replay the validated `.blog` prefix where applicable |
| 13 | .blog CRC/truncated tail | Stop at last valid frame; mark recovery state | Quarantine tail, keep original copy, resume only from verified checkpoint |
| 14 | Disk full | Stop authoritative mutation and event intake | Free/expand storage, flush, verify hashes and restart at last commit |
| 15 | Strategy exception | Isolate and mark FAILED | Inspect redacted reason, validate config, restart as a new run |
| 16 | Risk limit breach | Reject intent with rule ID | Inspect exposure/mark freshness; change limits only with audit record |
| 17 | Kill switch activation | Latch switch; cancel simulated open orders | Review reason and health; explicit authorized reset; confirm latch audit |
| 18 | Ledger reconciliation mismatch | Freeze account orders; mark portfolio degraded | Rebuild from immutable execution journal; compare balanced postings |
| 19 | Backtest/replay mismatch | Mark run invalid; preserve manifest and outputs | Compare dataset/config hashes and event sequence; rerun in clean state |
| 20 | API/MCP crash or invalid requests | Keep domain read-only if possible; bound/reject clients | Restart local adapter, verify health, tool schema, rate limit and redaction |

## Startup and shutdown

Startup validates configuration and the configured data-directory paths. Before allowing state-changing operations, verify active catalog JSON, canonical CSV history, `.blog` log consistency, checkpoints and kill-switch state with the corresponding application tools. If storage or risk state cannot be trusted, start read-only or fail closed. Shutdown stops accepting mutation requests, pauses providers/strategies, finishes or rejects in-flight work deterministically, flushes active files, writes a checkpoint when configured, closes listeners and records the final sequence/hash.

## Backups and retention

Back up the active JSON catalog, canonical CSV history, `.blog` journals, `BCP1` checkpoints and MCP audit JSONL as applicable; retain source data only under its licensing rules and store expected hashes with each backup. Verify restoration periodically in an isolated directory. Never overwrite the only copy during repair. SQLite is not an active store; see ADR-009 and ADR-010 before changing the persistence model.
