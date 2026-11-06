// Canvas drawing of the Monitoring scatter (MON-037, MON-042, MON-045): the Qt "Deformability vs Area
// (μm²)" chart of ExperimentMonitoringTab. Green valid cells and blue target-group cells over the
// isoelastic reference curves, a legend on the right ("Valid Frames", "Target Group", then each
// "x.xx kPa"), the selected cell as an orange 13 px marker. Pure canvas 2D, no React; the grid, ticks
// and plot rectangle are the Review scatter's (review/charts/drawScatter.ts), whose layout it shares.
import { metricNumber } from "../../../metricFormat";
import { niceTicks, toPx, type Range, type Viewport } from "../../../review/charts/chartMath";
import { CURVE_COLOURS, LEGEND_W, MARKER, decimalsFor, scatterViewport } from "../../../review/charts/drawScatter";
import type { ScatterData } from "../monitoringChartModel";
import type { AxisRanges } from "./useChartView";

export { scatterViewport };

/** Qt: QColor(0, 200, 0) valid, QColor(0, 120, 255) target group, QColor(0xf2, 0x8e, 0x2b) selection. */
export const VALID_COLOUR = "#00c800";
export const TARGET_COLOUR = "#0078ff";
export const SELECTION_COLOUR = "#f28e2b";
export const SELECTION_SIZE = 13;

export interface ReferenceCurve {
  modulus: number;
  points: [number, number][];
}

export interface MonitoringScatterModel {
  data: ScatterData | null;
  /** Highest modulus first, the legend's order (Qt adds the series in reverse). */
  curves: readonly ReferenceCurve[];
  ranges: AxisRanges;
  /** Index into `data` of the selected cell, or -1. */
  selected: number;
  /** A later gate overlay: drawn over the points, under the legend, clipped to the plot, with the live
   *  axis transform in `vp` (use toPx / toData from review/charts/chartMath). */
  overlay?: (g: CanvasRenderingContext2D, vp: Viewport) => void;
}

export interface ScatterDrawOptions {
  /** Draw the selected-cell marker; false for exports (Qt hides it while a chart is grabbed). */
  selection: boolean;
  /** A title above the plot (exports); the on-screen card has its own heading. */
  title?: string;
}

export function curveLabel(modulus: number): string {
  return `${metricNumber(modulus, 2)} kPa`;
}

export function drawMonitoringScatter(g: CanvasRenderingContext2D, w: number, h: number, m: MonitoringScatterModel, opts: ScatterDrawOptions): void {
  const vp = scatterViewport(w, h, m.ranges);
  g.clearRect(0, 0, w, h);
  g.fillStyle = "#fff";
  g.fillRect(0, 0, w, h);

  drawAxes(g, h, vp, m.ranges, opts.title);

  // Plot content, clipped to the plot rectangle.
  g.save();
  g.beginPath();
  g.rect(vp.left, vp.top, vp.width, vp.height);
  g.clip();
  m.curves.forEach((curve, k) => {
    g.strokeStyle = CURVE_COLOURS[k % CURVE_COLOURS.length];
    g.lineWidth = 1.5;
    g.beginPath();
    curve.points.forEach(([x, y], i) => {
      const [px, py] = toPx(vp, x, y);
      if (i === 0) g.moveTo(px, py);
      else g.lineTo(px, py);
    });
    g.stroke();
  });
  const data = m.data;
  const r = MARKER / 2;
  if (data) {
    // Valid cells first, target-group cells on top (Qt's series order).
    for (const group of [0, 1]) {
      g.fillStyle = group ? TARGET_COLOUR : VALID_COLOUR;
      g.beginPath();
      for (let i = 0; i < data.xs.length; i++) {
        if (data.target[i] !== group) continue;
        const x = data.xs[i], y = data.ys[i];
        if (x < vp.x0 || x > vp.x1 || y < vp.y0 || y > vp.y1) continue;
        const [px, py] = toPx(vp, x, y);
        g.moveTo(px + r, py);
        g.arc(px, py, r, 0, Math.PI * 2);
      }
      g.fill();
    }
    if (opts.selection && m.selected >= 0 && m.selected < data.xs.length) {
      const [px, py] = toPx(vp, data.xs[m.selected], data.ys[m.selected]);
      g.beginPath();
      g.arc(px, py, SELECTION_SIZE / 2, 0, Math.PI * 2);
      g.fillStyle = SELECTION_COLOUR;
      g.fill();
      g.strokeStyle = "#fff";
      g.lineWidth = 1.5;
      g.stroke();
    }
  }
  m.overlay?.(g, vp);
  g.restore();

  drawLegend(g, h, vp, m.curves);
  if (!data || data.xs.length === 0) {
    g.fillStyle = "#888";
    g.textAlign = "center";
    g.textBaseline = "alphabetic";
    g.font = "13px system-ui, sans-serif";
    g.fillText("No calibrated valid cells yet", vp.left + vp.width / 2, vp.top + vp.height / 2);
  }
}

function drawAxes(g: CanvasRenderingContext2D, h: number, vp: Viewport, ranges: { x: Range; y: Range }, title: string | undefined) {
  const xTicks = niceTicks(ranges.x, Math.max(3, Math.floor(vp.width / 90)));
  const yTicks = niceTicks(ranges.y, Math.max(3, Math.floor(vp.height / 50)));
  const xd = decimalsFor(xTicks), yd = decimalsFor(yTicks);
  g.font = "11px system-ui, sans-serif";
  g.strokeStyle = "#e6e6e6";
  g.lineWidth = 1;
  g.fillStyle = "#444";
  g.textAlign = "center";
  g.textBaseline = "top";
  for (const t of xTicks) {
    const [px] = toPx(vp, t, ranges.y.min);
    g.beginPath();
    g.moveTo(Math.round(px) + 0.5, vp.top);
    g.lineTo(Math.round(px) + 0.5, vp.top + vp.height);
    g.stroke();
    g.fillText(metricNumber(t, xd), px, vp.top + vp.height + 4);
  }
  g.textAlign = "right";
  g.textBaseline = "middle";
  for (const t of yTicks) {
    const [, py] = toPx(vp, ranges.x.min, t);
    g.beginPath();
    g.moveTo(vp.left, Math.round(py) + 0.5);
    g.lineTo(vp.left + vp.width, Math.round(py) + 0.5);
    g.stroke();
    g.fillText(metricNumber(t, yd), vp.left - 6, py);
  }
  g.strokeStyle = "#999";
  g.strokeRect(vp.left + 0.5, vp.top + 0.5, vp.width, vp.height);
  g.fillStyle = "#222";
  g.textAlign = "center";
  g.textBaseline = "alphabetic";
  if (title) {
    g.font = "bold 13px system-ui, sans-serif";
    g.fillText(title, vp.left + vp.width / 2, 18);
  }
  g.font = "12px system-ui, sans-serif";
  g.fillText("Area (μm²)", vp.left + vp.width / 2, h - 8);
  g.save();
  g.translate(14, vp.top + vp.height / 2);
  g.rotate(-Math.PI / 2);
  g.fillText("Deformability", 0, 0);
  g.restore();
}

/** The legend on the right (Qt: legend aligned right): the two series, then one entry per curve. */
function drawLegend(g: CanvasRenderingContext2D, h: number, vp: Viewport, curves: readonly ReferenceCurve[]) {
  const lx = vp.left + vp.width + 14;
  let ly = vp.top + 6;
  g.font = "11px system-ui, sans-serif";
  g.textAlign = "left";
  g.textBaseline = "middle";
  const entries: { name: string; color: string; line: boolean }[] = [
    { name: "Valid Frames", color: VALID_COLOUR, line: false },
    { name: "Target Group", color: TARGET_COLOUR, line: false },
    ...curves.map((c, k) => ({ name: curveLabel(c.modulus), color: CURVE_COLOURS[k % CURVE_COLOURS.length], line: true })),
  ];
  for (const e of entries) {
    if (ly > h - 10) break;
    if (e.line) {
      g.strokeStyle = e.color;
      g.lineWidth = 2;
      g.beginPath();
      g.moveTo(lx, ly);
      g.lineTo(lx + 16, ly);
      g.stroke();
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
}
