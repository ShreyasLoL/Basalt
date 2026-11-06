# Market Data, Instruments, Providers and Feed Health

Research checked 2026-11-06 against linked official documentation. Provider contracts and commercial/licensing terms change. Re-check current official plans and exchange entitlements before enabling a live connection. This module reads market data only; credentials must never enable order placement.

## Provider comparison

| Provider | Current documented market-data path | Depth / streaming facts | Pricing, eligibility and rights |
|---|---|---|---|
| Upstox | Official docs expose v3 market feed over WebSocket with Protobuf and candle endpoints. v2 market stream is marked discontinued. | v3 docs list subscription/connection limits by mode; these are account-plan dependent. | Do not use the seed's v2 recommendation. Current plan fees, account eligibility and redistribution/storage grants require verification from official commercial and exchange terms before activation. |
| FYERS | API v3 has quote, depth, historical candle APIs and Data/TBT sockets. Official docs were updated 2026-10-14. | Data socket carries live quotes/depth; TBT is a separate depth stream. Standard/Prime quotas vary. | Pricing and symbol history quotas are plan-dependent; verify against current official plans and terms. |
| DhanHQ | API v2 market feed, quote/depth, historical and full market depth. | Official docs describe 20-level depth for up to 50 instruments per connection; a separate 200-level stream is also documented. | Data APIs require a data subscription; exact price and eligibility must be checked in the account/platform UI and official terms. |
| Angel One SmartAPI | REST quote, OHLC/full quote, historical candle, binary WebSocket streaming. | Full REST quote includes best five bid/ask levels; docs state 50 symbols/request and one request/second for the market quote API. WebSocket limits are separately documented. | Account, licensing and feed rights are plan/contract-specific and require official confirmation. |
| Zerodha Kite Connect | Kite Connect v3 has binary WebSocket quotes and historical candles. | WebSocket full mode includes five bid and five offer levels; docs state up to 3000 instruments per connection. | API access is subscription-based; confirm current plan price and data redistribution restrictions on official plans/terms before enabling. |

Verified facts and sources: [Upstox v3 market feed](https://upstox.com/developer/api-documentation/v3/get-market-data-feed/), [Upstox v2 discontinuation](https://upstox.com/developer/api-documentation/announcements/v2-websocket-discontinued/), [Upstox rate limits](https://upstox.com/developer/api-documentation/rate-limiting/); [FYERS market data API](https://fyers.in/api-docs/v3/api/market-data/), [FYERS WebSockets](https://fyers.in/api-docs/v3/api/websockets/), [FYERS limits](https://fyers.in/api-docs/v3/getting-started/plans-and-limits/); [Dhan full market depth](https://dhanhq.co/docs/v2/full-market-depth/), [Dhan authentication and plan notes](https://dhanhq.co/docs/v2/authentication/); [Angel One SmartAPI](https://smartapi.angelbroking.com/docs), [Angel One WebSocket 2.0](https://smartapi.angelbroking.com/docs/WebSocket2); [Kite Connect v3](https://kite.trade/docs/connect/v3/), [Kite streaming](https://kite.trade/docs/connect/v3/websocket/), [Kite historical data](https://kite.trade/docs/connect/v3/historical/).

Pricing, KYC/account requirements, exchange redistribution rights, and retention are not inferred from SDK behavior. Treat each as **needs verification against official docs and the user's entitlement**. Do not compile plan numbers into code or present a provider as universally best.

## Instrument Master

InstrumentId is stable BASALT identity, not a provider token. An InstrumentDefinition carries exchange (NSE/BSE/SIM), segment, display and canonical symbols, currency, tick size in micro-rupees, lot size, quantity step, active interval, timezone, trading calendar/session windows, price bands, and provider mappings with effective dates. Load a versioned local catalog; reject ambiguous aliases, stale mappings and prices not aligned to tick size. Synthetic symbols are visibly SIM and cannot be confused with NSE/BSE instruments. Initial catalog targets RELIANCE, TCS, HDFCBANK, INFY, ICICIBANK, SBIN, BHARTIARTL, ITC, LT and NIFTY50; verify contract tokens from provider/exchange master files rather than inventing them.

## Provider-neutral interface

~~~cpp
struct ProviderCapabilities { bool trades; bool quotes; bool depth; bool bars; std::size_t max_subscriptions; };
class IMarketDataProvider {
public:
  virtual ~IMarketDataProvider() = default;
  virtual Result<void, ProviderError> connect(const ProviderConfig&) = 0;
  virtual Result<void, ProviderError> subscribe(std::span<const InstrumentId>) = 0;
  virtual void set_event_sink(std::function<void(RawMarketEvent)>) = 0;
  virtual void stop() noexcept = 0;
  virtual ProviderCapabilities capabilities() const noexcept = 0;
};
~~~

ProviderConfig stores provider name, endpoint, token environment-variable name and enabled/dry-run flags, never a plaintext token in logs or persisted config. The first implementations are DeterministicSyntheticProvider and CsvJsonReplayProvider. UpstoxProviderStub and FyersProviderAdapter are isolated opt-in adapters; a stub must say DISCONNECTED/diagnostic-only and must not claim live data. Implement no authenticated adapter until its schema, decoding, plan, and licensing are confirmed.

## Canonical market event

CanonicalMarketEvent contains schema_version, event_id, instrument_id, kind (trade, quote, depth, bar, status), exchange/provider event time, local receive monotonic time, provider sequence, source, feed quality, price ticks, quantity, optional bid/ask ladders, optional OHLCV fields, and provenance/license label. Missing values are optional rather than zero. Prices convert to fixed micro-rupees at the provider boundary with explicit precision and tick validation. Each event is immutable after emission.

## Twelve-stage normalization pipeline

1. Receive transport frame and assign local receive time.
2. Decode transport envelope (JSON, Protobuf or bounded binary); reject oversize frames.
3. Parse provider schema/version and validate required fields.
4. Resolve provider instrument key to stable InstrumentId and effective mapping.
5. Convert prices/quantities using checked fixed-point parsers and instrument tick/lot rules.
6. Normalize provider timestamp to UTC nanoseconds; retain raw time and offset.
7. Detect clock skew, regression, duplicate event IDs/sequences and sequence gaps.
8. Validate semantics: positive trade prices/qty, OHLC high/low ordering, sorted depth, no impossible negative values.
9. Update per-provider/instrument health, stale timers and reconnect generation.
10. Stamp DataSourceType and FeedQualityState; emit explicit gap/quality events.
11. Append to the ordered durable channel and optional historical store.
12. Fan out coalesced UI snapshots and immutable strategy events under their separate backpressure policies.

Pipeline errors are typed and counted. Invalid frames never reach strategies or paper execution. Dedup keys prefer provider sequence; otherwise use provider, instrument, event timestamp, event kind and normalized payload hash. Clock correction never rewrites original event time.

## Quality and feed health

Health states are OK, STALE, DEGRADED, DISCONNECTED. Thresholds are configurable per provider, instrument and event cadence. Missing heartbeat or event beyond stale timeout marks STALE; invalid payload bursts, gaps, queue pressure or clock skew mark DEGRADED; failed connection marks DISCONNECTED. Recovery requires reconnect, a fresh valid snapshot and gap reconciliation. The UI displays source and quality badges. Stale/degraded/disconnected events block new strategy orders by default; manual simulation can be explicitly configured and audit-logged.

## Queue bounds and backpressure

Durable market events, executions, orders and ledger records use a bounded ordered MPSC channel. Prefer one producer/one consumer SPSC only when topology is enforced. On full queue: backpressure the adapter for a short bounded interval, then mark DEGRADED, increment overflow counters, stop strategy intent generation, and surface a fault; never silently drop durable events. UI quote fanout uses per-instrument latest-value coalescing, counts replacements, and caps WS frequency. Replay uses a dedicated deterministic queue with no coalescing. Queue capacity and policy are observable configuration, not magic constants.

## Phase 3 local implementation

The `basalt_market_data` CMake library implements the provider-neutral interface, versioned `InstrumentMaster`, fixed-point normalizer, feed-health snapshots and bounded durable/quote event bus. Its seeded catalog contains ten visibly `SIM:` symbols matching the roadmap watchlist. Provider/exchange token identifiers are not fabricated; NSE/BSE entries can be added with verified tick, lot and effective-dated mapping data.

`DeterministicSyntheticProvider` accepts active SIM instruments only. `CsvJsonReplayProvider` reads from caller-supplied streams, so the caller controls which local file is opened. CSV headers are `event_id,provider_instrument_key,kind,event_time_ns,provider_sequence,price,quantity,bid,bid_quantity,ask,ask_quantity,open,high,low,close,volume,provenance_label`. JSON Lines use the same field names and may include `bids` and `asks` arrays of `{price,quantity}` objects. Both formats are capped at 1 MiB per record and one million events. Replay data is labeled `REPLAYED`; synthetic data is labeled `SYNTHETIC`.

`UpstoxProviderStub` targets the documented v3 boundary and `FyersProviderAdapter` exposes the current diagnostic boundary. Both deliberately return `DIAGNOSTIC_ONLY`, report no live capabilities, read no token value, open no network connection, and contain no order method. Their source registration must remain disconnected until provider schemas, entitlements and licensing are explicitly validated.
