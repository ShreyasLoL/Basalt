# BASALT Data Model and Fixed-Point Rules

## Authoritative numeric representation

All INR price, cash, fee and P&L amounts are signed 64-bit integers at PRICE_SCALE=10,000 micro-rupees per INR. Thus ₹1.00 is 10,000 units and a ₹0.05 tick is 500 units. Prices are validated against an instrument tick size before persistence or matching. Quantities are whole units; positions are signed 64-bit quantities, while live order remaining quantity is unsigned and nonzero. Never use floating point for an authoritative value. Convert external decimal strings with checked decimal parsing; round only under an explicit instrument precision policy. Use checked __int128 intermediates where available and a portable checked multiply fallback.

NotionalMicro = PriceMicro × signed Quantity. Reject overflow before mutation. Average cost is integer division toward zero with the remainder retained as a deterministic cost residual or assigned by the documented rounding policy. Every journal row has a unique event sequence and idempotency key.

## Domain entities

Common IDs are strongly typed aliases: InstrumentId, AccountId, StrategyId, OrderId, ExecutionId, EventSequence, PriceTicks, MicroUnits, Quantity. DataSourceType is LIVE_PROVIDER, DELAYED_PROVIDER, HISTORICAL, CACHED, SYNTHETIC, REPLAYED or DERIVED. FeedQualityState is OK, STALE, DEGRADED or DISCONNECTED. Order values include instrument, account, participant, strategy/client IDs, side, type, TIF, limit/stop price, original/filled/remaining quantity, priority sequence, state and source. Market events include event and receive timestamps, provider sequence, instrument, source, quality, schema version and a typed payload. Persisted events are immutable; corrections are new events referencing prior IDs.

## SQLite catalog schema

SQLite is opened with foreign keys, WAL journaling, synchronous=FULL for authoritative ledger/event commits, a busy timeout, and explicit schema migrations. All timestamps are UTC epoch nanoseconds; trading-session interpretation uses the instrument's exchange calendar and Asia/Kolkata display.

~~~sql
PRAGMA foreign_keys=ON;
CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, applied_at_ns INTEGER NOT NULL, checksum TEXT NOT NULL);
CREATE TABLE instruments(instrument_id INTEGER PRIMARY KEY, exchange TEXT NOT NULL, segment TEXT NOT NULL, symbol TEXT NOT NULL, provider_symbol TEXT, currency TEXT NOT NULL, tick_micro INTEGER NOT NULL CHECK(tick_micro>0), lot_size INTEGER NOT NULL CHECK(lot_size>0), session_json TEXT NOT NULL, active INTEGER NOT NULL, UNIQUE(exchange,segment,symbol));
CREATE TABLE provider_mappings(provider TEXT NOT NULL, instrument_id INTEGER NOT NULL REFERENCES instruments, provider_key TEXT NOT NULL, valid_from_ns INTEGER NOT NULL, valid_to_ns INTEGER, PRIMARY KEY(provider,provider_key,valid_from_ns));
CREATE TABLE market_events(event_seq INTEGER PRIMARY KEY, event_id TEXT NOT NULL UNIQUE, instrument_id INTEGER NOT NULL REFERENCES instruments, event_time_ns INTEGER NOT NULL, receive_time_ns INTEGER NOT NULL, provider_seq INTEGER, source TEXT NOT NULL, quality TEXT NOT NULL, kind TEXT NOT NULL, payload BLOB NOT NULL, payload_crc INTEGER NOT NULL);
CREATE INDEX market_events_by_instrument_time ON market_events(instrument_id,event_time_ns,event_seq);
CREATE TABLE bars(instrument_id INTEGER NOT NULL REFERENCES instruments, interval_ns INTEGER NOT NULL, open_time_ns INTEGER NOT NULL, open_micro INTEGER NOT NULL, high_micro INTEGER NOT NULL, low_micro INTEGER NOT NULL, close_micro INTEGER NOT NULL, volume INTEGER NOT NULL, source TEXT NOT NULL, quality TEXT NOT NULL, PRIMARY KEY(instrument_id,interval_ns,open_time_ns,source));
CREATE TABLE datasets(dataset_id TEXT PRIMARY KEY, instrument_id INTEGER REFERENCES instruments, format TEXT NOT NULL, source TEXT NOT NULL, path TEXT NOT NULL, sha256 TEXT NOT NULL, range_start_ns INTEGER, range_end_ns INTEGER, imported_at_ns INTEGER NOT NULL, license_tag TEXT NOT NULL);
CREATE TABLE accounts(account_id INTEGER PRIMARY KEY, name TEXT NOT NULL, currency TEXT NOT NULL, initial_cash_micro INTEGER NOT NULL, created_at_ns INTEGER NOT NULL);
CREATE TABLE strategies(strategy_id INTEGER PRIMARY KEY, name TEXT NOT NULL, version TEXT NOT NULL, config_json TEXT NOT NULL, created_at_ns INTEGER NOT NULL);
CREATE TABLE strategy_runs(run_id TEXT PRIMARY KEY, strategy_id INTEGER NOT NULL REFERENCES strategies, account_id INTEGER NOT NULL REFERENCES accounts, state TEXT NOT NULL, start_ns INTEGER, stop_ns INTEGER, seed INTEGER NOT NULL, config_hash TEXT NOT NULL);
CREATE TABLE orders(order_id INTEGER PRIMARY KEY, client_order_id TEXT UNIQUE, instrument_id INTEGER NOT NULL REFERENCES instruments, account_id INTEGER NOT NULL REFERENCES accounts, strategy_id INTEGER REFERENCES strategies, side TEXT NOT NULL, order_type TEXT NOT NULL, tif TEXT NOT NULL, limit_micro INTEGER, stop_micro INTEGER, quantity INTEGER NOT NULL, filled_quantity INTEGER NOT NULL, status TEXT NOT NULL, priority_seq INTEGER NOT NULL, created_ns INTEGER NOT NULL, updated_ns INTEGER NOT NULL);
CREATE TABLE order_events(event_seq INTEGER PRIMARY KEY, order_id INTEGER NOT NULL REFERENCES orders, event_type TEXT NOT NULL, quantity INTEGER NOT NULL, price_micro INTEGER, reason TEXT NOT NULL, event_time_ns INTEGER NOT NULL);
CREATE TABLE executions(execution_id INTEGER PRIMARY KEY, order_id INTEGER NOT NULL REFERENCES orders, instrument_id INTEGER NOT NULL REFERENCES instruments, account_id INTEGER NOT NULL REFERENCES accounts, side TEXT NOT NULL, price_micro INTEGER NOT NULL, quantity INTEGER NOT NULL, fee_micro INTEGER NOT NULL, event_seq INTEGER NOT NULL UNIQUE, event_time_ns INTEGER NOT NULL, model TEXT NOT NULL);
CREATE TABLE cash_ledger(entry_id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL REFERENCES accounts, execution_id INTEGER REFERENCES executions, event_seq INTEGER NOT NULL UNIQUE, debit_micro INTEGER NOT NULL, credit_micro INTEGER NOT NULL, fee_micro INTEGER NOT NULL, balance_micro INTEGER NOT NULL, memo TEXT NOT NULL, event_time_ns INTEGER NOT NULL, CHECK(debit_micro>=0 AND credit_micro>=0));
CREATE TABLE positions(account_id INTEGER NOT NULL REFERENCES accounts, instrument_id INTEGER NOT NULL REFERENCES instruments, quantity INTEGER NOT NULL, average_cost_micro INTEGER NOT NULL, realized_pnl_micro INTEGER NOT NULL, fees_micro INTEGER NOT NULL, updated_seq INTEGER NOT NULL, PRIMARY KEY(account_id,instrument_id));
CREATE TABLE risk_events(event_seq INTEGER PRIMARY KEY, account_id INTEGER NOT NULL REFERENCES accounts, strategy_id INTEGER REFERENCES strategies, instrument_id INTEGER REFERENCES instruments, code TEXT NOT NULL, accepted INTEGER NOT NULL, details_json TEXT NOT NULL, event_time_ns INTEGER NOT NULL);
CREATE TABLE feed_health(provider TEXT NOT NULL, instrument_id INTEGER REFERENCES instruments, state TEXT NOT NULL, last_event_ns INTEGER, gap_count INTEGER NOT NULL, duplicate_count INTEGER NOT NULL, detail TEXT NOT NULL, PRIMARY KEY(provider,instrument_id));
CREATE TABLE checkpoints(checkpoint_id TEXT PRIMARY KEY, event_seq INTEGER NOT NULL, created_ns INTEGER NOT NULL, state_hash TEXT NOT NULL, blob_path TEXT NOT NULL, blob_crc INTEGER NOT NULL);
CREATE TABLE backtest_runs(run_id TEXT PRIMARY KEY, strategy_id INTEGER NOT NULL REFERENCES strategies, dataset_id TEXT NOT NULL REFERENCES datasets, manifest_json TEXT NOT NULL, status TEXT NOT NULL, created_ns INTEGER NOT NULL, result_hash TEXT);
CREATE TABLE backtest_metrics(run_id TEXT NOT NULL REFERENCES backtest_runs, metric TEXT NOT NULL, value_micro_or_ratio TEXT NOT NULL, PRIMARY KEY(run_id,metric));
CREATE TABLE signals(signal_id INTEGER PRIMARY KEY, run_id TEXT REFERENCES strategy_runs, strategy_id INTEGER NOT NULL REFERENCES strategies, instrument_id INTEGER NOT NULL REFERENCES instruments, event_seq INTEGER NOT NULL, side TEXT NOT NULL, reason TEXT NOT NULL, values_json TEXT NOT NULL);
CREATE TABLE benchmark_runs(run_id TEXT PRIMARY KEY, workload TEXT NOT NULL, implementation TEXT NOT NULL, hardware_json TEXT NOT NULL, compiler_json TEXT NOT NULL, seed INTEGER NOT NULL, operation_count INTEGER NOT NULL, results_json TEXT NOT NULL, created_ns INTEGER NOT NULL);
CREATE TABLE audit_events(event_seq INTEGER PRIMARY KEY, actor TEXT NOT NULL, action TEXT NOT NULL, object_type TEXT NOT NULL, object_id TEXT, outcome TEXT NOT NULL, redacted_detail TEXT NOT NULL, event_time_ns INTEGER NOT NULL);
~~~

The schema has twenty application entity tables (instruments, provider mappings, market events, bars, datasets, accounts, strategies, strategy runs, orders, order events, executions, cash ledger rows, positions, risk events, feed health, checkpoints, backtest runs, backtest metrics, signals, benchmark runs) plus the schema_migrations and audit_events support tables. Schema evolution is additive where possible; destructive migrations require an export and a new backup.

## Double-entry and position invariants

Each fill writes one execution and balanced cash postings: buy debit = notional + fees; sell credit = notional - fees. A contra account records fee revenue/expense so debits equal credits per transaction. Cash balance is the initial balance plus credits minus debits. All position changes reference the same execution and event sequence.

Let q be signed current position, f signed fill quantity, p execution price and c average cost. Same-direction fills set c'=(|q|c+|f|p)/(|q|+|f|). Opposite-direction fills close min(|q|,|f|) at p and realize (p-c)×closed×sign(q); if the fill crosses through zero, the residual opens at p. Position quantity becomes q+f. Fees are posted once per fill and attributed by the selected cost schedule. Long unrealized P&L marks at best bid; short P&L marks at best ask. Missing quote uses a clearly identified fallback mark and never invents a quote.

Reconciliation: initial cash plus net ledger cash flows equals cash balance; position quantity equals sum signed fills; realized P&L equals closed quantity times price difference less allocated fees; ledger debits equal credits; event sequence and execution IDs are unique; a replay from checkpoint plus log reproduces state hash.
