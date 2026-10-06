// Scatter drawing, shared by the on-screen chart (ReviewScatter.tsx) and the
// exported snapshots (chartExport.ts, 1200 × 1200 like the Qt tab), so an
// export shows exactly what the view shows. Pure canvas 2D; no React.
import { levelColor, niceTicks, toPx, type Range, type Viewport } from "./chartMath";

export interface ContourFamily {
  name: string;
  color: string;
  dashed: boolean;
  loops: [number, number][][];
}

export interface IsoCurve {
  emodulus_kpa: number;
  points: [number, number][];
}

export interface ScatterModel {
  xs: Float64Array;
  ys: Float64Array;
  /** Per-point density level (parallel to xs), or null. */
  levels: ArrayLike<number> | null;
  levelCount: number;
  colourByDensity: boolean;
  curves: IsoCurve[];
  contours: ContourFamily[];
  /** Index into xs of the highlighted point, or -1. */
  highlight: number;
  ranges: { x: Range; y: Range };
}

export const SCATTER_MARGIN = { left: 64, top: 30, right: 12, bottom: 44 };
export const LEGEND_W = 158;
export const MARKER = 6; // Qt marker size (diameter, px)
const HIGHLIGHT = 13;
export const POINT_COLOUR = "#209fdf"; // Qt light theme, first series
const HIGHLIGHT_COLOUR = "#f28e2b";
export const CURVE_COLOURS = ["#99ca53", "#f6a625", "#6d5fd5", "#bf593e", "#7f7f7f", "#2bb5a3", "#d55fa8", "#8c6d31", "#5f8dd5", "#c2a400", "#3d9970"];

export function scatterViewport(w: number, h: number, ranges: { x: Range; y: Range }): Viewport {
  return {
    x0: ranges.x.min,
    x1: ranges.x.max,
    y0: ranges.y.min,
    y1: ranges.y.max,
    left: SCATTER_MARGIN.left,
    top: SCATTER_MARGIN.top,
    width: Math.max(10, w - SCATTER_MARGIN.left - SCATTER_MARGIN.right - LEGEND_W),
    height: Math.max(10, h - SCATTER_MARGIN.top - SCATTER_MARGIN.bottom),
  };
}

export function decimalsFor(ticks: number[]): number {
  if (ticks.length < 2) return 2;
  const step = Math.abs(ticks[1] - ticks[0]);
  return Math.min(6, Math.max(0, -Math.floor(Math.log10(step) + 1e-9)));
}

/** Draw the whole chart into a `w` × `h` CSS-pixel area of `g`. */
export function drawScatter(g: CanvasRenderingContext2D, w: number, h: number, m: ScatterModel): void {
  const vp = scatterViewport(w, h, m.ranges);
  const n = m.xs.length;
  g.clearRect(0, 0, w, h);
  g.fillStyle = "#fff";
  g.fillRect(0, 0, w, h);

  const xTicks = niceTicks(m.ranges.x, Math.max(3, Math.floor(vp.width / 90)));
  const yTicks = niceTicks(m.ranges.y, Math.max(3, Math.floor(vp.height / 50)));
  const xd = decimalsFor(xTicks), yd = decimalsFor(yTicks);
  g.font = "11px system-ui, sans-serif";
  g.strokeStyle = "#e6e6e6";
  g.lineWidth = 1;
  g.fillStyle = "#444";
  g.textAlign = "center";
  g.textBaseline = "top";
  for (const t of xTicks) {
    const [px] = toPx(vp, t, m.ranges.y.min);
    g.beginPath();
    g.moveTo(Math.round(px) + 0.5, vp.top);
    g.lineTo(Math.round(px) + 0.5, vp.top + vp.height);
    g.stroke();
    g.fillText(t.toFixed(xd), px, vp.top + vp.height + 4);
  }
  g.textAlign = "right";
  g.textBaseline = "middle";
  for (const t of yTicks) {
    const [, py] = toPx(vp, m.ranges.x.min, t);
    g.beginPath();
    g.moveTo(vp.left, Math.round(py) + 0.5);
    g.lineTo(vp.left + vp.width, Math.round(py) + 0.5);
    g.stroke();
    g.fillText(t.toFixed(yd), vp.left - 6, py);
  }
  g.strokeStyle = "#999";
  g.strokeRect(vp.left + 0.5, vp.top + 0.5, vp.width, vp.height);
  g.fillStyle = "#222";
  g.textAlign = "center";
  g.textBaseline = "alphabetic";
  g.font = "bold 13px system-ui, sans-serif";
  g.fillText("Deformability vs Area (μm²)", vp.left + vp.width / 2, 18);
  g.font = "12px system-ui, sans-serif";
  g.fillText("Area (μm²)", vp.left + vp.width / 2, h - 8);
  g.save();
  g.translate(14, vp.top + vp.height / 2);
  g.rotate(-Math.PI / 2);
  g.fillText("Deformability", 0, 0);
  g.restore();

  // Plot content, clipped to the plot rectangle.
  g.save();
  g.beginPath();
  g.rect(vp.left, vp.top, vp.width, vp.height);
  g.clip();
  const line = (pts: [number, number][], color: string, width: number, dash: number[]) => {
    g.strokeStyle = color;
    g.lineWidth = width;
    g.setLineDash(dash);
    g.beginPath();
    pts.forEach(([x, y], k) => {
      const [px, py] = toPx(vp, x, y);
      if (k === 0) g.moveTo(px, py);
      else g.lineTo(px, py);
    });
    g.stroke();
    g.setLineDash([]);
  };
  m.curves.forEach((cv, k) => line(cv.points, CURVE_COLOURS[k % CURVE_COLOURS.length], 1.5, []));

  // Points: one path per colour; denser levels drawn last (on top).
  const levels = m.colourByDensity && m.levels && m.levels.length === n ? m.levels : null;
  const buckets = new Map<string, Path2D>();
  const order: string[] = [];
  const indices = Array.from({ length: n }, (_, i) => i);
  if (levels) indices.sort((a, b) => levels[a] - levels[b]);
  const r = MARKER / 2;
  for (const i of indices) {
    const x = m.xs[i], y = m.ys[i];
    if (x < vp.x0 || x > vp.x1 || y < vp.y0 || y > vp.y1) continue;
    const color = levels ? levelColor(levels[i], m.levelCount) : POINT_COLOUR;
    let path = buckets.get(color);
    if (!path) {
      path = new Path2D();
      buckets.set(color, path);
      order.push(color);
    }
    const [px, py] = toPx(vp, x, y);
    path.moveTo(px + r, py);
    path.arc(px, py, r, 0, Math.PI * 2);
  }
  for (const color of order) {
    g.fillStyle = color;
    g.fill(buckets.get(color)!);
  }

  for (const fam of m.contours) for (const loop of fam.loops) line(loop, fam.color, 2, fam.dashed ? [7, 5] : []);

  if (m.highlight >= 0 && m.highlight < n) {
    const [px, py] = toPx(vp, m.xs[m.highlight], m.ys[m.highlight]);
    g.beginPath();
    g.arc(px, py, HIGHLIGHT / 2, 0, Math.PI * 2);
    g.fillStyle = HIGHLIGHT_COLOUR;
    g.fill();
    g.strokeStyle = "#fff";
    g.lineWidth = 1.5;
    g.stroke();
  }
  g.restore();

  // Legend (right).
  const entries: { name: string; color: string; dashed?: boolean; line: boolean }[] = [{ name: "Valid Frames", color: POINT_COLOUR, line: false }];
  for (const c of m.contours) entries.push({ name: c.name, color: c.color, dashed: c.dashed, line: true });
  m.curves.forEach((c, k) => entries.push({ name: `${c.emodulus_kpa.toFixed(2)} kPa`, color: CURVE_COLOURS[k % CURVE_COLOURS.length], line: true }));
  const lx = vp.left + vp.width + 14;
  let ly = vp.top + 6;
  g.font = "11px system-ui, sans-serif";
  g.textAlign = "left";
  g.textBaseline = "middle";
  for (const e of entries) {
    if (ly > h - 10) break;
    if (e.line) {
      g.strokeStyle = e.color;
      g.lineWidth = 2;
      g.setLineDash(e.dashed ? [5, 3] : []);
      g.beginPath();
      g.moveTo(lx, ly);
      g.lineTo(lx + 16, ly);
      g.stroke();
      g.setLineDash([]);
    } else {
      g.fillStyle = e.color;
      g.beginPath();
      g.arc(lx + 8, ly, 3.5, 0, Math.PI * 2);
      g.fill();
    }
    g.fillStyle = "#333";
    g.fillText(e.name, lx + 22, ly, LEGEND_W - 30);
    ly += 17;
  }
  if (levels && ly < h - 30) {
    ly += 4;
    g.fillStyle = "#333";
    g.fillText("Density", lx, ly);
    ly += 12;
    const lw = LEGEND_W - 30;
    for (let k = 0; k < lw; k++) {
      g.fillStyle = levelColor((k / (lw - 1)) * (m.levelCount - 1), m.levelCount);
      g.fillRect(lx + k, ly, 1, 9);
    }
    g.fillStyle = "#666";
    g.fillText("low", lx, ly + 18);
    g.textAlign = "right";
    g.fillText("high", lx + lw, ly + 18);
  }
  if (n === 0) {
    g.fillStyle = "#888";
    g.textAlign = "center";
    g.font = "13px system-ui, sans-serif";
    g.fillText("No valid cells to plot", vp.left + vp.width / 2, vp.top + vp.height / 2);
  }
}
