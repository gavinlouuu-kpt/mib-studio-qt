// Canvas drawing of the Monitoring scatter's density layer (epic #866 PR E2: MON-058, 060, 061). Qt draws
// the valid cells in eight level series (`kdeLevelSeries_`, circles) and the target group in eight more
// (`kdeTargetLevelSeries_`, squares, so the group stays identifiable without its blue), each series one
// ramp colour; the core contour is a solid 2 px line (#2a78d6), the reference contour dashed orange
// (#eb6834). Here each level is one path per shape: no per-point object, one fill per level and shape.
// drawMonitoringScatter calls these; with no density model it draws as it always did.
import { toPx, type Viewport } from "../../../review/charts/chartMath";
import { MARKER } from "../../../review/charts/drawScatter";
import type { ScatterData } from "../monitoringChartModel";
import { KDE_LEVELS, LEVEL_COLOURS, type Loop } from "./densityModel";

/** Qt: categorical blue and orange, never the density ramp. */
export const CORE_CONTOUR_COLOUR = "#2a78d6";
export const REFERENCE_CONTOUR_COLOUR = "#eb6834";
/** The Review scatter's dash for a dashed contour. */
export const REFERENCE_DASH = [7, 5];
/** The legend's dot for the two series while the density colours are on: the dark end of the ramp. */
export const LEGEND_DENSITY_COLOUR = LEVEL_COLOURS[KDE_LEVELS - 2];

/** The points by level, sparsest first so the denser ones draw on top (Qt adds the series in that order): circles
 *  for the valid cells, squares of the same size for the target group. `levels` is parallel to `data`. */
export function drawDensityPoints(g: CanvasRenderingContext2D, vp: Viewport, data: ScatterData, levels: ArrayLike<number>): void {
  const r = MARKER / 2;
  for (let level = 0; level < KDE_LEVELS; level += 1) {
    g.fillStyle = LEVEL_COLOURS[level];
    for (const square of [false, true]) {
      g.beginPath();
      let any = false;
      for (let i = 0; i < data.xs.length; i += 1) {
        if (levels[i] !== level || (data.target[i] === 1) !== square) continue;
        const x = data.xs[i], y = data.ys[i];
        if (x < vp.x0 || x > vp.x1 || y < vp.y0 || y > vp.y1) continue;
        const [px, py] = toPx(vp, x, y);
        if (square) g.rect(px - r, py - r, MARKER, MARKER);
        else {
          g.moveTo(px + r, py);
          g.arc(px, py, r, 0, Math.PI * 2);
        }
        any = true;
      }
      if (any) g.fill();
    }
  }
}

function drawLoops(g: CanvasRenderingContext2D, vp: Viewport, loops: readonly Loop[], colour: string, dash: number[]): void {
  if (loops.length === 0) return;
  g.save();
  g.strokeStyle = colour;
  g.lineWidth = 2;
  g.lineJoin = "round";
  g.setLineDash(dash);
  for (const loop of loops) {
    if (loop.length < 2) continue;
    g.beginPath();
    loop.forEach(([x, y], k) => {
      const [px, py] = toPx(vp, x, y);
      if (k === 0) g.moveTo(px, py);
      else g.lineTo(px, py);
    });
    g.stroke();
  }
  g.restore();
}

/** The core contour (solid) and then the reference (dashed), in data coordinates under the plot's clip. */
export function drawDensityContours(g: CanvasRenderingContext2D, vp: Viewport, contours: readonly Loop[], reference: readonly Loop[]): void {
  drawLoops(g, vp, contours, CORE_CONTOUR_COLOUR, []);
  drawLoops(g, vp, reference, REFERENCE_CONTOUR_COLOUR, REFERENCE_DASH);
}
