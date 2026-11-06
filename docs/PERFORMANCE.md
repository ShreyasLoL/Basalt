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

One machine, one run, timer granularity and current compiler build only. Use fresh samples for comparisons; do not use this table as a production target. A temporary loopback server smoke succeeded after an elevated local run: root and GET /api/state returned 200; POST order, DELETE cancel, simulation step, reset/seed and a two-row benchmark response succeeded. The prompt's /api/seed and /api/simulate aliases returned 404; the repository's documented equivalents are /api/reset with seed_demo and /api/sim/step.

## Arena acceptance

ObjectPool<Order> has explicit capacity, freelist, generation/debug checks and deterministic exhaustion error. Pool ownership is per book/shard; nodes are returned on fill/cancel/reset. No iterator or handle may outlive its node. Benchmark latency and allocation count versus existing map/list and vector books, but retain standard-container implementation as correctness fallback. Do not call an arena faster unless measured parity runs support that claim.

## Phase 9 measured Release run

Command: `basalt_bench --orders 10000` (fixed seed 42; 10,000 operations per order-book profile, 10,000 canonical market-data events per bus profile). Warm-up uses a separate 10,000-command sequence per order-book workload and is excluded from timed samples. The three order-book implementations were compared on full trade records, cancel count and final per-instrument snapshots; all parity checks passed. The pooled arena reports successful node acquisitions, peak simultaneously active nodes, capacity and exhaustion count. Standard-container allocation counts are not instrumented.

Environment observed for this run: Windows 10.0.26300.0, 12th Gen Intel Core i5-12450HX, 12 logical processors, CMake 4.3.1, Ninja, Clang 22.1.8, Release `-O3 -DNDEBUG`, `std::chrono::steady_clock`. Percentiles are measured single-run sample results from this host, not service guarantees. The event-burst profile passed 10,000/10,000 events with queue high-water 10,000; the slow-consumer profile accepted 64, reported 9,936 backpressure drops and reached queue high-water 64. The event-bus timings include raw event construction, normalization/validation, and durable enqueue. Full T0–T10 collection is provided by the bounded `PipelineTelemetry` recorder; the integrating application supplies each stage timestamp.
```text
Environment: Windows, Clang 22.1.8 (https://github.com/llvm/llvm-project ca7933e47d3a3451d81e72ac174dcb5aa28b59d1), hardware threads=12, steady_clock, seed=42
BASALT benchmark | operations per profile: 10000
Workload                       Implementation                        Ops/s    Avg ns    p50 ns    p90 ns    p95 ns    p99 ns   p99.9 ns   Accepted   Dropped  Q high  Pool alloc  Pool peak   Pool cap Pool errors
------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
1. Balanced Market Flow        StandardOrderBook (map/list)        1814388       525       300       600       800      2000      41100          0         0       0           0          0          0           0
1. Balanced Market Flow        BaselineOrderBook (vector)          1160847       836       600      1300      1500      2300      46200          0         0       0           0          0          0           0
1. Balanced Market Flow        ArenaOrderBook (pooled nodes)       3181572       292       200       400       500      1300      13100          0         0       0        5426       1350      10000           0
2. Buy Heavy                   StandardOrderBook (map/list)        2594303       363       200       400       600      1700      26400          0         0       0           0          0          0           0
2. Buy Heavy                   BaselineOrderBook (vector)           462969      2131      1700      3600      4200      8500      76900          0         0       0           0          0          0           0
2. Buy Heavy                   ArenaOrderBook (pooled nodes)       2579380       363       200       400       600      1300      26000          0         0       0        7549       5445      10000           0
3. Sell Heavy                  StandardOrderBook (map/list)        2174338       408       200       500       600      1900      29400          0         0       0           0          0          0           0
3. Sell Heavy                  BaselineOrderBook (vector)           446249      2197      1500      3800      4500      8100      55400          0         0       0           0          0          0           0
3. Sell Heavy                  ArenaOrderBook (pooled nodes)       2444151       385       200       400       600      1900      28000          0         0       0        7576       5484      10000           0
4. Aggressive Matching         StandardOrderBook (map/list)        1882849       507       300       600       800      1900      33300          0         0       0           0          0          0           0
4. Aggressive Matching         BaselineOrderBook (vector)          1787662       534       300       600       800      2000      30100          0         0       0           0          0          0           0
4. Aggressive Matching         ArenaOrderBook (pooled nodes)       2035623       464       300       500       700      1900      28600          0         0       0        5503        524      10000           0
5. Cancel Heavy                StandardOrderBook (map/list)        2513131       372       300       500       600       900      19000          0         0       0           0          0          0           0
5. Cancel Heavy                BaselineOrderBook (vector)          1122347       866       600      1200      1400      2300      39300          0         0       0           0          0          0           0
5. Cancel Heavy                ArenaOrderBook (pooled nodes)       3410060       270       200       300       400       800      11200          0         0       0        5028        872      10000           0
6. Concentrated Levels         StandardOrderBook (map/list)        1820300       519       300       600       800      2500      40500          0         0       0           0          0          0           0
6. Concentrated Levels         BaselineOrderBook (vector)          1509388       637       400      1100      1200      1800      24200          0         0       0           0          0          0           0
6. Concentrated Levels         ArenaOrderBook (pooled nodes)       2212487       427       200       500       700      1800      28600          0         0       0        5444       1495      10000           0
7. Wide Distribution           StandardOrderBook (map/list)        1682963       568       400       700       900      2400      36400          0         0       0           0          0          0           0
7. Wide Distribution           BaselineOrderBook (vector)           515496      1912      1300      3200      3900      8500     119700          0         0       0           0          0          0           0
7. Wide Distribution           ArenaOrderBook (pooled nodes)       1722060       553       300       600       800      2100      54000          0         0       0        5644       1758      10000           0
8. Bursty Arrivals             StandardOrderBook (map/list)        1921303       495       300       600       800      1900      39100          0         0       0           0          0          0           0
8. Bursty Arrivals             BaselineOrderBook (vector)          2492895       376       200       400       500      1500      25500          0         0       0           0          0          0           0
8. Bursty Arrivals             ArenaOrderBook (pooled nodes)       2558919       362       200       500       600      1400      12300          0         0       0        5022         52      10000           0
9. High Active Order Count     StandardOrderBook (map/list)        2182929       433       200       400       500       700      47100          0         0       0           0          0          0           0
9. High Active Order Count     BaselineOrderBook (vector)           194002      5117      3800      8300     10000     27300     152600          0         0       0           0          0          0           0
9. High Active Order Count     ArenaOrderBook (pooled nodes)       2778241       333       200       300       400       600      22400          0         0       0       10000      10000      10000           0
10. Fifty Instrument Shards    StandardOrderBook (map/list)        2448700       377       200       400       500      1500      31800          0         0       0           0          0          0           0
10. Fifty Instrument Shards    BaselineOrderBook (vector)          2669015       350       200       300       500      1600      26600          0         0       0           0          0          0           0
10. Fifty Instrument Shards    ArenaOrderBook (pooled nodes)       2983828       309       200       300       400       700      21700          0         0       0       10000      10000      10000           0
11. Market Data Burst          BoundedMarketEventBus                440492      2212      1600      2100      2300      7000     117300      10000         0   10000           0          0          0           0
12. Slow Consumer Backpressure BoundedMarketEventBus                478126      1988      1600      2200      2300      4900     103900         64      9936      64           0          0          0           0
```
