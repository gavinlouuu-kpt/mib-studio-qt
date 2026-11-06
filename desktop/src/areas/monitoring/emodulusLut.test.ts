// The E-modulus table as the Monitoring view holds it (epic #866, PR E1): the prepared grid, the tooltip's
// readout and the tune panel's hint. The lookup itself is checked against the Qt cases in gate/targetGate.test.ts.
import { describe, expect, it } from "vitest";
import type { EmodulusLutInfo } from "../../monitoringDensity";
import { emodulusReadout, emodulusTableHint, lookupEmodulus, prepareLut, tableLut, type EmodulusTableState } from "./emodulusLut";

const info: EmodulusLutInfo = { path: "/data/lut/scaled_isoelastic_LUT.txt", sha256: "ab", lut_id: "", revision: "", source: "", catalog_sha256: "", checksum_status: "" };
const grid = { area_min: 0, area_step: 100, area_bins: 3, deform_min: 0, deform_step: 0.1, deform_bins: 3, values: [1, 2, 3, 2, 3, 4, 3, null, 5] as (number | null)[] };
const loaded = (over: Partial<EmodulusLutInfo> = {}): EmodulusTableState => ({ status: "loaded", table: { identity: "ab|/x", info: { ...info, ...over }, lut: prepareLut(grid) } });

describe("prepareLut", () => {
  it("keeps the grid, turns null into NaN and derives the upper bounds", () => {
    const lut = prepareLut(grid);
    expect([lut.areaMax, lut.deformMax, lut.areaStep, lut.deformStep]).toEqual([200, 0.2, 100, 0.1]);
    expect(Number.isNaN(lut.values[7])).toBe(true);
    expect(lut.values[8]).toBe(5);
  });
  it("refuses a grid whose size is not area_bins x deform_bins", () => {
    expect(() => prepareLut({ ...grid, values: [1, 2] })).toThrow(/area_bins x deform_bins/);
  });
});

describe("the tooltip's E readout", () => {
  it("is the bilinear lookup with a table and a value there", () => {
    expect(emodulusReadout(loaded(), 50, 0.05)).toBeCloseTo(2, 12); // the mean of 1, 2, 2, 3
    expect(lookupEmodulus(tableLut(loaded()), 0, 0)).toBe(1);
  });
  it("is absent without a table, outside it, or where a corner has no value", () => {
    expect(emodulusReadout({ status: "none" }, 50, 0.05)).toBeNull();
    expect(emodulusReadout({ status: "unknown" }, 50, 0.05)).toBeNull();
    expect(emodulusReadout({ status: "error", message: "x" }, 50, 0.05)).toBeNull();
    expect(emodulusReadout(loaded(), 250, 0.05)).toBeNull();
    expect(emodulusReadout(loaded(), 150, 0.15)).toBeNull(); // the cell holds the missing value
  });
});

describe("the Young's-modulus hint (MON-077)", () => {
  it("says whether a table is loaded, and what happens to the range without one", () => {
    expect(emodulusTableHint(loaded())).toBe("E-modulus table loaded (scaled_isoelastic_LUT.txt): this range filters the target group.");
    expect(emodulusTableHint(loaded({ lut_id: "lut-6.16" }))).toContain("(lut-6.16)");
    expect(emodulusTableHint({ status: "none" })).toBe("No E-modulus table is loaded: this range is skipped and the target group is the area and deformability box alone.");
    expect(emodulusTableHint({ status: "error", message: "backend gone" })).toContain("backend gone");
    expect(emodulusTableHint({ status: "unknown" })).toMatch(/Checking/);
  });
});
