# Architecture Decision Records and Scoring

Scoring is 1–5. Product value, correctness, performance, learning value: 5 is high. Implementation complexity, operational complexity, maintenance burden, licensing cost and dependency risk: 1 is low. Scores are qualitative for this local research platform and should be revisited when evidence changes.

| Decision / option | Product | Correctness | Perf | Learning | Impl | Ops | Maint | License | Dep risk | Outcome |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| C++20 domain core | 5 | 5 | 5 | 5 | 3 | 1 | 2 | 1 | 1 | Must have |
| int64 fixed-point INR micro-units | 5 | 5 | 5 | 5 | 2 | 1 | 1 | 1 | 1 | Must have |
| std::map + list + iterator index | 4 | 5 | 4 | 5 | 2 | 1 | 2 | 1 | 1 | Preserve baseline |
| SQLite WAL + CRC .blog | 5 | 5 | 4 | 5 | 3 | 1 | 2 | 1 | 2 | Must have; local |
| React/TS/Vite/Tailwind/Zustand | 5 | 4 | 4 | 4 | 3 | 1 | 2 | 1 | 2 | Workstation UI |
| Lightweight Charts | 4 | 4 | 4 | 3 | 2 | 1 | 2 | 1 | 2 | Charting after license check |
| SQLite only, no durable event log | 3 | 3 | 3 | 2 | 2 | 1 | 2 | 1 | 1 | Rejected: weaker deterministic recovery |
| Apache/Kafka/Redis infrastructure | 1 | 3 | 3 | 2 | 5 | 5 | 5 | 2 | 4 | Explicitly rejected for single host |
| Provider SDK per vendor in core | 2 | 2 | 2 | 2 | 4 | 3 | 4 | 2 | 4 | Rejected; isolate adapters |
| Synthetic + file replay default | 5 | 5 | 4 | 5 | 2 | 1 | 1 | 1 | 1 | Must have for offline CI |
| Live broker order execution | 0 | 0 | 0 | 0 | — | — | — | — | — | Explicitly out of scope |
| Fixed-capacity arena | 3 | 4 | 5 | 5 | 4 | 1 | 3 | 1 | 1 | Phase 9 optional measured optimization |
| Python MCP sidecar | 4 | 4 | 3 | 4 | 2 | 2 | 2 | 1 | 2 | Phase 10 if runtime available |
| Separate microservices | 1 | 2 | 2 | 2 | 5 | 5 | 5 | 2 | 4 | Rejected absent demonstrated need |

## ADR-001 — Preserve core and evolve through facades

The existing map/list CLOB is a verified and tested educational engine. New instrument, strategy, risk, persistence and API modules wrap it through explicit domain contracts. This reduces regression risk, preserves deterministic matching and provides a rollback path. A replacement is considered only after parity tests and measured benefit.

## ADR-002 — Local modular monolith

All domain modules run in one local application process. Bounded queues separate producer/consumer lifecycles without introducing networked infrastructure. A separate Python MCP process is optional and communicates through narrow local facades. This fits offline demos and limits operational burden.

## ADR-003 — Fixed-point authoritative finance

Money uses 1/10,000 INR integer units, with checked wide intermediates and explicit rounding. Human-readable decimals are presentation only. Benchmark duration and descriptive chart ratios may use floating point after financial values have been finalized.

## ADR-004 — Source-labelled observations and simulation-only fills

External quotes never populate the internal CLOB. Named paper execution models define when a quote/bar/depth observation produces simulated fills. This protects against conflating broker market state with BASALT liquidity and makes results replayable.

## ADR-005 — Durable log plus catalog

SQLite WAL provides queryable local catalog and transactions; append-only CRC-framed .blog records event order for replay/recovery. Checkpoints are cacheable derived state, not sole truth. If SQLite packaging is unavailable, deterministic transactional file storage is an explicit platform-specific substitute, never a silent downgrade.

## ADR-006 — Provider contract, not provider ranking

Adapters implement a common interface and are opt-in. Vendor fees/limits/licensing are volatile. A development adapter does not imply data entitlements. Offline synthetic/replay is the default. Upstox's documented v2 stream discontinuation invalidates the seed's v2 primary recommendation; use current official API version only after contract verification.

## ADR-007 — UI data integrity over visual completeness

The workstation renders real API state with explicit empty/stale/degraded status. It avoids invented metrics and does not synthesize strategy returns. This is preferred over a visually rich mock dashboard because the product is an observability and research tool.

## Rejected infrastructure triggers

Consider message brokers or external time-series databases only after measured local bottlenecks (for example, sustained data volumes exceeding configured local retention or multiple independent consumers with incompatible availability needs). No numerical trigger is treated as universal; capture workload and operational cost first.
