// Zoom of a frame image (#867 REVIEW-029, REVIEW-032): the Qt
// FrameViewerDialog's rules. Zoom In multiplies by 1.2, Zoom Out divides by
// 1.2 down to 0.1, Ctrl+wheel multiplies by 1 + 0.1 per notch (a notch is
// 120 units of angleDelta) within 0.1 – 10; "Fit to Window" goes back to the
// view's own sizing. The Qt Zoom In button has no upper limit; here it stops
// at the wheel's 10 so a canvas cannot grow without bound.
import { useCallback, useEffect, useRef, useState, type RefObject } from "react";

export const ZOOM_STEP = 1.2;
export const ZOOM_MIN = 0.1;
export const ZOOM_MAX = 10;
export const WHEEL_STEP = 0.1;

const clamp = (z: number) => Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, z));

export const zoomIn = (z: number) => clamp(z * ZOOM_STEP);
export const zoomOut = (z: number) => clamp(z / ZOOM_STEP);
/** `notches` > 0 zooms in (wheel away from the user), < 0 out. */
export const wheelZoom = (z: number, notches: number) => clamp(z * (1 + notches * WHEEL_STEP));

/** Wheel notches of a DOM wheel event: Qt's angleDelta().y() / 120, with the
 * sign flipped (DOM deltaY > 0 scrolls down, which zooms out). A notch is 100
 * px, 3 lines or 1 page. */
export function wheelNotches(deltaY: number, deltaMode: number): number {
  const unit = deltaMode === 1 ? 3 : deltaMode === 2 ? 1 : 100;
  return -deltaY / unit;
}

/** Displayed pixels of a canvas of `natural` px at `zoom`. */
export const zoomedSize = (natural: number, zoom: number) => Math.max(1, Math.round(natural * zoom));

export interface ImageZoom {
  /** null = the view's own sizing ("Fit to Window"). */
  zoom: number | null;
  zoomIn: () => void;
  zoomOut: () => void;
  fit: () => void;
  /** Actual size (100 %). */
  actual: () => void;
  /** Ref callback for the scrolling element that shows the canvas: Ctrl+wheel zooms it. */
  wrapRef: (el: HTMLElement | null) => void;
}

/**
 * Zoom state for a canvas. From the view's own sizing, zooming starts at the
 * scale the canvas is shown at (displayed width / pixel width), so the first
 * step does not jump.
 */
export function useImageZoom(canvasRef: RefObject<HTMLCanvasElement>): ImageZoom {
  const [zoom, setZoom] = useState<number | null>(null);
  const [wrap, setWrap] = useState<HTMLElement | null>(null);
  const shown = useCallback(() => {
    const c = canvasRef.current;
    return c && c.width > 0 && c.clientWidth > 0 ? c.clientWidth / c.width : 1;
  }, [canvasRef]);
  const shownRef = useRef(shown);
  shownRef.current = shown;

  useEffect(() => {
    if (!wrap) return;
    const onWheel = (e: WheelEvent) => {
      if (!e.ctrlKey && !e.metaKey) return;
      e.preventDefault();
      setZoom((z) => wheelZoom(z ?? shownRef.current(), wheelNotches(e.deltaY, e.deltaMode)));
    };
    // Not passive: the page must not zoom with the wheel.
    wrap.addEventListener("wheel", onWheel, { passive: false });
    return () => wrap.removeEventListener("wheel", onWheel);
  }, [wrap]);

  return {
    zoom,
    zoomIn: useCallback(() => setZoom((z) => zoomIn(z ?? shownRef.current())), []),
    zoomOut: useCallback(() => setZoom((z) => zoomOut(z ?? shownRef.current())), []),
    fit: useCallback(() => setZoom(null), []),
    actual: useCallback(() => setZoom(1), []),
    wrapRef: setWrap,
  };
}
