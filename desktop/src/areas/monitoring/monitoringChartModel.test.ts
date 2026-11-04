import { describe, expect, it } from "vitest";
import type { MonitoringRow, MonitoringSnapshot } from "../../bridge";
import {
  extentHistogram, latestRows, MONITORING_FETCH_ROWS, MONITORING_WINDOW, monitoringChartData, monitoringRowKey, plotTooltip, pointTooltip,
  ringHistogram, sameSnapshot, snapshotFingerprint,
} from "./monitoringChartModel";

const row = (over: Partial<MonitoringRow> = {}): MonitoringRow => ({
  frame_index: 1, timestamp_ns: 1, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: 0, centroid_x: 0, centroid_y: 0,
  area: 100, deformability: 0.1, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 1, ...over,
});

describe("the rolling window (MON-036)", () => {
  it("fetches the whole backend window: 1000 valid and 1000 invalid rows", () => {
    expect(MONITORING_WINDOW).toBe(1000);
    expect(MONITORING_FETCH_ROWS).toBe(2000);
  });

  it("plots the newest 1000 valid rows, not the first 200, and ignores invalid rows", () => {
    const rows = [
      ...Array.from({ length: 1200 }, (_, i) => row({ frame_index: i, area: i })),
      ...Array.from({ length: 50 }, (_, i) => row({ frame_index: 5000 + i, valid: false })),
    ];
    const d = monitoringChartData(rows);
    expect(d.validRows).toBe(1000);
    expect(d.scatter.xs.length).toBe(1000);
    expect(d.scatter.xs[0]).toBe(200); // the oldest 200 fell out of the window
    expect(d.scatter.xs[999]).toBe(1199);
    expect(d.ratios.length).toBe(1000);
  });
});

describe("scatter points (MON-037)", () => {
  it("converts each row's area with its own analysis-time calibration", () => {
    const d = monitoringChartData([row({ area: 100, pixel_to_micron: 0.5 }), row({ area: 100, pixel_to_micron: 2 })]);
    expect(Array.from(d.scatter.xs)).toEqual([25, 400]);
  });

  it("omits and counts valid rows without a usable calibration, never using the current one", () => {
    const rows = [row({ pixel_to_micron: 0 }), row({ pixel_to_micron: undefined }), row({ pixel_to_micron: NaN }), row({ pixel_to_micron: -1 }), row()];
    const d = monitoringChartData(rows);
    expect(d.uncalibrated).toBe(4);
    expect(d.scatter.xs.length).toBe(1);
    expect(rows[4].area).toBe(100);
  });

  it("marks the target group and drops non-finite coordinates without inventing values", () => {
    const d = monitoringChartData([row({ target_group: true }), row({ area: Infinity }), row({ deformability: NaN }), row()]);
    expect(Array.from(d.scatter.target)).toEqual([1, 0]);
    expect(d.scatter.rows.length).toBe(2);
    expect(Array.from(d.scatter.frames)).toEqual([1, 1]);
  });

  it("samples ring ratio and modulus independently of calibration, valid rows only", () => {
    const d = monitoringChartData([
      row({ pixel_to_micron: 0, ring_ratio: 18, youngs_modulus: 2 }),
      row({ ring_ratio: 0, youngs_modulus: NaN }),
      row({ ring_ratio: -3 }),
      row({ ring_ratio: NaN }),
      row({ valid: false, ring_ratio: 19, youngs_modulus: 9 }),
    ]);
    expect(d.ratios).toEqual([18]);
    expect(d.moduli).toEqual([2]);
    expect(d.uncalibrated).toBe(1);
  });
});

describe("ring histogram (MON-049, MON-091)", () => {
  it("has (max - min) / bin width bins, 20 by default on 15..25", () => {
    expect(ringHistogram([], 15, 25, 0.5).counts.length).toBe(20);
    expect(ringHistogram([], 0, 100, 0.01).counts.length).toBe(10000);
    expect(ringHistogram([], 15, 15.2, 0.5).counts.length).toBe(1);
  });

  it("bins like Qt: clamped into the range, the last bin takes the top", () => {
    const h = ringHistogram([10, 15, 15.4, 15.5, 24.9, 25, 30, NaN], 15, 25, 0.5);
    expect(h.counts[0]).toBe(3); // 10 clamped, 15, 15.4
    expect(h.counts[1]).toBe(1);
    expect(h.counts[19]).toBe(3); // 24.9, 25, 30 clamped
    expect(h.total).toBe(7);
    expect(h.counts.reduce((a, b) => a + b, 0)).toBe(7);
  });

  it("rounds the bin count as Qt does", () => {
    expect(ringHistogram([], 0, 10.3, 0.5).counts.length).toBe(21);
  });
});

describe("Young's modulus histogram (MON-051, Tauri only)", () => {
  it("keeps ten bins over the data extent", () => {
    const h = extentHistogram([2, 6]);
    expect([h.min, h.max, h.total]).toEqual([2, 6, 2]);
    expect(h.counts[0]).toBe(1);
    expect(h.counts[9]).toBe(1);
  });
  it("widens a single value and is empty without data", () => {
    expect(extentHistogram([3])).toMatchObject({ min: 2.5, max: 3.5 });
    expect(extentHistogram([]).total).toBe(0);
  });
});

describe("row identity and snapshot change detection", () => {
  it("keys a row like a cell and tolerates a backend without run and capture", () => {
    expect(monitoringRowKey(row({ start_generation: "3", capture_epoch: "9", frame_index: 12, object_id: 2 }))).toBe("3:9:12:2");
    expect(monitoringRowKey(row({ frame_index: 12, object_id: 2 }))).toBe("0:0:12:2");
  });

  const snap = (over: Partial<MonitoringSnapshot> = {}): MonitoringSnapshot => ({
    valid: true, monitoring_active: true, valid_held: 1, invalid_held: 0, valid_appended: 1, invalid_appended: 0, capacity: 1000,
    latest_timestamp_ns: 5, rows: [row()], ...over,
  });
  it("sees a new row, a clear and a changed capacity, and nothing else", () => {
    expect(sameSnapshot(snap(), snap())).toBe(true);
    expect(sameSnapshot(snap(), snap({ valid_appended: 2, latest_timestamp_ns: 6, rows: [row(), row({ frame_index: 2 })] }))).toBe(false);
    expect(sameSnapshot(snap(), snap({ valid_held: 0, valid_appended: 0, rows: [] }))).toBe(false);
    expect(sameSnapshot(snap(), snap({ monitoring_active: false }))).toBe(false);
    expect(sameSnapshot(null, null)).toBe(true);
    expect(sameSnapshot(null, snap())).toBe(false);
    expect(snapshotFingerprint(null)).toBe("");
  });
});

describe("tooltips (MON-043)", () => {
  it("says area and deformability anywhere in the plot, and E when the table gives one", () => {
    expect(plotTooltip(123.456, 0.12345)).toBe("Area 123.5 µm² · deformability 0.123");
    expect(plotTooltip(123.456, 0.12345, 4.5)).toBe("Area 123.5 µm² · deformability 0.123 · E 4.50 kPa");
    expect(plotTooltip(1, 1, NaN)).toBe("Area 1.0 µm² · deformability 1.000");
    expect(plotTooltip(1, 1, null)).toBe("Area 1.0 µm² · deformability 1.000");
  });
  it("names the frame over a point", () => {
    expect(pointTooltip(77, 250.04, 0.12345)).toBe("Frame 77 · 250.0 µm² · deformability 0.1235 · click to view");
  });
});

describe("the latest-rows table", () => {
  it("shows the newest rows of both kinds, not the invalid tail of the snapshot", () => {
    // A snapshot lists valid rows (oldest first), then invalid rows (oldest first).
    const rows = [
      ...[1, 3, 5, 7, 9].map((t) => row({ frame_index: t, timestamp_ns: t })),
      ...[2, 4, 6].map((t) => row({ frame_index: t, timestamp_ns: t, valid: false })),
    ];
    expect(latestRows(rows, 4).map((r) => r.timestamp_ns)).toEqual([5, 6, 7, 9]);
    expect(latestRows(rows, 15).map((r) => r.timestamp_ns)).toEqual([1, 2, 3, 4, 5, 6, 7, 9]);
    expect(latestRows([], 15)).toEqual([]);
  });
});
