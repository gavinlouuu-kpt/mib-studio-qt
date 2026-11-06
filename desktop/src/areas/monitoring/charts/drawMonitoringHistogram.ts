// Canvas drawing of the Monitoring ring histogram (MON-049): the Qt histogram of ExperimentMonitoringTab
// (updateHistogram): bins over the fixed X range, Y from 0 to the fixed maximum, both zoomable. The
// measured value is the ring ratio; Qt titles the chart "Ring Width Distribution", Tauri says ring ratio
// because it is not a physical width (see MonitoringCharts.tsx).
import { niceTicks, toPx, type Viewport } from "../../../review/charts/chartMath";
import type { RingHistogram } from "../monitoringChartModel";
import type { AxisRanges } from "./useChartView";

// The right margin keeps the last X tick label (centred on the plot's right edge) inside the canvas.
export const HISTOGRAM_MARGIN = { left: 52, top: 12, right: 24, bottom: 40 };
export const BAR_COLOUR = "#209fdf"; // Qt light theme, first series

export function histogramViewport(w: number, h: number, ranges: AxisRanges): Viewport {
  return {
    x0: ranges.x.min,
    x1: ranges.x.max,
    y0: ranges.y.min,
    y1: ranges.y.max,
    left: HISTOGRAM_MARGIN.left,
    top: HISTOGRAM_MARGIN.top,
    width: Math.max(10, w - HISTOGRAM_MARGIN.left - HISTOGRAM_MARGIN.right),
    height: Math.max(10, h - HISTOGRAM_MARGIN.top - HISTOGRAM_MARGIN.bottom),
  };
}

export interface MonitoringHistogramModel {
  hist: RingHistogram;
  ranges: AxisRanges;
}

export function drawMonitoringHistogram(g: CanvasRenderingContext2D, w: number, h: number, m: MonitoringHistogramModel): void {
  const vp = histogramViewport(w, h, m.ranges);
  const { hist } = m;
  g.clearRect(0, 0, w, h);
  g.fillStyle = "#fff";
  g.fillRect(0, 0, w, h);

  g.font = "11px system-ui, sans-serif";
  g.lineWidth = 1;
  g.fillStyle = "#444";
  g.textAlign = "right";
  g.textBaseline = "middle";
  // Integer counts only: a frequency axis has no half cells.
  for (const t of niceTicks(m.ranges.y, Math.max(2, Math.floor(vp.height / 40))).filter(Number.isInteger)) {
    const [, py] = toPx(vp, m.ranges.x.min, t);
    g.strokeStyle = "#e6e6e6";
    g.beginPath();
    g.moveTo(vp.left, Math.round(py) + 0.5);
    g.lineTo(vp.left + vp.width, Math.round(py) + 0.5);
    g.stroke();
    g.fillText(String(t), vp.left - 6, py);
  }

  g.save();
  g.beginPath();
  g.rect(vp.left, vp.top, vp.width, vp.height);
  g.clip();
  g.fillStyle = BAR_COLOUR;
  const base = toPx(vp, 0, 0)[1];
  hist.counts.forEach((count, k) => {
    if (count === 0) return;
    const start = hist.min + k * hist.binWidth;
    const end = Math.min(hist.max, start + hist.binWidth);
    if (end <= vp.x0 || start >= vp.x1) return;
    const [x0, top] = toPx(vp, start, count);
    const [x1] = toPx(vp, end, count);
    const gap = x1 - x0 > 3 ? 1 : 0;
    g.fillRect(x0 + gap / 2, top, Math.max(1, x1 - x0 - gap), base - top);
  });
  g.restore();

  g.strokeStyle = "#999";
  g.strokeRect(vp.left + 0.5, vp.top + 0.5, vp.width, vp.height);
  g.fillStyle = "#444";
  g.textAlign = "center";
  g.textBaseline = "top";
  // Six ticks over the visible range (Qt setTickCount(6), format %.2f).
  for (let k = 0; k <= 5; k++) {
    const v = m.ranges.x.min + ((m.ranges.x.max - m.ranges.x.min) * k) / 5;
    g.fillText(v.toFixed(2), toPx(vp, v, 0)[0], vp.top + vp.height + 4);
  }
  g.fillStyle = "#222";
  g.textBaseline = "alphabetic";
  g.font = "12px system-ui, sans-serif";
  g.fillText("Ring ratio", vp.left + vp.width / 2, h - 6);
  g.save();
  g.translate(12, vp.top + vp.height / 2);
  g.rotate(-Math.PI / 2);
  g.fillText("Frequency", 0, 0);
  g.restore();
  if (hist.total === 0) {
    g.fillStyle = "#888";
    g.textAlign = "center";
    g.font = "13px system-ui, sans-serif";
    g.fillText("No ring ratios yet", vp.left + vp.width / 2, vp.top + vp.height / 2);
  }
}
