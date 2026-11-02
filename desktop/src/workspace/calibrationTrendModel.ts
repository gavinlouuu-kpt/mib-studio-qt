// I11 presentation only. Every metric, limit and verdict comes from a recorded calibration.
// Never derive new QC statistics or reinterpret an old run under current settings.
import type { CalibrationRecord } from "../calibrationRecords";

export const TREND_METRICS = [
  { id: "diameter_cv", label: "Diameter CV", unit: "%", field: "cv_diameter_percent", gate: "max_cv_percent" },
  { id: "objects", label: "Valid objects", unit: "objects", field: "valid_objects", gate: "min_valid_objects" },
  { id: "diameter", label: "Median bead diameter", unit: "px", field: "median_diameter_px" },
  { id: "factor", label: "Proposed pixel → µm", unit: "µm/px", field: "proposed_pixel_to_micron" },
  { id: "focus", label: "Median focus", unit: "Laplacian variance", field: "focus" },
  { id: "flow", label: "Event-rate CV", unit: "%", field: "event_rate" },
] as const;
export type TrendMetricId = typeof TREND_METRICS[number]["id"];
export interface TrendPoint {
  record: CalibrationRecord;
  timestamp: number | null;
  value: number | null;
  unavailable: string;
  limit: number | null;
  passed: boolean | null;
  gateReason: string | null;
}
const obj = (v: unknown): Record<string, unknown> => v && typeof v === "object" && !Array.isArray(v) ? v as Record<string, unknown> : {};
const text = (v: unknown): string => typeof v === "string" && v.trim() ? v : "Not recorded";
const finite = (v: unknown): number | null => typeof v === "number" && Number.isFinite(v) ? v : null;

export function trendSetupKey(r: CalibrationRecord): string {
  const identity = obj(r.identity), method = obj(r.method);
  const fields = [identity.camera, identity.roi, identity.chip, identity.objective, method.method_id, method.revision_id];
  // Unknown identities are never assumed compatible with each other.
  return fields.some(f => typeof f !== "string" || !f || f === "not tracked")
    ? JSON.stringify([r.record_id, ...fields]) : JSON.stringify(fields);
}

export function trendSetupLabel(r: CalibrationRecord): string {
  const i = obj(r.identity), m = obj(r.method);
  return `${text(i.camera)} · ROI ${text(i.roi)} · ${text(i.chip)} · ${text(i.objective)} · ${text(m.method_id)} @ ${text(m.revision_id)}`;
}

/** A calibration list contains latest record versions. Keep a repeated older version from
 * becoming a second measurement if the adapter supplies history as well. */
export function latestTrendRecords(records: readonly CalibrationRecord[]): CalibrationRecord[] {
  const latest = new Map<string, CalibrationRecord>();
  for (const r of records) {
    const prev = latest.get(r.record_id);
    if (!prev || r.version > prev.version || (r.version === prev.version && r.sequence > prev.sequence)) latest.set(r.record_id, r);
  }
  return [...latest.values()];
}

export function calibrationTrend(records: readonly CalibrationRecord[], metric: TrendMetricId): TrendPoint[] {
  const spec = TREND_METRICS.find(m => m.id === metric)!;
  return latestTrendRecords(records).map(record => {
    const results = obj(record.results);
    let value = finite(results[spec.field]);
    let unavailable = value === null ? "Not recorded" : "";
    if (metric === "focus" || metric === "flow") {
      const group = obj(results[spec.field]);
      const expected = metric === "focus" ? "recorded" : "computed";
      value = group.status === expected ? finite(group[metric === "focus" ? "median" : "cv_percent"]) : null;
      unavailable = value === null ? text(group.status) : "";
    }
    const gateId = "gate" in spec ? spec.gate : null;
    const gate = gateId && Array.isArray(record.gates) ? record.gates.find(g => g.id === gateId) : null;
    const timestamp = typeof record.created_at_utc === "string" ? Date.parse(record.created_at_utc) : NaN;
    return { record, timestamp: Number.isFinite(timestamp) ? timestamp : null, value, unavailable,
      limit: gate ? finite(gate.limit) : null,
      passed: gate && typeof gate.pass === "boolean" ? gate.pass : null,
      gateReason: gate && typeof gate.reason === "string" ? gate.reason : null };
  }).sort((a, b) => (a.timestamp ?? Infinity) - (b.timestamp ?? Infinity) || a.record.record_id.localeCompare(b.record.record_id));
}
