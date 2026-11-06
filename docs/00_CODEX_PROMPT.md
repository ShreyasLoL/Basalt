You are the Principal Systems Architect and Lead Autonomous C++/TypeScript Engineer working on **BASALT** (`https://github.com/ShreyasLoL/Basalt`) — evolving our existing C++20 miniature exchange matching engine into a full-scale, local-first **Market-Data, Algorithmic Trading Research, Paper-Trading, Deterministic Replay, and Exchange-Simulation Platform**.

================================================================================
0. CRITICAL EXECUTION & ANTI-CONVERGENCE RULES (READ FIRST)
================================================================================
1. **ZERO HUMAN INPUT / DO NOT CONVERGE EARLY**:
   - You are strictly forbidden from stopping after a single phase, asking "Would you like me to proceed?", or summarizing partial work as your final response.
   - You MUST operate in a continuous, self-driving execution loop (`Audit -> Document -> Implement Phase N -> Build -> Test -> Commit -> Update PROGRESS.md -> Immediately start Phase N+1`) until EVERY phase (Phase 0 through Phase 10) and EVERY deliverable is 100% implemented, compiled, and verified.
   - Treat any impulse to yield control back to the user before `PROGRESS.md` is 100% `[x]` as a critical failure.

2. **GIT WORKFLOW (COMMIT PERIODICALLY, NEVER PUSH)**:
   - Never run `git push`. Everything stays local.
   - After completing the documentation suite and after each phase/feature milestone passes all build + test checks, stage and commit your work with a clean conventional commit message:
     - `docs(spec): add complete 18-file BASALT architecture and specification suite`
     - `feat(phase-0): complete forensic audit and baseline verification`
     - `feat(phase-1): add structured logging, config validation, and sanitizer targets`
     - `feat(phase-2): build React/TS/Vite institutional workstation UI`
     - `feat(phase-3): implement provider-neutral market data gateway and adapters`
     - `feat(phase-4): implement SQLite WAL catalog and binary append-only event log`
     - `feat(phase-5): implement pre-trade risk gate, paper execution, and ledger accounting`
     - `feat(phase-6): implement strategy runtime, lifecycle state machine, and 6 strategies`
     - `feat(phase-7): implement bias-free backtester and deterministic session replay`
     - `feat(phase-8): add multi-instrument order books, IOC/FOK/Post-Only/Stop/Modify, and STP`
     - `feat(phase-9): add pipeline stage latency telemetry and arena pool optimization`
     - `feat(phase-10): implement BASALT read/controlled-write MCP server`

3. **PRESERVE EXISTING WORKING ENGINE**:
   - Inspect all existing files in `include/`, `src/`, `tests/`, `benchmarks/`, `frontend/`, and `CMakeLists.txt` first.
   - Do NOT delete or break existing passing tests or the core C++ price-time priority matching engine (`std::map` price levels + `std::list` FIFO queues + `std::unordered_map` ID lookup). Evolve and wrap it cleanly.

4. **SIMULATION-FIRST SAFETY & NO FABRICATED DATA**:
   - All trading in BASALT is strictly simulated/paper trading. Do not implement live brokerage order placement.
   - Clearly label every market event with its `DataSourceType` (`LIVE_PROVIDER`, `DELAYED_PROVIDER`, `HISTORICAL`, `CACHED`, `SYNTHETIC`, `REPLAYED`, `DERIVED`) and `FeedQualityState` (`OK`, `STALE`, `DEGRADED`, `DISCONNECTED`).
   - Never use `float` or `double` for authoritative prices, quantities, cash, or P&L. Use fixed-point `int64_t` scaled by `10,000` (`1 INR = 10,000` micro-units; `0.01 INR = 100` micro-units).

5. **SEED FILES (READ BEFORE WRITING THE 18-FILE SUITE)**:
   - The folder `docs/seed/` contains pre-researched seed material: `AUDIT.seed.md`, `ARCHITECTURE.seed.md`, `DECISIONS.seed.md`, `MARKET_DATA.seed.md`, `DATA_MODEL.seed.md`, `PAPER_TRADING.seed.md`, `MCP.seed.md`.
   - Treat them as starting inputs, NOT as final truth. Verify the audit table against the real repository (file names, symbols, namespaces — e.g. `atlas` vs `basalt` — may differ from the seed). Where the repo disagrees with a seed, the repo wins; record the correction.
   - Provider pricing, rate limits and licensing claims in `MARKET_DATA.seed.md` are unverified. Mark each as "needs verification against official docs" in the final `MARKET_DATA.md` unless you can confirm it from an official source, and never hard-code those numbers into code.
   - Expand and merge the seeds into the 18 final files; do not just copy them.

================================================================================
1. STEP 1: FORENSIC REPOSITORY AUDIT & 18-FILE SPECIFICATION SUITE GENERATION
================================================================================
Before modifying any C++ or frontend source code, inspect every file currently in the repository (`CMakeLists.txt`, `include/**`, `src/**`, `tests/**`, `benchmarks/**`, `frontend/**`, `documents.md`, `README.md`).

Then, create/update `PROGRESS.md` in the root directory with checkboxes for every single file and phase below, and generate the complete, exhaustive, repository-grounded **18-file specification suite** (placing `documents.md`, `README.md`, and `PROGRESS.md` in the root, and the modular specs either in root or `docs/` with root links so both layouts work seamlessly):

1. `documents.md` — The Master 55-Section Architecture & Implementation Specification:
   - Sections 1–3: Executive Summary, Product Vision, Goals & Non-Goals.
   - Sections 4–5: Verified Repository Forensic Audit table (`Path | Responsibility | Language | Current status | Dependencies | Keep/refactor/replace`) tracing exact symbols & call sequences for server startup, HTTP request handling, order validation, matching, trade recording, snapshots, cancellation, reset/seed/simulation, benchmarks, and GoogleTest suite.
   - Sections 6–10: Target Modular Monolith Architecture, 10 Mermaid Diagrams (System Context, Container/Process View, Component View, Market-Data Flow, Order Lifecycle, Strategy Execution, Persistence & Replay, Frontend Communication, MCP Integration, Failure/Recovery Flow), Process & Threading Model.
   - Sections 11–17: Instrument Master, Market-Data Provider Research (Nov 2026 comparison of FYERS API v3, Upstox API v2, DhanHQ v2, Angel One SmartAPI, Zerodha Kite Connect), Provider Adapter Interface, Canonical Market-Data Model, 12-Stage Pipeline, Data Quality & Feed Health, Bounded SPSC/MPSC Queues & Backpressure Policy.
   - Sections 18–20: Historical Data, Hybrid Storage Architecture (SQLite WAL + Binary Append-Only Event Log `.blog` + CSV/Parquet export), Deterministic Replay.
   - Sections 21–24: Multi-Instrument Matching Engine Evolution, Order Management, Order Types (`LIMIT`, `MARKET`, `IOC`, `FOK`, `POST_ONLY`, `STOP_MARKET`, `STOP_LIMIT`) & `Cancel/Modify` + Self-Trade Prevention (`CANCEL_NEWEST`, `CANCEL_OLDEST`, `CANCEL_BOTH`), 4 Paper Execution Models (Conservative Quote-Based, Bar-Based Approximation, Order-Book Replay, Internal CLOB).
   - Sections 25–27: Strategy Runtime & 10-State Lifecycle (`CREATED`, `CONFIGURED`, `VALIDATED`, `INITIALISING`, `WARMING_UP`, `RUNNING`, `PAUSED`, `STOPPING`, `STOPPED`, `FAILED`), Strategy API, 6 Initial Strategies (SMA Crossover, RSI Mean Reversion, Bollinger Band Breakout, Intraday VWAP Reversion, Momentum Breakout, Order-Book Imbalance).
   - Sections 28–31: 16-Rule Pre-Trade Risk Engine & Kill Switch, Double-Entry Portfolio Accounting Ledger, Bias-Free Backtesting Engine, Mathematical Definitions of 18 Analytics Metrics (Total Return, Benchmark-Relative Return, CAGR, Volatility, Sharpe, Sortino, Max Drawdown, Calmar, Win Rate, Profit Factor, Avg Win/Loss, Expectancy, Turnover, Exposure, Trade Count, Fee Impact, Slippage Impact, Capacity).
   - Sections 32–36: Institutional React + TypeScript + Vite + Tailwind + Zustand + Lightweight Charts Workstation Architecture, Design System Tokens & Figma MCP Workflow, REST API v1 Contracts, WebSocket Streaming Protocol v1, BASALT MCP Server Specification.
   - Sections 37–55: Security, Indian Market-Data Licensing & SEBI Boundaries, Observability, 20 Failure Scenarios & Recovery Runbooks, Testing Pyramid, 12-Workload Benchmark Methodology, Performance Engineering, Build System, Local Dev, Deployment, Configuration, Repository Structure, Migration Plan, 10-Phase Roadmap, Acceptance Criteria, Open Questions, ADRs, Glossary, Official References.
2. `README.md` — Project overview, architecture summary, quickstart build/run/test/benchmark instructions.
3. `ARCHITECTURE.md` — Deep dive into the 20 logical modules, process boundaries, threading model, and all 10 Mermaid diagrams.
4. `ROADMAP.md` — Detailed breakdown of Phases 0 through 10 with scope, prerequisites, files affected, acceptance criteria, and rollback plans.
5. `DATA_MODEL.md` — Fixed-point `int64_t` arithmetic rules (`PRICE_SCALE = 10000`), SQLite DDL schemas for all 20 entities, and double-entry cash/position ledger invariants.
6. `API_SPEC.md` — Schema-versioned (`v1`) REST endpoints and WebSocket frame specifications (`tick`, `quote`, `ohlc`, `depth`, `execution`, `order`, `position`, `signal`, `feed_status`, `system_metrics`).
7. `MARKET_DATA.md` — Provider comparison matrix, `IMarketDataProvider` C++ interface, 12-stage normalization pipeline, clock-skew/deduplication/stale-feed handling.
8. `STRATEGIES.md` — `IStrategy` interface, lifecycle state machine, and exact mathematical formulas/entry/exit/sizing/test specs for the 6 initial strategies.
9. `BACKTESTING.md` — Anti-look-ahead bias enforcement, bar vs. tick execution semantics, train/validation/test split support, and reproducibility metadata manifest.
10. `PAPER_TRADING.md` — Separation between external exchange feeds and internal simulated liquidity, plus the 4 paper execution models and Indian equity fee/slippage schedules.
11. `RISK.md` — Pre-trade risk gate pipeline (`Strategy -> OrderIntent -> RiskEngine -> ExecutionAdapter -> PortfolioLedger`), 16 configurable checks, rejection codes, and emergency Kill Switch semantics.
12. `FRONTEND.md` — Multi-workspace terminal layout (`Overview`, `Market Watch`, `Instrument Workstation`, `Order Book`, `Strategy Lab`, `Paper Trading`, `Portfolio & Risk`, `Backtesting`, `Market Replay`, `Analytics`, `Data Sources`, `System Monitor`, `Settings`), component state matrix, and high-frequency WebSocket throttling outside React render loops.
13. `MCP.md` — Figma MCP integration workflow and BASALT's own JSON-RPC 2.0 stdio/SSE MCP server (`mcp/`) separating read-only inspection tools from rate-limited, audited state-changing simulation tools.
14. `PERFORMANCE.md` — 11-stage timestamped latency instrumentation (`T0` decode to `T10` WS broadcast), 12 synthetic workload profiles, warm-up/seed discipline, and hardware/compiler reporting.
15. `SECURITY.md` — Localhost-only default binding (`127.0.0.1`), `.env` credential isolation, log redaction, CORS, and zero live-order execution guarantee.
16. `TESTING.md` — Complete testing pyramid covering C++ unit tests, invariant checkers, market-data edge cases, strategy determinism, risk boundary tests, portfolio accounting tests, integration flows, and chaos/soak tests.
17. `OPERATIONS.md` — Detection, containment, UI state, and recovery procedures for all 20 required failure scenarios.
18. `DECISIONS.md` — Architecture Decision Records (ADRs) with 1–5 scoring matrices across Product Value, Correctness, Performance, Learning Value, Implementation Complexity, Operational Complexity, Maintenance, Licensing Cost, and Dependency Risk.

Commit all documentation files (`git commit -m "docs(spec): generate complete BASALT master architecture and 18-file specification suite"`) before starting Phase 0 code execution.

================================================================================
2. STEP 2: PHASED IMPLEMENTATION ROADMAP (EXECUTE PHASES 0 TO 10 CONTINUOUSLY)
================================================================================
After committing the documentation suite, execute every phase below in order. Do NOT skip phases, do NOT leave `TODO` stubs, and do NOT stop between phases:

### PHASE 0 — Forensic Audit & Baseline Verification
- Run existing build, unit tests, and benchmark binary. Record exact baseline numbers in `PROGRESS.md` and `PERFORMANCE.md`.
- Verify all existing invariants and endpoints (`/api/state`, `/api/orders`, `/api/reset`, `/api/seed`, `/api/simulate`).
- Commit: `feat(phase-0): record verified repository audit and benchmark baseline`

### PHASE 1 — Foundation, Configuration, Logging & Code Quality
- Add `include/basalt/common/types.hpp` (defining fixed-point `PriceTicks`, `Quantity`, `MicroUnits`, `InstrumentId`, `DataSourceType`, `FeedQualityState`, `Result<T, Error>`), `include/basalt/common/logger.hpp` (structured JSON/console logger with secret redaction), and `include/basalt/common/config.hpp` (validated configuration loader binding to `127.0.0.1:8080` by default).
- Preserve 100% backward compatibility with existing matching engine tests while refactoring headers cleanly under `include/basalt/` (keeping legacy headers or aliases so existing tests compile untouched).
- Add new unit tests for fixed-point overflow protection, config validation, and logger redaction. Build, run `ctest`, and commit.

### PHASE 2 — Institutional Workstation Frontend (`frontend/`)
- Build a modern, dark-first institutional trading and research workstation in `frontend/` (and serve its built static bundle or seamlessly integrate with the C++ server so opening `http://127.0.0.1:8080` works out of the box, while keeping a fallback or build script).
- Implement the workspaces:
  1. **Market Watch**: Watchlist table with symbol, exchange (`NSE`/`BSE`/`SIM`), LTP, Bid/Ask, Spread, Chg/Chg%, Volume, Session H/L, `DataSourceType` badge, and `FeedQualityState` staleness badge.
  2. **Instrument Workstation**: Candlestick + Volume chart (Canvas / Lightweight Charts), 20-level Order Book Depth Ladder with cumulative depth bars, Time & Sales tape, and Paper Order Ticket (`LIMIT`, `MARKET`, `IOC`, `FOK`, `POST_ONLY`, `STOP_LIMIT`).
  3. **Strategy Lab**: Create/configure/start/pause/stop strategies, parameter editor, live signal log, and per-strategy metrics.
  4. **Paper Trading & Portfolio/Risk**: Virtual account selector, cash ledger, open orders blotter, execution blotter, positions table (Avg Cost, Realized P&L, Unrealized P&L, Fees), exposure gauges, risk-limit utilization bars, rejection log, and Emergency Kill Switch button.
  5. **Backtesting & Market Replay**: Historical run launcher, deterministic replay transport controls (`Play`, `Pause`, `Step +1 Event`, `Speed 1x/5x/10x/MAX`, `Seek`), equity & drawdown curves, and 18-metric analytics tearsheet.
  6. **System Monitor**: Live telemetry for pipeline latency (`p50`/`p95`/`p99`), message rates, dropped/duplicate/out-of-order counts, queue depth, and provider health.
- Ensure keyboard shortcuts, clean responsive dark theme (`#090D16` background, `#111827` panels, `#10B981` bid/positive, `#F43F5E` ask/negative, `#F59E0B` stale/warning, monospace tabular numbers `JetBrains Mono` / `monospace`), and zero fake metrics. Build, verify, and commit.

### PHASE 3 — Market-Data Gateway, Instrument Master & Provider Adapters
- Implement `InstrumentMaster` (`NSE`, `BSE`, `SIM` instruments with tick size, lot size, symbol mapping, and session timings—pre-populated with major Indian equities/indices like `RELIANCE`, `TCS`, `HDFCBANK`, `INFY`, `ICICIBANK`, `SBIN`, `BHARTIARTL`, `ITC`, `LT`, `NIFTY50` plus synthetic simulation symbols).
- Implement `IMarketDataProvider` interface and the 12-stage `MarketDataPipeline` (decode -> validate -> resolve instrument -> normalize timestamps -> deduplicate by sequence/timestamp -> quality check -> emit `CanonicalMarketEvent` -> distribute via bounded queues with explicit backpressure policies).
- Implement three concrete adapters:
  1. `DeterministicSyntheticProvider` (configurable Geometric Brownian Motion + Poisson order-flow generator for offline/demo operation, explicitly tagged `SYNTHETIC`).
  2. `CsvJsonReplayProvider` (streams historical tick/bar files, explicitly tagged `REPLAYED`/`HISTORICAL`).
  3. `UpstoxProviderStub` & `FyersProviderAdapter` (configurable REST/WS adapter following official Upstox v2 / FYERS v3 schemas that reads credentials from environment variables `BASALT_UPSTOX_TOKEN` / `BASALT_FYERS_TOKEN`, operates safely in dry-run/offline diagnostic mode when credentials are absent, and never exposes tokens).
- Write comprehensive market-data unit tests (malformed payload, duplicate event, out-of-order timestamp, stale feed transition, reconnect gap detection). Build, run `ctest`, and commit.

### PHASE 4 — Persistence, Historical Storage & Append-Only Event Log
- Implement lightweight embedded persistence (using SQLite3 amalgamation/embedded SQL or deterministic transactional file store if SQLite library isn't pre-installed, plus a binary append-only `.blog` event log with CRC32 frame checksums for deterministic replay).
- Persist Instruments, OHLC Bars, Orders, Executions, Cash Ledger entries, Positions, Strategy Runs, Backtest Results, and Risk Events.
- Add corruption detection and snapshot + log recovery tests. Build, run `ctest`, and commit.

### PHASE 5 — Pre-Trade Risk Engine, Paper Execution Simulator & Portfolio Ledger
- Implement `RiskEngine` enforcing all 16 pre-trade controls: Max Order Notional, Max Order Quantity, Max Position Size, Max Gross Exposure, Max Net Exposure, Max Concentration %, Max Daily Loss, Max Drawdown %, Max Open Orders, Max Order Rate (orders/sec), Per-Instrument Limit, Per-Strategy Limit, Stale-Data Rejection, Market-Session Check, Duplicate-Order Protection, and Global/Strategy Kill Switch.
- Implement `PaperExecutionSimulator` supporting all 4 execution models:
  1. Conservative Quote-Based (buys fill at `Ask + slippage`, sells fill at `Bid - slippage`, never optimistically at LTP).
  2. Bar-Based Approximation (open/high/low/close pessimistic fill rules with volume participation caps).
  3. Order-Book Replay Model (sweeps depth levels).
  4. BASALT Internal CLOB Model (routes into the C++ `MatchingEngine`).
- Implement `PortfolioLedger` with exact fixed-point accounting: Cash balance, Average Cost basis, Realized P&L, Unrealized P&L (marked to conservative Bid/Ask), and configurable Indian equity transaction cost model (Brokerage, STT, Exchange Txn Charge, SEBI Turnover Fee, Stamp Duty, GST).
- Write unit tests for every risk rejection reason and partial-fill/short-flip/fee accounting formula. Build, run `ctest`, and commit.

### PHASE 6 — Strategy Engine & 6 Built-In Quantitative Strategies
- Implement `StrategyRuntime` with the 10-state lifecycle (`CREATED` -> `CONFIGURED` -> `VALIDATED` -> `INITIALISING` -> `WARMING_UP` -> `RUNNING` -> `PAUSED` -> `STOPPING` -> `STOPPED` / `FAILED`) and exception isolation so a crashing strategy transitions to `FAILED` without crashing the engine.
- Implement reusable O(1) incremental indicators (`SMA`, `EMA`, `RSI`, `BollingerBands`, `SessionVWAP`, `BookImbalance`).
- Implement 6 built-in strategies:
  1. `SmaCrossoverStrategy` (Fast/Slow SMA crossover with warm-up gate)
  2. `RsiMeanReversionStrategy` (Oversold/Overbought entry/exit)
  3. `BollingerBandStrategy` (Volatility band breakout/reversion)
  4. `VwapReversionStrategy` (Intraday deviation from volume-weighted average price)
  5. `MomentumBreakoutStrategy` (N-bar high/low channel breakout with volume confirmation)
  6. `OrderBookImbalanceStrategy` (Top-5 bid/ask queue imbalance ratio; auto-disables if depth quality is unavailable)
- Ensure strategies only emit `OrderIntent` objects into `RiskEngine` and cannot directly mutate the order book or portfolio. Build, run `ctest`, and commit.

### PHASE 7 — Bias-Free Backtesting & Deterministic Market Replay
- Implement `BacktestEngine` (strict chronological event dispatch where signals generated on Bar `t` close execute earliest at Bar `t+1` open + slippage, preventing look-ahead bias; full reproducibility manifest with dataset hash, strategy version, seed, fee model, and 18 output metrics).
- Implement `ReplayEngine` (loads recorded `.blog` or CSV sessions, supports `Play`, `Pause`, `Step`, `Seek`, and `Speed` multipliers, and verifies bit-for-bit identical trade/P&L outputs across repeated runs with the same seed).
- Add determinism verification tests. Build, run `ctest`, and commit.

### PHASE 8 — Advanced Multi-Instrument Matching Engine Evolution
- Evolve the C++ matching core to support `MultiInstrumentEngine` (isolated `OrderBook` per `InstrumentId`).
- Add support for advanced order types and lifecycle actions:
  - `IOC` (Immediate-Or-Cancel)
  - `FOK` (Fill-Or-Kill — pre-checks cumulative available qty across compatible price levels in O(levels) without mutating book if insufficient)
  - `POST_ONLY` (Maker-only — rejects if order would immediately cross the spread)
  - `STOP_MARKET` & `STOP_LIMIT` (triggered book resting until reference trade price reaches stop trigger)
  - `ModifyOrder` (in-place quantity decrease preserves FIFO timestamp priority; quantity increase or price change loses time priority and re-inserts at tail)
  - Self-Trade Prevention (`STPPolicy::CANCEL_NEWEST`, `CANCEL_OLDEST`, `CANCEL_BOTH` matching on `account_id`/`participant_id`).
- Add comprehensive unit tests for every new order type, modification rule, and STP policy, plus invariant checks after every operation. Build, run `ctest`, and commit.

### PHASE 9 — Low-Latency Telemetry, 12-Profile Benchmark Suite & Memory Arena
- Expand the benchmark suite (`benchmarks/`) to support the 12 workload profiles (Balanced, Buy-Heavy, Sell-Heavy, Aggressive Matching, Cancellation-Heavy, Wide Price Distribution, Concentrated Price Levels, Bursty Traffic, High Active-Order Count, Multi-Instrument 50-Symbol, Market-Data Burst, Slow-Consumer Backpressure).
- Add fixed-capacity `ObjectPool<Order>` / contiguous arena allocator for order nodes to eliminate hot-path heap allocations during matching, and measure `BaselineOrderBook` (`std::vector`) vs. `StandardOrderBook` (`std::map` + `std::list`) vs. `ArenaOrderBook` (`std::map` + pooled intrusive FIFO list).
- Run the actual benchmark executable, capture real hardware/latency percentiles (`p50`, `p90`, `p95`, `p99`, `p99.9`, throughput), and update `PERFORMANCE.md` with the real measured outputs. Build, run `ctest`, and commit.

### PHASE 10 — BASALT MCP Server & AI-Assisted Inspection Interface
- Implement the BASALT Model Context Protocol (MCP) server (`mcp/basalt_mcp_server.py` or C++ stdio/HTTP JSON-RPC 2.0 handler, plus config in `.mcp.json`) exposing:
  - Read-Only Tools: `get_system_health`, `get_market_watch`, `get_instrument_snapshot`, `get_order_book`, `get_recent_trades`, `get_paper_positions`, `get_portfolio_summary`, `get_strategy_status`, `inspect_feed_health`.
  - Controlled State-Changing Tools (with input schema validation, rate limiting, bounded execution sizes, and audit logging): `run_backtest`, `run_replay`, `run_test_suite`, `run_benchmark`, `trigger_kill_switch`.
- Add automated tests verifying MCP JSON-RPC tool discovery (`tools/list`) and tool execution (`tools/call`), confirming secrets/shell commands are never exposed. Build, run all tests, update `PROGRESS.md` to 100% `[x]`, and make the final commit.

Begin immediately by inspecting the workspace, reading `docs/seed/`, creating `PROGRESS.md`, generating the 18 specification `.md` files, and executing Phases 0 through 10 continuously without stopping for user input.
