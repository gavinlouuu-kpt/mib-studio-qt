import { metricNumber } from "../../metricFormat";
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
import { paddedExtent, panRanges, toData, zoomAxes, zoomRange, type Range } from "./chartMath";
import { drawScatter, MARKER, scatterViewport, type ContourFamily, type IsoCurve } from "./drawScatter";
import { doubleClickResets, GestureTracker } from "./scatterGestures";
import { nearestPoint } from "./scatterHitTest";
import type { ReviewScatter as ScatterData } from "../reviewBridge";

export type { ContourFamily, IsoCurve };

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

  const vp = useMemo(() => scatterViewport(size.w, size.h, ranges), [ranges, size]);

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
    drawScatter(g, size.w, size.h, {
      xs: points.xs,
      ys: points.ys,
      levels,
      levelCount,
      colourByDensity,
      curves,
      contours,
      highlight: points.byPos.get(selected) ?? -1,
      ranges,
    });
  }, [size, ranges, points, levels, levelCount, colourByDensity, curves, contours, selected]);

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
          Frame {data?.frame_index[hovered.i]} · {metricNumber(points.xs[hovered.i], 1)} µm² · deformability {metricNumber(points.ys[hovered.i], 4)}
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
