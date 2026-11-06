# BASALT Performance and Benchmark Methodology

## Instrumentation model

Capture eleven monotonic timestamps without changing business state: T0 frame received, T1 decoded, T2 validated, T3 instrument-resolved, T4 normalized, T5 deduplicated/quality-checked, T6 event enqueued, T7 strategy/risk decision complete, T8 paper execution report, T9 ledger/store commit, T10 WebSocket publish. Derive stage and end-to-end latency from nanosecond counters. Record clock source/resolution and dropped span count. Instrumentation is bounded and can be sampled; never label timing as exchange latency unless provider timestamps support that interpretation.

For local book benchmarks, pre-generate all commands, reserve latency buffers, warm up, time only command execution, then compute p50/p90/p95/p99/p99.9 and throughput. Keep workload seed, compiler/version/flags, OS, CPU, thread configuration, operation count, book depth, implementation and result hash. Validate equal trades, cancels and final snapshots for vector/map/arena books. Report actual measurements and rerun on target hardware; numbers are not guarantees.

## Twelve deterministic workloads

1. Balanced buy/sell flow.
2. Buy-heavy accumulation.
3. Sell-heavy accumulation.
4. Aggressive matching / sweeps.
5. Cancellation-heavy.
6. Wide price distribution.
7. Concentrated levels / long FIFO queues.
8. Bursty arrivals with deterministic burst schedule.
9. High active-order count and deep cancellation.
10. Fifty-instrument shard dispatch.
11. Market-data burst with decode/normalize/fanout.
12. Slow-consumer backpressure and UI coalescing.

Run each with fixed seed and explicit count. Report warm-up separately from timed duration. Do not include random generation, logging, JSON, HTTP or result formatting in the CLOB hot-loop timing. Pipeline benchmarks report both total and per-stage distribution and queue high-water marks. Backpressure tests report replacement/drop/gap counts explicitly.

## Phase 0 baseline

Captured 2026-11-06 in the provided Windows x64 workspace using CMake 4.3.1, Ninja generator (invoked through CMake), Clang 22.1.8 targeting x86_64-pc-windows-msvc, Release, 50,000 commands/profile. Direct ninja command was not on PATH; CMake's generated Ninja build succeeded. Build completed all 22 target steps. CTest passed 18/18. Baseline timing output:

| Workload | Implementation | Ops/s | Avg ns | p50 | p95 | p99 | p99.9 |
|---|---|---:|---:|---:|---:|---:|---:|
| Balanced | Map/list | 5,203,563 | 177 | 200 | 300 | 500 | 1,500 |
| Balanced | Vector | 874,432 | 1,125 | 800 | 2,700 | 3,500 | 14,900 |
| Buy-heavy | Map/list | 5,009,368 | 183 | 100 | 300 | 500 | 7,700 |
| Buy-heavy | Vector | 161,713 | 6,160 | 4,000 | 19,600 | 29,000 | 59,600 |
| Sell-heavy | Map/list | 2,155,182 | 440 | 200 | 600 | 1,000 | 28,400 |
| Sell-heavy | Vector | 178,939 | 5,565 | 4,100 | 17,400 | 22,800 | 36,700 |
| Aggressive | Map/list | 4,413,686 | 210 | 200 | 400 | 600 | 1,700 |
| Aggressive | Vector | 2,182,224 | 441 | 300 | 1,100 | 1,300 | 1,600 |
| Cancel-heavy | Map/list | 5,188,929 | 176 | 200 | 300 | 400 | 2,200 |
| Cancel-heavy | Vector | 845,250 | 1,164 | 1,000 | 2,500 | 2,900 | 8,900 |
| Concentrated | Map/list | 5,326,629 | 172 | 100 | 400 | 500 | 2,100 |
| Concentrated | Vector | 1,221,493 | 800 | 600 | 2,000 | 2,400 | 3,900 |
| Wide | Map/list | 4,191,361 | 222 | 200 | 400 | 600 | 2,500 |
| Wide | Vector | 363,099 | 2,734 | 2,200 | 6,700 | 10,400 | 23,100 |

One machine, one run, timer granularity and current compiler build only. Use fresh samples for comparisons; do not use this table as a production target. Endpoint runtime smoke was blocked because loopback socket access is denied by the workspace sandbox; static route call paths are audited in docs/documents.md.

## Arena acceptance

ObjectPool<Order> has explicit capacity, freelist, generation/debug checks and deterministic exhaustion error. Pool ownership is per book/shard; nodes are returned on fill/cancel/reset. No iterator or handle may outlive its node. Benchmark latency and allocation count versus existing map/list and vector books, but retain standard-container implementation as correctness fallback. Do not call an arena faster unless measured parity runs support that claim.
