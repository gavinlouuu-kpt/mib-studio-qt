import { describe, expect, it } from "vitest";
import type { MonitoringRow } from "../../../bridge";
import { densityRowKey } from "../../../monitoringDensity";
import { monitoringRowKey } from "../monitoringChartModel";
import { densityReply } from "./densityFixtures";
import {
  densityLevel, densityLevels, densityTooltip, estimateOfReply, KDE_LEVELS, KDE_TOOLTIP_OFF, LEVEL_COLOURS, pinnedLabel,
  type DensityEstimate,
} from "./densityModel";

const row = (over: Partial<MonitoringRow> = {}): MonitoringRow => ({
  frame_index: 1, timestamp_ns: 1, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: 0, centroid_x: 0, centroid_y: 0,
  area: 100, deformability: 0.2, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 1, start_generation: "1", capture_epoch: "2", ...over,
});

const reply = densityReply;

describe("the density level (Qt kdeLevelForDensity)", () => {
  it("truncates 0..1 into eight levels, the densest sample in the last", () => {
    expect(KDE_LEVELS).toBe(8);
    expect([0, 0.1249, 0.125, 0.2499, 0.25, 0.5, 0.874, 0.875, 0.999, 1].map(densityLevel)).toEqual([0, 0, 1, 1, 2, 4, 6, 7, 7, 7]);
  });
  it("clamps out-of-range values and reads anything that is not finite as the sparsest level", () => {
    expect([-3, 2, 1e9].map(densityLevel)).toEqual([0, 7, 7]);
    expect([NaN, Infinity, -Infinity, null, undefined].map(densityLevel)).toEqual([0, 0, 0, 0, 0]);
  });
});

describe("the ramp (Qt densityRampColor, MonitoringDensity.h)", () => {
  // The header's ten stops, written out here independently of the contract constants the chart reads.
  const STOPS = [0x86b6ef, 0x6da7ec, 0x5598e7, 0x3987e5, 0x2a78d6, 0x256abf, 0x1c5cab, 0x184f95, 0x104281, 0x0d366b].map((v) => [(v >> 16) & 255, (v >> 8) & 255, v & 255]);
  const qtRamp = (t: number) => {
    const last = STOPS.length - 1, pos = Math.min(1, Math.max(0, t)) * last, lo = Math.min(Math.floor(pos), last - 1), f = pos - lo;
    const [a, b] = [STOPS[lo], STOPS[lo + 1]];
    return `rgb(${[0, 1, 2].map((k) => Math.round(a[k] + (b[k] - a[k]) * f)).join(", ")})`;
  };
  it("gives each level kdeLevelColor(level, 8) = densityRampColor(level / 7), light blue to dark blue", () => {
    expect(LEVEL_COLOURS).toHaveLength(8);
    expect(LEVEL_COLOURS).toEqual(Array.from({ length: 8 }, (_, k) => qtRamp(k / 7)));
    expect(LEVEL_COLOURS[0]).toBe("rgb(134, 182, 239)");
    expect(LEVEL_COLOURS[7]).toBe("rgb(13, 54, 107)");
    expect(new Set(LEVEL_COLOURS).size).toBe(8);
  });
});

describe("a level per plotted point", () => {
  const rows = [row({ frame_index: 10 }), row({ frame_index: 11, object_id: 3 }), row({ frame_index: 12 }), row({ frame_index: 13, capture_epoch: "9" })];
  const est = estimateOfReply(reply([
    { frame: 10, density: 1 }, { frame: 11, object: 3, density: 0.5 }, { frame: 13, density: 0.9 /* another capture epoch than the row's */ }, { frame: 99, density: 0.3 },
  ]));
  it("names a row the way the estimate names its points", () => {
    const r = row({ frame_index: 10, object_id: 3, start_generation: "5", capture_epoch: "6" });
    expect(monitoringRowKey(r)).toBe(densityRowKey({ start_generation: "5", capture_epoch: "6", frame_index: 10, object_id: 3 }));
    expect(est.byRow.get(monitoringRowKey(rows[1]))).toBe(0.5);
  });
  it("puts a point newer than the estimate, or from another capture, in the sparsest level", () => {
    expect(Array.from(densityLevels(rows, est.byRow))).toEqual([7, 4, 0, 0]);
  });
  it("is all sparsest before the first estimate and for an empty one", () => {
    expect(Array.from(densityLevels(rows, null))).toEqual([0, 0, 0, 0]);
    expect(Array.from(densityLevels(rows, new Map()))).toEqual([0, 0, 0, 0]);
    expect(densityLevels([], est.byRow)).toHaveLength(0);
  });
  it("skips a point whose density is not finite", () => {
    const e = estimateOfReply(reply([{ frame: 10, density: null }, { frame: 11, object: 3, density: 0.5 }]));
    expect(Array.from(densityLevels(rows, e.byRow))).toEqual([0, 4, 0, 0]);
  });
});

describe("the estimate of a reply", () => {
  it("keeps what the tooltip and the layer need", () => {
    const e = estimateOfReply(reply([{ frame: 10, density: 1 }, { frame: 11, density: 0.5 }]));
    expect(e).toMatchObject({ generation: "7", pointCount: 2, bandwidthX: 12.345, bandwidthY: 0.01234, computeMs: 21, coreLevel: 0.4, coreCount: 3, refreshMs: 3000 });
    expect(e.contours).toEqual([[[1, 0.1], [2, 0.2], [1, 0.1]]]);
    expect(e.byRow.size).toBe(2);
  });
  it("is empty when the service holds no estimate (disabled, or the ring emptied)", () => {
    const base = reply([]);
    const e = estimateOfReply({ ...base, result: null });
    expect(e).toMatchObject({ generation: "7", pointCount: 0, coreLevel: null, coreCount: 0, refreshMs: 3000 });
    expect(e.byRow.size).toBe(0);
    expect(e.contours).toEqual([]);
  });
});

describe("the toggle's tooltip (MON-059, Qt refreshKdeTooltip)", () => {
  const estimate: DensityEstimate = estimateOfReply(reply([{ frame: 1, density: 1 }, { frame: 2, density: 0.5 }]));
  const base = { on: true, estimate, intervalMs: 2000, corePercent: 90, reference: null };

  it("says what the toggle does while KDE is off", () => {
    expect(densityTooltip({ ...base, on: false })).toBe(KDE_TOOLTIP_OFF);
    expect(KDE_TOOLTIP_OFF).toBe("Colour scatter points by local population density (Gaussian KDE, recomputed periodically)");
  });
  it("gives the point count, bandwidth (x, y), compute ms, the refresh spacing and the core cells and loops", () => {
    expect(densityTooltip(base)).toBe(
      "Density (KDE) over 2 points; bandwidth 12.3 μm² × 0.0123; computed in 21 ms; refreshed every 3 s\nCore 90%: 3 cells, 1 contour loop(s)",
    );
  });
  it("shows the larger of the interval setting and the service's own spacing", () => {
    expect(densityTooltip({ ...base, intervalMs: 5000 })).toContain("refreshed every 5 s");
    expect(densityTooltip({ ...base, intervalMs: 500, estimate: { ...estimate, refreshMs: 2500 } })).toContain("refreshed every 2.5 s");
    expect(densityTooltip({ ...base, intervalMs: 60000 })).toContain("refreshed every 60 s");
    expect(densityTooltip({ ...base, intervalMs: 1234 })).toContain("refreshed every 3 s"); // service spacing 3000 wins
    expect(densityTooltip({ ...base, estimate: { ...estimate, refreshMs: 500 }, intervalMs: 1234 })).toContain("refreshed every 1.23 s");
  });
  it("says so when there are too few cells for a contour, and adds the reference label", () => {
    const few = { ...estimate, coreLevel: null, coreCount: 0, contours: [] };
    expect(densityTooltip({ ...base, estimate: few })).toMatch(/\nCore: too few cells for a contour$/);
    const ref = { label: "run7.h5, full-run 90%", contours: [[[0, 0], [1, 1], [0, 0]] as [number, number][]] };
    expect(densityTooltip({ ...base, reference: ref })).toMatch(/\nReference contour: run7\.h5, full-run 90%$/);
    expect(densityTooltip({ ...base, reference: { label: "x", contours: [] } })).not.toContain("Reference contour");
  });
  it("reads as Qt's does before the first estimate", () => {
    expect(densityTooltip({ ...base, estimate: null })).toBe(
      "Density (KDE) over 0 points; bandwidth 0.0 μm² × 0.0000; computed in 0 ms; refreshed every 2 s\nCore: too few cells for a contour",
    );
  });
  it("labels a pinned reference like Qt: pinned HH:mm:ss", () => {
    expect(pinnedLabel(new Date(2026, 10, 5, 9, 4, 7))).toBe("pinned 09:04:07");
  });
});
