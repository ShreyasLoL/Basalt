import { useEffect, useState } from "react";
import { fetchOptional } from "./api";
import { formatMoney, formatNumber } from "./format";

interface Instrument {
  instrumentId: string;
  tickSizeMicro: number;
  lotSize: number;
  quantityStep: number;
  segment: string;
  timezone: string;
  sessions: { openLocal: string; closeLocal: string }[];
  mappingStatus: string;
}

export function InstrumentDetails({ id }: { id: string }) {
  const [instrument, setInstrument] = useState<Instrument | null>(null);
  const [error, setError] = useState("");
  const [attempt, setAttempt] = useState(0);
  useEffect(() => {
    const controller = new AbortController();
    setInstrument(null);
    setError("");
    void fetchOptional<Instrument>("/api/v1/instruments/" + encodeURIComponent(id), { signal: controller.signal })
      .then(data => {
        if (data.instrumentId !== id) throw new Error("Instrument metadata does not match the selection.");
        if (!controller.signal.aborted) setInstrument(data);
      }).catch((cause: unknown) => {
        if (!controller.signal.aborted) setError(cause instanceof Error ? cause.message : "Catalog unavailable.");
      });
    return () => controller.abort();
  }, [id, attempt]);
  return <section className="panel">
    <div className="section-heading"><h2>Contract details</h2><span className="quiet-label">{id}</span></div>
    {error ? <div role="status">{error} <button className="text-button" onClick={() => setAttempt(value => value + 1)}>Retry catalog</button></div>
      : !instrument ? <p role="status">Loading instrument metadata…</p> : <dl className="facts">
        <dt>Segment</dt><dd>{instrument.segment || "—"}</dd>
        <dt>Price tick</dt><dd>{formatMoney(instrument.tickSizeMicro)}</dd>
        <dt>Lot size</dt><dd>{formatNumber(instrument.lotSize)}</dd>
        <dt>Quantity step</dt><dd>{formatNumber(instrument.quantityStep)}</dd>
        <dt>Local sessions</dt><dd>{instrument.sessions?.map(s => s.openLocal + "–" + s.closeLocal).join(", ") || "—"}</dd>
        <dt>Time zone</dt><dd>{instrument.timezone || "—"}</dd>
        <dt>Provider mapping</dt><dd>{instrument.mappingStatus || "—"}</dd>
      </dl>}
  </section>;
}
