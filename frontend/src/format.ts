import type { PriceUnit } from "./types";

const integer = new Intl.NumberFormat("en-IN", { maximumFractionDigits: 0 });
const currency = new Intl.NumberFormat("en-IN", { style: "currency", currency: "INR", minimumFractionDigits: 2, maximumFractionDigits: 4 });

export function formatNumber(value: number | null | undefined): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return "—";
  return integer.format(value);
}

export function formatMoney(micro: number | null | undefined): string {
  if (micro === null || micro === undefined || !Number.isFinite(micro)) return "—";
  const rupees = micro / 10_000;
  return currency.format(rupees);
}

export function formatPrice(value: number | null, unit: PriceUnit): string {
  if (value === null) return "—";
  return unit === "MICRO_INR" ? formatMoney(value) : formatNumber(value) + " ticks";
}

export function formatEventTime(nanoseconds: number | null): string {
  return formatNumber(nanoseconds === null ? null : Math.floor(nanoseconds / 1_000_000));
}
