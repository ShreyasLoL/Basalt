# Phase 6 Design Notes

Each decision below is scoped to the Phase 6 hardening work and stays within one page.

## 1. Persistence scope

**Decision:** trim the logical persistence spec to match the active implementation; do not add SQLite. The current code has deterministic JSON dataset-catalog metadata, canonical CSV history files, CRC-framed event logs for selected journals, and checkpoint files. The twenty-table SQLite block in `DATA_MODEL.md` is not built or used. Replace it with a factual inventory of durable formats and identify the domain entities that remain in-memory or otherwise unpersisted. Keep the Phase 5 ADR explaining the missing SQLite package and add the concrete scope decision here. This avoids a new storage dependency and prevents the data model from promising absent durability.

## 2. Optional fee schedule

**Decision:** preserve the zero-fee `FeeSchedule{}` default and never auto-load an example. Add a small, bounded, strict JSON parser/file loader for the existing `FeeSchedule` value type, ship `config/fees.example.json` with visibly illustrative values and provenance, and let callers explicitly inject a successfully loaded schedule into `ExecutionConfig`. A backtest with no configured components adds the exact visible warning `fees: none (cost-free)` to its result; serialization and the demo print that warning. Validate format version, ranges, required fields and component kinds; test both rejection and successful loading of the checked-in example.

## 3. Multi-instrument portfolio backtest

**Decision:** add a portfolio runner that schedules instrument bars by event time and uses one account, one `RiskEngine`, one `PortfolioLedger`, one cash balance and shared exposure snapshots. Each instrument keeps its own strategy runtime, instrument metadata, execution config and per-instrument risk caps. Apply pending orders only at a later bar open, update portfolio marks at completed bar closes, and run pre-trade checks against portfolio-wide plus instrument limits. Return one reconciled combined ledger and per-instrument result counts. Add a two-instrument integration fixture and a runnable deterministic demo. Do not combine independent single-instrument ledgers after the fact, which would misstate available capital.

## 4. SPSC event ring

**Decision:** add a bounded generic SPSC ring with producer-owned head and consumer-owned tail counters, release/acquire publication, and separate 64-byte cache-line-aligned counters. Use it for the event bus durable lane, whose intended topology is one publisher and one consumer; retain the mutex/condition-variable wait and quote-coalescing behavior around the ring. Document the SPSC contract. Add wrap/full/empty tests and a TSan stress test that checks exact sequence delivery. Benchmark the ring against a mutex-protected queue with one producer and one consumer, reporting throughput and p50/p95/p99/p99.9 enqueue-to-dequeue latency; retain results even if the mutex queue wins. TSan execution remains subject to a supported Linux runner.

## 5. Ledger arithmetic

**Decision:** keep ledger accounting formulas and rounding unchanged. Add a reusable checked signed subtraction helper alongside the existing shared checked add/multiply functions and replace the ledger's private subtraction overflow predicate. Reuse the same helper for `0 - INT64_MIN` magnitude detection where it makes the error path clearer. Add boundary tests for both overflow directions and rerun all existing ledger reconciliation tests.
