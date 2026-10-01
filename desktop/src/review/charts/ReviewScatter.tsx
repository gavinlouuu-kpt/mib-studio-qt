// Deformability-vs-area scatter for the review Charts view (plan
// 2026-10-01-standalone-review-app, PR 3) — the Qt HdfReviewTab scatter on
// a canvas: one marker per valid cell (optionally coloured by the backend's
// density level), isoelastic reference curves, stored / computed KDE core
// contours, the selected cell's highlight, and the ZoomableChartView
// gestures (wheel zoom by region / modifier, drag to pan past a 10 px
// threshold, click selects the nearest point, double-click off points
// resets). Hit testing and gestures live in scatterHitTest.ts /
// scatterGestures.ts, shared with the Qt tab through a fixture.
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import { levelColor, niceTicks, paddedExtent, panRanges, toData, toPx, zoomAxes, zoomRange, type Range, type Viewport } from "./chartMath";
import { doubleClickResets, GestureTracker } from "./scatterGestures";
import { nearestPoint } from "./scatterHitTest";
import type { ReviewScatter as ScatterData } from "../reviewBridge";

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

export interface MenuItem {
  label: string;
  onClick: () => void;
  disabled?: boolean;
  checked?: boolean;
}

export interface ReviewScatterProps {
  data: ScatterData | null;
  /** Per-point density level (parallel to `data`), or null. */
  levels: number[] | null;
  levelCount: number;
  colourByDensity: boolean;
  curves: IsoCurve[];
  contours: ContourFamily[];
  /** Selected valid-set position (-1 = none). */
  selected: number;
  /** Changes on every open/close: zoom resets to the data extent. */
  fileKey: string;
  onSelect: (validPosition: number) => void;
  /** Extra context-menu entries after "Reset zoom". */
  menu: MenuItem[];
  /** Exposes resetZoom to the host toolbar. */
  resetRef?: React.MutableRefObject<(() => void) | null>;
}

const MARGIN = { left: 64, top: 30, right: 12, bottom: 44 };
const LEGEND_W = 158;
const MARKER = 6; // Qt marker size (diameter, px)
const HIGHLIGHT = 13;
const POINT_COLOUR = "#209fdf"; // Qt light theme, first series
const HIGHLIGHT_COLOUR = "#f28e2b";
const CURVE_COLOURS = ["#99ca53", "#f6a625", "#6d5fd5", "#bf593e", "#7f7f7f", "#2bb5a3", "#d55fa8", "#8c6d31", "#5f8dd5", "#c2a400", "#3d9970"];

function decimalsFor(ticks: number[]): number {
  if (ticks.length < 2) return 2;
  const step = Math.abs(ticks[1] - ticks[0]);
  return Math.min(6, Math.max(0, -Math.floor(Math.log10(step) + 1e-9)));
}

export function ReviewScatter(props: ReviewScatterProps) {
  const { data, levels, levelCount, colourByDensity, curves, contours, selected, fileKey, onSelect, menu, resetRef } = props;
  const wrap = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const [size, setSize] = useState({ w: 600, h: 400 });
  const gestures = useRef(new GestureTracker());
  const clickHits = useRef<boolean[]>([]);
  const [hover, setHover] = useState<{ i: number; x: number; y: number } | null>(null);
  const [ctxMenu, setCtxMenu] = useState<{ x: number; y: number } | null>(null);

  const points = useMemo(() => {
    const n = data?.valid ? data.area_um2.length : 0;
    const xs = new Float64Array(n), ys = new Float64Array(n), pos = new Float64Array(n);
    const byPos = new Map<number, number>();
    for (let i = 0; i < n; i++) {
      xs[i] = data!.area_um2[i];
      ys[i] = data!.deformability[i];
      pos[i] = Number(data!.valid_position[i]);
      byPos.set(pos[i], i);
    }
    return { n, xs, ys, pos, byPos };
  }, [data]);

  const home = useMemo(() => paddedExtent(points.xs, points.ys), [points]);
  const [ranges, setRanges] = useState<{ x: Range; y: Range }>(home);
  // New file / new data: back to the data extent.
  useEffect(() => setRanges(home), [home, fileKey]);
  const resetZoom = useCallback(() => setRanges(home), [home]);
  useEffect(() => {
    if (resetRef) resetRef.current = resetZoom;
  }, [resetRef, resetZoom]);

  const legendEntries = useMemo(() => {
    const out: { name: string; color: string; dashed?: boolean; line: boolean }[] = [{ name: "Valid Frames", color: POINT_COLOUR, line: false }];
    for (const c of contours) out.push({ name: c.name, color: c.color, dashed: c.dashed, line: true });
    curves.forEach((c, k) => out.push({ name: `${c.emodulus_kpa.toFixed(2)} kPa`, color: CURVE_COLOURS[k % CURVE_COLOURS.length], line: true }));
    return out;
  }, [contours, curves]);

  const vp: Viewport = useMemo(
    () => ({
      x0: ranges.x.min,
      x1: ranges.x.max,
      y0: ranges.y.min,
      y1: ranges.y.max,
      left: MARGIN.left,
      top: MARGIN.top,
      width: Math.max(10, size.w - MARGIN.left - MARGIN.right - LEGEND_W),
      height: Math.max(10, size.h - MARGIN.top - MARGIN.bottom),
    }),
    [ranges, size],
  );

  useLayoutEffect(() => {
    const el = wrap.current;
    if (!el) return;
    const measure = () => setSize({ w: Math.max(200, el.clientWidth), h: Math.max(160, el.clientHeight) });
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // ---- drawing -------------------------------------------------------------
  useEffect(() => {
    const c = canvas.current;
    if (!c) return;
    const dpr = window.devicePixelRatio || 1;
    c.width = Math.round(size.w * dpr);
    c.height = Math.round(size.h * dpr);
    c.style.width = `${size.w}px`;
    c.style.height = `${size.h}px`;
    const g = c.getContext("2d");
    if (!g) return;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, size.w, size.h);
    g.fillStyle = "#fff";
    g.fillRect(0, 0, size.w, size.h);

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
      g.fillText(t.toFixed(xd), px, vp.top + vp.height + 4);
    }
    g.textAlign = "right";
    g.textBaseline = "middle";
    for (const t of yTicks) {
      const [, py] = toPx(vp, ranges.x.min, t);
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
    g.fillText("Area (μm²)", vp.left + vp.width / 2, size.h - 8);
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
    curves.forEach((cv, k) => line(cv.points, CURVE_COLOURS[k % CURVE_COLOURS.length], 1.5, []));

    // Points: one path per colour; denser levels drawn last (on top).
    const useLevels = colourByDensity && levels && levels.length === points.n;
    const buckets = new Map<string, Path2D>();
    const order: string[] = [];
    const indices = Array.from({ length: points.n }, (_, i) => i);
    if (useLevels) indices.sort((a, b) => levels![a] - levels![b]);
    const r = MARKER / 2;
    for (const i of indices) {
      const x = points.xs[i], y = points.ys[i];
      if (x < vp.x0 || x > vp.x1 || y < vp.y0 || y > vp.y1) continue;
      const color = useLevels ? levelColor(levels![i], levelCount) : POINT_COLOUR;
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

    for (const fam of contours) for (const loop of fam.loops) line(loop, fam.color, 2, fam.dashed ? [7, 5] : []);

    const hi = points.byPos.get(selected);
    if (hi !== undefined) {
      const [px, py] = toPx(vp, points.xs[hi], points.ys[hi]);
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
    const lx = vp.left + vp.width + 14;
    let ly = vp.top + 6;
    g.font = "11px system-ui, sans-serif";
    g.textAlign = "left";
    g.textBaseline = "middle";
    for (const e of legendEntries) {
      if (ly > size.h - 10) break;
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
    if (useLevels && ly < size.h - 30) {
      ly += 4;
      g.fillStyle = "#333";
      g.fillText("Density", lx, ly);
      ly += 12;
      const w = LEGEND_W - 30;
      for (let k = 0; k < w; k++) {
        g.fillStyle = levelColor((k / (w - 1)) * (levelCount - 1), levelCount);
        g.fillRect(lx + k, ly, 1, 9);
      }
      g.fillStyle = "#666";
      g.fillText("low", lx, ly + 18);
      g.textAlign = "right";
      g.fillText("high", lx + w, ly + 18);
    }
    if (points.n === 0) {
      g.fillStyle = "#888";
      g.textAlign = "center";
      g.font = "13px system-ui, sans-serif";
      g.fillText("No valid cells to plot", vp.left + vp.width / 2, vp.top + vp.height / 2);
    }
  }, [size, ranges, vp, points, levels, levelCount, colourByDensity, curves, contours, selected, legendEntries]);

  // ---- interaction ---------------------------------------------------------
  const local = (e: { clientX: number; clientY: number }): [number, number] => {
    const rect = canvas.current!.getBoundingClientRect();
    return [e.clientX - rect.left, e.clientY - rect.top];
  };
  const hitAt = useCallback(
    (px: number, py: number) => nearestPoint(points.xs, points.ys, points.pos, vp, px, py, Math.max(MARKER, 8)),
    [points, vp],
  );

  // Wheel needs a non-passive listener to keep the page from scrolling.
  useEffect(() => {
    const c = canvas.current;
    if (!c) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const [px, py] = local(e);
      const axes = zoomAxes(px, py, vp, e.ctrlKey, e.shiftKey);
      const delta = e.deltaMode === 1 ? e.deltaY * 40 : e.deltaY;
      const [ax, ay] = toData(vp, Math.min(Math.max(px, vp.left), vp.left + vp.width), Math.min(Math.max(py, vp.top), vp.top + vp.height));
      setRanges((r) => ({ x: axes.x ? zoomRange(r.x, ax, delta) : r.x, y: axes.y ? zoomRange(r.y, ay, delta) : r.y }));
    };
    c.addEventListener("wheel", onWheel, { passive: false });
    return () => c.removeEventListener("wheel", onWheel);
  }, [vp]);

  const onPointerDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (e.button === 1) e.preventDefault(); // no autoscroll / paste
    setCtxMenu(null);
    const [px, py] = local(e);
    gestures.current.down(px, py, e.button);
    if (gestures.current.active) e.currentTarget.setPointerCapture(e.pointerId);
  };
  const onPointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const [px, py] = local(e);
    const a = gestures.current.move(px, py, e.buttons);
    if (a.kind === "pan") {
      setHover(null);
      setRanges((r) => panRanges({ ...vp, x0: r.x.min, x1: r.x.max, y0: r.y.min, y1: r.y.max }, a.dx, a.dy));
    } else if (a.kind === "hover") {
      const i = hitAt(px, py);
      setHover((h) => (i < 0 ? null : h && h.i === i ? h : { i, x: px, y: py }));
    }
  };
  const onPointerUp = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const [px, py] = local(e);
    const a = gestures.current.up(px, py, e.button);
    if (a.kind !== "click" || a.button !== 0) return;
    const i = hitAt(px, py);
    clickHits.current = [...clickHits.current.slice(-1), i >= 0];
    if (i >= 0) onSelect(points.pos[i]); // empty space keeps the selection
  };
  const onDoubleClick = () => {
    const [a = false, b = false] = clickHits.current;
    clickHits.current = [];
    if (doubleClickResets(a, b)) resetZoom();
  };

  useEffect(() => {
    if (!ctxMenu) return;
    const close = (e: Event) => {
      if (e instanceof KeyboardEvent && e.key !== "Escape") return;
      setCtxMenu(null);
    };
    window.addEventListener("pointerdown", close);
    window.addEventListener("keydown", close);
    window.addEventListener("blur", close);
    return () => {
      window.removeEventListener("pointerdown", close);
      window.removeEventListener("keydown", close);
      window.removeEventListener("blur", close);
    };
  }, [ctxMenu]);

  const hovered = hover && hover.i < points.n ? hover : null;
  const items: MenuItem[] = [{ label: "Reset zoom", onClick: resetZoom }, ...menu];

  return (
    <div ref={wrap} className="scatter-wrap">
      <canvas
        ref={canvas}
        className="scatter-canvas"
        style={{ cursor: gestures.current.isPanning ? "grabbing" : hovered ? "pointer" : "default" }}
        aria-label="Deformability vs area scatter"
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onPointerCancel={() => gestures.current.cancel()}
        onPointerLeave={() => setHover(null)}
        onDoubleClick={onDoubleClick}
        onContextMenu={(e) => {
          e.preventDefault();
          gestures.current.cancel();
          const [x, y] = local(e);
          setCtxMenu({ x, y });
        }}
      />
      {hovered && (
        <div className="scatter-tip mono" style={{ left: hovered.x + 14, top: hovered.y + 12 }}>
          Frame {data?.frame_index[hovered.i]} · {points.xs[hovered.i].toFixed(1)} µm² · deformability {points.ys[hovered.i].toFixed(4)}
        </div>
      )}
      {ctxMenu && (
        <div className="menu-popup scatter-menu" role="menu" style={{ left: ctxMenu.x, top: ctxMenu.y }} onPointerDown={(e) => e.stopPropagation()}>
          {items.map((it) => (
            <button
              key={it.label}
              role="menuitem"
              disabled={it.disabled}
              onClick={() => {
                setCtxMenu(null);
                it.onClick();
              }}
            >
              {it.checked !== undefined ? (it.checked ? "✓ " : " ") : ""}
              {it.label}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
