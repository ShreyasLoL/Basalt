export type DataSourceType =
  | "LIVE_PROVIDER"
  | "DELAYED_PROVIDER"
  | "HISTORICAL"
  | "CACHED"
  | "SYNTHETIC"
  | "REPLAYED"
  | "DERIVED";
export type FeedQualityState = "OK" | "STALE" | "DEGRADED" | "DISCONNECTED";
export type PriceUnit = "MICRO_INR" | "TICKS";
export type ApiMode = "connecting" | "v1" | "legacy" | "disconnected";

export interface MarketRow {
  instrumentId: string;
  symbol: string;
  exchange: "NSE" | "BSE" | "SIM" | string;
  priceUnit: PriceUnit;
  lastPrice: number | null;
  bidPrice: number | null;
  askPrice: number | null;
  spread: number | null;
  volume: number | null;
  source: DataSourceType | null;
  quality: FeedQualityState;
  eventTimeNs: number | null;
  receiveTimeNs: number | null;
}

export interface PriceLevel {
  price: number;
  totalVolume: number;
  orderCount: number;
  side: "BUY" | "SELL";
}

export interface RestingOrder {
  id: number;
  side: "BUY" | "SELL";
  type: string;
  price: number;
  remainingQuantity: number;
  sequence: number;
  status: string;
}

export interface BookSnapshot {
  instrumentId: string;
  priceUnit: PriceUnit;
  bids: PriceLevel[];
  asks: PriceLevel[];
  restingOrders: RestingOrder[];
  bestBid: number | null;
  bestAsk: number | null;
  spread: number | null;
}

export interface TradeRecord {
  tradeId: number;
  instrumentId: string;
  price: number;
  priceUnit: PriceUnit;
  quantity: number;
  buyOrderId: number;
  sellOrderId: number;
  aggressorSide: "BUY" | "SELL";
  eventTimeNs: number | null;
  model: string;
}

export interface ActivityRecord {
  eventSeq: number;
  action: string;
  status: string;
  message: string;
  eventTimeNs: number | null;
}

export interface StreamEvent {
  eventSeq: number | null;
  type: string;
  instrumentId: string | null;
  source: DataSourceType | null;
  quality: FeedQualityState | null;
  eventTimeNs: number | null;
  payload: Record<string, unknown>;
}

export interface SystemHealth {
  state: "OK" | "DEGRADED" | "ERROR" | "UNKNOWN";
  version: string | null;
  storage: string | null;
  message: string;
}

export interface StrategySummary {
  id: string;
  name: string;
  version: string;
  state: string;
  lastSignal: string | null;
  error: string | null;
}

export interface PortfolioSummary {
  accountId: string;
  cashMicro: number | null;
  equityMicro: number | null;
  realizedPnlMicro: number | null;
  unrealizedPnlMicro: number | null;
  feesMicro: number | null;
  positions: Array<{
    instrumentId: string;
    quantity: number;
    averageCostMicro: number;
    markMicro: number | null;
  }>;
}

export interface RiskSummary {
  killSwitch: boolean;
  limits: Array<{ name: string; used: number | null; limit: number | null; unit: string }>;
  rejections: Array<{ code: string; message: string; eventSeq: number }>;
}

export interface DatasetSummary {
  id: string;
  label: string;
  source: DataSourceType;
  license: string;
  range: string;
  hash: string;
}

export interface SystemMetrics {
  stages: Array<{ name: string; p50Ns: number | null; p95Ns: number | null; p99Ns: number | null }>;
  eventRate: number | null;
  queueDepth: number | null;
  replacements: number | null;
  dropped: number | null;
  duplicates: number | null;
  outOfOrder: number | null;
}

export interface DashboardState {
  mode: ApiMode;
  connected: boolean;
  streamConnected: boolean;
  lastUpdated: number | null;
  lastV1ProbeAt: number | null;
  error: string | null;
  health: SystemHealth | null;
  marketWatch: MarketRow[];
  selectedInstrumentId: string | null;
  book: BookSnapshot | null;
  trades: TradeRecord[];
  activity: ActivityRecord[];
  streamEvents: StreamEvent[];
  lastEventSeq: number;
  replayGapDetected: boolean;
  strategies: StrategySummary[];
  portfolio: PortfolioSummary | null;
  risk: RiskSummary | null;
  datasets: DatasetSummary[];
  systemMetrics: SystemMetrics | null;
}

export const EMPTY_DASHBOARD: DashboardState = {
  mode: "connecting",
  connected: false,
  streamConnected: false,
  lastUpdated: null,
  lastV1ProbeAt: null,
  error: null,
  health: null,
  marketWatch: [],
  selectedInstrumentId: null,
  book: null,
  trades: [],
  activity: [],
  streamEvents: [],
  lastEventSeq: 0,
  replayGapDetected: false,
  strategies: [],
  portfolio: null,
  risk: null,
  datasets: [],
  systemMetrics: null,
};
