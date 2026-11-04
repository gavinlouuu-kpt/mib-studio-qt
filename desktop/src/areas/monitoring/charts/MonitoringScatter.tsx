// The Monitoring scatter (MON-037..043, MON-045): deformability vs area (µm²) of the valid cells in the
// rolling window, on a canvas, with the Qt ZoomableChartView gestures, fixed default axes from the
// Monitoring Settings, the isoelastic reference curves and legend, pointer tooltips and point hit
// testing. It draws only when its data, view, selection or overlay change, never per hover.
//
// Seams for the work that follows it:
//  - click to view: `onSelectPoint(row)` (a click on a cell; empty space keeps the selection) and
//    `selectedKey` (monitoringRowKey of the selected row; the orange 13 px marker follows it).
//  - the target-group gate: `overlay` draws on the plot with the live axis transform, can take a press
//    on its handles (interceptor) and a hover, and repaints when `version` changes.
//  - the E readout in the tooltip: `emodulusAt(area, deformability)`; absent (or with no value there), the
//    tooltip says area and deformability only.
//  - the density (KDE) layer (PR E2): `density` carries a level per point and the contours; absent, the
//    chart is the plain one.
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import type { MonitoringRow } from "../../../bridge";
import { toData, type Viewport } from "../../../review/charts/chartMath";
import { MARKER } from "../../../review/charts/drawScatter";
import { doubleClickResets } from "../../../review/charts/scatterGestures";
import { insidePlot, nearestPoint } from "../../../review/charts/scatterHitTest";
import type { ScatterDensityModel } from "../density/densityModel";
import { monitoringRowKey, plotTooltip, pointTooltip, type ScatterData } from "../monitoringChartModel";
import type { MonitoringSettings } from "../monitoringSettings";
import { drawMonitoringScatter, scatterViewport, type ReferenceCurve } from "./drawMonitoringScatter";
import { paintCanvas, useChartView, type PressInterceptor } from "./useChartView";

/** What an overlay says about the pointer position it is over (a gate handle): the cursor to show and the
 *  tooltip text, both optional. */
export interface OverlayHover {
  cursor?: string;
  tooltip?: string;
}

export interface ScatterOverlay {
  /** Draw on the plot: clipped to it, over the points, with the current axis transform in `vp`. */
  draw?: (g: CanvasRenderingContext2D, vp: Viewport) => void;
  /** Take a left press (a gate handle) before it becomes a pan or a click. */
  interceptor?: PressInterceptor;
  /** Say what the pointer is over: true (or an OverlayHover) when it is over something of the overlay, which wins
   *  over a cell and over the empty-plot readout: no hand cursor, and the tooltip is `tooltip` or none. A
   *  falsy value means nothing of the overlay is there. */
  hover?: (px: number, py: number, vp: Viewport) => boolean | OverlayHover | null | undefined;
  /** The pointer left the plot or began a pan: whatever hover state the overlay holds is over. */
  onLeave?: () => void;
  /** Changes when the overlay's picture changed and the plot must be redrawn. */
  version?: number | string;
}

export interface MonitoringScatterProps {
  data: ScatterData;
  /** Reference curves, highest modulus first. */
  curves: readonly ReferenceCurve[];
  settings: Pick<MonitoringSettings, "scatterXMin" | "scatterXMax" | "scatterYMin" | "scatterYMax" | "scatterEpoch">;
  selectedKey?: string | null;
  onSelectPoint?: (row: MonitoringRow) => void;
  overlay?: ScatterOverlay;
  emodulusAt?: (area: number, deformability: number) => number | null | undefined;
  /** The density (KDE) layer: memoise it, the plot is redrawn when it changes. */
  density?: ScatterDensityModel;
}

const MIN_SIZE = { w: 420, h: 260 };

export function MonitoringScatter(props: MonitoringScatterProps) {
  const { data, curves, settings, selectedKey, onSelectPoint, overlay, emodulusAt, density } = props;
  const { scatterXMin, scatterXMax, scatterYMin, scatterYMax, scatterEpoch } = settings;
  const defaults = useMemo(
    () => ({ x: { min: scatterXMin, max: scatterXMax }, y: { min: scatterYMin, max: scatterYMax } }),
    [scatterXMin, scatterXMax, scatterYMin, scatterYMax],
  );
  const [hover, setHover] = useState<{ px: number; py: number; over?: OverlayHover } | null>(null);
  const clickHits = useRef<boolean[]>([]);
  const tolerance = Math.max(MARKER, 8);
  const hitAt = (vp: Viewport, px: number, py: number) => nearestPoint(data.xs, data.ys, data.frames, vp, px, py, tolerance);
  const hitRef = useRef(hitAt);
  hitRef.current = hitAt;
  const overlayRef = useRef(overlay);
  overlayRef.current = overlay;
  const selectRef = useRef(onSelectPoint);
  selectRef.current = onSelectPoint;

  const view = useChartView({
    defaults,
    epoch: scatterEpoch,
    viewport: scatterViewport,
    minSize: MIN_SIZE,
    onHover: (px, py, vp) => {
      const over = overlayRef.current?.hover?.(px, py, vp);
      if (over) {
        const o: OverlayHover = over === true ? {} : over;
        return setHover((h) => (h && h.px === px && h.py === py && h.over?.cursor === o.cursor && h.over?.tooltip === o.tooltip ? h : { px, py, over: o }));
      }
      setHover((h) => (h && !h.over && h.px === px && h.py === py ? h : { px, py }));
    },
    onLeave: () => {
      overlayRef.current?.onLeave?.();
      setHover(null);
    },
    onClick: (px, py, vp) => {
      const i = hitRef.current(vp, px, py);
      clickHits.current = [...clickHits.current.slice(-1), i >= 0];
      if (i >= 0) selectRef.current?.(data.rows[i]); // empty space keeps the selection
    },
    shouldReset: (px, py, vp) => {
      const [a = false, b = false] = clickHits.current;
      clickHits.current = [];
      // The second click of the double-click selected a cell already: never zoom out from under it.
      return doubleClickResets(a, b) && hitRef.current(vp, px, py) < 0;
    },
    interceptor: overlay?.interceptor && {
      press: (px, py, vp) => {
        const taken = overlayRef.current?.interceptor?.press(px, py, vp) ?? false;
        // A drag on a handle shows no tooltip (it would stay at the press point); the cursor stays.
        if (taken) setHover((h) => (h?.over?.tooltip ? { ...h, over: { cursor: h.over.cursor } } : h));
        return taken;
      },
      move: (px, py, vp) => overlayRef.current?.interceptor?.move(px, py, vp),
      end: (px, py, vp, completed) => overlayRef.current?.interceptor?.end(px, py, vp, completed),
    },
  });
  const { canvasRef, size, ranges, vp } = view;

  const selected = useMemo(() => {
    if (!selectedKey) return -1;
    return data.rows.findIndex((r) => monitoringRowKey(r) === selectedKey);
  }, [data, selectedKey]);

  const overlayDraw = overlay?.draw, overlayVersion = overlay?.version;
  useEffect(() => {
    paintCanvas(canvasRef.current, size, (g) =>
      drawMonitoringScatter(g, size.w, size.h, { data, curves, ranges, selected, overlay: overlayDraw, density }, { selection: true }),
    );
  }, [size, ranges, data, curves, selected, overlayDraw, overlayVersion, density]);

  // What the pointer is over, worked out at render from the latest data and view. Something of the overlay (a
  // gate handle) wins over a cell and over the empty plot.
  const over = hover?.over;
  const hit = hover && !over ? hitAt(vp, hover.px, hover.py) : -1;
  let tip: string | null = null;
  if (over) tip = over.tooltip ?? null;
  else if (hover && hit >= 0 && hit < data.xs.length) tip = pointTooltip(data.frames[hit], data.xs[hit], data.ys[hit]);
  else if (hover && insidePlot(vp, hover.px, hover.py)) {
    const [area, deformability] = toData(vp, hover.px, hover.py);
    tip = plotTooltip(area, deformability, emodulusAt?.(area, deformability));
  }

  // The tooltip sits right of the pointer, left of it when it would leave the chart, and never outside it: a long text (a
  // gate handle's "... — drag to change") is placed from its measured width, before the browser paints.
  const tipRef = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const el = tipRef.current;
    if (!el || !hover) return;
    const w = el.offsetWidth;
    if (!w) return; // no layout (a test environment): the left-anchored style stands
    let left = hover.px + 14;
    if (left + w > size.w - 2) left = hover.px - 14 - w;
    el.style.left = `${Math.max(2, Math.min(left, size.w - w - 2))}px`;
  });

  return (
    <div className="mon-chart" data-tour="monitoring-scatter">
      <div ref={view.wrapRef} className="mon-chart-wrap mon-scatter-wrap">
        <canvas
          ref={canvasRef}
          className="mon-chart-canvas"
          role="img"
          aria-label={`Deformability versus calibrated area, ${data.xs.length} samples`}
          style={{ cursor: view.panning ? "grabbing" : over?.cursor ?? (hit >= 0 ? "pointer" : "default") }}
          {...view.canvasProps}
        />
        {tip && hover && (
          <div ref={tipRef} className="mon-chart-tip mono" role="tooltip" style={{ left: hover.px + 14, top: hover.py + 12 }}>
            {tip}
          </div>
        )}
      </div>
      <div className="mon-chart-actions">
        <button className="btn" onClick={view.resetZoom} disabled={!view.zoomed} data-tour="monitoring-scatter-reset-zoom">
          Reset zoom
        </button>
        <span className="mon-chart-hint">Wheel zooms (Ctrl: X, Shift: Y), drag pans, double-click on empty space resets.</span>
      </div>
    </div>
  );
}
