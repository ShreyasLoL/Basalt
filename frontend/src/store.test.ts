import { beforeEach, describe, expect, it } from "vitest";
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
});
