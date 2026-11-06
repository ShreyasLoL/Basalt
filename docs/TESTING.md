# BASALT Testing Strategy

## Testing pyramid and gates

1. **C++ unit tests:** fixed-point arithmetic, parsers, instruments, pipeline, indicators, strategies, risk, execution models, CLOB, ledger, storage, API serializers and MCP schema.
2. **Invariant/property tests:** book price/FIFO/volume/index invariants; accounting debits=credits; position equals signed fills; event sequence monotonic; replay hash parity; provider source/quality always present.
3. **Deterministic property/fuzz tests:** malformed and boundary payloads, integer overflow, duplicate/out-of-order ticks, cancel/modify/fill sequences, random seeds retained on failure.
4. **Component integration:** provider fixture through pipeline/store/strategy/risk/paper execution/ledger; REST/WS lifecycle; persisted restart; MCP tools/list and tools/call.
5. **End-to-end UI:** API-backed market watch, source/quality badges, order/risk errors, kill switch, replay gap recovery, keyboard and responsive modes. No mocked success for unavailable server state.
6. **Operational/chaos/soak:** log disk full, database busy/corrupt tail, stale feed, queue saturation, slow WS consumer, shutdown/restart, strategy crash, long replay and bounded memory.

## Existing compatibility suite

Keep tests/test_matching_engine.cpp and its eighteen named tests intact. Phase 0 baseline on 2026-11-06: strict Release build succeeded; CTest 18/18 passed. Each phase reruns all suites and does not mark complete until all applicable tests pass. Use CTest and report exact count/failures. Sanitizer targets are optional per compiler/platform and may not be marked passed if unavailable.

## Financial and temporal properties

- No authoritative money/price/qty path uses float/double.
- Price-to-tick conversion rejects precision loss and overflow.
- Market events preserve source, quality, event time, receive time and stable sequence through persistence and replay.
- Duplicate/reordered events cannot create duplicate executions.
- Bar t close intent cannot fill before t+1 open.
- Position flips split close/open legs correctly; fee postings balance and cash/position reconciliation holds.
- Replaying the same manifest produces the same hashes.
- FOK failure leaves book, sequences and trade IDs unchanged; Post-Only never crosses; STP policy is deterministic.
- Kill switch blocks new intents after activation and cannot be cleared by an untrusted tool.

## Test data and verification output

Fixtures are synthetic, labeled SYNTHETIC, fixed-seed and not represented as real market data. Tests contain no credentials. Record build type, compiler, dependency versions, target names, CTest output, benchmark parity and browser limitations in PROGRESS.md. Do not equate compilation with runtime verification.
