// The cases of tests/frontend/target_gate_preview_test.cpp (Qt), on the TypeScript port:
//  - isTarget / wouldBeTarget equal the ProcessingScience.cpp target-group expression (inclusive bounds, a NaN
//    E never passes an enabled E gate, E gate off ignores E, gate off is never target) on a dense sample that
//    crosses every bound and the table's edge;
//  - the sampled mask is that predicate evaluated at the cell centres;
//  - iso-E contours: every vertex satisfies |lookup - level| ~ 0, both saddle resolutions of a bilinear cell,
//    a closed loop, nothing outside the table;
//  - hit testing picks the nearest box edge or E contour within the tolerance, edges end where the box ends,
//    coincident edges split by pointer side;
//  - dragging is relative to the press (no jump), clamps to the control range, rounds to the display
//    precision, keeps min <= max, ignores positions without an E value;
//  - the live count follows the same predicate (a NaN E fails an enabled E gate).
import { describe, expect, it } from "vitest";
import type { Viewport } from "../../../review/charts/chartMath";
import { lookupEmodulus, prepareLut, type PreparedLut } from "../emodulusLut";
import {
  beginDrag, computeMask, constrainEdit, contourLabelAnchor, countTargets, dragValue, eLayerFilters, fieldLimits, gateSpecFromValues,
  handleAnchor, handleField, hitTest, isoContour, isTarget, lookupE, maskAt, maskOnCount, NO_CONTOURS, previewText, roundToField,
  sameGate, toDataPoint, toPixel, wouldBeTarget, type EContours, type GateSpec, type HandleKind,
} from "./targetGate";

const NaNv = Number.NaN;

// E(area, deformability) = 0.5 + area / 100 * (1 - 2 * deformability): exactly representable by the bilinear
// interpolation, so lookups are checkable by formula.
const synthE = (area: number, deform: number) => 0.5 + (area / 100) * (1 - 2 * deform);

/** 61 x 51 grid over area 0..600 µm², deformability 0..0.5; the top-right corner block is NaN ("no value"). */
function makeLut(): PreparedLut {
  const na = 61, nd = 51;
  const values: (number | null)[] = [];
  for (let di = 0; di < nd; di++) for (let ai = 0; ai < na; ai++) values.push(ai >= 55 && di >= 45 ? null : synthE(10 * ai, 0.01 * di));
  return prepareLut({ area_min: 0, area_step: 10, area_bins: na, deform_min: 0, deform_step: 0.01, deform_bins: nd, values });
}
function tableOf(width: number, height: number, f: (ai: number, di: number) => number): PreparedLut {
  const values: number[] = [];
  for (let di = 0; di < height; di++) for (let ai = 0; ai < width; ai++) values.push(f(ai, di));
  return prepareLut({ area_min: 0, area_step: 1, area_bins: width, deform_min: 0, deform_step: 1, deform_bins: height, values });
}

/** The expression of ProcessingScience.cpp (isTargetGroupMember) for a valid cell with E already known. */
function scienceTarget(c: GateSpec, tableLoaded: boolean, area: number, deform: number, e: number) {
  if (!c.enabled) return false;
  const tgArea = area >= c.areaMin && area <= c.areaMax;
  const tgDeform = deform >= c.deformMin && deform <= c.deformMax;
  const tgEmod = !c.emodEnabled || !tableLoaded || (!Number.isNaN(e) && e >= c.emodMin && e <= c.emodMax);
  return tgArea && tgDeform && tgEmod;
}

const vp: Viewport = { x0: 0, x1: 600, y0: 0, y1: 0.5, left: 50, top: 20, width: 600, height: 400 }; // 1 px per µm², 800 px per deformability unit
const boxSpec = (emod = false, eMin = 1, eMax = 3): GateSpec => ({
  enabled: true, areaMin: 100, areaMax: 300, deformMin: 0.1, deformMax: 0.3, emodEnabled: emod, emodMin: eMin, emodMax: eMax,
});
const lut = makeLut();

describe("1. the predicate is the science expression", () => {
  it("looks up E through the table and knows when the E layer filters", () => {
    expect(lookupE(null, 100, 0.1)).toBeNaN();
    expect(Math.abs(lookupE(lut, 123.4, 0.137) - synthE(123.4, 0.137))).toBeLessThan(1e-9);
    expect(eLayerFilters(boxSpec(true), true)).toBe(true);
    expect(eLayerFilters(boxSpec(true), false)).toBe(false);
    expect(eLayerFilters(boxSpec(false), true)).toBe(false);
  });

  it("equals the science expression on a dense sweep, with and without a table", () => {
    let compared = 0, targets = 0, nanE = 0;
    const mismatches: string[] = [];
    for (let variant = 0; variant < 8; variant++) {
      // 3: wide, 4: narrow band, 5: a band only the NaN-free part of the table can hold, 6: a positive minimum no
      // placeholder E (0.0) satisfies, 7: a band above the table's values
      const c: GateSpec = {
        enabled: variant !== 0, areaMin: 100, areaMax: 300, deformMin: 0.1, deformMax: 0.3, emodEnabled: variant >= 3,
        emodMin: variant === 4 ? 1.5 : variant === 6 ? 5 : variant === 7 ? 50 : 0,
        emodMax: variant === 4 ? 2.5 : variant === 5 ? 4 : variant === 7 ? 60 : 10,
      };
      for (let ia = 0; ia <= 140; ia++) {
        for (let id = 0; id <= 55; id++) {
          const a = 5 * ia, d = 0.01 * id;
          const e = lookupE(lut, a, d);
          if (Number.isNaN(e)) nanE++;
          const want = scienceTarget(c, true, a, d, e);
          // No table: the E layer is skipped whatever E the cell carries (0.0 placeholder, NaN, or a value).
          const wantBox = scienceTarget(c, false, a, d, e);
          const ok = wouldBeTarget(c, lut, a, d) === want && isTarget(c, true, a, d, e) === want
            && wouldBeTarget(c, null, a, d) === wantBox && isTarget(c, false, a, d, 0) === wantBox && isTarget(c, false, a, d, NaNv) === wantBox
            && isTarget(c, false, a, d, e) === wantBox && wantBox === (c.enabled && a >= 100 && a <= 300 && d >= 0.1 && d <= 0.3);
          if (!ok) mismatches.push(`variant ${variant} area ${a} deformability ${d}`);
          compared++;
          if (want) targets++;
        }
      }
    }
    expect(mismatches).toEqual([]);
    expect(compared).toBe(8 * 141 * 56);
    expect(targets).toBeGreaterThan(100);
    expect(nanE).toBeGreaterThan(1000);
  });

  it("handles the single cases", () => {
    const on = boxSpec(true, 1, 3);
    expect(isTarget(on, true, 200, 0.2, NaNv)).toBe(false); // NaN E fails an enabled E gate (table loaded)
    expect(isTarget(on, false, 200, 0.2, NaNv) && isTarget(on, false, 200, 0.2, 0) && isTarget(boxSpec(true, 5, 10), false, 200, 0.2, 0)).toBe(true);
    expect(isTarget(on, false, 50, 0.2, 2) || isTarget(on, false, 200, 0.5, 2)).toBe(false); // no table: outside the box is out
    expect(isTarget(on, true, 200, 0.2, 2)).toBe(true);
    expect(isTarget(on, true, 200, 0.2, 0)).toBe(false);
    expect(isTarget(on, true, 200, 0.2, 3.5)).toBe(false);
    expect(isTarget(boxSpec(false), true, 200, 0.2, NaNv)).toBe(true); // NaN E is ignored with the E gate off
    expect(isTarget(boxSpec(false), true, NaNv, 0.2, 2) || isTarget(boxSpec(false), true, 200, NaNv, 2)).toBe(false);
    const off = { ...boxSpec(false), enabled: false };
    expect(isTarget(off, true, 200, 0.2, 2) || isTarget(off, false, 200, 0.2, 2)).toBe(false);
    expect(isTarget(boxSpec(false), true, 100, 0.1, 0) && isTarget(boxSpec(false), true, 300, 0.3, 0)).toBe(true); // inclusive
    expect(isTarget(boxSpec(false), true, 99.999, 0.2, 0) || isTarget(boxSpec(false), true, 300.001, 0.2, 0)).toBe(false);
    expect(sameGate(boxSpec(), boxSpec())).toBe(true);
    expect(sameGate(boxSpec(), boxSpec(true))).toBe(false);
  });

  it("reads the gate from the tune draft's target-group fields", () => {
    const values = {
      targetGroupEnabled: true, targetAreaMin: 72, targetAreaMax: 191, targetDeformabilityMin: 0, targetDeformabilityMax: 0.3,
      targetEmodulusEnabled: true, targetEmodulusMin: 1, targetEmodulusMax: 4,
    };
    expect(gateSpecFromValues(values as never)).toEqual({
      enabled: true, areaMin: 72, areaMax: 191, deformMin: 0, deformMax: 0.3, emodEnabled: true, emodMin: 1, emodMax: 4,
    });
  });
});

describe("EModulusLut::lookup (the table behind the predicate)", () => {
  it("is bilinear inside the table and NaN outside it, with inclusive bounds", () => {
    expect(lookupEmodulus(lut, 0, 0)).toBeCloseTo(0.5, 12);
    expect(lookupEmodulus(lut, 600, 0)).toBeCloseTo(6.5, 12);
    expect(lookupEmodulus(lut, 600.001, 0)).toBeNaN();
    expect(lookupEmodulus(lut, -0.001, 0)).toBeNaN();
    expect(lookupEmodulus(lut, 100, 0.5001)).toBeNaN();
    expect(lookupEmodulus(lut, NaNv, 0.1)).toBeNaN();
    expect(lookupEmodulus(lut, 100, NaNv)).toBeNaN();
    expect(lookupEmodulus(null, 100, 0.1)).toBeNaN();
  });
  it("returns NaN when any corner of the surrounding cell has no value", () => {
    expect(lookupEmodulus(lut, 560, 0.46)).toBeNaN(); // inside the NaN block
    expect(lookupEmodulus(lut, 545, 0.44)).toBeNaN(); // the cell touches the block's first column
    expect(lookupEmodulus(lut, 530, 0.44)).toBeCloseTo(synthE(530, 0.44), 9);
  });
  it("interpolates between corners with different values", () => {
    const t = tableOf(2, 2, (ai, di) => [0, 4, 8, 12][di * 2 + ai]);
    expect(lookupEmodulus(t, 0.5, 0.5)).toBeCloseTo(6, 12);
    expect(lookupEmodulus(t, 0.25, 0)).toBeCloseTo(1, 12);
    expect(lookupEmodulus(t, 1, 1)).toBeCloseTo(12, 12);
  });
});

describe("2. the mask is the predicate at the cell centres", () => {
  it.each([false, true])("matches the predicate (E gate %s)", (emod) => {
    const spec = boxSpec(emod, 1, 3);
    const m = computeMask(spec, lut, vp, 3);
    expect([m.cols, m.rows, m.on.length]).toEqual([200, 134, 200 * 134]);
    let allMatch = true;
    for (let r = 0; r < m.rows; r++) {
      for (let c = 0; c < m.cols; c++) {
        const px = vp.left + (c + 0.5) * m.stepPx, py = vp.top + (r + 0.5) * m.stepPx;
        const inside = px <= vp.left + vp.width && py <= vp.top + vp.height;
        const [x, y] = toDataPoint(vp, px, py);
        if (maskAt(m, c, r) !== (inside && wouldBeTarget(spec, lut, x, y))) allMatch = false;
      }
    }
    expect(allMatch).toBe(true);
    if (!emod) {
      const expected = (200 * 160) / 9; // 200 µm² x 0.2 deformability = 200 px x 160 px in 3 px cells
      expect(Math.abs(maskOnCount(m) - expected)).toBeLessThan(0.03 * expected);
    } else {
      const plain = computeMask(boxSpec(false), lut, vp, 3);
      expect(maskOnCount(m)).toBeGreaterThan(0);
      expect(maskOnCount(m)).toBeLessThan(maskOnCount(plain));
    }
  });
  it("is empty with the gate off, tinting the box alone without a table, and empty on a degenerate viewport", () => {
    expect(maskOnCount(computeMask({ ...boxSpec(true), enabled: false }, lut, vp, 3))).toBe(0);
    const plainBox = computeMask(boxSpec(false), null, vp, 3);
    expect(maskOnCount(plainBox)).toBeGreaterThan(0);
    for (const eMin of [0, 5]) {
      expect(Array.from(computeMask(boxSpec(true, eMin, 10), null, vp, 3).on)).toEqual(Array.from(plainBox.on));
    }
    expect(maskOnCount(computeMask(boxSpec(true, 1, 3), lut, vp, 3))).toBeLessThan(maskOnCount(plainBox)); // with a table the E range removes cells
    expect(computeMask(boxSpec(), lut, { ...vp, x1: vp.x0 }, 3).on.length).toBe(0);
  });
});

describe("3. iso-E contours", () => {
  it.each([0.7, 1, 2.5, 4, 5.5])("level %s: every vertex satisfies |lookup - level| ~ 0", (level) => {
    const contour = isoContour(lut, level);
    expect(contour.length).toBeGreaterThan(0);
    let vertices = 0, worst = 0;
    for (const line of contour) {
      expect(line.length).toBeGreaterThanOrEqual(2);
      for (const [x, y] of line) {
        const e = lookupEmodulus(lut, x, y);
        // lookup reads the cell above / right of a point on a grid line, so a vertex on the border of the
        // missing-value block (area >= 550, deformability >= 0.45) has no value there; every other vertex must.
        if (Number.isNaN(e)) {
          expect(x >= 540 && y >= 0.44).toBe(true);
          continue;
        }
        worst = Math.max(worst, Math.abs(e - level));
        vertices++;
      }
    }
    expect(vertices).toBeGreaterThanOrEqual(4);
    expect(worst).toBeLessThan(1e-9);
  });
  it("has nothing for a level outside the table, a NaN level or a table of nothing", () => {
    expect(isoContour(lut, 100)).toEqual([]);
    expect(isoContour(lut, -1)).toEqual([]);
    expect(isoContour(lut, NaNv)).toEqual([]);
  });
  it("resolves both saddles of a bilinear cell", () => {
    const saddle = tableOf(2, 2, (ai, di) => [0, 1, 1, 0][di * 2 + ai]);
    for (const level of [0.4, 0.6]) {
      const c = isoContour(saddle, level);
      expect(c).toHaveLength(2);
      for (const line of c) {
        expect(line).toHaveLength(2);
        for (const [x, y] of line) expect(Math.abs(lookupEmodulus(saddle, x, y) - level)).toBeLessThan(1e-12);
      }
    }
  });
  it("closes a loop around a peak", () => {
    const peak = tableOf(7, 7, (ai, di) => 10 - ((ai - 3) * (ai - 3) + (di - 3) * (di - 3)));
    const loop = isoContour(peak, 5.5);
    expect(loop).toHaveLength(1);
    expect(loop[0].length).toBeGreaterThanOrEqual(5);
    expect(loop[0][0]).toEqual(loop[0][loop[0].length - 1]);
    for (const [x, y] of loop[0]) expect(Math.abs(lookupEmodulus(peak, x, y) - 5.5)).toBeLessThan(1e-12);
  });
});

describe("4. hit testing", () => {
  const spec = boxSpec();
  // x(area) = 50 + area, y(deform) = 20 + (0.5 - deform) * 800: box x 150..350, y 180..340.
  const hit = (px: number, py: number, s: GateSpec = boxSpec(), c: EContours = NO_CONTOURS) => hitTest(s, c, vp, px, py);

  it("picks the nearest edge within 6 px, and an edge ends where the box ends", () => {
    expect(hit(350, 260).kind).toBe("areaMax");
    expect(hit(350, 260).distancePx).toBeLessThan(1e-9);
    expect(hit(355, 260).kind).toBe("areaMax");
    expect(hit(355, 260).distancePx).toBeCloseTo(5, 9);
    expect(hit(357, 260).kind).toBe("none");
    expect(hit(150, 260).kind).toBe("areaMin");
    expect(hit(250, 180).kind).toBe("deformMax"); // the upper edge
    expect(hit(250, 340).kind).toBe("deformMin");
    expect(hit(250, 260).kind).toBe("none");
    expect(hit(350, 100).kind).toBe("none");
    expect(hit(152, 182).kind).toBe("areaMin"); // a corner: the tie goes to the area edge
    expect(hit(150, 260, spec).anchor).toEqual([150, 260]);
  });
  it("finds nothing with the gate off or outside the plot, and splits coincident edges by the pointer's side", () => {
    expect(hit(350, 260, { ...spec, enabled: false }).kind).toBe("none");
    expect(hitTest(spec, NO_CONTOURS, vp, 40, 260).kind).toBe("none");
    const same = { ...boxSpec(), areaMin: 200, areaMax: 200 };
    expect(hit(252, 260, same).kind).toBe("areaMax");
    expect(hit(248, 260, same).kind).toBe("areaMin");
    const flat = { ...boxSpec(), deformMin: 0.2, deformMax: 0.2 };
    expect(hit(250, 258, flat).kind).toBe("deformMax");
    expect(hit(250, 262, flat).kind).toBe("deformMin");
    expect(handleAnchor("areaMax", { ...boxSpec(), areaMax: 900 }, NO_CONTOURS, vp)).toBeNull(); // an off-screen edge has no anchor
  });
  it("hits an E contour, drawn only with the E gate on and a contour for it", () => {
    const ge: GateSpec = { ...boxSpec(true, 1, 3), areaMin: 0, areaMax: 600, deformMin: 0, deformMax: 0.5 }; // the box is the plot
    const cMin = isoContour(lut, ge.emodMin), cMax = isoContour(lut, ge.emodMax);
    const both: EContours = { min: cMin, max: cMax };
    for (const kind of ["emodMin", "emodMax"] as const) {
      const anchor = handleAnchor(kind, ge, both, vp)!;
      expect(anchor).not.toBeNull();
      const h = hitTest(ge, both, vp, anchor[0], anchor[1]);
      expect(h.kind).toBe(kind);
      expect(h.distancePx).toBeLessThan(1e-6);
      expect(hitTest(ge, both, vp, anchor[0], anchor[1] + 4).kind).toBe(kind); // 4 px off still hits
    }
    const anchorMin = handleAnchor("emodMin", ge, both, vp)!;
    const geOff = { ...ge, emodEnabled: false };
    expect(hitTest(geOff, both, vp, anchorMin[0], anchorMin[1]).kind).toBe("none");
    expect(handleAnchor("emodMin", geOff, both, vp)).toBeNull();
    expect(hitTest(ge, NO_CONTOURS, vp, anchorMin[0], anchorMin[1]).kind).toBe("none");
    // The label sits at the right-most visible vertex.
    const label = contourLabelAnchor(cMin, vp)!;
    expect(label).not.toBeNull();
    for (const line of cMin) {
      for (const d of line) {
        const [px, py] = toPixel(vp, d[0], d[1]);
        if (px >= vp.left && px <= vp.left + vp.width && py >= vp.top && py <= vp.top + vp.height) expect(px).toBeLessThanOrEqual(label[0] + 1e-9);
      }
    }
    expect([handleField("areaMax"), handleField("deformMin"), handleField("emodMax")]).toEqual(["targetAreaMax", "targetDeformabilityMin", "targetEmodulusMax"]);
  });
});

describe("5. rounding, clamping, min <= max", () => {
  const s = boxSpec(true, 1, 3);
  const areaLim = fieldLimits("targetAreaMax"), defLim = fieldLimits("targetDeformabilityMax"), eLim = fieldLimits("targetEmodulusMax");
  it("rounds to the field's display precision", () => {
    expect([roundToField("targetAreaMax", 190.6), roundToField("targetAreaMax", 190.4)]).toEqual([191, 190]);
    expect([roundToField("targetDeformabilityMax", 0.30437), roundToField("targetDeformabilityMax", 0.3071)]).toEqual([0.3, 0.31]);
    expect(roundToField("targetEmodulusMin", 1.23456)).toBe(1.23);
  });
  it("takes the limits from the tune panel's ranges", () => {
    expect(areaLim).toEqual({ lo: 0, hi: 100000 });
    expect(defLim).toEqual({ lo: 0, hi: 1 });
    expect(eLim).toEqual({ lo: 0, hi: 1000 });
  });
  it("clamps, keeps min <= max and rounds", () => {
    expect(constrainEdit("areaMax", 250.4, s, areaLim)).toBe(250);
    expect(constrainEdit("areaMax", 50, s, areaLim)).toBe(100); // area max cannot pass area min
    expect(constrainEdit("areaMin", 400, s, areaLim)).toBe(300);
    expect(constrainEdit("areaMin", -20, s, areaLim)).toBe(0);
    expect(constrainEdit("areaMax", 1e9, s, areaLim)).toBe(100000);
    expect(constrainEdit("deformMax", 1.7, s, defLim)).toBe(1);
    expect(constrainEdit("deformMax", 0.05, s, defLim)).toBe(0.1);
    expect(constrainEdit("deformMin", 0.298, s, defLim)).toBe(0.3);
    expect(constrainEdit("emodMin", 5, s, eLim)).toBe(3);
    expect(constrainEdit("emodMax", 0.2, s, eLim)).toBe(1);
    expect(constrainEdit("emodMax", 2000, s, eLim)).toBe(1000);
    expect(constrainEdit("areaMax", 5000, s, { lo: 0, hi: 1000 })).toBe(1000); // limits come from the caller
  });
});

describe("6. dragging", () => {
  const s = boxSpec();
  const areaLim = fieldLimits("targetAreaMax"), defLim = fieldLimits("targetDeformabilityMax");
  const start = (kind: HandleKind, px: number, py: number, spec: GateSpec = boxSpec(), c: EContours = NO_CONTOURS, v: Viewport = vp) => {
    const h = hitTest(spec, c, v, px, py);
    expect(h.kind).toBe(kind);
    return beginDrag(h, spec, lut, v, px, py);
  };
  it("moves an edge by the travel of the pointer, relative to the press", () => {
    const max = start("areaMax", 353, 260); // pressed 3 px beside the edge
    expect(max.valueAtPress).toBe(300);
    expect(dragValue(max, s, lut, vp, 353, 260, areaLim)).toBe(300); // no travel, no jump
    expect(dragValue(max, s, lut, vp, 383, 260, areaLim)).toBe(330); // 30 px right = +30 µm²
    expect(dragValue(max, s, lut, vp, 383, 400, areaLim)).toBe(330); // vertical travel does not matter for an area edge
    expect(dragValue(max, s, lut, vp, 60, 260, areaLim)).toBe(100); // past the minimum: stops there
    const def = start("deformMax", 250, 183);
    expect(dragValue(def, s, lut, vp, 250, 183, defLim)).toBe(0.3);
    expect(dragValue(def, s, lut, vp, 250, 103, defLim)).toBe(0.4); // 80 px up = +0.10 (y grows downwards)
    expect(dragValue(def, s, lut, vp, 250, 186.7, defLim)).toBe(0.3); // a 3.7 px move rounds back to the displayed value
    expect(dragValue(def, s, lut, vp, 250, 600, defLim)).toBe(0.1);
    expect(dragValue(def, s, lut, vp, 0, 103, defLim)).toBe(0.4);
    const min = start("deformMin", 250, 338);
    expect(dragValue(min, s, lut, vp, 250, 298, defLim)).toBe(0.15);
    expect(dragValue(min, s, lut, vp, 250, 100, defLim)).toBe(0.3); // min stops at max
  });
  it("only rewrites a draft value finer than the display on an actual move", () => {
    const fine = { ...boxSpec(), deformMin: 0.123456 };
    const y = 20 + (0.5 - 0.123456) * 800;
    const session = start("deformMin", 250, y, fine);
    expect(session.valueAtPress).toBe(0.123456);
    expect(dragValue(session, fine, lut, vp, 250, y - 8, defLim)).toBe(0.13); // 8 px up: 0.133456 -> 0.13
  });
  it("moves an E contour by the change of E under its anchor point, and skips positions without an E", () => {
    const ge: GateSpec = { ...boxSpec(true, 2, 4), areaMin: 0, areaMax: 600, deformMin: 0, deformMax: 0.5 };
    const both: EContours = { min: isoContour(lut, 2), max: isoContour(lut, 4) };
    const eLim = fieldLimits("targetEmodulusMin");
    const [pressX, pressY] = handleAnchor("emodMin", ge, both, vp)!;
    const session = start("emodMin", pressX, pressY, ge, both);
    expect(session.valueAtPress).toBe(2);
    expect(Math.abs(session.eAtAnchor - 2)).toBeLessThan(0.05);
    expect(dragValue(session, ge, lut, vp, pressX, pressY, eLim)).toBe(2);
    // Moving right (larger area) raises E; E min stops at E max (4.0); left lowers it.
    const up = dragValue(session, ge, lut, vp, pressX + 20, pressY, eLim);
    const down = dragValue(session, ge, lut, vp, pressX - 20, pressY, eLim);
    expect(up).not.toBeNull();
    expect(down).not.toBeNull();
    expect(up!).toBeGreaterThan(2);
    expect(down!).toBeLessThan(2);
    expect(Math.abs(up! - (2 + 0.2 * (1 - 2 * toDataPoint(vp, pressX, pressY)[1])))).toBeLessThan(0.011); // 20 px = 20 µm²
    expect(Math.abs(up! * 100 - Math.round(up! * 100))).toBeLessThan(1e-9); // 2 decimals
    const [hx, hy] = toPixel(vp, 580, 0);
    expect(dragValue(session, ge, lut, vp, hx - session.anchorOffsetX, hy - session.anchorOffsetY, eLim)).toBe(4); // stops at E max
    // Beyond the table there is no E: the edit is skipped.
    const wide: Viewport = { ...vp, x1: 1200, width: 1200 };
    const wideSession = start("emodMin", pressX, pressY, ge, both, wide);
    expect(dragValue(wideSession, ge, lut, wide, wide.left + 1100, pressY, eLim)).toBeNull();
    expect(dragValue(session, ge, null, vp, pressX + 20, pressY, eLim)).toBeNull(); // no table: no E drag
    expect(dragValue({ ...session, kind: "none" }, ge, lut, vp, pressX, pressY, eLim)).toBeNull();
  });
});

describe("7. the live count", () => {
  const xs = [150, 350, 200, 200, 250], ys = [0.2, 0.2, 0.05, 0.25, 0.15];
  const e = [2, 2, 2, NaNv, 5];
  it("counts the box, an E range with a table, and nothing with the gate off", () => {
    // box only: 1, 4, 5 are inside (350 is out, 0.05 is out)
    expect(countTargets(boxSpec(false), true, xs, ys, e)).toEqual({ target: 3, total: 5 });
    // with E in [1, 3] and a table: cell 4 has no E, cell 5 has E 5 -> only cell 1
    expect(countTargets(boxSpec(true, 1, 3), true, xs, ys, e)).toEqual({ target: 1, total: 5 });
    const off = { ...boxSpec(true, 1, 3), enabled: false };
    expect(countTargets(off, true, xs, ys, e).target).toBe(0);
    expect(countTargets(off, false, xs, ys, e).target).toBe(0);
    expect(countTargets(boxSpec(true, 1, 3), true, xs, ys, []).target).toBe(0); // table loaded but no E values
  });
  it("is the box without a table, whatever the stored E and the E range", () => {
    const placeholder = [0, 0, 0, 0, 0];
    for (const eMin of [0, 5]) {
      for (const values of [e, placeholder, []]) expect(countTargets(boxSpec(true, eMin, 10), false, xs, ys, values)).toEqual({ target: 3, total: 5 });
      expect(countTargets(boxSpec(true, eMin, 10), false, xs, ys).target).toBe(3);
    }
  });
  it("words the count like Qt", () => {
    expect(countTargets(boxSpec(false), true, [], [], []).total).toBe(0);
    expect(previewText({ target: 3, total: 5 })).toBe("Target group preview: 3 of 5 plotted valid cells (60.0%)");
    expect(previewText({ target: 0, total: 0 })).toContain("no plotted valid cells");
  });
});
