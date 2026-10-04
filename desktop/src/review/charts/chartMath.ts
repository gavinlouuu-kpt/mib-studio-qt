// Pure chart maths for the review Charts view (plan
// 2026-10-01-standalone-review-app, PR 3). No DOM. Mirrors the Qt tab and
// ZoomableChartView so both shells show and behave alike; unit-tested in
// charts.test.ts.
import { REVIEW_DENSITY } from "../../bridgeContract";

export interface Range {
  min: number;
  max: number;
}

export interface Viewport {
  x0: number;
  x1: number;
  y0: number;
  y1: number;
  left: number;
  top: number;
  width: number;
  height: number;
}

/** Data extent padded by 10 % per axis (Qt generateScatterPlot); the Qt
 * fallback (0–1000 × 0–1) when there is no data. */
export function paddedExtent(xs: ArrayLike<number>, ys: ArrayLike<number>): { x: Range; y: Range } {
  let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
  for (let i = 0; i < xs.length; i++) {
    const x = xs[i], y = ys[i];
    if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
    if (x < x0) x0 = x;
    if (x > x1) x1 = x;
    if (y < y0) y0 = y;
    if (y > y1) y1 = y;
  }
  if (!Number.isFinite(x0)) return { x: { min: 0, max: 1000 }, y: { min: 0, max: 1 } };
  const px = x1 > x0 ? (x1 - x0) * 0.1 : Math.max(1, Math.abs(x0) * 0.1);
  const py = y1 > y0 ? (y1 - y0) * 0.1 : Math.max(0.01, Math.abs(y0) * 0.1);
  return { x: { min: x0 - px, max: x1 + px }, y: { min: y0 - py, max: y1 + py } };
}

/** Wheel zoom around `anchor` (ZoomableChartView: ~10 % per 120-unit notch,
 * no inversion). `deltaY` uses the DOM sign (negative = away = zoom in). */
export function zoomRange(r: Range, anchor: number, deltaY: number): Range {
  const factor = 1 + -deltaY / 1200;
  if (!(factor > 0)) return r;
  const min = anchor - (anchor - r.min) / factor;
  const max = anchor + (r.max - anchor) / factor;
  return min < max ? { min, max } : r;
}

/** Which axes a wheel event zooms (Ctrl: x, Shift: y, over the y-axis labels:
 * y, over the x-axis labels: x, inside the plot: both). */
export function zoomAxes(px: number, py: number, vp: Viewport, ctrl: boolean, shift: boolean): { x: boolean; y: boolean } {
  if (ctrl) return { x: true, y: false };
  if (shift) return { x: false, y: true };
  if (px < vp.left) return { x: false, y: true };
  if (py > vp.top + vp.height) return { x: true, y: false };
  return { x: true, y: true };
}

/** Shift ranges by a pointer drag of (dx, dy) px (content follows the pointer). */
export function panRanges(vp: Viewport, dx: number, dy: number): { x: Range; y: Range } {
  const sx = (vp.x1 - vp.x0) / vp.width;
  const sy = (vp.y1 - vp.y0) / vp.height;
  return {
    x: { min: vp.x0 - dx * sx, max: vp.x1 - dx * sx },
    y: { min: vp.y0 + dy * sy, max: vp.y1 + dy * sy },
  };
}

export const toPx = (vp: Viewport, x: number, y: number): [number, number] => [
  vp.left + ((x - vp.x0) / (vp.x1 - vp.x0)) * vp.width,
  vp.top + ((vp.y1 - y) / (vp.y1 - vp.y0)) * vp.height,
];

export const toData = (vp: Viewport, px: number, py: number): [number, number] => [
  vp.x0 + ((px - vp.left) / vp.width) * (vp.x1 - vp.x0),
  vp.y1 - ((py - vp.top) / vp.height) * (vp.y1 - vp.y0),
];

/** "Nice" axis ticks (1/2/5 × 10^k) covering the range, about `target` of them. */
export function niceTicks(r: Range, target = 6): number[] {
  const span = r.max - r.min;
  if (!(span > 0) || !Number.isFinite(span)) return [];
  const raw = span / Math.max(1, target);
  const mag = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 5, 10].map((m) => m * mag).find((s) => s >= raw) ?? 10 * mag;
  const out: number[] = [];
  for (let v = Math.ceil(r.min / step) * step; v <= r.max + step * 1e-9; v += step) out.push(Number(v.toPrecision(12)));
  return out;
}

export interface Histogram {
  min: number;
  max: number;
  binWidth: number;
  counts: number[];
  maxCount: number;
}

/** Ring-ratio histogram exactly as the Qt tab bins it: valid cells with
 * ringRatio > 0, values clamped to [min, max], 0.5-wide bins, the last bin
 * closed at max. */
export function ringRatioHistogram(values: ArrayLike<number>, min: number, max: number, binWidth = 0.5): Histogram {
  const lo = Number.isFinite(min) ? min : 15;
  const hi = Number.isFinite(max) && max > lo ? max : lo + binWidth;
  const bins = Math.max(1, Math.floor((hi - lo) / binWidth));
  const counts = new Array<number>(bins).fill(0);
  for (let i = 0; i < values.length; i++) {
    const v = values[i];
    if (!(v > 0) || !Number.isFinite(v)) continue;
    const c = Math.min(hi, Math.max(lo, v));
    const b = Math.min(bins - 1, Math.max(0, Math.floor((c - lo) / binWidth)));
    counts[b]++;
  }
  return { min: lo, max: hi, binWidth, counts, maxCount: Math.max(0, ...counts) };
}

/** Level colour on the sequential density ramp (kdeLevelColor:
 * t = level / (levels − 1), linear between the contract stops). */
export function levelColor(level: number, levels: number): string {
  const t = levels > 1 ? level / (levels - 1) : 1;
  return rampColor(t);
}

export function rampColor(t: number): string {
  const stops = REVIEW_DENSITY.ramp_rgb as readonly (readonly number[])[];
  const last = stops.length - 1;
  const c = Math.min(1, Math.max(0, Number.isFinite(t) ? t : 0));
  const pos = c * last;
  const lo = Math.min(Math.floor(pos), last - 1);
  const f = pos - lo;
  const mix = (a: number, b: number) => Math.round(a + (b - a) * f);
  const [r, g, b] = [0, 1, 2].map((k) => mix(stops[lo][k], stops[lo + 1][k]));
  return `rgb(${r}, ${g}, ${b})`;
}

/** A stored / computed KDE core record's loops (µm² × deformability). */
export function recordContours(
  json: string,
): { provisional: boolean; source: string; coreFraction: number; cellCount: number; populationCount: number; loops: [number, number][][] } | null {
  if (!json) return null;
  try {
    const o = JSON.parse(json) as Record<string, unknown>;
    const raw = (o.contours ?? o.loops) as unknown;
    if (!Array.isArray(raw)) return null;
    const loops: [number, number][][] = [];
    for (const loop of raw) {
      if (!Array.isArray(loop)) continue;
      const pts: [number, number][] = [];
      for (const p of loop) {
        if (Array.isArray(p) && p.length >= 2 && Number.isFinite(p[0]) && Number.isFinite(p[1])) pts.push([p[0], p[1]]);
        else if (p && typeof p === "object" && Number.isFinite((p as { x: number }).x)) pts.push([(p as { x: number }).x, (p as { y: number }).y]);
      }
      if (pts.length >= 2) loops.push(pts);
    }
    return {
      provisional: o.provisional !== false,
      source: typeof o.source === "string" ? o.source : "",
      coreFraction: typeof o.core_fraction === "number" ? o.core_fraction : typeof o.coreFraction === "number" ? o.coreFraction : 0.9,
      cellCount: typeof o.cell_count === "number" ? o.cell_count : 0,
      populationCount: typeof o.population_count === "number" ? o.population_count : 0,
      loops,
    };
  } catch {
    return null;
  }
}
