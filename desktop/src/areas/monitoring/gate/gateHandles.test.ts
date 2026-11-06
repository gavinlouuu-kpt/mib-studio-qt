// What the gate's handles say and allow (MON-065, 066): Qt's cursors and tooltip, the editable rule, the unapplied pill.
import { describe, expect, it } from "vitest";
import { initialTuneState, tuneReducer, type TuneState, type TuneValues } from "../tuneDraft";
import { TARGET_GATE_FIELDS, gateUnapplied, handleCursor, handleEditable, handleToolTip } from "./gateHandles";

const running: TuneValues = {
  areaEnabled: true, areaMin: 60, areaMax: 290, deformabilityEnabled: false, deformabilityMin: 0, deformabilityMax: 1,
  ringRatioEnabled: true, ringRatioMin: 15, ringRatioMax: 25, areaRatioEnabled: false, areaRatioMax: 1.5, borderEnabled: true,
  singleInnerContourEnabled: true, targetGroupEnabled: true, targetAreaMin: 72, targetAreaMax: 400, targetDeformabilityMin: 0.05,
  targetDeformabilityMax: 0.3, targetEmodulusEnabled: true, targetEmodulusMin: 2, targetEmodulusMax: 6.5, multiImageEnabled: false, multiImageCount: 1,
};
const loaded = (): TuneState => tuneReducer(initialTuneState(), { type: "external", values: running });
const edited = (field: Parameters<typeof tuneReducer>[1] extends infer A ? A extends { type: "edit"; field: infer F } ? F : never : never, value: unknown) =>
  tuneReducer(loaded(), { type: "edit", field, value });

describe("the tooltip of a handle", () => {
  it("is Qt's, with the field's label, the draft value at its decimals and the unit", () => {
    const d = running;
    expect(handleToolTip("areaMax", d, false)).toBe("Target group area maximum (µm²): 400 µm² — drag to change");
    expect(handleToolTip("deformMin", d, false)).toBe("Target group deformability minimum: 0.05 — drag to change");
    expect(handleToolTip("emodMax", d, false)).toBe("Target group Young's modulus maximum (kPa): 6.50 kPa — drag to change");
  });
  it("loses the hint while dragging", () => {
    expect(handleToolTip("areaMax", { ...running, targetAreaMax: 330 }, true)).toBe("Target group area maximum (µm²): 330 µm²");
  });
});

describe("cursors", () => {
  it("resize horizontally for an area edge, vertically for a deformability edge, in any direction for an E contour", () => {
    expect([handleCursor("areaMin"), handleCursor("areaMax")]).toEqual(["ew-resize", "ew-resize"]);
    expect([handleCursor("deformMin"), handleCursor("deformMax")]).toEqual(["ns-resize", "ns-resize"]);
    expect([handleCursor("emodMin"), handleCursor("emodMax")]).toEqual(["move", "move"]);
  });
});

describe("a handle is editable exactly when its control would be enabled", () => {
  it("needs the running configuration, the gate on, and for an E contour the E range on", () => {
    expect(handleEditable("areaMax", initialTuneState(), false)).toBe(false); // the running configuration is not known yet
    expect(handleEditable("areaMax", loaded(), false)).toBe(true);
    expect(handleEditable("emodMin", loaded(), false)).toBe(true);
    expect(handleEditable("areaMax", edited("targetGroupEnabled", false), false)).toBe(false);
    expect(handleEditable("emodMin", edited("targetEmodulusEnabled", false), false)).toBe(false);
    expect(handleEditable("areaMax", edited("targetEmodulusEnabled", false), false)).toBe(true);
    expect(handleEditable("none", loaded(), false)).toBe(false);
  });
  it("is closed while an apply is in flight, and for a client that does not control the instrument", () => {
    const applying = tuneReducer(loaded(), { type: "begin", requestId: 1 });
    expect(handleEditable("areaMax", applying, false)).toBe(false);
    expect(handleEditable("areaMax", loaded(), true)).toBe(false);
  });
});

describe("unapplied", () => {
  it("is true as soon as any target-group field differs from the applied baseline, and not for another field", () => {
    expect(gateUnapplied(loaded())).toBe(false);
    for (const field of TARGET_GATE_FIELDS) {
      const value = typeof running[field] === "boolean" ? !running[field] : Number(running[field]) + 1;
      expect(gateUnapplied(edited(field, value)), field).toBe(true);
    }
    expect(gateUnapplied(edited("areaMin", 80))).toBe(false); // an acceptance field is not the gate
  });
  it("goes back to false when the edit returns to the baseline", () => {
    let s = edited("targetAreaMax", 500);
    expect(gateUnapplied(s)).toBe(true);
    s = tuneReducer(s, { type: "edit", field: "targetAreaMax", value: 400 });
    expect(gateUnapplied(s)).toBe(false);
  });
});
