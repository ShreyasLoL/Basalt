import { create } from "zustand";
import { fetchOptional, getDashboardSnapshot, V1_REPROBE_INTERVAL_MS } from "./api";
import {
  EMPTY_DASHBOARD,
  type ActivityRecord,
  type DashboardState,
  type DataSourceType,
  type FeedQualityState,
  type MarketRow,
  type StreamEvent,
  type SystemMetrics,
  type TradeRecord,
} from "./types";

const STREAM_EVENT_CAPACITY = 512;
const TRADE_CAPACITY = 200;
const ACTIVITY_CAPACITY = 200;
const SOURCE_TYPES = new Set<DataSourceType>([
  "LIVE_PROVIDER", "DELAYED_PROVIDER", "HISTORICAL", "CACHED", "SYNTHETIC", "REPLAYED", "DERIVED",
]);
const QUALITY_STATES = new Set<FeedQualityState>(["OK", "STALE", "DEGRADED", "DISCONNECTED"]);

function objectValue(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function numberValue(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

function idValue(value: unknown): string | null {
  if (typeof value === "string" && value.length > 0) return value;
  return typeof value === "number" && Number.isSafeInteger(value) ? String(value) : null;
}

function sourceValue(value: unknown): DataSourceType | null {
  return typeof value === "string" && SOURCE_TYPES.has(value as DataSourceType)
    ? value as DataSourceType
    : null;
}

function qualityValue(value: unknown): FeedQualityState | null {
  return typeof value === "string" && QUALITY_STATES.has(value as FeedQualityState)
    ? value as FeedQualityState
    : null;
}

function streamEvent(frame: Record<string, unknown>): StreamEvent | null {
  if (typeof frame.type !== "string") return null;
  const payload = objectValue(frame.payload) ?? {};
  return {
    eventSeq: numberValue(frame.event_seq),
    type: frame.type,
    instrumentId: idValue(frame.instrument_id),
    source: sourceValue(frame.source),
    quality: qualityValue(frame.quality),
    eventTimeNs: numberValue(frame.event_time_ns),
    payload,
  };
}

function parseSystemMetrics(payload: Record<string, unknown>): SystemMetrics | null {
  const rawStages = Array.isArray(payload.stages) ? payload.stages : null;
  if (!rawStages && !("event_rate" in payload) && !("eventRate" in payload)) return null;
  const stages = (rawStages ?? []).flatMap((raw) => {
    const stage = objectValue(raw);
    if (!stage || typeof stage.name !== "string") return [];
    return [{
      name: stage.name,
      p50Ns: numberValue(stage.p50_ns ?? stage.p50Ns),
      p95Ns: numberValue(stage.p95_ns ?? stage.p95Ns),
      p99Ns: numberValue(stage.p99_ns ?? stage.p99Ns),
    }];
  });
  return {
    stages,
    eventRate: numberValue(payload.event_rate ?? payload.eventRate),
    queueDepth: numberValue(payload.queue_depth ?? payload.queueDepth),
    replacements: numberValue(payload.replacements ?? payload.queue_replacements),
    dropped: numberValue(payload.dropped ?? payload.dropped_events),
    duplicates: numberValue(payload.duplicates ?? payload.duplicate_events),
    outOfOrder: numberValue(payload.out_of_order ?? payload.out_of_order_events),
  };
}

function mergeMarketWatch(
  currentRows: MarketRow[],
  snapshotRows: MarketRow[],
  streamHasNewerSeq: boolean,
): MarketRow[] {
  if (snapshotRows.length === 0) return currentRows;
  const currentById = new Map(currentRows.map((row) => [row.instrumentId, row]));
  return snapshotRows.map((snapshotRow) => {
    const currentRow = currentById.get(snapshotRow.instrumentId);
    if (!currentRow) return snapshotRow;
    const currentIsNewerByTime =
      currentRow.eventTimeNs !== null &&
      (snapshotRow.eventTimeNs === null || currentRow.eventTimeNs >= snapshotRow.eventTimeNs);
    if (streamHasNewerSeq || currentIsNewerByTime) {
      return {
        ...snapshotRow,
        priceUnit: currentRow.priceUnit,
        lastPrice: currentRow.lastPrice ?? snapshotRow.lastPrice,
        bidPrice: currentRow.bidPrice ?? snapshotRow.bidPrice,
        askPrice: currentRow.askPrice ?? snapshotRow.askPrice,
        spread: currentRow.spread ?? snapshotRow.spread,
        volume: currentRow.volume ?? snapshotRow.volume,
        source: currentRow.source ?? snapshotRow.source,
        quality: currentRow.quality,
        eventTimeNs: currentRow.eventTimeNs ?? snapshotRow.eventTimeNs,
        receiveTimeNs: currentRow.receiveTimeNs ?? snapshotRow.receiveTimeNs,
      };
    }
    return snapshotRow;
  });
}

function mergeTradesBySequence(
  currentTrades: TradeRecord[],
  snapshotTrades: TradeRecord[],
): TradeRecord[] {
  const byId = new Map<number, TradeRecord>();
  for (const trade of snapshotTrades) {
    byId.set(trade.tradeId, trade);
  }
  for (const trade of currentTrades) {
    byId.set(trade.tradeId, trade);
  }
  return Array.from(byId.values())
    .sort((left, right) => left.tradeId - right.tradeId)
    .slice(-TRADE_CAPACITY);
}

function mergeActivityBySequence(
  currentActivity: ActivityRecord[],
  snapshotActivity: ActivityRecord[],
): ActivityRecord[] {
  const bySeq = new Map<number, ActivityRecord>();
  for (const record of snapshotActivity) {
    bySeq.set(record.eventSeq, record);
  }
  for (const record of currentActivity) {
    bySeq.set(record.eventSeq, record);
  }
  return Array.from(bySeq.values())
    .sort((left, right) => left.eventSeq - right.eventSeq)
    .slice(-ACTIVITY_CAPACITY);
}

export interface RefreshOptions {
  forceV1Probe?: boolean;
}

interface WorkstationStore extends DashboardState {
  optionalStatus: Record<string, "ready" | "unavailable">;
  refresh: (options?: RefreshOptions) => Promise<void>;
  refreshOptional: () => Promise<void>;
  setStreamConnected: (connected: boolean) => void;
  applyStreamFrames: (frames: Record<string, unknown>[]) => void;
  markReplayGap: () => void;
  setSelectedInstrument: (instrumentId: string) => void;
}

export const useWorkstation = create<WorkstationStore>((set, get) => ({
  ...EMPTY_DASHBOARD,
  optionalStatus: {},
  refresh: async (options) => {
    const initialState = get();
    const now = Date.now();
    const skipV1Probe =
      initialState.mode === "legacy" &&
      !options?.forceV1Probe &&
      initialState.lastV1ProbeAt !== null &&
      now - initialState.lastV1ProbeAt < V1_REPROBE_INTERVAL_MS;

    set({ mode: initialState.mode === "connecting" ? "connecting" : initialState.mode, error: null });
    try {
      const probeTimestamp = Date.now();
      const snapshot = await getDashboardSnapshot({ skipV1Probe });
      const current = get();
      const snapshotMaxSeq = Math.max(0, ...snapshot.data.activity.map((item) => item.eventSeq));
      const nextLastEventSeq = Math.max(current.lastEventSeq, snapshotMaxSeq);
      const preserveStreamState =
        snapshot.mode === "v1" &&
        current.mode === "v1" &&
        current.streamConnected &&
        !current.replayGapDetected;
      const streamHasNewerSeq = current.lastEventSeq > snapshotMaxSeq;
      const validSelection =
        current.selectedInstrumentId !== null &&
        snapshot.data.marketWatch.some((row) => row.instrumentId === current.selectedInstrumentId);

      set({
        marketWatch: preserveStreamState
          ? mergeMarketWatch(current.marketWatch, snapshot.data.marketWatch, streamHasNewerSeq)
          : snapshot.data.marketWatch,
        selectedInstrumentId: validSelection
          ? current.selectedInstrumentId
          : snapshot.data.selectedInstrumentId,
        book: snapshot.data.book,
        trades: preserveStreamState
          ? mergeTradesBySequence(current.trades, snapshot.data.trades)
          : snapshot.data.trades,
        activity: preserveStreamState
          ? mergeActivityBySequence(current.activity, snapshot.data.activity)
          : snapshot.data.activity,
        lastEventSeq: snapshot.mode === "v1" ? nextLastEventSeq : current.lastEventSeq,
        mode: snapshot.mode,
        connected: true,
        streamConnected: snapshot.mode === "v1" ? current.streamConnected : false,
        health: snapshot.health,
        lastUpdated: Date.now(),
        lastV1ProbeAt: snapshot.probedV1 ? probeTimestamp : current.lastV1ProbeAt,
        replayGapDetected: false,
        error: null,
      });
      if (snapshot.mode === "v1") await get().refreshOptional();
    } catch (error) {
      set({
        mode: "disconnected",
        connected: false,
        streamConnected: false,
        error: error instanceof Error ? error.message : "Could not load BASALT state",
        health: { state: "ERROR", version: null, storage: null, message: "API unavailable" },
      });
    }
  },
  refreshOptional: async () => {
    if (get().mode !== "v1") return;
    const endpoints = [
      ["strategies", "/api/v1/strategies"],
      ["portfolio", "/api/v1/portfolio/1"],
      ["risk", "/api/v1/risk/1"],
      ["datasets", "/api/v1/datasets"],
      ["systemMetrics", "/api/v1/system/metrics"],
    ] as const;
    const statuses: Record<string, "ready" | "unavailable"> = {};
    for (const [key, path] of endpoints) {
      try {
        const value: unknown = await fetchOptional(path);
        statuses[key] = "ready";
        if (key === "strategies" && value && typeof value === "object" && "strategies" in value) {
          set({ strategies: (value as { strategies: WorkstationStore["strategies"] }).strategies });
        } else if (key === "portfolio") {
          set({ portfolio: value as WorkstationStore["portfolio"] });
        } else if (key === "risk") {
          set({ risk: value as WorkstationStore["risk"] });
        } else if (key === "datasets" && value && typeof value === "object" && "datasets" in value) {
          set({ datasets: (value as { datasets: WorkstationStore["datasets"] }).datasets });
        } else if (key === "systemMetrics") {
          set({ systemMetrics: value as WorkstationStore["systemMetrics"] });
        }
      } catch {
        statuses[key] = "unavailable";
      }
    }
    set({ optionalStatus: statuses });
  },
  setStreamConnected: (streamConnected) => set({ streamConnected }),
  applyStreamFrames: (frames) => set((state) => {
    let marketWatch = state.marketWatch;
    let trades = state.trades;
    let activity = state.activity;
    let systemMetrics = state.systemMetrics;
    let health = state.health;
    let lastEventSeq = state.lastEventSeq;
    let replayGapDetected = state.replayGapDetected;
    const streamEvents = [...state.streamEvents];

    for (const frame of frames) {
      const event = streamEvent(frame);
      if (!event) continue;
      const payload = event.payload;
      if (event.eventSeq !== null) {
        if (event.eventSeq <= lastEventSeq && event.type !== "replay_gap") continue;
        lastEventSeq = Math.max(lastEventSeq, event.eventSeq);
      }
      streamEvents.push(event);

      if (event.type === "replay_gap") {
        replayGapDetected = true;
        activity = [...activity, {
          eventSeq: event.eventSeq ?? lastEventSeq,
          action: "STREAM_GAP",
          status: "SNAPSHOT_REQUIRED",
          message: "The stream reported a sequence gap; a REST snapshot is being requested.",
          eventTimeNs: event.eventTimeNs,
        }].slice(-ACTIVITY_CAPACITY);
      } else if (event.type === "quote" && event.instrumentId !== null) {
        marketWatch = marketWatch.map((row) => {
          if (row.instrumentId !== event.instrumentId) return row;
          const priceUnit = payload.price_unit === "TICKS" ? "TICKS" : "MICRO_INR";
          return {
            ...row,
            priceUnit,
            lastPrice: "last_price_micro" in payload ? numberValue(payload.last_price_micro) : row.lastPrice,
            bidPrice: "bid_micro" in payload ? numberValue(payload.bid_micro) : row.bidPrice,
            askPrice: "ask_micro" in payload ? numberValue(payload.ask_micro) : row.askPrice,
            spread: "spread_micro" in payload ? numberValue(payload.spread_micro) : row.spread,
            volume: "volume" in payload ? numberValue(payload.volume) : row.volume,
            source: event.source ?? row.source,
            quality: event.quality ?? row.quality,
            eventTimeNs: event.eventTimeNs ?? row.eventTimeNs,
            receiveTimeNs: numberValue(frame.receive_time_ns) ?? row.receiveTimeNs,
          };
        });
      } else if (event.type === "execution" && event.instrumentId !== null) {
        const tradeId = numberValue(payload.execution_id ?? payload.trade_id);
        const price = numberValue(payload.price_micro ?? payload.price);
        const quantity = numberValue(payload.quantity);
        const aggressorSide = payload.aggressor_side;
        if (tradeId !== null && price !== null && quantity !== null &&
            (aggressorSide === "BUY" || aggressorSide === "SELL")) {
          const trade: TradeRecord = {
            tradeId,
            instrumentId: event.instrumentId,
            price,
            priceUnit: payload.price_unit === "TICKS" ? "TICKS" : "MICRO_INR",
            quantity,
            buyOrderId: numberValue(payload.buy_order_id) ?? 0,
            sellOrderId: numberValue(payload.sell_order_id) ?? 0,
            aggressorSide,
            eventTimeNs: event.eventTimeNs,
            model: typeof payload.execution_model === "string" ? payload.execution_model : "PAPER",
          };
          trades = [...trades, trade].slice(-TRADE_CAPACITY);
        }
      } else if (event.type === "order" || event.type === "feed_status" || event.type === "error") {
        const status = typeof payload.status === "string" ? payload.status : event.type.toUpperCase();
        const message = typeof payload.message === "string"
          ? payload.message
          : `${event.type.replaceAll("_", " ")} ${typeof payload.order_id === "number" ? `#${payload.order_id}` : "event"}`;
        const record: ActivityRecord = {
          eventSeq: event.eventSeq ?? lastEventSeq,
          action: event.type.toUpperCase(),
          status,
          message,
          eventTimeNs: event.eventTimeNs,
        };
        activity = [...activity, record].slice(-ACTIVITY_CAPACITY);
        if (event.type === "feed_status" && (status === "OK" || status === "DEGRADED")) {
          health = { ...(health ?? { version: null, storage: null }), state: status, message };
        }
      } else if (event.type === "system_metrics") {
        systemMetrics = parseSystemMetrics(payload) ?? systemMetrics;
      }
    }

    return {
      marketWatch,
      trades,
      activity,
      systemMetrics,
      health,
      lastEventSeq,
      replayGapDetected,
      streamEvents: streamEvents.slice(-STREAM_EVENT_CAPACITY),
      lastUpdated: Date.now(),
    };
  }),
  markReplayGap: () => set((state) => ({
    replayGapDetected: true,
    activity: [...state.activity, {
      eventSeq: state.lastEventSeq,
      action: "STREAM_GAP",
      status: "SNAPSHOT_REQUIRED",
      message: "The stream buffer overflowed or contained an invalid frame; a REST snapshot is being requested.",
      eventTimeNs: null,
    }].slice(-ACTIVITY_CAPACITY),
  })),
  setSelectedInstrument: (selectedInstrumentId) => set({ selectedInstrumentId }),
}));
