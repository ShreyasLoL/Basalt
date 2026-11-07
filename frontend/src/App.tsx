import { memo, useCallback, useEffect, useMemo, useState, type FormEvent, type ReactNode } from "react";
import { ChartPanel } from "./ChartPanel";
import {
  cancelOrder,
  connectWebSocket,
  fetchOptional,
  resetSimulation,
  submitOrder,
  type OrderCommand,
} from "./api";
import { useWorkstation } from "./store";
import type {
  ActivityRecord,
  ApiMode,
  BookSnapshot,
  FeedQualityState,
  MarketRow,
  PriceLevel,
  PriceUnit,
  RestingOrder,
  TradeRecord,
} from "./types";

const NAV = [
  { group: "WORKSPACE", items: ["Overview", "Market Watch", "Instrument Workstation", "Order Book"] },
  { group: "PAPER", items: ["Strategy Lab", "Paper Trading", "Portfolio & Risk"] },
  { group: "RESEARCH", items: ["Backtesting", "Market Replay", "Analytics"] },
  { group: "SYSTEM", items: ["Data Sources", "System Monitor", "Settings"] },
] as const;

type WorkspaceName = (typeof NAV)[number]["items"][number];

function formatNumber(value: number | null | undefined): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return "—";
  return new Intl.NumberFormat("en-IN", { maximumFractionDigits: 0 }).format(value);
}

function formatMoney(micro: number | null | undefined): string {
  if (micro === null || micro === undefined || !Number.isFinite(micro)) return "—";
  const rupees = micro / 10_000;
  return new Intl.NumberFormat("en-IN", {
    style: "currency",
    currency: "INR",
    minimumFractionDigits: 2,
    maximumFractionDigits: 4,
  }).format(rupees);
}

function formatPrice(value: number | null, unit: PriceUnit): string {
  if (value === null) return "—";
  return unit === "MICRO_INR" ? formatMoney(value) : formatNumber(value) + " ticks";
}

function sourceLabel(source: string | null): string {
  if (!source) return "SOURCE UNAVAILABLE";
  return source.replaceAll("_", " ");
}

function StatusDot({ state }: { state: FeedQualityState | "OK" | "ERROR" | "UNKNOWN" }) {
  const color = state === "OK" ? "green" : state === "STALE" || state === "DEGRADED" ? "amber" : "red";
  return <span className={"status-dot " + color} aria-hidden="true" />;
}

function QualityBadge({ quality }: { quality: FeedQualityState }) {
  return <span className={"quality-badge " + quality.toLowerCase()}>{quality}</span>;
}

function PageHeader({
  title,
  subtitle,
  action,
}: {
  title: string;
  subtitle: string;
  action?: ReactNode;
}) {
  return (
    <div className="page-header">
      <div>
        <div className="eyebrow">BASALT / LOCAL RESEARCH</div>
        <h1>{title}</h1>
        <p>{subtitle}</p>
      </div>
      {action}
    </div>
  );
}

function Metric({
  label,
  value,
  detail,
  tone = "neutral",
}: {
  label: string;
  value: string;
  detail?: string;
  tone?: "neutral" | "positive" | "negative" | "warning";
}) {
  return (
    <div className="metric-card">
      <div className="eyebrow">{label}</div>
      <div className={"metric-value " + tone}>{value}</div>
      {detail && <div className="metric-detail">{detail}</div>}
    </div>
  );
}

function EmptyState({
  title,
  detail,
  action,
}: {
  title: string;
  detail: string;
  action?: ReactNode;
}) {
  return (
    <div className="empty-state">
      <div className="empty-mark" aria-hidden="true">—</div>
      <strong>{title}</strong>
      <p>{detail}</p>
      {action}
    </div>
  );
}

const BookLadder = memo(function BookLadder({
  levels,
  unit,
  best,
}: {
  levels: PriceLevel[];
  unit: PriceUnit;
  best: number | null;
}) {
  const max = Math.max(1, ...levels.map((level) => level.totalVolume));
  if (levels.length === 0) {
    return <div className="table-empty">No resting liquidity in this book.</div>;
  }
  return (
    <div className="ladder-list">
      {levels.map((level) => (
        <div className={"ladder-row " + (level.price === best ? "best-level" : "")} key={level.price}>
          <div className={"depth-track " + level.side.toLowerCase()} style={{ width: (level.totalVolume / max * 100) + "%" }} />
          <span className="ladder-price">{formatPrice(level.price, unit)}</span>
          <span className="ladder-qty">{formatNumber(level.totalVolume)}</span>
          <span className="ladder-orders">{level.orderCount}</span>
        </div>
      ))}
    </div>
  );
});

const BookPane = memo(function BookPane({
  book,
  side,
}: {
  book: BookSnapshot | null;
  side: "BUY" | "SELL";
}) {
  const levels = side === "BUY" ? book?.bids ?? [] : book?.asks ?? [];
  const best = side === "BUY" ? book?.bestBid ?? null : book?.bestAsk ?? null;
  return (
    <section className="panel book-pane">
      <div className="section-heading">
        <div>
          <div className={"eyebrow " + (side === "BUY" ? "positive" : "negative")}>
            {side === "BUY" ? "BIDS" : "ASKS"}
          </div>
          <h2>{side === "BUY" ? "Buy interest" : "Sell interest"}</h2>
        </div>
        <span className="quiet-label">PRICE / QTY / ORDERS</span>
      </div>
      <BookLadder levels={levels} unit={book?.priceUnit ?? "TICKS"} best={best} />
    </section>
  );
});

const TradeTable = memo(function TradeTable({
  trades,
  unit,
}: {
  trades: TradeRecord[];
  unit: PriceUnit;
}) {
  if (trades.length === 0) {
    return <EmptyState title="No executions recorded" detail="Paper fills will appear here with their execution model and source." />;
  }
  return (
    <div className="table-scroll">
      <table>
        <thead><tr><th>Trade</th><th>Price</th><th>Qty</th><th>Buy ID</th><th>Sell ID</th><th>Aggressor</th><th>Model</th></tr></thead>
        <tbody>
          {trades.map((trade) => (
            <tr key={trade.tradeId}>
              <td className="mono">#{trade.tradeId}</td>
              <td>{formatPrice(trade.price, trade.priceUnit ?? unit)}</td>
              <td className="mono">{formatNumber(trade.quantity)}</td>
              <td className="mono">{trade.buyOrderId}</td>
              <td className="mono">{trade.sellOrderId}</td>
              <td><span className={"side-label " + trade.aggressorSide.toLowerCase()}>{trade.aggressorSide}</span></td>
              <td>{trade.model}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
});

const ActivityList = memo(function ActivityList({ records }: { records: ActivityRecord[] }) {
  if (records.length === 0) {
    return <EmptyState title="No system events" detail="Order, risk, feed and recovery events will be recorded here." />;
  }
  return (
    <div className="event-list">
      {records.map((record) => (
        <div className="event-row" key={record.eventSeq}>
          <span className="mono quiet">#{record.eventSeq}</span>
          <span className={"event-type " + record.action.toLowerCase()}>{record.action}</span>
          <div className="event-copy"><strong>{record.status}</strong><span>{record.message}</span></div>
        </div>
      ))}
    </div>
  );
});

const MarketWatch = memo(function MarketWatch({
  rows,
  selected,
  onSelect,
}: {
  rows: MarketRow[];
  selected: string | null;
  onSelect: (id: string) => void;
}) {
  return (
    <div className="panel table-panel">
      <div className="section-heading">
        <div><div className="eyebrow">WATCHLIST</div><h2>Market Watch</h2></div>
        <span className="quiet-label">Source and feed quality travel with each row</span>
      </div>
      {rows.length === 0 ? (
        <EmptyState title="No instruments available" detail="The instrument catalog or market watch endpoint has not returned data." />
      ) : (
        <div className="table-scroll">
          <table>
            <thead><tr><th>Instrument</th><th>Exchange</th><th>LTP</th><th>Bid</th><th>Ask</th><th>Spread</th><th>Volume</th><th>Source</th><th>Feed</th></tr></thead>
            <tbody>
              {rows.map((row) => (
                <tr
                  className={"clickable-row " + (selected === row.instrumentId ? "selected-row" : "")}
                  key={row.instrumentId}
                  onClick={() => onSelect(row.instrumentId)}
                  tabIndex={0}
                  onKeyDown={(event) => {
                    if (event.key === "Enter" || event.key === " ") onSelect(row.instrumentId);
                  }}
                >
                  <td><strong>{row.symbol}</strong><small>{row.instrumentId}</small></td>
                  <td>{row.exchange}</td>
                  <td className="mono">{row.lastPrice === null ? "—" : formatPrice(row.lastPrice, row.priceUnit)}</td>
                  <td className="mono positive">{row.bidPrice === null ? "—" : formatPrice(row.bidPrice, row.priceUnit)}</td>
                  <td className="mono negative">{row.askPrice === null ? "—" : formatPrice(row.askPrice, row.priceUnit)}</td>
                  <td className="mono">{row.spread === null ? "—" : formatPrice(row.spread, row.priceUnit)}</td>
                  <td className="mono">{formatNumber(row.volume)}</td>
                  <td><span className="source-label">{sourceLabel(row.source)}</span></td>
                  <td><QualityBadge quality={row.quality} /></td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
});

function parseDecimalMicro(value: string): number | null {
  const match = /^(\d{1,11})(?:\.(\d{1,4}))?$/.exec(value.trim());
  if (!match) return null;
  const fraction = (match[2] ?? "").padEnd(4, "0");
  const micro = Number(match[1]) * 10_000 + Number(fraction || "0");
  return Number.isSafeInteger(micro) && micro > 0 ? micro : null;
}

const OrderTicket = memo(function OrderTicket({
  mode,
  instrumentId,
  onMessage,
}: {
  mode: ApiMode;
  instrumentId: string | null;
  onMessage: (message: string, error?: boolean) => void;
}) {
  const [side, setSide] = useState<"BUY" | "SELL">("BUY");
  const [type, setType] = useState<OrderCommand["type"]>("LIMIT");
  const [timeInForce, setTimeInForce] = useState<OrderCommand["timeInForce"]>("GTC");
  const [price, setPrice] = useState("");
  const [stopPrice, setStopPrice] = useState("");
  const [executionModel, setExecutionModel] = useState<OrderCommand["executionModel"]>("INTERNAL_CLOB_MODEL");
  const [quantity, setQuantity] = useState("1");
  const [busy, setBusy] = useState(false);
  const legacy = mode !== "v1";

  async function handleSubmit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const amount = Number(quantity);
    if (!Number.isSafeInteger(amount) || amount < 1 || amount > 1_000_000_000) {
      onMessage("Quantity must be a whole number between 1 and 1,000,000,000.", true);
      return;
    }
    let priceValue: number | undefined;
    if (type !== "MARKET" && type !== "STOP_MARKET") {
      if (legacy) {
        if (!/^\d{1,12}$/.test(price)) {
          onMessage("Enter an integer price in ticks for the legacy CLOB.", true);
          return;
        }
        priceValue = Number(price);
      } else {
        const parsed = parseDecimalMicro(price);
        if (parsed === null) {
          onMessage("Enter a positive INR price with up to four decimal places.", true);
          return;
        }
        priceValue = parsed;
      }
    }
    if (legacy && (!["LIMIT", "MARKET"].includes(type) || timeInForce !== "GTC" || executionModel !== "INTERNAL_CLOB_MODEL")) {
      onMessage("The current server supports LIMIT and MARKET only. Advanced types require REST v1.", true);
      return;
    }
    let stopPriceValue: number | undefined;
    if (type === "STOP_MARKET" || type === "STOP_LIMIT") {
      stopPriceValue = legacy ? Number(stopPrice) : parseDecimalMicro(stopPrice) ?? undefined;
      if (stopPriceValue === undefined || !Number.isSafeInteger(stopPriceValue) || stopPriceValue <= 0) {
        onMessage("Enter a valid stop trigger price.", true);
        return;
      }
    }
    setBusy(true);
    try {
      const result = await submitOrder({
        side,
        type,
        timeInForce,
        quantity: amount,
        price: priceValue,
        stopPrice: stopPriceValue,
        instrumentId: instrumentId ?? undefined,
        executionModel,
      }, mode);
      onMessage(result.message);
    } catch (error) {
      onMessage(error instanceof Error ? error.message : "Order request failed.", true);
    } finally {
      setBusy(false);
    }
  }

  return (
    <section className="panel order-ticket">
      <div className="section-heading">
        <div><div className="eyebrow">PAPER ORDER</div><h2>Order ticket</h2></div>
        <span className="sim-tag">SIMULATION ONLY</span>
      </div>
      {legacy && <div className="inline-notice">Legacy gateway active · LIMIT and MARKET only · price entered in integer ticks</div>}
      <form className="ticket-form" onSubmit={(event) => void handleSubmit(event)}>
        <div className="segmented">
          <button type="button" className={side === "BUY" ? "active-buy" : ""} onClick={() => setSide("BUY")}>BUY</button>
          <button type="button" className={side === "SELL" ? "active-sell" : ""} onClick={() => setSide("SELL")}>SELL</button>
        </div>
        <label>Order type
          <select value={type} onChange={(event) => setType(event.target.value as OrderCommand["type"])}>
            <option value="LIMIT">LIMIT</option>
            <option value="MARKET">MARKET</option>
            <option value="POST_ONLY" disabled={legacy}>POST ONLY</option>
            <option value="STOP_MARKET" disabled={legacy}>STOP MARKET</option>
            <option value="STOP_LIMIT" disabled={legacy}>STOP LIMIT</option>
          </select>
        </label>
        <label>Time in force
          <select value={timeInForce} onChange={(event) => setTimeInForce(event.target.value as OrderCommand["timeInForce"])}>
            <option value="GTC">GTC</option>
            <option value="IOC" disabled={legacy}>IOC</option>
            <option value="FOK" disabled={legacy}>FOK</option>
          </select>
        </label>
        {type !== "MARKET" && type !== "STOP_MARKET" && (
          <label>Limit price {legacy ? "(ticks)" : "(INR)"}
            <input inputMode="decimal" value={price} onChange={(event) => setPrice(event.target.value)} placeholder={legacy ? "Integer ticks" : "INR price"} required />
          </label>
        )}
        {(type === "STOP_MARKET" || type === "STOP_LIMIT") && (
          <label>Stop trigger (INR)
            <input inputMode="decimal" value={stopPrice} onChange={(event) => setStopPrice(event.target.value)} placeholder="Trigger price" />
          </label>
        )}
        <label>Quantity
          <input inputMode="numeric" value={quantity} onChange={(event) => setQuantity(event.target.value)} required />
        </label>
        <label>Execution model
          <select value={executionModel} onChange={(event) => setExecutionModel(event.target.value as OrderCommand["executionModel"])}>
            <option value="INTERNAL_CLOB_MODEL">Internal CLOB</option>
            <option value="CONSERVATIVE_QUOTE_MODEL" disabled={legacy}>Conservative quote</option>
            <option value="BAR_APPROXIMATION_MODEL" disabled={legacy}>Bar approximation</option>
            <option value="DEPTH_REPLAY_MODEL" disabled={legacy}>Depth replay</option>
          </select>
        </label>
        <button className="primary-button" type="submit" disabled={busy || mode === "disconnected"}>
          {busy ? "Submitting…" : "Submit paper order"}
        </button>
      </form>
    </section>
  );
});

const Overview = memo(function Overview({
  onNavigate,
}: {
  onNavigate: (workspace: WorkspaceName) => void;
}) {
  const rows = useWorkstation((state) => state.marketWatch);
  const book = useWorkstation((state) => state.book);
  const trades = useWorkstation((state) => state.trades);
  const health = useWorkstation((state) => state.health);
  const recentTrades = useMemo(() => trades.slice(0, 8), [trades]);
  const row = rows[0];
  return (
    <>
      <PageHeader title="Overview" subtitle="Local market state, paper activity, and system health in one view." />
      <div className="metric-grid">
        <Metric label="Best bid" value={book?.bestBid === null || book?.bestBid === undefined ? "—" : formatPrice(book.bestBid, book.priceUnit)} tone="positive" detail="Internal simulated book" />
        <Metric label="Best ask" value={book?.bestAsk === null || book?.bestAsk === undefined ? "—" : formatPrice(book.bestAsk, book.priceUnit)} tone="negative" detail="Internal simulated book" />
        <Metric label="Spread" value={book?.spread === null || book?.spread === undefined ? "—" : formatPrice(book.spread, book.priceUnit)} detail="No implicit currency conversion" />
        <Metric label="Recent executions" value={formatNumber(trades.length)} detail="Bounded recent history" />
      </div>
      <div className="overview-grid">
        <section className="panel">
          <div className="section-heading">
            <div><div className="eyebrow">MARKET WATCH</div><h2>{row?.symbol ?? "No active instrument"}</h2></div>
            {row && <QualityBadge quality={row.quality} />}
          </div>
          {row ? (
            <div className="quote-summary">
              <div><span>Last price</span><strong>{row.lastPrice === null ? "—" : formatPrice(row.lastPrice, "MICRO_INR")}</strong></div>
              <div><span>Source</span><strong>{sourceLabel(row.source)}</strong></div>
              <div><span>Exchange</span><strong>{row.exchange}</strong></div>
              <div><span>Feed quality</span><strong>{row.quality}</strong></div>
            </div>
          ) : <EmptyState title="Waiting for instrument data" detail="The market watch endpoint returned no instruments." />}
          <button className="text-button" onClick={() => onNavigate("Market Watch")}>Open Market Watch →</button>
        </section>
        <section className="panel">
          <div className="section-heading"><div><div className="eyebrow">SYSTEM</div><h2>Service status</h2></div><StatusDot state={health?.state ?? "UNKNOWN"} /></div>
          <div className="health-copy">
            <strong>{health?.state ?? "UNKNOWN"}</strong>
            <span>{health?.message ?? "Waiting for the local service."}</span>
            <span>Storage: {health?.storage ?? "not reported"}</span>
          </div>
          <button className="text-button" onClick={() => onNavigate("System Monitor")}>Inspect system →</button>
        </section>
      </div>
      <section className="panel">
        <div className="section-heading">
          <div><div className="eyebrow">RECENT PAPER EXECUTIONS</div><h2>Time and sales</h2></div>
          <button className="text-button" onClick={() => onNavigate("Paper Trading")}>Paper trading →</button>
        </div>
        <TradeTable trades={recentTrades} unit={book?.priceUnit ?? "TICKS"} />
      </section>
    </>
  );
});

const ActiveOrders = memo(function ActiveOrders({
  orders,
  unit,
  mode,
  onMessage,
}: {
  orders: RestingOrder[];
  unit: PriceUnit;
  mode: ApiMode;
  onMessage: (message: string, error?: boolean) => void;
}) {
  if (orders.length === 0) {
    return <EmptyState title="No open paper orders" detail="Working internal CLOB orders appear with their FIFO sequence and cancel control." />;
  }
  return (
    <div className="table-scroll">
      <table>
        <thead><tr><th>Order ID</th><th>Side</th><th>Price</th><th>Remaining</th><th>FIFO seq</th><th>Status</th><th /></tr></thead>
        <tbody>
          {orders.map((order) => (
            <tr key={order.id}>
              <td className="mono">{order.id}</td>
              <td><span className={"side-label " + order.side.toLowerCase()}>{order.side}</span></td>
              <td className="mono">{formatPrice(order.price, unit)}</td>
              <td className="mono">{formatNumber(order.remainingQuantity)}</td>
              <td className="mono">{order.sequence}</td>
              <td>{order.status}</td>
              <td>
                <button
                  className="subtle-button danger-button"
                  onClick={() => void cancelOrder(order.id, mode).then(
                    () => onMessage("Order " + order.id + " cancelled."),
                    (error: unknown) => onMessage(error instanceof Error ? error.message : "Cancel failed.", true),
                  )}
                  disabled={mode === "disconnected"}
                >
                  Cancel
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
});

const MarketWatchWorkspace = memo(function MarketWatchWorkspace({
  filter,
  onSelect,
}: {
  filter: string;
  onSelect: (id: string) => void;
}) {
  const marketWatch = useWorkstation((state) => state.marketWatch);
  const selectedInstrumentId = useWorkstation((state) => state.selectedInstrumentId);
  const filteredRows = useMemo(() => {
    const query = filter.trim().toLowerCase();
    return marketWatch.filter((row) =>
      query.length === 0 || row.symbol.toLowerCase().includes(query) || row.exchange.toLowerCase().includes(query),
    );
  }, [filter, marketWatch]);

  return (
    <>
      <PageHeader title="Market Watch" subtitle="Instrument quotes with source, quality and event freshness." />
      <MarketWatch rows={filteredRows} selected={selectedInstrumentId} onSelect={onSelect} />
    </>
  );
});

const InstrumentWorkstation = memo(function InstrumentWorkstation({
  onMessage,
}: {
  onMessage: (message: string, error?: boolean) => void;
}) {
  const selectedInstrumentId = useWorkstation((state) => state.selectedInstrumentId);
  const book = useWorkstation((state) => state.book);
  const mode = useWorkstation((state) => state.mode);
  const trades = useWorkstation((state) => state.trades);
  const recentTrades = useMemo(() => trades.slice(0, 20), [trades]);

  return (
    <>
      <PageHeader title="Instrument Workstation" subtitle={selectedInstrumentId ?? "Select an instrument from Market Watch."} />
      <div className="workstation-grid">
        <div className="stack">
          <ChartPanel instrumentId={selectedInstrumentId} mode={mode} />
          <section className="panel">
            <div className="section-heading"><div><div className="eyebrow">TIME AND SALES</div><h2>Recent executions</h2></div></div>
            <TradeTable trades={recentTrades} unit={book?.priceUnit ?? "TICKS"} />
          </section>
        </div>
        <div className="stack">
          <div className="book-grid"><BookPane book={book} side="BUY" /><BookPane book={book} side="SELL" /></div>
          <OrderTicket mode={mode} instrumentId={selectedInstrumentId} onMessage={onMessage} />
        </div>
      </div>
    </>
  );
});

const OrdersScreen = memo(function OrdersScreen({
  onMessage,
}: {
  onMessage: (message: string, error?: boolean) => void;
}) {
  const book = useWorkstation((state) => state.book);
  const mode = useWorkstation((state) => state.mode);
  return (
    <>
      <PageHeader title="Order Book" subtitle="External observed liquidity and internal simulated liquidity are kept separate." />
      <div className="inline-notice">External market depth is not connected to this legacy server.</div>
      <div className="book-grid large-book">
        <BookPane book={book} side="BUY" />
        <BookPane book={book} side="SELL" />
      </div>
      <section className="panel">
        <div className="section-heading"><div><div className="eyebrow">INTERNAL CLOB</div><h2>Active resting queue</h2></div><span className="quiet-label">Price time priority · cancel by order ID</span></div>
        <ActiveOrders orders={book?.restingOrders ?? []} unit={book?.priceUnit ?? "TICKS"} mode={mode} onMessage={onMessage} />
      </section>
    </>
  );
});

const StrategyLab = memo(function StrategyLab() {
  const strategies = useWorkstation((state) => state.strategies);
  const status = useWorkstation((state) => state.optionalStatus.strategies);
  return (
    <>
      <PageHeader title="Strategy Lab" subtitle="Configure and run isolated strategies that can emit intents only through risk controls." />
      {status === "unavailable" && <EndpointNotice endpoint="/api/v1/strategies" />}
      {strategies.length === 0 ? (
        <EmptyState title="No strategy runs available" detail="The current server does not expose strategy runtime state. No sample signals or performance values are shown." />
      ) : (
        <div className="panel table-panel"><div className="table-scroll"><table><thead><tr><th>Strategy</th><th>Version</th><th>Lifecycle</th><th>Last signal</th><th>Last error</th><th>Controls</th></tr></thead><tbody>
          {strategies.map((strategy) => <tr key={strategy.id}><td>{strategy.name}</td><td>{strategy.version}</td><td>{strategy.state}</td><td>{strategy.lastSignal ?? "—"}</td><td>{strategy.error ?? "—"}</td><td><StrategyControls id={strategy.id} /></td></tr>)}
        </tbody></table></div></div>
      )}
      <section className="panel info-panel">
        <div className="eyebrow">AVAILABLE BUILT-INS</div>
        <p>SMA crossover · RSI mean reversion · Bollinger bands · Intraday VWAP reversion · Momentum breakout · Order-book imbalance</p>
        <small>Signal generation remains disabled until a strategy API, fresh data, and risk engine are available.</small>
      </section>
    </>
  );
});

function StrategyControls({ id }: { id: string }) {
  const [message, setMessage] = useState("");
  const [busy, setBusy] = useState(false);
  async function command(action: string) {
    setBusy(true);
    try {
      await fetchOptional("/api/v1/strategies/" + encodeURIComponent(id) + "/commands", {
        method: "POST",
        body: JSON.stringify({ action }),
      });
      setMessage("Command accepted");
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "Command unavailable");
    } finally {
      setBusy(false);
    }
  }
  return <div className="button-row"><button className="subtle-button" disabled={busy} onClick={() => void command("start")}>Start</button><button className="subtle-button" disabled={busy} onClick={() => void command("pause")}>Pause</button><span className="quiet-label">{message}</span></div>;
}

const PaperTrading = memo(function PaperTrading({
  onMessage,
}: {
  onMessage: (message: string, error?: boolean) => void;
}) {
  const book = useWorkstation((state) => state.book);
  const mode = useWorkstation((state) => state.mode);
  const trades = useWorkstation((state) => state.trades);
  const [busy, setBusy] = useState(false);
  async function reset(seed: boolean) {
    setBusy(true);
    try {
      if (mode !== "legacy") throw new Error("Paper account controls require the REST v1 gateway.");
      await resetSimulation(seed);
      await useWorkstation.getState().refresh();
      onMessage(seed ? "Seeded the synthetic demo book." : "Cleared the legacy simulation book.");
    } catch (error) {
      onMessage(error instanceof Error ? error.message : "Simulation reset failed.", true);
    } finally {
      setBusy(false);
    }
  }
  return (
    <>
      <PageHeader title="Paper Trading" subtitle="Every order and fill in this workspace is simulated; no broker order path is present." />
      <div className="paper-grid">
        <OrderTicket mode={mode} instrumentId={book?.instrumentId ?? null} onMessage={onMessage} />
        <section className="panel">
          <div className="section-heading"><div><div className="eyebrow">SIMULATION CONTROLS</div><h2>Internal CLOB session</h2></div><span className="sim-tag">PAPER ONLY</span></div>
          <p className="body-copy">The live market-data gateway and paper account ledger are separate from this compatibility CLOB.</p>
          <div className="button-row">
            <button className="subtle-button" disabled={busy || mode !== "legacy"} onClick={() => void reset(true)}>Seed demo book</button>
            <button className="subtle-button danger-button" disabled={busy || mode !== "legacy"} onClick={() => void reset(false)}>Reset empty book</button>
          </div>
          <div className="inline-notice">The legacy server supports LIMIT and MARKET orders. Advanced types remain disabled until the v1 command gateway is implemented.</div>
        </section>
      </div>
      <section className="panel">
        <div className="section-heading"><div><div className="eyebrow">OPEN ORDERS</div><h2>Working order blotter</h2></div><span className="quiet-label">FIFO sequence is preserved by the C++ engine</span></div>
        <ActiveOrders orders={book?.restingOrders ?? []} unit={book?.priceUnit ?? "TICKS"} mode={mode} onMessage={onMessage} />
      </section>
      <section className="panel">
        <div className="section-heading"><div><div className="eyebrow">EXECUTION BLOTTER</div><h2>Paper fills</h2></div></div>
        <TradeTable trades={trades} unit={book?.priceUnit ?? "TICKS"} />
      </section>
    </>
  );
});

function EndpointNotice({ endpoint }: { endpoint: string }) {
  return <div className="inline-notice">This server build does not provide {endpoint}; BASALT will show an empty state until the corresponding phase is available.</div>;
}

const PortfolioRisk = memo(function PortfolioRisk({
  onMessage,
}: {
  onMessage: (message: string, error?: boolean) => void;
}) {
  const portfolio = useWorkstation((state) => state.portfolio);
  const risk = useWorkstation((state) => state.risk);
  const mode = useWorkstation((state) => state.mode);
  const riskStatus = useWorkstation((state) => state.optionalStatus.risk);
  const portfolioStatus = useWorkstation((state) => state.optionalStatus.portfolio);
  const [busy, setBusy] = useState(false);

  async function killSwitch() {
    if (!window.confirm("Latch the paper-trading kill switch and stop new simulated orders?")) return;
    setBusy(true);
    try {
      await fetchOptional("/api/v1/kill-switch", {
        method: "POST",
        body: JSON.stringify({ scope: "GLOBAL", reason: "Operator action from workstation" }),
      });
      onMessage("Paper kill switch latched.");
    } catch (error) {
      onMessage(error instanceof Error ? error.message : "Kill switch endpoint is unavailable.", true);
    } finally {
      setBusy(false);
    }
  }
  return (
    <>
      <PageHeader title="Portfolio & Risk" subtitle="Account state uses ledger postings and conservative bid/ask marks." action={<button className="danger-button primary-button" onClick={() => void killSwitch()} disabled={busy || mode !== "v1"}>Emergency kill switch</button>} />
      {!portfolio && portfolioStatus === "unavailable" && <EndpointNotice endpoint="/api/v1/portfolio/1" />}
      {!risk && riskStatus === "unavailable" && <EndpointNotice endpoint="/api/v1/risk/1" />}
      <div className="metric-grid">
        <Metric label="Cash balance" value={portfolio ? formatMoney(portfolio.cashMicro) : "—"} detail={portfolio?.accountId ?? "No account data"} />
        <Metric label="Equity" value={portfolio ? formatMoney(portfolio.equityMicro) : "—"} detail="Mark source is reported by the API" />
        <Metric label="Realized P&L" value={portfolio ? formatMoney(portfolio.realizedPnlMicro) : "—"} tone={portfolio?.realizedPnlMicro && portfolio.realizedPnlMicro < 0 ? "negative" : "positive"} />
        <Metric label="Unrealized P&L" value={portfolio ? formatMoney(portfolio.unrealizedPnlMicro) : "—"} detail={portfolio ? "Conservative quote marks" : "No ledger snapshot"} />
      </div>
      <div className="two-column">
        <section className="panel">
          <div className="section-heading"><div><div className="eyebrow">POSITIONS</div><h2>Open exposure</h2></div></div>
          {portfolio?.positions.length ? (
            <div className="table-scroll"><table><thead><tr><th>Instrument</th><th>Qty</th><th>Average cost</th><th>Mark</th></tr></thead><tbody>
              {portfolio.positions.map((position) => <tr key={position.instrumentId}><td>{position.instrumentId}</td><td className="mono">{formatNumber(position.quantity)}</td><td>{formatMoney(position.averageCostMicro)}</td><td>{formatMoney(position.markMicro)}</td></tr>)}
            </tbody></table></div>
          ) : <EmptyState title="No position snapshot" detail="Positions will appear after the portfolio ledger endpoint is available." />}
        </section>
        <section className="panel">
          <div className="section-heading"><div><div className="eyebrow">RISK GATES</div><h2>Limit utilization</h2></div><span className={risk?.killSwitch ? "quality-badge stale" : "quality-badge ok"}>{risk?.killSwitch ? "KILL SWITCH ON" : "STATUS UNKNOWN"}</span></div>
          {risk?.limits.length ? <div className="limit-list">{risk.limits.map((limit) => <div className="limit-row" key={limit.name}><div><strong>{limit.name}</strong><span>{formatNumber(limit.used)} / {formatNumber(limit.limit)} {limit.unit}</span></div><div className="limit-track"><span style={{ width: limit.used !== null && limit.limit ? Math.min(100, Math.max(0, limit.used / limit.limit * 100)) + "%" : "0%" }} /></div></div>)}</div> : <EmptyState title="Risk limits unavailable" detail="No utilization values are presented until the risk service responds." />}
        </section>
      </div>
      {risk?.rejections.length ? <section className="panel"><div className="section-heading"><div><div className="eyebrow">RISK EVENTS</div><h2>Recent rejections</h2></div></div><div className="event-list">{risk.rejections.map((entry, index) => <div className="event-row" key={entry.eventSeq + ":" + index}><span className="mono quiet">#{entry.eventSeq}</span><span className="event-type reject">{entry.code}</span><div className="event-copy"><span>{entry.message}</span></div></div>)}</div></section> : null}
    </>
  );
});

const Backtesting = memo(function Backtesting({
  onMessage,
}: {
  onMessage: (message: string, error?: boolean) => void;
}) {
  const datasets = useWorkstation((state) => state.datasets);
  const datasetStatus = useWorkstation((state) => state.optionalStatus.datasets);
  const [dataset, setDataset] = useState("");
  const [busy, setBusy] = useState(false);
  async function run(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    setBusy(true);
    try {
      const result = await fetchOptional<{ run_id: string }>("/api/v1/backtests", {
        method: "POST",
        body: JSON.stringify({ dataset_id: dataset, strategy_id: "1", split: "chronological", execution_model: "BAR_APPROXIMATION_MODEL" }),
      });
      onMessage("Backtest queued: " + result.run_id);
    } catch (error) {
      onMessage(error instanceof Error ? error.message : "Backtest endpoint unavailable.", true);
    } finally {
      setBusy(false);
    }
  }
  return (
    <>
      <PageHeader title="Backtesting" subtitle="Chronological runs enforce next-bar execution and preserve a reproducibility manifest." />
      {datasetStatus === "unavailable" && <EndpointNotice endpoint="/api/v1/datasets" />}
      <div className="research-grid">
        <section className="panel">
          <div className="section-heading"><div><div className="eyebrow">NEW RUN</div><h2>Backtest configuration</h2></div></div>
          {datasets.length === 0 ? <EmptyState title="No licensed dataset is indexed" detail="Import a permitted local dataset before starting a run." /> : (
            <form className="ticket-form" onSubmit={(event) => void run(event)}>
              <label>Dataset<select value={dataset} onChange={(event) => setDataset(event.target.value)} required><option value="">Select dataset</option>{datasets.map((item) => <option value={item.id} key={item.id}>{item.label} · {item.license}</option>)}</select></label>
              <label>Strategy<select><option>SMA Crossover</option><option>RSI Mean Reversion</option><option>Bollinger Bands</option><option>VWAP Reversion</option><option>Momentum Breakout</option><option>Order-Book Imbalance</option></select></label>
              <label>Execution model<select><option>Bar-Based Approximation</option><option>Order-Book Replay</option></select></label>
              <button className="primary-button" type="submit" disabled={busy}>{busy ? "Queueing…" : "Run backtest"}</button>
            </form>
          )}
        </section>
        <section className="panel">
          <div className="section-heading"><div><div className="eyebrow">RUN HISTORY</div><h2>Results and manifests</h2></div></div>
          <EmptyState title="No completed backtests" detail="Metrics remain blank until a reproducible run is returned by the API." />
        </section>
      </div>
    </>
  );
});

const Replay = memo(function Replay() {
  const [session, setSession] = useState("");
  const [message, setMessage] = useState("No recorded session selected.");
  async function command(action: string, extra: Record<string, unknown> = {}) {
    if (!session) {
      setMessage("Select a stored session before controlling replay.");
      return;
    }
    try {
      await fetchOptional("/api/v1/replay/" + encodeURIComponent(session) + "/commands", {
        method: "POST",
        body: JSON.stringify({ action, ...extra }),
      });
      setMessage("Replay command accepted: " + action);
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "Replay endpoint unavailable.");
    }
  }
  return (
    <>
      <PageHeader title="Market Replay" subtitle="Play recorded events on a virtual clock without modifying the source session." />
      <section className="panel replay-panel">
        <div className="section-heading"><div><div className="eyebrow">REPLAY TRANSPORT</div><h2>Deterministic session</h2></div><span className="quality-badge degraded">NO SESSION</span></div>
        <label>Session ID<input value={session} onChange={(event) => setSession(event.target.value)} placeholder="Stored .blog or imported session ID" /></label>
        <div className="transport-controls">
          <button className="subtle-button" onClick={() => void command("play")}>Play</button>
          <button className="subtle-button" onClick={() => void command("pause")}>Pause</button>
          <button className="subtle-button" onClick={() => void command("step")}>Step +1 event</button>
          <label>Speed<select defaultValue="1x" onChange={(event) => void command("speed", { speed: event.target.value })}><option>1x</option><option>5x</option><option>10x</option><option>MAX</option></select></label>
        </div>
        <label className="seek-label">Seek<input type="range" min="0" max="100" defaultValue="0" disabled={!session} onChange={(event) => void command("seek", { position: Number(event.target.value) })} /></label>
        <div className="inline-notice" role="status">{message}</div>
      </section>
    </>
  );
});

const Analytics = memo(function Analytics() {
  return (
    <>
      <PageHeader title="Analytics" subtitle="Performance metrics are calculated from completed paper or backtest ledgers." />
      <div className="inline-notice">No completed run is selected. BASALT does not show placeholder returns or synthetic performance figures.</div>
      <div className="metric-grid analytics-grid">
        {["Total return", "Benchmark-relative return", "CAGR", "Volatility", "Sharpe", "Sortino", "Maximum drawdown", "Calmar", "Win rate", "Profit factor", "Average win/loss", "Expectancy", "Turnover", "Exposure", "Trade count", "Fee impact", "Slippage impact", "Capacity"].map((name) => <Metric key={name} label={name} value="—" detail="Requires a completed run" />)}
      </div>
    </>
  );
});

const DataSources = memo(function DataSources() {
  const mode = useWorkstation((state) => state.mode);
  const [sources, setSources] = useState<Array<{ id: string; name: string; state: string; source: string; quality: string; subscriptions?: number }>>([]);
  const [available, setAvailable] = useState(false);
  useEffect(() => {
    if (mode !== "v1") {
      setSources([]);
      setAvailable(false);
      return;
    }
    let active = true;
    void fetchOptional<{ providers: typeof sources }>("/api/v1/data-sources").then((response) => {
      if (active) {
        setSources(response.providers ?? []);
        setAvailable(true);
      }
    }).catch(() => {
      if (active) setAvailable(false);
    });
    return () => { active = false; };
  }, [mode]);
  return (
    <>
      <PageHeader title="Data Sources" subtitle="Provider connectivity is separate from internal simulated liquidity." />
      {!available && <EndpointNotice endpoint="/api/v1/data-sources" />}
      {sources.length === 0 ? <EmptyState title="No provider adapters connected" detail="Synthetic and recorded data remain available offline; provider credentials are never displayed." /> : (
        <section className="panel table-panel"><div className="table-scroll"><table><thead><tr><th>Provider</th><th>State</th><th>Source type</th><th>Quality</th><th>Subscriptions</th></tr></thead><tbody>{sources.map((source) => <tr key={source.id}><td>{source.name}</td><td>{source.state}</td><td>{source.source}</td><td><QualityBadge quality={source.quality as FeedQualityState} /></td><td>{formatNumber(source.subscriptions)}</td></tr>)}</tbody></table></div></section>
      )}
      <section className="panel info-panel"><div className="eyebrow">PROVENANCE</div><p>Every canonical event records its source, quality, event time, receive time, and instrument mapping.</p></section>
    </>
  );
});

const SystemMonitor = memo(function SystemMonitor() {
  const mode = useWorkstation((state) => state.mode);
  const metrics = useWorkstation((state) => state.systemMetrics);
  const activity = useWorkstation((state) => state.activity);
  const streamEventCount = useWorkstation((state) => state.streamEvents.length);
  const streamConnected = useWorkstation((state) => state.streamConnected);
  const lastEventSeq = useWorkstation((state) => state.lastEventSeq);
  const replayGapDetected = useWorkstation((state) => state.replayGapDetected);
  const status = useWorkstation((state) => state.optionalStatus.systemMetrics);
  const streamDotState = streamConnected ? "OK" : mode === "legacy" ? "DEGRADED" : "ERROR";
  const streamLabel = streamConnected
    ? "Stream connected"
    : mode === "legacy"
      ? "Stream unavailable in legacy mode"
      : "Stream disconnected";
  return (
    <>
      <PageHeader title="System Monitor" subtitle="Pipeline timings and queue health come from measured local telemetry." />
      <section className="panel stream-status" aria-live="polite">
        <div><StatusDot state={streamDotState} /><strong>{streamLabel}</strong></div>
        <span className="mono">Last event sequence: {lastEventSeq || "—"}</span>
        {replayGapDetected && <span className="warning-copy">Sequence recovery requested; REST snapshot reconciliation is in progress.</span>}
        <span className="quiet-label">Bounded event buffer · {streamEventCount}/512</span>
      </section>
      {status === "unavailable" && <EndpointNotice endpoint="/api/v1/system/metrics" />}
      {metrics ? (
        <>
          <div className="metric-grid"><Metric label="Event rate" value={metrics.eventRate === null ? "—" : formatNumber(metrics.eventRate) + " / s"} /><Metric label="Queue depth" value={formatNumber(metrics.queueDepth)} /><Metric label="Replacements" value={formatNumber(metrics.replacements)} /><Metric label="Dropped durable events" value={formatNumber(metrics.dropped)} tone={metrics.dropped ? "warning" : "neutral"} /></div>
          <section className="panel table-panel"><div className="section-heading"><div><div className="eyebrow">T0–T10 PIPELINE</div><h2>Stage latency</h2></div><span className="quiet-label">Nanoseconds · monotonic clock</span></div><div className="table-scroll"><table><thead><tr><th>Stage</th><th>p50 ns</th><th>p95 ns</th><th>p99 ns</th></tr></thead><tbody>{metrics.stages.map((stage) => <tr key={stage.name}><td>{stage.name}</td><td className="mono">{formatNumber(stage.p50Ns)}</td><td className="mono">{formatNumber(stage.p95Ns)}</td><td className="mono">{formatNumber(stage.p99Ns)}</td></tr>)}</tbody></table></div></section>
        </>
      ) : <EmptyState title="No runtime telemetry" detail="Latency, queue, duplicate and ordering metrics remain unavailable until the system metrics endpoint responds." />}
      <section className="panel"><div className="section-heading"><div><div className="eyebrow">AUDIT TRAIL</div><h2>Recent system events</h2></div></div><ActivityList records={activity} /></section>
    </>
  );
});

const Settings = memo(function Settings() {
  const mode = useWorkstation((state) => state.mode);
  const health = useWorkstation((state) => state.health);
  return (
    <>
      <PageHeader title="Settings" subtitle="Local endpoint and display settings for this workstation." />
      <section className="panel settings-grid">
        <div><div className="eyebrow">API MODE</div><strong>{mode.toUpperCase()}</strong><small>Schema-versioned REST with legacy CLOB compatibility fallback.</small></div>
        <div><div className="eyebrow">BIND DEFAULT</div><strong>127.0.0.1:8080</strong><small>Remote binding requires explicit configuration.</small></div>
        <div><div className="eyebrow">SIMULATION</div><strong>Paper only</strong><small>No brokerage order placement is implemented.</small></div>
        <div><div className="eyebrow">HEALTH</div><strong>{health?.state ?? "UNKNOWN"}</strong><small>{health?.message ?? "No health response"}</small></div>
      </section>
      <section className="panel info-panel"><div className="eyebrow">CREDENTIALS</div><p>Provider tokens are loaded by the local service from environment or protected configuration and are never rendered in this page.</p></section>
    </>
  );
});

export default function App() {
  const mode = useWorkstation((state) => state.mode);
  const connected = useWorkstation((state) => state.connected);
  const error = useWorkstation((state) => state.error);
  const health = useWorkstation((state) => state.health);
  const lastUpdated = useWorkstation((state) => state.lastUpdated);
  const primarySource = useWorkstation((state) => state.marketWatch[0]?.source ?? null);
  const quality = useWorkstation((state) => state.marketWatch[0]?.quality ?? "DISCONNECTED");
  const refresh = useWorkstation((state) => state.refresh);
  const setSelected = useWorkstation((state) => state.setSelectedInstrument);
  const applyFrames = useWorkstation((state) => state.applyStreamFrames);
  const setStreamConnected = useWorkstation((state) => state.setStreamConnected);
  const [workspace, setWorkspace] = useState<WorkspaceName>("Overview");
  const [toast, setToast] = useState<{ message: string; error: boolean } | null>(null);
  const [filter, setFilter] = useState("");

  useEffect(() => {
    void refresh();
    const interval = window.setInterval(() => void refresh(), 1_000);
    return () => window.clearInterval(interval);
  }, [refresh]);

  useEffect(() => {
    if (mode !== "v1") {
      setStreamConnected(false);
      return;
    }
    return connectWebSocket(
      applyFrames,
      setStreamConnected,
      () => useWorkstation.getState().lastEventSeq,
      () => {
        useWorkstation.getState().markReplayGap();
        void refresh();
      },
    );
  }, [mode, applyFrames, setStreamConnected, refresh]);

  useEffect(() => {
    if (!toast) return;
    const timeout = window.setTimeout(() => setToast(null), 6_000);
    return () => window.clearTimeout(timeout);
  }, [toast]);

  const notify = useCallback((message: string, isError = false) => {
    setToast({ message, error: isError });
  }, []);

  const selectInstrument = useCallback((instrumentId: string) => {
    setSelected(instrumentId);
    setWorkspace("Instrument Workstation");
  }, [setSelected]);

  function renderWorkspace() {
    switch (workspace) {
      case "Overview":
        return <Overview onNavigate={setWorkspace} />;
      case "Market Watch":
        return <MarketWatchWorkspace filter={filter} onSelect={selectInstrument} />;
      case "Instrument Workstation":
        return <InstrumentWorkstation onMessage={notify} />;
      case "Order Book":
        return <OrdersScreen onMessage={notify} />;
      case "Strategy Lab":
        return <StrategyLab />;
      case "Paper Trading":
        return <PaperTrading onMessage={notify} />;
      case "Portfolio & Risk":
        return <PortfolioRisk onMessage={notify} />;
      case "Backtesting":
        return <Backtesting onMessage={notify} />;
      case "Market Replay":
        return <Replay />;
      case "Analytics":
        return <Analytics />;
      case "Data Sources":
        return <DataSources />;
      case "System Monitor":
        return <SystemMonitor />;
      case "Settings":
        return <Settings />;
    }
  }

  const sidebarStatusDot = !connected ? "ERROR" : mode === "legacy" ? "DEGRADED" : "OK";
  const sidebarStatusText = !connected
    ? "Waiting for local service"
    : mode === "legacy"
      ? "Legacy CLOB compatibility"
      : "Local service online";

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brand-mark">B</div>
          <div><strong>BASALT</strong><span>RESEARCH WORKSTATION</span></div>
        </div>
        <div className="sidebar-status">
          <div className="status-line"><StatusDot state={sidebarStatusDot} /><span>{sidebarStatusText}</span></div>
          <div className="status-meta">PAPER TRADING · NO LIVE ORDERS</div>
        </div>
        <nav aria-label="Workspaces">
          {NAV.map((group) => (
            <div className="nav-group" key={group.group}>
              <div className="nav-heading">{group.group}</div>
              {group.items.map((item) => (
                <button
                  key={item}
                  className={"nav-item " + (workspace === item ? "selected" : "")}
                  onClick={() => setWorkspace(item)}
                  aria-current={workspace === item ? "page" : undefined}
                >
                  <span className="nav-indicator" />
                  {item}
                </button>
              ))}
            </div>
          ))}
        </nav>
        <div className="sidebar-footer"><span>ENGINE</span><strong>{health?.state ?? "UNKNOWN"}</strong><small>{health?.version ?? "Local build"}</small></div>
      </aside>

      <main className="main-shell">
        <header className="topbar">
          <div className="breadcrumb"><span>Workspace</span><i>/</i><strong>{workspace}</strong></div>
          <div className="topbar-actions">
            <label className="search-box"><span aria-hidden="true">⌕</span><input value={filter} onChange={(event) => setFilter(event.target.value)} placeholder="Filter instruments" aria-label="Filter instruments" /></label>
            <div className="feed-indicator"><StatusDot state={quality} /><span>{sourceLabel(primarySource)}</span><QualityBadge quality={quality} /></div>
            <button className="icon-button" onClick={() => void refresh({ forceV1Probe: true })} aria-label="Refresh market and system state" title="Refresh">↻</button>
          </div>
        </header>

        {error ? (
          <div className="connection-banner" role="alert">
            <span>{error}</span>
            <button className="text-button" onClick={() => void refresh({ forceV1Probe: true })}>Retry</button>
          </div>
        ) : mode === "legacy" ? (
          <div className="connection-banner degraded-banner" role="status">
            <span>Legacy CLOB compatibility mode — REST v1, WebSocket streaming, and historical bars are unavailable on this server. Re-probing v1 every 30s.</span>
            <button className="text-button" onClick={() => void refresh({ forceV1Probe: true })}>Retry</button>
          </div>
        ) : null}
        {toast && <div className={"toast " + (toast.error ? "error" : "success")} role="status">{toast.message}<button onClick={() => setToast(null)} aria-label="Dismiss notification">×</button></div>}

        <div className="workspace-content">{renderWorkspace()}</div>
        <footer className="app-footer">
          <span>BASALT · LOCAL-FIRST MARKET RESEARCH</span>
          <span>{lastUpdated ? "Updated " + new Date(lastUpdated).toLocaleTimeString() : "Awaiting data"} · {mode.toUpperCase()}</span>
        </footer>
      </main>
    </div>
  );
}
