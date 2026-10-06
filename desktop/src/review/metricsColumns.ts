// Metrics table columns (plan 2026-10-01-standalone-review-app, PR 2): every
// FilterResult column the Qt HdfMetricsModel shows, with a default visible
// set and formatting. Pure; unit-tested in metricsColumns.test.ts.
import type { ReviewRow } from "./reviewBridge";

export interface MetricColumn {
  id: string;
  label: string;
  /** Shown by default. */
  initial: boolean;
  /** Recording files carry index + timestamp only. */
  recording?: boolean;
  format: (r: ReviewRow) => string;
}

const num = (digits: number) => (v: number | null | undefined) =>
  v === null || v === undefined || !Number.isFinite(v) ? "—" : v.toFixed(digits);
const yes = (b: boolean) => (b ? "yes" : "no");

export const METRIC_COLUMNS: MetricColumn[] = [
  { id: "frame_index", label: "Index", initial: true, recording: true, format: (r) => r.frame_index },
  { id: "timestamp_ns", label: "Timestamp (ns)", initial: false, recording: true, format: (r) => r.timestamp_ns },
  { id: "valid", label: "Valid", initial: false, format: (r) => yes(r.valid) },
  { id: "target_group", label: "Target", initial: true, format: (r) => yes(r.target_group) },
  { id: "object_id", label: "Object Id", initial: true, format: (r) => String(r.object_id) },
  { id: "object_count", label: "Objects", initial: false, format: (r) => String(r.object_count) },
  { id: "track_id", label: "Track Id", initial: true, format: (r) => String(r.track_id) },
  { id: "track_first_frame", label: "Track first", initial: false, format: (r) => r.track_first_frame },
  { id: "track_last_frame", label: "Track last", initial: false, format: (r) => r.track_last_frame },
  { id: "track_observation_count", label: "Track obs.", initial: false, format: (r) => String(r.track_observation_count) },
  { id: "area", label: "Area (px²)", initial: true, format: (r) => num(1)(r.area) },
  { id: "area_um2", label: "Area (µm²)", initial: true, format: (r) => num(2)(r.area_um2) },
  { id: "deformability", label: "Deformability", initial: true, format: (r) => num(4)(r.deformability) },
  { id: "area_ratio", label: "Area ratio", initial: false, format: (r) => num(4)(r.area_ratio) },
  { id: "ring_ratio", label: "Ring ratio", initial: true, format: (r) => num(3)(r.ring_ratio) },
  { id: "youngs_modulus", label: "E (kPa)", initial: true, format: (r) => num(2)(r.youngs_modulus) },
  { id: "centroid", label: "Centroid", initial: false, format: (r) => `${num(1)(r.centroid_x)}, ${num(1)(r.centroid_y)}` },
  { id: "bbox", label: "BBox", initial: false, format: (r) => `${num(0)(r.bbox_x)},${num(0)(r.bbox_y)} ${num(0)(r.bbox_width)}×${num(0)(r.bbox_height)}` },
  { id: "touches_border", label: "Touches border", initial: false, format: (r) => yes(r.touches_border) },
  { id: "has_single_inner_contour", label: "Single inner", initial: false, format: (r) => yes(r.has_single_inner_contour) },
  { id: "inner_contour_count", label: "Inner contours", initial: false, format: (r) => String(r.inner_contour_count) },
  { id: "in_range", label: "In range", initial: false, format: (r) => yes(r.in_range) },
  { id: "in_channel", label: "In channel", initial: false, format: (r) => yes(r.in_channel) },
  { id: "laplacian_variance", label: "Laplacian var.", initial: false, format: (r) => num(2)(r.laplacian_variance) },
  { id: "brightness", label: "Brightness q1–q4", initial: false, format: (r) => [r.brightness_q1, r.brightness_q2, r.brightness_q3, r.brightness_q4].map(num(1)).join(" / ") },
];

export const DEFAULT_COLUMNS = METRIC_COLUMNS.filter((c) => c.initial).map((c) => c.id);

/** Columns to render, in table order, for the chosen ids and file kind. */
export function visibleColumns(ids: readonly string[], recordingFile: boolean): MetricColumn[] {
  const wanted = new Set(ids);
  return METRIC_COLUMNS.filter((c) => (recordingFile ? c.recording : wanted.has(c.id)));
}

/** Toggle one column, never leaving the table empty. */
export function toggleColumn(ids: readonly string[], id: string): string[] {
  if (ids.includes(id)) return ids.length > 1 ? ids.filter((x) => x !== id) : [...ids];
  return METRIC_COLUMNS.filter((c) => c.id === id || ids.includes(c.id)).map((c) => c.id);
}

/** Page offset that contains item `index`. */
export function pageOffsetFor(index: number, pageSize: number): number {
  return Math.floor(Math.max(0, index) / pageSize) * pageSize;
}
