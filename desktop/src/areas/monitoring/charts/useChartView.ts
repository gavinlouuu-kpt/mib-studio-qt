// The part of a Monitoring chart that is not drawing: its size, the user's zoom, and the Qt
// ZoomableChartView gestures (src/frontend/widgets/ZoomableChartView.cpp) on a canvas: wheel zoom around
// the pointer (~10 % a notch; Ctrl = X only, Shift = Y only, over an axis = that axis only), a left or
// middle press that becomes a pan after the drag threshold, a click when it never did, and a
// double-click that resets the zoom. The scatter and the histogram share it; the maths and the gesture
// state machine are the Review charts' (review/charts/chartMath.ts, scatterGestures.ts), imported, not
// copied.
import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import { panRanges, toData, zoomAxes, zoomRange, type Range, type Viewport } from "../../../review/charts/chartMath";
import { DRAG_THRESHOLD_PX, GestureTracker } from "../../../review/charts/scatterGestures";

export interface AxisRanges {
  x: Range;
  y: Range;
}

/** The user's zoom, tagged with the settings epoch it was made under: a change of the epoch (the dialog's
 *  Apply, the config's ring-ratio range) drops it, like Qt's resetZoom() in the range setters. */
export interface ZoomState {
  epoch: number;
  ranges: AxisRanges;
}

export function activeZoom(zoom: ZoomState | null, epoch: number): AxisRanges | null {
  return zoom && zoom.epoch === epoch ? zoom.ranges : null;
}

/** Wheel zoom of both ranges: which axes follows the modifiers and the pointer's region. `deltaY` is the
 *  DOM sign (negative = zoom in). The anchor is the data value under the pointer, clamped to the plot. */
export function wheelRanges(r: AxisRanges, vp: Viewport, px: number, py: number, deltaY: number, ctrl: boolean, shift: boolean): AxisRanges {
  const axes = zoomAxes(px, py, vp, ctrl, shift);
  const [ax, ay] = toData(vp, Math.min(Math.max(px, vp.left), vp.left + vp.width), Math.min(Math.max(py, vp.top), vp.top + vp.height));
  return { x: axes.x ? zoomRange(r.x, ax, deltaY) : r.x, y: axes.y ? zoomRange(r.y, ay, deltaY) : r.y };
}

const viewportOf = (vp: Viewport, r: AxisRanges): Viewport => ({ ...vp, x0: r.x.min, x1: r.x.max, y0: r.y.min, y1: r.y.max });

/** A left-press interceptor, the way a gate overlay takes a press on one of its handles (Qt
 *  ZoomableChartView pressInterceptor_): `press` returns true to take the gesture; once the pointer
 *  travels the drag threshold `move` follows it and `end` closes it (`completed` false when the release
 *  was lost); a press that never travels is an ordinary click. */
export interface PressInterceptor {
  press: (px: number, py: number, vp: Viewport) => boolean;
  move: (px: number, py: number, vp: Viewport) => void;
  end: (px: number, py: number, vp: Viewport, completed: boolean) => void;
}

export interface ChartViewOptions {
  defaults: AxisRanges;
  epoch: number;
  viewport: (w: number, h: number, ranges: AxisRanges) => Viewport;
  minSize: { w: number; h: number };
  onHover?: (px: number, py: number, vp: Viewport) => void;
  onLeave?: () => void;
  /** A left click that never became a drag. */
  onClick?: (px: number, py: number, vp: Viewport) => void;
  /** A double-click: return false to keep the zoom (the scatter keeps it when a point was hit). */
  shouldReset?: (px: number, py: number, vp: Viewport) => boolean;
  interceptor?: PressInterceptor;
}

export function useChartView(opts: ChartViewOptions) {
  const { defaults, epoch, viewport, minSize } = opts;
  const wrapRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [size, setSize] = useState({ w: minSize.w, h: minSize.h });
  const [zoom, setZoom] = useState<ZoomState | null>(null);
  const zoomed = activeZoom(zoom, epoch);
  const ranges = zoomed ?? defaults;
  const vp = viewport(size.w, size.h, ranges);
  const gestures = useRef(new GestureTracker());
  const press = useRef<{ x: number; y: number; dragging: boolean; pointerId: number } | null>(null);
  const optsRef = useRef(opts);
  optsRef.current = opts;
  const vpRef = useRef(vp);
  vpRef.current = vp;

  useLayoutEffect(() => {
    const el = wrapRef.current;
    if (!el) return;
    const measure = () => setSize((s) => {
      const w = Math.max(minSize.w, el.clientWidth), h = Math.max(minSize.h, el.clientHeight);
      return s.w === w && s.h === h ? s : { w, h };
    });
    measure();
    if (typeof ResizeObserver === "undefined") return;
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, [minSize.w, minSize.h]);

  const resetZoom = useCallback(() => setZoom(null), []);
  const local = (e: { clientX: number; clientY: number }): [number, number] => {
    const rect = canvasRef.current!.getBoundingClientRect();
    return [e.clientX - rect.left, e.clientY - rect.top];
  };

  // The wheel needs a non-passive listener, or the page scrolls under the chart. Zooming reads the
  // ranges of the render it belongs to; the ranges are replaced, so successive notches compound.
  useEffect(() => {
    const c = canvasRef.current;
    if (!c) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const [px, py] = local(e);
      const delta = e.deltaMode === 1 ? e.deltaY * 40 : e.deltaY;
      setZoom((z) => {
        const base = activeZoom(z, epoch) ?? defaults;
        return { epoch, ranges: wheelRanges(base, viewportOf(vpRef.current, base), px, py, delta, e.ctrlKey, e.shiftKey) };
      });
    };
    c.addEventListener("wheel", onWheel, { passive: false });
    return () => c.removeEventListener("wheel", onWheel);
  }, [epoch, defaults]);

  const onPointerDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (e.button === 1) e.preventDefault(); // no autoscroll
    const [px, py] = local(e);
    const o = optsRef.current;
    if (e.button === 0 && o.interceptor?.press(px, py, vpRef.current)) {
      press.current = { x: px, y: py, dragging: false, pointerId: e.pointerId };
    } else {
      gestures.current.down(px, py, e.button);
      if (!gestures.current.active) return;
    }
    try {
      e.currentTarget.setPointerCapture?.(e.pointerId);
    } catch {
      /* a pointer that is already gone */
    }
  };

  const finishIntercept = (px: number, py: number, completed: boolean) => {
    const p = press.current;
    press.current = null;
    if (p?.dragging) optsRef.current.interceptor?.end(px, py, vpRef.current, completed);
  };

  const onPointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const [px, py] = local(e);
    const o = optsRef.current;
    const p = press.current;
    if (p) {
      if (!(e.buttons & 1)) return finishIntercept(px, py, false); // the release went elsewhere
      if (!p.dragging && Math.abs(px - p.x) + Math.abs(py - p.y) >= DRAG_THRESHOLD_PX) p.dragging = true;
      if (p.dragging) o.interceptor?.move(px, py, vpRef.current);
      return;
    }
    const a = gestures.current.move(px, py, e.buttons);
    if (a.kind === "pan") {
      o.onLeave?.();
      setZoom((z) => {
        const base = activeZoom(z, epoch) ?? defaults;
        return { epoch, ranges: panRanges(viewportOf(vpRef.current, base), a.dx, a.dy) };
      });
    } else if (a.kind === "hover") {
      o.onHover?.(px, py, vpRef.current);
    }
  };

  const onPointerUp = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const [px, py] = local(e);
    const p = press.current;
    if (p) {
      const wasDrag = p.dragging;
      finishIntercept(px, py, true);
      // A press that never travelled is a click like any other.
      if (!wasDrag && e.button === 0) optsRef.current.onClick?.(px, py, vpRef.current);
      return;
    }
    const a = gestures.current.up(px, py, e.button);
    if (a.kind === "click" && a.button === 0) optsRef.current.onClick?.(px, py, vpRef.current);
  };

  const cancel = () => {
    gestures.current.cancel();
    if (press.current) {
      const p = press.current;
      finishIntercept(p.x, p.y, false);
    }
  };

  const onDoubleClick = (e: React.MouseEvent<HTMLCanvasElement>) => {
    const [px, py] = local(e);
    if (optsRef.current.shouldReset?.(px, py, vpRef.current) ?? true) resetZoom();
  };

  return {
    wrapRef,
    canvasRef,
    size,
    ranges,
    vp,
    zoomed: zoomed !== null,
    resetZoom,
    panning: gestures.current.isPanning,
    canvasProps: {
      onPointerDown,
      onPointerMove,
      onPointerUp,
      onPointerCancel: cancel,
      onPointerLeave: () => optsRef.current.onLeave?.(),
      onDoubleClick,
    },
  };
}

/** Size a canvas for the device pixel ratio and hand `draw` a context in CSS pixels. */
export function paintCanvas(canvas: HTMLCanvasElement | null, size: { w: number; h: number }, draw: (g: CanvasRenderingContext2D) => void): void {
  if (!canvas) return;
  const dpr = (typeof window !== "undefined" && window.devicePixelRatio) || 1;
  canvas.width = Math.round(size.w * dpr);
  canvas.height = Math.round(size.h * dpr);
  canvas.style.width = `${size.w}px`;
  canvas.style.height = `${size.h}px`;
  const g = canvas.getContext("2d");
  if (!g) return;
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  draw(g);
}
