import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { connectWebSocket } from "./api";

class FakeWebSocket extends EventTarget {
  static instances: FakeWebSocket[] = [];
  readonly sent: string[] = [];

  constructor(readonly address: string) {
    super();
    FakeWebSocket.instances.push(this);
  }

  send(value: string) { this.sent.push(value); }
  close() { this.dispatchEvent(new Event("close")); }
  open() { this.dispatchEvent(new Event("open")); }
  message(value: Record<string, unknown>) {
    this.dispatchEvent(new MessageEvent("message", { data: JSON.stringify(value) }));
  }
}

describe("WebSocket v1 stream client", () => {
  beforeEach(() => {
    FakeWebSocket.instances = [];
    vi.useFakeTimers();
    vi.stubGlobal("WebSocket", FakeWebSocket);
    vi.stubGlobal("crypto", { randomUUID: () => "request-id" });
  });

  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  it("resumes from the last event, coalesces quotes, preserves executions, and reconnects", async () => {
    const lastSequence = { value: 42 };
    const onFrames = vi.fn((frames: Record<string, unknown>[]) => {
      lastSequence.value = Math.max(lastSequence.value, ...frames.map((frame) => Number(frame.event_seq ?? 0)));
    });
    const onStatus = vi.fn();
    const onGap = vi.fn();
    const dispose = connectWebSocket(onFrames, onStatus, () => lastSequence.value, onGap);
    const first = FakeWebSocket.instances[0];

    expect(first.address).toBe("ws://localhost:3000/api/v1/stream");
    first.open();
    expect(JSON.parse(first.sent[0])).toMatchObject({ type: "subscribe", last_event_seq: 42 });
    first.message({ schema_version: 1, type: "quote", event_seq: 43, instrument_id: 1, payload: { last_price_micro: 100 } });
    first.message({ schema_version: 1, type: "quote", event_seq: 44, instrument_id: 1, payload: { last_price_micro: 101 } });
    first.message({ schema_version: 1, type: "execution", event_seq: 45, instrument_id: 1, payload: { quantity: 2 } });
    await vi.advanceTimersByTimeAsync(50);

    const firstBatch = onFrames.mock.calls[0][0];
    expect(firstBatch).toHaveLength(2);
    expect(firstBatch.find((frame) => frame.type === "quote")).toMatchObject({ event_seq: 44 });
    expect(firstBatch.find((frame) => frame.type === "execution")).toMatchObject({ event_seq: 45 });
    first.message({ schema_version: 1, type: "replay_gap", event_seq: 46, payload: { latest_event_seq: 46 } });
    await vi.advanceTimersByTimeAsync(50);
    expect(onGap).toHaveBeenCalledOnce();

    first.close();
    await vi.advanceTimersByTimeAsync(250);
    const second = FakeWebSocket.instances[1];
    expect(second).toBeDefined();
    second.open();
    expect(JSON.parse(second.sent[0])).toMatchObject({ type: "subscribe", last_event_seq: 46 });
    expect(onStatus).toHaveBeenCalledWith(true);
    dispose();
  });
});
