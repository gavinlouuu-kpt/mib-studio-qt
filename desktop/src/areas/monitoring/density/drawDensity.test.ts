import { describe, expect, it } from "vitest";
import type { MonitoringRow } from "../../../bridge";
import { MARKER } from "../../../review/charts/drawScatter";
import { drawMonitoringScatter, scatterViewport, SELECTION_COLOUR, TARGET_COLOUR, VALID_COLOUR } from "../charts/drawMonitoringScatter";
import { recordingContext } from "../charts/recordingContext";
import { monitoringChartData } from "../monitoringChartModel";
import { callsDigest } from "./densityFixtures";
import { LEVEL_COLOURS, type ScatterDensityModel } from "./densityModel";
import { CORE_CONTOUR_COLOUR, REFERENCE_CONTOUR_COLOUR, REFERENCE_DASH } from "./drawDensity";

const row = (over: Partial<MonitoringRow>): MonitoringRow => ({
  frame_index: 1, timestamp_ns: 1, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: 0, centroid_x: 0, centroid_y: 0,
  area: 100, deformability: 0.1, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 1, ...over,
});
const data = monitoringChartData([
  row({ frame_index: 1, area: 100, deformability: 0.2 }),
  row({ frame_index: 2, area: 300, deformability: 0.4, target_group: true }),
  row({ frame_index: 3, area: 5000, deformability: 0.5 }), // outside the X range: never drawn
  row({ frame_index: 4, area: 700, deformability: 0.8 }),
  row({ frame_index: 5, area: 710, deformability: 0.81, target_group: true }),
]).scatter;
const ranges = { x: { min: 0, max: 1000 }, y: { min: 0, max: 1 } };
const curves = [{ modulus: 2, points: [[25, 0.1], [400, 0.3]] as [number, number][] }, { modulus: 1, points: [[25, 0.2], [400, 0.5]] as [number, number][] }];
const vp = scatterViewport(700, 360, ranges);

describe("with the density layer off the scatter is drawn exactly as before PR E2", () => {
  // The digest of every canvas call (name, arguments, fill and stroke style in force) the plain chart made at the
  // base commit be64eac8 of PR E2, for this model: four points, a selection, an overlay, two curves. A change to
  // the plain drawing, however small, changes it; the density layer must not. (Taken at the base commit as a
  // SHA-256, a9cddccc...dff802, then re-taken with callsDigest, which needs no Node types.)
  const PLAIN_CALLS = 115;
  const PLAIN_DIGEST = "172ca763a03de4";
  const plainModel = () => {
    const four = monitoringChartData([
      row({ frame_index: 1, area: 100, deformability: 0.2 }), row({ frame_index: 2, area: 300, deformability: 0.4, target_group: true }),
      row({ frame_index: 3, area: 5000, deformability: 0.5 }), row({ frame_index: 4, area: 700, deformability: 0.8 }),
    ]).scatter;
    return { data: four, curves, ranges, selected: 1, overlay: (g: CanvasRenderingContext2D) => g.fillText("OVERLAY", 1, 2) };
  };
  const digest = callsDigest;

  it("makes the same canvas calls as the chart did before the layer existed", () => {
    const rec = recordingContext();
    drawMonitoringScatter(rec.g, 700, 360, plainModel(), { selection: true });
    expect(rec.calls).toHaveLength(PLAIN_CALLS);
    expect(digest(rec.calls)).toBe(PLAIN_DIGEST);
  });
  it("an absent layer and an undefined one are the same thing", () => {
    const rec = recordingContext();
    drawMonitoringScatter(rec.g, 700, 360, { ...plainModel(), density: undefined }, { selection: true });
    expect(digest(rec.calls)).toBe(PLAIN_DIGEST);
  });
});

describe("with the density layer on (MON-058, 060, 061)", () => {
  // Levels per plotted point of `data` (the third is outside the axes; it still has a level).
  const levels = Uint8Array.from([7, 2, 5, 0, 6]);
  const density = (over: Partial<ScatterDensityModel> = {}): ScatterDensityModel => ({ levels, contours: [], reference: [], ...over });
  const draw = (d: ScatterDensityModel | undefined, extra: { selected?: number; overlay?: (g: CanvasRenderingContext2D) => void } = {}) => {
    const rec = recordingContext();
    drawMonitoringScatter(rec.g, 700, 360, { data, curves, ranges, selected: extra.selected ?? -1, overlay: extra.overlay, density: d }, { selection: true });
    return rec;
  };
  const px = (area: number, d: number) => [vp.left + (area / 1000) * vp.width, vp.top + (1 - d) * vp.height];

  it("colours each valid cell by its level, sparse to dense, and no longer in the plain green or blue", () => {
    const rec = draw(density());
    const fills = rec.of("fill").map((f) => f.fillStyle);
    const used = LEVEL_COLOURS.filter((c) => fills.includes(c));
    // Levels 7 (frame 1), 2 (frame 2, target), 0 (frame 4) and 6 (frame 5, target) are in range; frame 3 (level 5) is off the axes.
    expect(used).toEqual([LEVEL_COLOURS[0], LEVEL_COLOURS[2], LEVEL_COLOURS[6], LEVEL_COLOURS[7]]);
    expect(fills.indexOf(LEVEL_COLOURS[0])).toBeLessThan(fills.indexOf(LEVEL_COLOURS[7])); // denser levels are painted last, on top
    expect(fills).not.toContain(VALID_COLOUR);
    expect(fills).not.toContain(TARGET_COLOUR);
  });

  it("draws the valid cells as circles and the target group as squares of the same size", () => {
    const rec = draw(density());
    const r = MARKER / 2;
    const circles = rec.of("arc").filter((a) => a.args[2] === r);
    const [x1, y1] = px(100, 0.2), [x4, y4] = px(700, 0.8);
    expect(circles.map((a) => [a.args[0], a.args[1]])).toEqual(expect.arrayContaining([[x1, y1], [x4, y4]]));
    // Two plotted valid circles (frames 1, 4) plus the legend's dot of "Valid Frames" (radius 3.5).
    expect(circles).toHaveLength(2);
    const squares = rec.of("rect").filter((c) => c.args[2] === MARKER && c.args[3] === MARKER);
    const [x2, y2] = px(300, 0.4), [x5, y5] = px(710, 0.81);
    expect(squares.map((c) => [c.args[0], c.args[1]])).toEqual(expect.arrayContaining([[x2 - r, y2 - r], [x5 - r, y5 - r]]));
    expect(squares).toHaveLength(2);
  });

  it("keeps the two series apart in the legend by shape, not by colour", () => {
    const rec = draw(density());
    const legend = rec.of("rect").filter((c) => c.args[2] === 7 && c.args[3] === 7);
    expect(legend).toHaveLength(1);
    expect(rec.texts()).toContain("Valid Frames");
    expect(rec.texts()).toContain("Target Group");
    // The legend dots are not the plain green and blue either.
    expect(rec.of("fill").some((f) => f.fillStyle === VALID_COLOUR || f.fillStyle === TARGET_COLOUR)).toBe(false);
  });

  it("draws the core contour solid and the reference dashed orange, over the points and under the selection and the overlay", () => {
    const core = [[[100, 0.1], [400, 0.1], [400, 0.5], [100, 0.1]]] as [number, number][][];
    const ref = [[[200, 0.2], [600, 0.2], [600, 0.7], [200, 0.2]]] as [number, number][][];
    const rec = draw(density({ contours: core, reference: ref }), { selected: 0, overlay: (g) => g.fillText("OVERLAY", 0, 0) });
    const strokes = rec.of("stroke").filter((s) => s.strokeStyle === CORE_CONTOUR_COLOUR || s.strokeStyle === REFERENCE_CONTOUR_COLOUR);
    expect(strokes.map((s) => s.strokeStyle)).toEqual([CORE_CONTOUR_COLOUR, REFERENCE_CONTOUR_COLOUR]);
    expect(CORE_CONTOUR_COLOUR).toBe("#2a78d6");
    expect(REFERENCE_CONTOUR_COLOUR).toBe("#eb6834");
    const dashes = rec.of("setLineDash").map((c) => c.args[0]);
    expect(dashes).toEqual([[], REFERENCE_DASH]); // solid, then dashed (7 on, 5 off, as the Review scatter's)
    // The first vertex of the core loop in plot pixels.
    const moves = rec.of("moveTo").map((m) => m.args);
    expect(moves).toContainEqual(px(100, 0.1));
    // Order: points, contours, selection marker, overlay, legend.
    const names = rec.calls.map((c) => (c.fn === "fillText" ? `text:${String(c.args[0])}` : c.fn === "fill" ? `fill:${String(c.fillStyle)}` : c.fn));
    const lastPoint = names.lastIndexOf(`fill:${LEVEL_COLOURS[7]}`);
    const firstStroke = rec.calls.findIndex((c) => c.fn === "stroke" && c.strokeStyle === CORE_CONTOUR_COLOUR);
    const selection = names.indexOf(`fill:${SELECTION_COLOUR}`);
    expect(lastPoint).toBeLessThan(firstStroke);
    expect(firstStroke).toBeLessThan(selection);
    expect(selection).toBeLessThan(names.indexOf("text:OVERLAY"));
    expect(names.indexOf("text:OVERLAY")).toBeLessThan(names.indexOf("text:Valid Frames"));
    // The contours are inside the plot's clip.
    expect(names.lastIndexOf("clip", firstStroke)).toBeGreaterThan(-1);
    expect(names.indexOf("restore", firstStroke)).toBeGreaterThan(firstStroke);
  });

  it("does not leak the contour's line style into the overlay or the selection", () => {
    const rec = draw(density({ contours: [[[100, 0.1], [400, 0.5]]], reference: [[[200, 0.2], [600, 0.7]]] }));
    const names = rec.calls.map((c) => c.fn);
    const saves = names.filter((n) => n === "save").length, restores = names.filter((n) => n === "restore").length;
    expect(saves).toBe(restores);
  });

  it("with no estimate yet (every level sparsest) still draws every point, in the sparsest colour", () => {
    const rec = draw(density({ levels: new Uint8Array(5) }));
    const fills = rec.of("fill").map((f) => f.fillStyle);
    expect(fills.filter((f) => f === LEVEL_COLOURS[0])).toHaveLength(2); // the circles, then the squares
    expect(rec.of("arc").filter((a) => a.args[2] === MARKER / 2)).toHaveLength(2);
  });

  it("draws a loop of fewer than two vertices as nothing", () => {
    const rec = draw(density({ contours: [[[100, 0.1]]], reference: [[]] }));
    expect(rec.of("stroke").filter((s) => s.strokeStyle === CORE_CONTOUR_COLOUR)).toHaveLength(0);
  });
});
