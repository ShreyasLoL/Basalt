import type {
  ActivityRecord,
  ApiMode,
  BookSnapshot,
  DataSourceType,
  DashboardState,
  MarketRow,
  PriceUnit,
  RestingOrder,
  TradeRecord,
} from "./types";

export class ApiError extends Error {
  readonly status: number;

  constructor(message: string, status: number) {
    super(message);
    this.name = "ApiError";
    this.status = status;
  }
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const response = await fetch(path, {
    ...init,
    headers: { "Content-Type": "application/json", ...init.headers },
  });
  const body = (await response.json().catch(() => ({}))) as {
    message?: string;
    error?: string;
  };
  if (!response.ok) {
    throw new ApiError(body.message ?? body.error ?? "BASALT API request failed", response.status);
  }
  return body as T;
}

interface LegacyPriceLevel {
  price: number;
  total_volume: number;
  order_count: number;
}

interface LegacyOrder {
  id: number;
  side: "BUY" | "SELL";
  type: string;
  price: number;
  remaining_quantity: number;
  sequence: number;
  status?: string;
}

interface LegacyTrade {
  trade_id: number;
  price: number;
  quantity: number;
  buy_order_id: number;
  sell_order_id: number;
  aggressor_side: "BUY" | "SELL";
  timestamp_ns?: number;
}

interface LegacyActivity {
  sequence: number;
  action: string;
  status: string;
  message: string;
}

export interface LegacyApiState {
  market: {
    best_bid: number;
    has_best_bid: boolean;
    best_ask: number;
    has_best_ask: boolean;
    spread: number;
    has_spread: boolean;
    last_trade_price: number;
    has_last_trade_price: boolean;
    last_trade_quantity: number;
    total_volume_traded: number;
    active_order_count: number;
  };
  book: {
    bids: LegacyPriceLevel[];
    asks: LegacyPriceLevel[];
    resting_orders: LegacyOrder[];
  };
  trades: LegacyTrade[];
  activity: LegacyActivity[];
}

interface V1HealthResponse {
  schema_version: number;
  state: string;
  version?: string;
  storage?: string;
  message?: string;
}

export interface OrderCommand {
  side: "BUY" | "SELL";
  type: "LIMIT" | "MARKET" | "POST_ONLY" | "STOP_MARKET" | "STOP_LIMIT";
  timeInForce?: "GTC" | "IOC" | "FOK";
  price?: number;
  stopPrice?: number;
  quantity: number;
  accountId?: string;
  instrumentId?: string;
  executionModel?: "INTERNAL_CLOB_MODEL" | "CONSERVATIVE_QUOTE_MODEL" | "BAR_APPROXIMATION_MODEL" | "DEPTH_REPLAY_MODEL";
}

export async function fetchV1Health(): Promise<V1HealthResponse> {
  return request<V1HealthResponse>("/api/v1/health");
}

export async function fetchLegacyState(): Promise<LegacyApiState> {
  return request<LegacyApiState>("/api/state?depth=20");
}

function mapLegacyState(payload: LegacyApiState): Pick<
  DashboardState,
  "marketWatch" | "book" | "trades" | "activity" | "selectedInstrumentId"
> {
  const instrumentId = "SIM:LEGACY";
  const row: MarketRow = {
    instrumentId,
    symbol: "BASALT-SIM",
    exchange: "SIM",
    priceUnit: "TICKS" satisfies PriceUnit,
    lastPrice: payload.market.has_last_trade_price ? payload.market.last_trade_price : null,
    bidPrice: payload.market.has_best_bid ? payload.market.best_bid : null,
    askPrice: payload.market.has_best_ask ? payload.market.best_ask : null,
    spread: payload.market.has_spread ? payload.market.spread : null,
    volume: payload.market.total_volume_traded,
    source: "DERIVED" satisfies DataSourceType,
    quality: "DEGRADED",
    eventTimeNs: null,
    receiveTimeNs: null,
  };
  const mapLevels = (levels: LegacyPriceLevel[], side: "BUY" | "SELL") =>
    levels.map((level) => ({
      price: level.price,
      totalVolume: level.total_volume,
      orderCount: level.order_count,
      side,
    }));
  const orders: RestingOrder[] = payload.book.resting_orders.map((order) => ({
    id: order.id,
    side: order.side,
    type: order.type,
    price: order.price,
    remainingQuantity: order.remaining_quantity,
    sequence: order.sequence,
    status: order.status ?? "WORKING",
  }));
  const book: BookSnapshot = {
    instrumentId,
    priceUnit: "TICKS" satisfies PriceUnit,
    bids: mapLevels(payload.book.bids, "BUY"),
    asks: mapLevels(payload.book.asks, "SELL"),
    restingOrders: orders,
    bestBid: payload.market.has_best_bid ? payload.market.best_bid : null,
    bestAsk: payload.market.has_best_ask ? payload.market.best_ask : null,
    spread: payload.market.has_spread ? payload.market.spread : null,
  };
  const trades: TradeRecord[] = payload.trades.map((trade) => ({
    tradeId: trade.trade_id,
    instrumentId,
    price: trade.price,
    priceUnit: "TICKS",
    quantity: trade.quantity,
    buyOrderId: trade.buy_order_id,
    sellOrderId: trade.sell_order_id,
    aggressorSide: trade.aggressor_side,
    eventTimeNs: trade.timestamp_ns ?? null,
    model: "Internal CLOB (legacy)",
  }));
  const activity: ActivityRecord[] = payload.activity.map((record) => ({
    eventSeq: record.sequence,
    action: record.action,
    status: record.status,
    message: record.message,
    eventTimeNs: null,
  }));
  return {
    marketWatch: [row],
    selectedInstrumentId: instrumentId,
    book,
    trades,
    activity,
  };
}

export const V1_REPROBE_INTERVAL_MS = 30_000;

export interface SnapshotOptions {
  skipV1Probe?: boolean;
}

export async function getDashboardSnapshot(options: SnapshotOptions = {}): Promise<{
  mode: Exclude<ApiMode, "connecting" | "disconnected">;
  health: DashboardState["health"];
  legacy: boolean;
  probedV1: boolean;
  data: Pick<DashboardState, "marketWatch" | "book" | "trades" | "activity" | "selectedInstrumentId">;
}> {
  if (options.skipV1Probe) {
    try {
      const legacyState = await fetchLegacyState();
      return {
        mode: "legacy",
        health: {
          state: "DEGRADED",
          version: null,
          storage: null,
          message: "Legacy CLOB API; canonical feed metadata is unavailable",
        },
        legacy: true,
        probedV1: false,
        data: mapLegacyState(legacyState),
      };
    } catch {
      throw new Error("BASALT API is unavailable. Check the local server and reconnect.");
    }
  }

  try {
    const health = await fetchV1Health();
    const watch = await request<{ instruments: MarketRow[] }>("/api/v1/market/watch");
    const instrumentId = watch.instruments[0]?.instrumentId ?? null;
    const snapshot = instrumentId
      ? await request<{
          book: BookSnapshot;
          trades: TradeRecord[];
          activity: ActivityRecord[];
        }>("/api/v1/instruments/" + encodeURIComponent(instrumentId) + "/snapshot?depth=20")
      : null;
    return {
      mode: "v1",
      health: {
        state: health.state === "OK" ? "OK" : health.state === "DEGRADED" ? "DEGRADED" : "UNKNOWN",
        version: health.version ?? null,
        storage: health.storage ?? null,
        message: health.message ?? "REST v1 connected",
      },
      legacy: false,
      probedV1: true,
      data: {
        marketWatch: watch.instruments,
        selectedInstrumentId: instrumentId,
        book: snapshot?.book ?? null,
        trades: snapshot?.trades ?? [],
        activity: snapshot?.activity ?? [],
      },
    };
  } catch (v1Error) {
    try {
      const legacyState = await fetchLegacyState();
      return {
        mode: "legacy",
        health: {
          state: "DEGRADED",
          version: null,
          storage: null,
          message: "Legacy CLOB API; canonical feed metadata is unavailable",
        },
        legacy: true,
        probedV1: true,
        data: mapLegacyState(legacyState),
      };
    } catch {
      if (v1Error instanceof ApiError && v1Error.status !== 404) throw v1Error;
      throw new Error("BASALT API is unavailable. Check the local server and reconnect.");
    }
  }
}

export async function submitOrder(command: OrderCommand, mode: ApiMode): Promise<{ message: string }> {
  if (mode === "v1") {
    const result = await request<{ order_id: number; status: string; message?: string }>("/api/v1/orders", {
      method: "POST",
      body: JSON.stringify({
        request_id: crypto.randomUUID(),
        side: command.side,
        type: command.type,
        limit_price_micro: command.price,
        stop_price_micro: command.stopPrice,
        quantity: command.quantity,
        account_id: command.accountId ?? "1",
        instrument_id: command.instrumentId ?? "1",
        time_in_force: command.timeInForce ?? "GTC",
        execution_model: command.executionModel ?? "INTERNAL_CLOB_MODEL",
      }),
    });
    return { message: result.message ?? ("Order " + result.order_id + ": " + result.status) };
  }
  if (command.type !== "LIMIT" && command.type !== "MARKET") {
    throw new Error("Advanced order types require the BASALT REST v1 gateway.");
  }
  if (command.timeInForce && command.timeInForce !== "GTC") {
    throw new Error("IOC and FOK require the BASALT REST v1 gateway.");
  }
  const result = await request<{ order_id: number; status: string; message: string }>("/api/orders", {
    method: "POST",
    body: JSON.stringify({
      id: 0,
      side: command.side,
      type: command.type,
      price: command.price ?? 0,
      quantity: command.quantity,
    }),
  });
  return { message: "Order " + result.order_id + ": " + result.status };
}

export async function cancelOrder(orderId: number, mode: ApiMode): Promise<void> {
  const path = mode === "v1" ? "/api/v1/orders/" + orderId : "/api/orders/" + orderId;
  await request(path, { method: "DELETE" });
}

export async function resetSimulation(seedDemo: boolean): Promise<void> {
  await request("/api/reset", { method: "POST", body: JSON.stringify({ seed_demo: seedDemo }) });
}

export async function fetchOptional<T>(path: string, init: RequestInit = {}): Promise<T> {
  return request<T>(path, init);
}

const STREAM_TYPES = new Set([
  "tick", "quote", "ohlc", "depth", "execution", "order", "position", "signal",
  "feed_status", "system_metrics", "replay_gap", "heartbeat", "subscribed", "error",
]);
const QUOTE_BUFFER_LIMIT = 256;
const DURABLE_BUFFER_LIMIT = 1024;
const STREAM_FLUSH_MS = 50;

function validateStreamFrame(value: unknown): Record<string, unknown> | null {
  if (value === null || typeof value !== "object" || Array.isArray(value)) return null;
  const frame = value as Record<string, unknown>;
  if (frame.schema_version !== 1 || typeof frame.type !== "string" || !STREAM_TYPES.has(frame.type)) return null;
  if ("event_seq" in frame && (!Number.isSafeInteger(frame.event_seq) || Number(frame.event_seq) < 0)) return null;
  if (typeof frame.instrument_id === "number" && !Number.isSafeInteger(frame.instrument_id)) return null;
  if ("payload" in frame && frame.payload !== null &&
      (typeof frame.payload !== "object" || Array.isArray(frame.payload))) return null;
  return frame;
}

export function connectWebSocket(
  onFrames: (frames: Record<string, unknown>[]) => void,
  onStatus: (connected: boolean) => void,
  lastEventSequence: () => number,
  onGap: () => void,
): () => void {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const address = protocol + "//" + window.location.host + "/api/v1/stream";
  let closedByClient = false;
  let activeSocket: WebSocket | null = null;
  let retryTimer: number | null = null;
  let flushTimer: number | null = null;
  let retryCount = 0;
  let overflowed = false;
  const pendingDurable: Record<string, unknown>[] = [];
  const pendingQuotes = new Map<string, Record<string, unknown>>();

  function flush() {
    flushTimer = null;
    const frames = [...pendingDurable, ...pendingQuotes.values()];
    pendingDurable.length = 0;
    pendingQuotes.clear();
    const overflow = overflowed;
    overflowed = false;
    frames.sort((left, right) => Number(left.event_seq ?? 0) - Number(right.event_seq ?? 0));
    if (frames.length > 0) onFrames(frames);
    if (overflow || frames.some((frame) => frame.type === "replay_gap")) onGap();
  }

  function scheduleFlush() {
    if (flushTimer === null) flushTimer = window.setTimeout(flush, STREAM_FLUSH_MS);
  }

  function scheduleReconnect() {
    if (closedByClient || retryTimer !== null) return;
    const delay = Math.min(250 * (2 ** Math.min(retryCount, 6)), 15_000);
    retryCount += 1;
    retryTimer = window.setTimeout(() => {
      retryTimer = null;
      openSocket();
    }, delay);
  }

  function openSocket() {
    if (closedByClient) return;
    let socket: WebSocket;
    try {
      socket = new WebSocket(address);
    } catch {
      onStatus(false);
      scheduleReconnect();
      return;
    }
    activeSocket = socket;
    socket.addEventListener("open", () => {
      if (closedByClient || activeSocket !== socket) return;
      onStatus(true);
      const sequence = lastEventSequence();
      socket.send(JSON.stringify({
        schema_version: 1,
        type: "subscribe",
        request_id: crypto.randomUUID(),
        channels: ["quote", "depth", "execution", "order", "feed_status", "system_metrics"],
        instrument_ids: [],
        ...(sequence > 0 ? { last_event_seq: sequence } : {}),
      }));
    });
    socket.addEventListener("message", (event: MessageEvent<string>) => {
      let frame: Record<string, unknown> | null = null;
      try {
        frame = validateStreamFrame(JSON.parse(event.data) as unknown);
      } catch {
        overflowed = true;
        scheduleFlush();
        return;
      }
      if (!frame) {
        overflowed = true;
        scheduleFlush();
        return;
      }
      retryCount = 0;
      if (frame.type === "quote") {
        const key = String(frame.instrument_id ?? "unknown");
        if (!pendingQuotes.has(key) && pendingQuotes.size >= QUOTE_BUFFER_LIMIT) {
          overflowed = true;
        } else {
          pendingQuotes.set(key, frame);
        }
      } else {
        if (pendingDurable.length >= DURABLE_BUFFER_LIMIT) {
          pendingDurable.shift();
          overflowed = true;
        }
        pendingDurable.push(frame);
      }
      scheduleFlush();
    });
    socket.addEventListener("close", () => {
      if (activeSocket !== socket) return;
      activeSocket = null;
      onStatus(false);
      scheduleReconnect();
    });
    socket.addEventListener("error", () => onStatus(false));
  }

  openSocket();
  return () => {
    closedByClient = true;
    if (retryTimer !== null) window.clearTimeout(retryTimer);
    if (flushTimer !== null) window.clearTimeout(flushTimer);
    retryTimer = null;
    flushTimer = null;
    pendingDurable.length = 0;
    pendingQuotes.clear();
    const socket = activeSocket;
    activeSocket = null;
    socket?.close();
  };
}

