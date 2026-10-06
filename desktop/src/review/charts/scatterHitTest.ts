// Which scatter point is under the pointer — a port of
// include/frontend/utils/ScatterHitTest.h (issue #467). Both shells are
// checked against tests/fixtures/review_scatter_hits.json so they cannot
// drift on what a click selects.
//
// Rule: only points whose data value lies inside the visible ranges count;
// the nearest one within `tolerancePx` wins; equal distances go to the
// lowest frame index.
import type { Viewport } from "./chartMath";

export const HIT_TOLERANCE_PX = 8;

export function insidePlot(v: Viewport, px: number, py: number): boolean {
  return px >= v.left && px <= v.left + v.width && py >= v.top && py <= v.top + v.height;
}

/** Index into the point arrays of the hit, or -1. `frames[i]` is the frame
 * the point stands for (tie-break key). */
export function nearestPoint(
  xs: ArrayLike<number>,
  ys: ArrayLike<number>,
  frames: ArrayLike<number>,
  v: Viewport,
  px: number,
  py: number,
  tolerancePx = HIT_TOLERANCE_PX,
): number {
  if (!(v.x1 > v.x0) || !(v.y1 > v.y0) || !(v.width > 0) || !(v.height > 0)) return -1;
  if (!insidePlot(v, px, py)) return -1;
  const sx = v.width / (v.x1 - v.x0);
  const sy = v.height / (v.y1 - v.y0);
  const tol2 = tolerancePx * tolerancePx;
  let best = -1;
  let bestD2 = Infinity;
  for (let i = 0; i < xs.length; i++) {
    const x = xs[i], y = ys[i];
    if (x < v.x0 || x > v.x1 || y < v.y0 || y > v.y1) continue;
    const dx = v.left + (x - v.x0) * sx - px;
    const dy = v.top + (v.y1 - y) * sy - py;
    const d2 = dx * dx + dy * dy;
    if (d2 > tol2) continue;
    if (d2 < bestD2 || (d2 === bestD2 && best >= 0 && frames[i] < frames[best])) {
      bestD2 = d2;
      best = i;
    }
  }
  return best;
}
