// Monitoring charts (epic #866, MON-036..043, MON-049..051): what the charts plot from a monitoring
// snapshot, and the texts they show. Pure: no React, no DOM.
//
// The window. Qt keeps the 1000 newest valid and the 1000 newest invalid frames and redraws them every
// 500 ms (ExperimentMonitoringTab.h MAX_RECENT_FRAMES, UPDATE_INTERVAL_MS). The backend's monitoring
// ring already holds exactly that (ProcessingService MAX_MONITORING_FRAMES = 1000 per ring, reported as
// MonitoringSnapshot.capacity), and fetch_monitoring_snapshot(maxRows) returns the newest valid rows
// first (oldest to newest), then the newest invalid rows with what is left of maxRows. So one poll with
// 2 x 1000 rows is the whole window; nothing is merged or kept on the UI side between polls.
import type { MonitoringRow, MonitoringSnapshot } from "../../bridge";

/** Rows per kind the charts use (Qt MAX_RECENT_FRAMES). */
export const MONITORING_WINDOW = 1000;
/** maxRows for fetch_monitoring_snapshot: the valid window and the invalid window. */
export const MONITORING_FETCH_ROWS = 2 * MONITORING_WINDOW;

/** A row's identity (run, capture, frame, object), the same string monitoringCells.cellKeyString makes
 *  for a cell; a backend older than ABI 41 sends no run or capture and gets "0" for both. */
export function monitoringRowKey(row: Pick<MonitoringRow, "start_generation" | "capture_epoch" | "frame_index" | "object_id">): string {
  return `${row.start_generation ?? "0"}:${row.capture_epoch ?? "0"}:${row.frame_index}:${row.object_id}`;
}

export interface ScatterData {
  /** Calibrated area, µm² (the row's own analysis-time factor squared times its pixel area). */
  xs: Float64Array;
  ys: Float64Array;
  /** Frame index per point (the hit test's tie-break). */
  frames: Float64Array;
  /** 1 = target group. */
  target: Uint8Array;
  /** The row behind each point. */
  rows: MonitoringRow[];
}

export interface MonitoringChartData {
  scatter: ScatterData;
  /** Valid rows without a usable analysis-time calibration: not plotted, never recalibrated. */
  uncalibrated: number;
  /** Valid rows in the window. */
  validRows: number;
  /** Ring ratios > 0 of the valid rows. */
  ratios: number[];
  /** Stored Young's moduli > 0 of the valid rows (kPa). */
  moduli: number[];
}

const positive = (v: number) => Number.isFinite(v) && v > 0;

/** The charts' input: the newest MONITORING_WINDOW valid rows of the snapshot, split into the scatter
 *  points and the two histograms' samples. */
export function monitoringChartData(rows: readonly MonitoringRow[]): MonitoringChartData {
  const valid = rows.filter((r) => r.valid);
  const window = valid.length > MONITORING_WINDOW ? valid.slice(valid.length - MONITORING_WINDOW) : valid;
  const xs: number[] = [], ys: number[] = [], frames: number[] = [], target: number[] = [], plotted: MonitoringRow[] = [];
  const ratios: number[] = [], moduli: number[] = [];
  let uncalibrated = 0;
  for (const r of window) {
    if (positive(r.ring_ratio)) ratios.push(r.ring_ratio);
    if (positive(r.youngs_modulus)) moduli.push(r.youngs_modulus);
    const factor = r.pixel_to_micron;
    if (typeof factor !== "number" || !positive(factor)) {
      uncalibrated++;
      continue;
    }
    const area = r.area * factor * factor;
    if (!Number.isFinite(area) || !Number.isFinite(r.deformability)) continue;
    xs.push(area);
    ys.push(r.deformability);
    frames.push(r.frame_index);
    target.push(r.target_group ? 1 : 0);
    plotted.push(r);
  }
  return {
    scatter: { xs: Float64Array.from(xs), ys: Float64Array.from(ys), frames: Float64Array.from(frames), target: Uint8Array.from(target), rows: plotted },
    uncalibrated,
    validRows: window.length,
    ratios,
    moduli,
  };
}

export interface RingHistogram {
  min: number;
  max: number;
  binWidth: number;
  counts: number[];
  /** Samples counted (all of them: out-of-range values land in the first / last bin). */
  total: number;
}

/** The Qt Monitoring histogram (updateHistogram): bins = max(1, round((max - min) / binWidth)); a value is
 *  clamped into [min, max], its bin is the truncated (value - min) / binWidth, the last bin takes the rest. */
export function ringHistogram(values: ArrayLike<number>, min: number, max: number, binWidth: number): RingHistogram {
  const width = binWidth > 0 ? binWidth : 1;
  const bins = Math.max(1, Math.round((max - min) / width));
  const counts = new Array<number>(bins).fill(0);
  let total = 0;
  for (let i = 0; i < values.length; i++) {
    const v = values[i];
    if (!Number.isFinite(v)) continue;
    const clamped = Math.min(max, Math.max(min, v));
    const bin = Math.min(bins - 1, Math.max(0, Math.trunc((clamped - min) / width)));
    counts[bin]++;
    total++;
  }
  return { min, max, binWidth: width, counts, total };
}

export interface ExtentHistogram {
  min: number;
  max: number;
  counts: number[];
  total: number;
}

/** The Young's-modulus histogram (Tauri only, MON-051): `bins` equal bins over the data extent. */
export function extentHistogram(values: readonly number[], bins = 10): ExtentHistogram {
  if (!values.length) return { min: 0, max: 1, counts: new Array<number>(bins).fill(0), total: 0 };
  let low = Infinity, high = -Infinity;
  for (const v of values) {
    if (v < low) low = v;
    if (v > high) high = v;
  }
  const [min, max] = low === high ? [low - 0.5, high + 0.5] : [low, high];
  const counts = new Array<number>(bins).fill(0);
  for (const v of values) counts[Math.min(bins - 1, Math.max(0, Math.floor(((v - min) / (max - min)) * bins)))]++;
  return { min, max, counts, total: values.length };
}

/** The newest `count` rows of a snapshot by timestamp, oldest first. A snapshot lists the valid rows
 *  and then the invalid rows, each oldest to newest, so its last `count` entries would be invalid rows
 *  whenever the ring holds any; the latest-rows table wants the newest of both. */
export function latestRows(rows: readonly MonitoringRow[], count: number): MonitoringRow[] {
  const best: MonitoringRow[] = [];
  for (const r of rows) {
    if (best.length === count && r.timestamp_ns < best[0].timestamp_ns) continue;
    let at = best.length;
    while (at > 0 && best[at - 1].timestamp_ns > r.timestamp_ns) at--;
    best.splice(at, 0, r);
    if (best.length > count) best.shift();
  }
  return best;
}

/** A cheap identity of a snapshot's content: the ring only changes by appending or clearing, and
 *  appended counters, held counts and the newest timestamp move with either. Equal fingerprints mean
 *  the rows are the same, so a poll that finds nothing new neither re-renders the Monitoring view nor
 *  redraws the charts. */
export function snapshotFingerprint(s: MonitoringSnapshot | null): string {
  if (!s) return "";
  const last = s.rows[s.rows.length - 1];
  return [
    s.valid, s.monitoring_active, s.capacity, s.valid_held, s.invalid_held, s.valid_appended, s.invalid_appended,
    s.latest_timestamp_ns, s.rows.length, last ? `${last.frame_index}/${last.object_id}` : "",
  ].join("|");
}

export function sameSnapshot(a: MonitoringSnapshot | null, b: MonitoringSnapshot | null): boolean {
  return a === b || (a !== null && b !== null && snapshotFingerprint(a) === snapshotFingerprint(b));
}

/** Pointer tooltip anywhere in the plot (Qt onScatterHover): data coordinates under the pointer, and
 *  the modulus the E table gives there when the caller has one. */
export function plotTooltip(area: number, deformability: number, emodulusKpa?: number | null): string {
  const e = typeof emodulusKpa === "number" && Number.isFinite(emodulusKpa) ? ` · E ${emodulusKpa.toFixed(2)} kPa` : "";
  return `Area ${area.toFixed(1)} µm² · deformability ${deformability.toFixed(3)}${e}`;
}

/** Pointer tooltip over a point (Qt onScatterHover). */
export function pointTooltip(frameIndex: number, area: number, deformability: number): string {
  return `Frame ${frameIndex} · ${area.toFixed(1)} µm² · deformability ${deformability.toFixed(4)} · click to view`;
}
