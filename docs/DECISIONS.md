# Architecture Decision Records and Scoring

Scoring is 1–5. Product value, correctness, performance, learning value: 5 is high. Implementation complexity, operational complexity, maintenance burden, licensing cost and dependency risk: 1 is low. Scores are qualitative for this local research platform and should be revisited when evidence changes.

| Decision / option | Product | Correctness | Perf | Learning | Impl | Ops | Maint | License | Dep risk | Outcome |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| C++20 domain core | 5 | 5 | 5 | 5 | 3 | 1 | 2 | 1 | 1 | Must have |
| int64 fixed-point INR micro-units | 5 | 5 | 5 | 5 | 2 | 1 | 1 | 1 | 1 | Must have |
| std::map + list + iterator index | 4 | 5 | 4 | 5 | 2 | 1 | 2 | 1 | 1 | Preserve baseline |
| JSON catalog + CSV history + CRC .blog | 4 | 5 | 3 | 4 | 2 | 1 | 2 | 1 | 1 | Active local backend; SQLite deferred |
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

## ADR-005 — Durable event log plus file catalog

Append-only CRC-framed `.blog` records preserve event order for replay and recovery; checkpoints are derived state, not sole truth. The active dataset catalog is a deterministic JSON file and historical bars are canonical CSV files with content hashes. SQLite is not linked or used in the current build. The earlier SQLite WAL statement described a desired option, not an implemented backend; ADR-009 records the measured packaging constraint and conditions for reconsidering it.

## ADR-006 — Provider contract, not provider ranking

Adapters implement a common interface and are opt-in. Vendor fees/limits/licensing are volatile. A development adapter does not imply data entitlements. Offline synthetic/replay is the default. Upstox's documented v2 stream discontinuation invalidates the seed's v2 primary recommendation; use current official API version only after contract verification.

## ADR-007 — UI data integrity over visual completeness

The workstation renders real API state with explicit empty/stale/degraded status. It avoids invented metrics and does not synthesize strategy returns. This is preferred over a visually rich mock dashboard because the product is an observability and research tool.

## ADR-008 — Canonical durable kill-switch events redact free text

Trading and MCP adapters serialize kill-switch state through one schema-versioned encoder. Durable `reason` is always `[REDACTED]`; raw reasons remain available only to the in-process event result. Durable `actor` is retained only as a stable identifier made of 1–64 ASCII letters, digits, underscore, dash, dot or colon; all other values are redacted. Restore applies the same policy, including when reading legacy frames without a schema version. Append-only legacy frames are not rewritten, so deployments that have already persisted free-text reasons must handle those old files as sensitive data. EventLog retains an open append handle, flushes the stream and OS file buffers on each successful append, and requires `close()` before operator rotation; automatic rotation and multi-segment replay remain unsupported.

## ADR-009 — Defer SQLite and use deterministic file persistence

The active build does not use SQLite: the configured Windows/Clang environment did not provide the package, and no SQLite dependency is declared in CMake. The repository therefore uses deterministic JSON catalog metadata, canonical CSV history files with SHA-256 verification, and CRC-framed `.blog` journals for selected event and trading state. This keeps the current implementation offline, inspectable and testable without adding a second storage engine whose packaging, migration and backup behavior would also need support. It is a documented file-backed implementation, not a hidden in-memory substitute or a claim that the logical `DATA_MODEL.md` entities are all implemented.

Revisit SQLite when measured catalog query/update cost or transactional cross-entity requirements exceed the current single-file design, or when a supported, pinned SQLite distribution and migration/backup plan are available across Windows, Linux and macOS. Any adoption should sit behind the existing storage boundary, preserve deterministic `.blog` replay, include migration/recovery tests, and replace this ADR only after comparative evidence. Until then, update logical entity counts to match the actual persisted model rather than implying that SQLite is present.

## Rejected infrastructure triggers

Consider message brokers or external time-series databases only after measured local bottlenecks (for example, sustained data volumes exceeding configured local retention or multiple independent consumers with incompatible availability needs). No numerical trigger is treated as universal; capture workload and operational cost first.
