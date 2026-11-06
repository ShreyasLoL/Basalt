# Strategy API, Lifecycle and Initial Strategies

## Strategy contract

IStrategy is versioned and deterministic. It receives immutable InstrumentDefinition, StrategyConfig, MarketEvent and Portfolio/Risk snapshot values. It may return zero or more OrderIntent values with instrument, side, type, limit/stop, quantity, TIF, execution model and reason metadata. It cannot call the order book, ledger, filesystem, network, process APIs or broker APIs. Runtime submits intents only through RiskEngine. A strategy declares required warm-up events, required data channels/quality, parameter schema, supported instruments and a stable version/hash. Randomness uses an injected seeded generator.

~~~cpp
class IStrategy {
public:
  virtual ~IStrategy() = default;
  virtual StrategyDescriptor descriptor() const = 0;
  virtual Result<void, StrategyError> configure(const StrategyConfig&) = 0;
  virtual Result<void, StrategyError> initialise(const StrategyContext&) = 0;
  virtual std::vector<OrderIntent> on_event(const CanonicalMarketEvent&, const StrategyContext&) = 0;
  virtual void on_execution(const ExecutionReport&, const StrategyContext&) = 0;
  virtual void on_stop() noexcept = 0;
};
~~~

## Ten-state lifecycle

CREATED -> CONFIGURED -> VALIDATED -> INITIALISING -> WARMING_UP -> RUNNING -> PAUSED -> RUNNING -> STOPPING -> STOPPED. Any operational transition may move to FAILED on an uncaught strategy exception, invalid invariant, or unrecoverable required-data failure. A failed strategy is isolated, its outstanding simulated orders follow configured cancel-on-failure policy, and the engine remains available. Illegal transitions return typed errors. Stop is idempotent. Restart creates a new strategy run and manifest rather than erasing history.

| State | Meaning / exit gate |
|---|---|
| CREATED | Instance allocated, no config. |
| CONFIGURED | Config schema parsed and stored. |
| VALIDATED | Parameters/instrument/data requirements pass. |
| INITIALISING | Indicators and private state allocated. |
| WARMING_UP | Consumes history; emits no order intent. |
| RUNNING | Event handler emits validated intents. |
| PAUSED | No new intents; existing order policy remains explicit. |
| STOPPING | Cancels or retains simulated orders by policy, flushes state. |
| STOPPED | Clean terminal state. |
| FAILED | Exception isolated; error and recovery action recorded. |

## Incremental indicators

SMA uses a rolling sum and fixed-capacity ring. EMA uses alpha=2/(n+1) and stable event order. RSI uses Wilder-smoothed average gains/losses after n warm-up samples; zero-loss and zero-gain cases are defined as 100 and 0, and flat as 50. Bollinger bands use rolling population variance with documented integer/fixed scale and deterministic square-root rounding. Session VWAP accumulates price×volume in checked wide integers and resets only at exchange session boundary. Book imbalance is (sum bid qty - sum ask qty)/(sum bid qty + sum ask qty) over configured top levels; zero total is unavailable, not zero-direction signal. Non-authoritative ratio signals may use fixed-point basis points.

## Six built-in strategies

1. **SMA crossover:** long entry when fast SMA crosses above slow; exit on reverse cross. Warm-up requires slow-window history. No pyramiding by default.
2. **RSI mean reversion:** long entry when RSI crosses up through oversold threshold; exit at neutral/overbought or stop rule. Short symmetric only when account/risk config allows.
3. **Bollinger breakout/reversion:** selectable mode. Breakout enters on close beyond upper/lower band with volatility gate; reversion enters after price re-enters band from an extreme. Mode is explicit in config.
4. **Intraday VWAP reversion:** enter only when price deviates by configured basis points and begins moving back toward session VWAP; exit at VWAP or risk stop. Session boundary resets VWAP.
5. **Momentum breakout:** rolling N-bar high/low channel break with volume above a rolling threshold; only completed bars are used for the signal.
6. **Order-book imbalance:** compare top-five bid/ask queue totals and emit a directional intent only above configured threshold, with depth quality OK and fresh. Auto-disables when reliable depth is unavailable.

Every strategy has explicit entry/exit/sizing parameters, fee/slippage-aware sizing, warm-up and freshness guards, deterministic signal IDs, signal explanations, and fixture-based tests. Strategies emit intent only; risk decides acceptance. No strategy is represented as profitable by default.
