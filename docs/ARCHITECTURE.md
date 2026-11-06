# BASALT Target Architecture

Status: target architecture for the 2026 roadmap; Phase 0 remains the legacy single-instrument C++ engine. Read with docs/documents.md and docs/ROADMAP.md.

## 1. Architecture rules

- One local process, modular libraries, explicit domain interfaces. No microservices, broker order placement, Kafka, Redis, Kubernetes, or mandatory cloud service.
- C++ owns authoritative market events, order state, risk decisions, fills, accounting, replay and metrics. The browser renders server state and submits intents; it never simulates executions.
- One ordered command stream per instrument. A single matching thread owns each order book; instruments can be sharded later, but one book is never concurrently mutated.
- Provider observations and internal simulated liquidity are distinct types and stores. Only a selected PaperExecutionModel can translate observations into paper fills.
- Prices and money use checked signed 64-bit fixed point, PRICE_SCALE=10,000 micro-units per INR. Quantities remain signed for portfolio positions and unsigned at order-book boundaries. Intermediate products use checked wide arithmetic.
- Default operation is offline with synthetic or recorded events. Missing credentials disable provider adapters cleanly.

## 2. Logical modules (20)

| Module | Owns | Main dependency direction |
|---|---|---|
| Common | IDs, fixed-point, Result/Error, clocks, enums | none |
| Config | validated settings, env overlay, secrets references | Common |
| Logging | structured events, redaction, rotation policy | Common |
| Instrument Master | instrument IDs, exchange/segment, tick/lot/session | Common, persistence port |
| Market Data Provider API | provider-neutral start/subscribe/stop | Common, Instrument Master |
| Provider Adapters | synthetic, CSV/JSON replay, Upstox diagnostic, FYERS adapter | provider API |
| Market Data Pipeline | decode, normalize, validate, health, fanout | provider API, instrument master |
| Event Bus | bounded durable and coalescing queues | Common |
| Persistence | SQLite WAL repository and .blog writer/reader | Common, event bus |
| Historical Catalog | bars, dataset identity and export | Persistence |
| Replay | event clock, seek/checkpoints, deterministic dispatch | Persistence, event bus |
| Indicators | incremental SMA/EMA/RSI/Bollinger/VWAP/imbalance | Common |
| Strategy Runtime | lifecycle, plugin contract, fault isolation | Indicators, event bus |
| Risk | sixteen pre-trade gates and kill switches | Common, instrument, portfolio snapshot |
| Paper Execution | quote/bar/depth/CLOB models | market data, matching engine |
| Matching Engine | per-instrument CLOB and order lifecycle | Common, instrument |
| Portfolio Ledger | cash/position journal, fees and marks | Common, execution reports |
| Backtest and Analytics | causal dispatch, runs, metrics and manifests | replay, strategies, risk, ledger |
| API Gateway | loopback REST v1, WS v1, auth policy | read models and command facade |
| MCP Inspection | JSON-RPC tools, rate limits, audit | API/read models, backtest facade |

Dependencies point inward to domain contracts. The core has no HTTP, JSON, SQLite, React, or provider SDK types. Provider/API data are converted at their boundaries.

## 3. System context

~~~mermaid
flowchart LR
  Provider[Authorised feed or local files] --> Gateway[Market-data adapters]
  Browser[Local research workstation] <--> API[Loopback REST and WebSocket]
  AI[Codex or MCP client] <--> MCP[BASALT MCP]
  subgraph Local[BASALT local process]
    Gateway --> Pipe[Normalize and quality pipeline] --> Bus[Bounded event bus]
    Bus --> Store[SQLite WAL and append-only log]
    Bus --> Strat[Strategies] --> Risk[Risk gate] --> Paper[Paper execution]
    Paper --> CLOB[Internal CLOB]
    Paper --> Ledger[Portfolio ledger]
    CLOB --> Ledger
    Replay[Replay and backtest] <--> Store
    API --> Views[Read models and command facade]
    Views --> Ledger
    Views --> CLOB
    MCP --> Views
  end
~~~

## 4. Required architecture diagrams

### 4.1 Container/process view
~~~mermaid
flowchart TB
  subgraph P[BASALT process]
    Main[Lifecycle and configuration]
    IO[Provider and file I/O workers]
    Core[Ordered domain dispatcher]
    DB[Persistence worker]
    Web[HTTP/WS workers]
    Main --> IO --> Core
    Core --> DB
    Web --> Core
  end
  UI[Browser] <-->|HTTP and WS| Web
  Feed[Provider or local replay] --> IO
~~~

### 4.2 Component view
~~~mermaid
flowchart LR
  InstrumentMaster --> Adapter[Provider adapters]
  Adapter --> Pipeline --> EventBus
  EventBus --> Catalog[SQLite and log]
  EventBus --> StrategyRuntime --> RiskEngine --> ExecutionRouter
  ExecutionRouter --> PaperModels
  PaperModels --> MultiInstrumentEngine
  ExecutionRouter --> PortfolioLedger
  MultiInstrumentEngine --> PortfolioLedger
  Catalog --> ReplayEngine --> StrategyRuntime
  PortfolioLedger --> API
  MultiInstrumentEngine --> API
  API --> WebUI[React workstation]
  API --> MCP
~~~

### 4.3 Market-data flow
~~~mermaid
sequenceDiagram
  participant A as Adapter
  participant P as Pipeline
  participant I as Instrument master
  participant Q as Bounded queues
  participant S as Store
  participant U as UI
  A->>P: raw frame + source metadata
  P->>I: resolve venue symbol
  P->>P: decode, validate, normalize, deduplicate, quality
  P->>Q: canonical immutable event
  Q->>S: durable ordered append
  Q->>U: coalesced latest quote
~~~

### 4.4 Order lifecycle
~~~mermaid
stateDiagram-v2
  [*] --> Received
  Received --> Rejected: invalid or risk denied
  Received --> Working: accepted limit
  Received --> TriggerPending: stop order
  Working --> PartiallyFilled: execution report
  PartiallyFilled --> Filled: remaining is zero
  PartiallyFilled --> Cancelled: cancel or IOC expiry
  Working --> Cancelled: cancel
  TriggerPending --> Working: trigger satisfied
  TriggerPending --> Cancelled: cancel
  Rejected --> [*]
  Filled --> [*]
  Cancelled --> [*]
~~~

### 4.5 Strategy execution
~~~mermaid
sequenceDiagram
  participant E as Event bus
  participant R as Strategy runtime
  participant S as Strategy
  participant G as Risk gate
  participant X as Paper router
  E->>R: canonical market event
  R->>S: on_market_event
  S-->>R: OrderIntent or no-op
  R->>G: intent + strategy context
  alt accepted
    G->>X: approved order
    X-->>R: simulated report
  else rejected
    G-->>R: typed rejection code
  end
~~~

### 4.6 Persistence and replay
~~~mermaid
flowchart LR
  Domain[Committed domain event] --> WAL[SQLite WAL catalog]
  Domain --> Log[CRC framed .blog log]
  Snap[Periodic checkpoint] --> WAL
  WAL --> Recovery[Validate snapshot and log tail]
  Log --> Recovery
  Recovery --> Replay[Virtual-clock replay]
  Replay --> Domain
~~~

### 4.7 Frontend communication
~~~mermaid
sequenceDiagram
  participant UI as React UI
  participant REST as REST v1
  participant WS as WebSocket v1
  participant D as Domain facade
  UI->>REST: command with request ID
  REST->>D: validate and enqueue intent
  D-->>REST: accepted/rejected result
  REST-->>UI: schema-versioned JSON
  D-->>WS: immutable event
  WS-->>UI: typed frame with sequence and source
~~~

### 4.8 MCP integration
~~~mermaid
flowchart LR
  Client[MCP client] <-->|stdio JSON-RPC| Server[BASALT MCP server]
  Server --> Read[Read-only snapshots]
  Server --> Guard[Bounded audited simulation tools]
  Read --> API[Read model facade]
  Guard --> Replay[Backtest, replay, benchmark, kill switch]
~~~

### 4.9 Failure and recovery
~~~mermaid
flowchart TD
  Fault[Failure detected] --> Classify{Domain}
  Classify -->|feed| Degrade[Mark STALE or DISCONNECTED; block new strategy intents]
  Classify -->|log| Halt[Stop accepting state mutations; preserve valid prefix]
  Classify -->|strategy| Isolate[Mark FAILED; isolate exception]
  Classify -->|UI| Reconnect[Keep core running; reconnect from sequence]
  Classify -->|risk| Kill[Latch kill switch; cancel paper orders]
  Degrade --> Recover[Validate and reconnect]
  Halt --> Recover
  Isolate --> Recover
  Reconnect --> Recover
  Kill --> Recover
~~~

### 4.10 Queue and backpressure flow
~~~mermaid
flowchart LR
  Decoder --> Durable[Bounded ordered event queue]
  Decoder --> UIQ[Per-instrument latest-value queue]
  Durable -->|full: block briefly then mark overflow and stop source| Store
  UIQ -->|full: replace older quote for same instrument| WS[WebSocket broadcaster]
  Store --> Replay[Replay reader]
~~~

## 5. Process, threads, and event ordering

The main thread validates configuration and coordinates shutdown. Provider adapters may own I/O threads and must not call the matching engine. They post immutable normalized events. A domain dispatcher serializes state-changing commands; a future implementation may use one dispatcher per instrument shard. SQLite access and file writes are serialized by the persistence worker. HTTP and WebSocket workers read immutable snapshots and submit typed commands through the domain facade. MCP stdio has its own request loop and shares only facades, never internal locks.

Each event carries a durable monotonically increasing event sequence, provider event time, receive time, instrument ID, source, quality, schema version, and payload. Provider sequence gaps, clock regression, duplicates, and queue overflow become explicit quality/telemetry events. Replay orders on persisted sequence, with a deterministic tie-breaker for imported files. Wall clocks are not used as authoritative order priority.

## 6. Event bus policy

The durable channel never silently drops orders, executions, ledger events, or replay input. At capacity it applies bounded producer backpressure; after a configured deadline it marks the feed DEGRADED, stops new strategy submissions, and raises a visible health error. The browser quote channel is lossy by design: coalesce by instrument and publish the newest state at a bounded rate. SPSC is used only where one producer and one consumer are enforced in code. Other fanout uses bounded MPSC queues or a mutex-protected channel; no queue is described as lock-free unless its implementation and tests prove that property.

## 7. Module contracts

Public APIs exchange domain value types and Result values. Adapter callbacks do not retain caller references. Strategy code receives immutable market snapshots and can return only OrderIntent values. RiskEngine owns final simulation approval. Execution models return FillReport values; PortfolioLedger is the sole owner of balances and positions. The multi-instrument CLOB owns a map from InstrumentId to independently invariant-checked books. API and MCP command handlers call the same validated application facade.

## 8. Migration compatibility

Keep existing basalt::OrderBook and MatchingEngine APIs available. First wrap MatchingEngine behind an ExecutionModel and instrument router, then add instrument/account context to new command types. Do not rewrite the tested map/list book during infrastructure work. Retain web/index.html as a static fallback until the frontend bundle builds and is served by default.

See docs/ROADMAP.md for phase delivery and rollback boundaries.
