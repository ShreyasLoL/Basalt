import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { EMPTY_DASHBOARD, type MarketRow } from "./types";
import { useWorkstation } from "./store";

const marketRow: MarketRow = {
  instrumentId: "NSE:123",
  symbol: "BASALT",
  exchange: "NSE",
  priceUnit: "MICRO_INR",
  lastPrice: null,
  bidPrice: null,
  askPrice: null,
  spread: null,
  volume: null,
  source: "LIVE_PROVIDER",
  quality: "OK",
  eventTimeNs: null,
  receiveTimeNs: null,
};

describe("bounded WebSocket event store", () => {
  beforeEach(() => useWorkstation.setState(EMPTY_DASHBOARD));

  it("applies typed quote frames to the matching instrument", () => {
    useWorkstation.setState({ marketWatch: [marketRow] });
    useWorkstation.getState().applyStreamFrames([{
      schema_version: 1,
      type: "quote",
      event_seq: 21,
      instrument_id: "NSE:123",
      event_time_ns: 100,
      receive_time_ns: 110,
      source: "LIVE_PROVIDER",
      quality: "STALE",
      payload: { last_price_micro: 1_234_500, bid_micro: 1_234_000, ask_micro: 1_235_000, volume: 12 },
    }]);

    const state = useWorkstation.getState();
    expect(state.marketWatch[0]).toMatchObject({
      lastPrice: 1_234_500,
      bidPrice: 1_234_000,
      askPrice: 1_235_000,
      volume: 12,
      quality: "STALE",
      eventTimeNs: 100,
      receiveTimeNs: 110,
    });
    expect(state.lastEventSeq).toBe(21);
  });

  it("bounds retained stream history and records sequence-gap recovery", () => {
    const frames = Array.from({ length: 600 }, (_, index) => ({
      schema_version: 1,
      type: "heartbeat",
      event_seq: index + 1,
    }));
    useWorkstation.getState().applyStreamFrames(frames);
    useWorkstation.getState().applyStreamFrames([{
      schema_version: 1,
      type: "replay_gap",
      event_seq: 601,
      payload: { latest_event_seq: 601 },
    }]);

    const state = useWorkstation.getState();
    expect(state.streamEvents).toHaveLength(512);
    expect(state.lastEventSeq).toBe(601);
    expect(state.replayGapDetected).toBe(true);
    expect(state.activity.at(-1)?.action).toBe("STREAM_GAP");
  });

  afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
  });

  it("merges REST refresh by sequence without regressing lastEventSeq or overwriting newer stream state", async () => {
    useWorkstation.setState({
      ...EMPTY_DASHBOARD,
      mode: "v1",
      connected: true,
      streamConnected: true,
      lastEventSeq: 120,
      replayGapDetected: false,
      marketWatch: [{
        ...marketRow,
        lastPrice: 1_500_000,
        bidPrice: 1_499_000,
        askPrice: 1_501_000,
        volume: 90,
        eventTimeNs: 5_000,
        receiveTimeNs: 5_010,
      }],
      trades: [{
        tradeId: 99,
        instrumentId: "NSE:123",
        price: 1_500_000,
        priceUnit: "MICRO_INR",
        quantity: 10,
        buyOrderId: 11,
        sellOrderId: 12,
        aggressorSide: "BUY",
        eventTimeNs: 5_000,
        model: "PAPER",
      }],
      activity: [{
        eventSeq: 120,
        action: "ORDER",
        status: "FILLED",
        message: "Newer stream fill",
        eventTimeNs: 5_000,
      }],
    });

    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL) => {
      const url = String(input);
      if (url.includes("/api/v1/health")) {
        return new Response(JSON.stringify({ schema_version: 1, state: "OK", version: "1.0.0" }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        });
      }
      if (url.includes("/api/v1/market/watch")) {
        return new Response(JSON.stringify({
          instruments: [{
            ...marketRow,
            lastPrice: 1_400_000,
            bidPrice: 1_399_000,
            askPrice: 1_401_000,
            volume: 40,
            eventTimeNs: 3_000,
            receiveTimeNs: 3_010,
          }],
        }), { status: 200, headers: { "Content-Type": "application/json" } });
      }
      if (url.includes("/snapshot")) {
        return new Response(JSON.stringify({
          book: {
            instrumentId: "NSE:123",
            priceUnit: "MICRO_INR",
            bids: [],
            asks: [],
            restingOrders: [],
            bestBid: 1_499_000,
            bestAsk: 1_501_000,
            spread: 2_000,
          },
          trades: [{
            tradeId: 50,
            instrumentId: "NSE:123",
            price: 1_400_000,
            priceUnit: "MICRO_INR",
            quantity: 5,
            buyOrderId: 1,
            sellOrderId: 2,
            aggressorSide: "SELL",
            eventTimeNs: 3_000,
            model: "PAPER",
          }],
          activity: [{
            eventSeq: 80,
            action: "ORDER",
            status: "NEW",
            message: "Older REST snapshot event",
            eventTimeNs: 3_000,
          }],
        }), { status: 200, headers: { "Content-Type": "application/json" } });
      }
      return new Response(JSON.stringify({}), { status: 404 });
    }));

    await useWorkstation.getState().refresh();
    const state = useWorkstation.getState();

    expect(state.lastEventSeq).toBe(120);
    expect(state.marketWatch[0].lastPrice).toBe(1_500_000);
    expect(state.marketWatch[0].eventTimeNs).toBe(5_000);
    expect(state.trades.map((trade) => trade.tradeId)).toEqual([50, 99]);
    expect(state.activity.map((record) => record.eventSeq)).toEqual([80, 120]);
  });
});
