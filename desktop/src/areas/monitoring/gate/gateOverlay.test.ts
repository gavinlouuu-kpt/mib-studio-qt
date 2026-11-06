// The gate overlay's drawing (MON-064, MON-068) against a recording canvas: the tint (an exact rectangle when the E layer does
// not filter, the sampled mask otherwise, cached so a redraw does not resample), the dashed box, the dash-dot iso-E contours
// with their labels, the pills, and the caches of the contours.
import { describe, expect, it } from "vitest";
import type { Viewport } from "../../../review/charts/chartMath";
import { recordingContext } from "../charts/recordingContext";
import {
  CONTOUR_CACHE, GATE_ACTIVE, GATE_EDGE, GATE_EMOD, GATE_TINT, GATE_WARN, GateOverlayModel, NO_TABLE_NOTICE, gateNotice, tableErrorNotice,
  type GateView,
} from "./gateOverlay";
import { syntheticLut } from "./syntheticLut";
import type { GateSpec } from "./targetGate";

// 1 px per µm² and 800 px per deformability unit over area 0..600, deformability 0..0.5 (the synthetic table's extent).
const vp: Viewport = { x0: 0, x1: 600, y0: 0, y1: 0.5, left: 50, top: 20, width: 600, height: 400 };
const lut = syntheticLut();
const spec = (over: Partial<GateSpec> = {}): GateSpec => ({
  enabled: true, areaMin: 100, areaMax: 300, deformMin: 0.1, deformMax: 0.3, emodEnabled: false, emodMin: 1, emodMax: 3, ...over,
});
const view = (over: Partial<GateView> = {}): GateView => ({
  spec: spec(), unapplied: false, lut: null, table: "none", tableError: "", hover: "none", dragging: "none", dragText: "", ...over,
});
function drawn(model: GateOverlayModel, v: GateView, viewport = vp) {
  const rec = recordingContext();
  model.draw(rec.g, viewport, v);
  return rec;
}
const strokes = (rec: ReturnType<typeof recordingContext>) => rec.of("stroke").map((c) => c.strokeStyle);

describe("the pills", () => {
  it("names the gate, and says when it is unapplied", () => {
    const m = new GateOverlayModel();
    expect(drawn(m, view()).texts()).toEqual(["Target group"]);
    expect(drawn(m, view({ unapplied: true })).texts()).toEqual(["Target group (unapplied)"]);
  });
  it("draws nothing while the draft gate is off or the viewport is degenerate", () => {
    const m = new GateOverlayModel();
    expect(drawn(m, view({ spec: spec({ enabled: false }) })).calls).toEqual([]);
    expect(drawn(m, view(), { ...vp, x1: vp.x0 }).calls).toEqual([]);
  });
  it("warns in red when the E gate is on and the table is missing, in words", () => {
    const m = new GateOverlayModel();
    const rec = drawn(m, view({ spec: spec({ emodEnabled: true }), table: "none" }));
    expect(rec.texts()).toEqual(["Target group", NO_TABLE_NOTICE]);
    const notice = rec.of("fillText").find((c) => c.args[0] === NO_TABLE_NOTICE)!;
    expect(notice.fillStyle).toBe(GATE_WARN);
    expect(NO_TABLE_NOTICE).toBe("No E-modulus table loaded: the target group uses the box only");
  });
  it("has no notice while the table is only unknown, or the E gate is off; an unreadable table is its own notice", () => {
    expect(gateNotice(view({ spec: spec({ emodEnabled: true }), table: "unknown" }))).toBe("");
    expect(gateNotice(view({ spec: spec({ emodEnabled: false }), table: "none" }))).toBe("");
    expect(gateNotice(view({ spec: spec({ emodEnabled: true }), table: "loaded", lut }))).toBe("");
    expect(gateNotice(view({ spec: spec({ emodEnabled: true }), table: "error", tableError: "backend gone" }))).toBe(tableErrorNotice("backend gone"));
    expect(tableErrorNotice("x")).toContain("box only");
  });
  it("wraps a pill that is wider than the plot", () => {
    const m = new GateOverlayModel();
    const narrow: Viewport = { ...vp, width: 120 };
    const rec = drawn(m, view({ spec: spec({ emodEnabled: true }), table: "none" }), narrow);
    // Without a text measurement each character is 6 px: the notice is cut into several lines, none lost.
    const lines = rec.of("fillText").map((c) => String(c.args[0])).filter((t) => t !== "Target group");
    expect(lines.length).toBeGreaterThan(1);
    expect(lines.join(" ")).toBe(NO_TABLE_NOTICE);
  });
});

describe("the tint", () => {
  it("is the exact rectangle when the E gate is off, in Qt's blue, clipped to the plot", () => {
    const m = new GateOverlayModel();
    const rec = drawn(m, view());
    const tint = rec.of("fillRect")[0];
    expect(tint.fillStyle).toBe(GATE_TINT);
    expect(tint.args).toEqual([150, 180, 200, 160]); // box x 150..350, y 180..340
    expect(m.maskComputations).toBe(0);
    // A box partly outside the plot is cut to it.
    const cut = drawn(m, view({ spec: spec({ areaMax: 900 }) })).of("fillRect")[0];
    expect(cut.args).toEqual([150, 180, 500, 160]);
  });
  it("is the exact rectangle with the E gate on and no table (the box alone), and tints nothing off screen", () => {
    const m = new GateOverlayModel();
    expect(drawn(m, view({ spec: spec({ emodEnabled: true, emodMin: 5 }), table: "none" })).of("fillRect")[0].args).toEqual([150, 180, 200, 160]);
    expect(drawn(m, view({ spec: spec({ areaMin: 700, areaMax: 800 }) })).of("fillRect")).toHaveLength(0);
  });
  it("is the sampled predicate with the E gate on and a table, filled as one path clipped to the box", () => {
    const m = new GateOverlayModel();
    const v = view({ spec: spec({ emodEnabled: true, emodMin: 2, emodMax: 2.5 }), lut, table: "loaded" });
    const rec = drawn(m, v);
    expect(rec.of("fillRect")).toHaveLength(0);
    const rects = rec.of("rect").filter((c) => c.fillStyle === GATE_TINT && c.args[3] === 3); // the runs are one 3 px row high
    expect(rects.length).toBeGreaterThan(0);
    const fills = rec.of("fill").filter((c) => c.fillStyle === GATE_TINT);
    expect(fills).toHaveLength(1);
    // Every sampled run lies inside the 3 px grid of the plot, and the E range removed cells of the plain box.
    const area = rects.reduce((sum, c) => sum + (c.args[2] as number) * (c.args[3] as number), 0);
    expect(area).toBeGreaterThan(0);
    expect(area).toBeLessThan(200 * 160);
  });
  it("does not sample the table again for a redraw, but does for another gate, table or viewport", () => {
    const m = new GateOverlayModel();
    const v = view({ spec: spec({ emodEnabled: true }), lut, table: "loaded" });
    drawn(m, v);
    drawn(m, v);
    drawn(m, { ...v, hover: "areaMax" }); // a hover changes the picture, not the tint
    drawn(m, { ...v, unapplied: true });
    expect(m.maskComputations).toBe(1);
    drawn(m, { ...v, spec: { ...v.spec, emodMax: 2 } });
    expect(m.maskComputations).toBe(2);
    drawn(m, { ...v, spec: { ...v.spec, emodMax: 2 } }, { ...vp, x1: 700 });
    expect(m.maskComputations).toBe(3);
    drawn(m, { ...v, spec: { ...v.spec, emodMax: 2 }, lut: syntheticLut() }, { ...vp, x1: 700 });
    expect(m.maskComputations).toBe(4);
  });
});

describe("the box and the contours", () => {
  it("draws the box dashed in Qt's blue and an active edge solid, heavier and orange", () => {
    const m = new GateOverlayModel();
    const rec = drawn(m, view());
    expect(rec.of("stroke").slice(0, 4).map((c) => c.strokeStyle)).toEqual([GATE_EDGE, GATE_EDGE, GATE_EDGE, GATE_EDGE]);
    expect(rec.of("setLineDash").slice(0, 4).map((c) => c.args[0])).toEqual([[8, 4], [8, 4], [8, 4], [8, 4]]);
    const hovered = drawn(m, view({ hover: "areaMax" }));
    expect(hovered.of("stroke").slice(0, 4).filter((c) => c.strokeStyle === GATE_ACTIVE)).toHaveLength(1);
    expect(hovered.of("setLineDash").slice(0, 4).map((c) => c.args[0])).toContainEqual([]);
    const dragged = drawn(m, view({ dragging: "deformMin", dragText: "Target group deformability minimum: 0.10" }));
    expect(dragged.texts()).toEqual(["Target group", "Target group deformability minimum: 0.10"]);
    expect(dragged.of("stroke").filter((c) => c.strokeStyle === GATE_ACTIVE).length).toBeGreaterThanOrEqual(1);
  });
  it("draws dash-dot iso-E contours with 'E ≥ x kPa' and 'E ≤ x kPa' labels while the E gate is on with a table", () => {
    const m = new GateOverlayModel();
    const rec = drawn(m, view({ spec: spec({ emodEnabled: true, emodMin: 2, emodMax: 4 }), lut, table: "loaded" }));
    expect(rec.texts()).toEqual(expect.arrayContaining(["Target group", "E ≥ 2.00 kPa", "E ≤ 4.00 kPa"]));
    expect(rec.texts()).not.toContain(NO_TABLE_NOTICE);
    expect(strokes(rec)).toContain(GATE_EMOD);
    expect(rec.of("setLineDash").map((c) => c.args[0])).toContainEqual([8, 4, 2, 4]);
    const info = m.describe(view({ spec: spec({ emodEnabled: true, emodMin: 2, emodMax: 4 }), lut, table: "loaded" }), vp);
    expect(info.contourLabels).toEqual(["E ≥ 2.00 kPa", "E ≤ 4.00 kPa"]);
    expect(info.contourLines).toBeGreaterThanOrEqual(2);
  });
  it("draws no contours without the E gate, without a table or for a level outside the table", () => {
    const m = new GateOverlayModel();
    for (const v of [
      view({ lut, table: "loaded" }), // E gate off
      view({ spec: spec({ emodEnabled: true }), table: "none" }), // no table
      view({ spec: spec({ emodEnabled: true, emodMin: 50, emodMax: 60 }), lut, table: "loaded" }), // above the table's values
    ]) {
      const rec = drawn(m, v);
      expect(strokes(rec)).not.toContain(GATE_EMOD);
      expect(rec.texts().filter((t) => t.startsWith("E "))).toEqual([]);
    }
  });
  it("keeps a label out of the way of the name pill", () => {
    const m = new GateOverlayModel();
    // A contour whose right-most visible vertex is in the plot's top-left corner would sit under the pills.
    const rec = drawn(m, view({ spec: spec({ emodEnabled: true, emodMin: 0.6, emodMax: 0.7 }), lut, table: "loaded" }));
    expect(rec.texts()).toContain("E ≥ 0.60 kPa");
  });
});

describe("the contour cache", () => {
  it("keeps one contour per level (at most 8), and drops them all with the table", () => {
    const m = new GateOverlayModel();
    expect(m.contourFor(null, 2)).toBeNull();
    const a = m.contourFor(lut, 2);
    expect(m.contourFor(lut, 2)).toBe(a);
    expect(m.contourComputations).toBe(1);
    for (let level = 1; level <= CONTOUR_CACHE; level++) m.contourFor(lut, 2 + level * 0.1);
    expect(m.contourComputations).toBe(1 + CONTOUR_CACHE);
    // 9 entries asked for with a cap of 8: the cache restarted once, so the first level is computed again.
    m.contourFor(lut, 2);
    expect(m.contourComputations).toBe(2 + CONTOUR_CACHE);
    const other = syntheticLut();
    m.contourFor(other, 2);
    expect(m.contourComputations).toBe(3 + CONTOUR_CACHE);
    expect(m.contourFor(other, 2)).toBe(m.contourFor(other, 2));
  });
  it("hands the hit test the contours of the draft's levels only", () => {
    const m = new GateOverlayModel();
    const gate = spec({ emodEnabled: true, emodMin: 2, emodMax: 4, areaMin: 0, areaMax: 600, deformMin: 0, deformMax: 0.5 });
    const v = view({ spec: gate, lut, table: "loaded" });
    const hit = m.hit(v, vp, 350, 400);
    expect(["emodMin", "emodMax", "none"]).toContain(hit.kind);
    expect(m.hit(view({ spec: { ...gate, enabled: false }, lut }), vp, 350, 400).kind).toBe("none");
    expect(m.contoursOf(gate, null)).toEqual({ min: null, max: null });
    expect(m.contoursOf({ ...gate, emodEnabled: false }, lut)).toEqual({ min: null, max: null });
  });
});
