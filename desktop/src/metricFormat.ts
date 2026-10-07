/** JSON metrics may be absent or non-finite for some processing methods. */
export function metricNumber(value: number | null | undefined, digits = 1, precision = false): string {
  if (typeof value !== "number" || !Number.isFinite(value)) return "—";
  return precision ? Number(value.toPrecision(digits)).toString() : value.toFixed(digits);
}
