import { describe, expect, it } from "vitest";
import { gridGeometry, imageBand, pagesFor, PageCache, scrollToReveal, THUMB_PAGE, THUMB_SIZE, thumbTooltip, tileRgba, visibleRange } from "./thumbnails";
import { FRAME_PACKET } from "../bridgeContract";
import { columnText, DEFAULT_COLUMNS, METRIC_COLUMNS, pageOffsetFor, toggleColumn, visibleColumns } from "./metricsColumns";
import type { ReviewInfo, ReviewRow } from "./reviewBridge";
import { initialTab } from "./ReviewPanel";
import { leaveTab, newTabMemory, recallScroll, recallSelection, rememberScroll } from "./tabMemory";

describe("thumbnail grid geometry", () => {
  it("fits whole columns and rounds rows up", () => {
    const g = gridGeometry(250, 700, 136);
    expect(g.columns).toBe(5);
    expect(g.rows).toBe(50);
    expect(g.height).toBe(50 * 136);
    expect(gridGeometry(3, 10, 136).columns).toBe(1);
    expect(gridGeometry(0, 700, 136).rows).toBe(0);
  });

  it("visible range covers the viewport plus overscan, clamped to the data", () => {
    const g = gridGeometry(1000, 680, 136); // 5 columns
    expect(visibleRange(0, 400, g, 1000, 0)).toEqual({ start: 0, end: 15 });
    expect(visibleRange(136 * 10, 400, g, 1000, 1)).toEqual({ start: 45, end: 70 });
    expect(visibleRange(g.height, 400, g, 1000, 2).end).toBe(1000);
    expect(visibleRange(0, 400, g, 0)).toEqual({ start: 0, end: 0 });
  });

  it("non-square cells: columns by width pitch, rows by height pitch", () => {
    const g = gridGeometry(100, 700, 40, 136);
    expect(g.columns).toBe(5);
    expect(g.height).toBe(20 * 40);
    expect(visibleRange(0, 200, g, 100, 0)).toEqual({ start: 0, end: 30 });
  });

  it("image band matches the backend letterbox (512x96 into 128)", () => {
    expect(imageBand(128, 512, 96)).toEqual({ x: 0, y: 52, w: 128, h: 24 });
    expect(imageBand(128, 96, 512)).toEqual({ x: 52, y: 0, w: 24, h: 128 });
    expect(imageBand(128, 0, 0)).toEqual({ x: 0, y: 0, w: 128, h: 128 });
  });

  it("pages cover a range in page-size steps", () => {
    expect(pagesFor(0, 15, 100)).toEqual([0]);
    expect(pagesFor(95, 205, 100)).toEqual([0, 100, 200]);
    expect(pagesFor(10, 10)).toEqual([]);
  });

  it("a thumbnail page fits one frame packet", () => {
    expect(THUMB_SIZE * THUMB_PAGE).toBeLessThanOrEqual(FRAME_PACKET.max_dimension);
  });

  it("scrolls only when the item is outside the viewport", () => {
    const g = gridGeometry(1000, 680, 100);
    expect(scrollToReveal(2, 0, 300, g)).toBeNull();
    // 680 px / 100 px = 6 columns → item 50 is on row 8 (top 800).
    expect(scrollToReveal(50, 0, 300, g)).toBe(800 + 100 - 300);
    expect(scrollToReveal(0, 500, 300, g)).toBe(0);
  });
});

describe("packed thumbnail strips", () => {
  it("extracts mono and rgb tiles", () => {
    const size = 2;
    const mono = new Uint8Array([1, 2, 3, 4, 9, 9, 9, 9]);
    expect(Array.from(tileRgba(mono, size, 1, 1)).slice(0, 4)).toEqual([9, 9, 9, 255]);
    expect(Array.from(tileRgba(mono, size, 1, 0)).slice(4, 8)).toEqual([2, 2, 2, 255]);
    const rgb = new Uint8Array(2 * 2 * 3 * 2);
    rgb.set([10, 20, 30], 12);
    expect(Array.from(tileRgba(rgb, size, 3, 1)).slice(0, 4)).toEqual([10, 20, 30, 255]);
    expect(Array.from(tileRgba(rgb, size, 3, 5))).toEqual(new Array(16).fill(0));
  });

  it("page cache evicts least recently used", () => {
    const c = new PageCache<string>(2);
    c.set(0, "a");
    c.set(100, "b");
    expect(c.get(0)).toBe("a");
    c.set(200, "c");
    expect(c.has(100)).toBe(false);
    expect(c.has(0) && c.has(200)).toBe(true);
    c.clear();
    expect(c.size).toBe(0);
  });
});

describe("metrics columns", () => {
  const row = {
    frame_index: "42", timestamp_ns: "1000", valid: true, target_group: false, touches_border: false,
    has_single_inner_contour: true, in_range: true, in_channel: true, inner_contour_count: 1, object_id: 3,
    object_count: 1, track_id: 7, track_first_frame: "40", track_last_frame: "44", track_observation_count: 5,
    bbox_x: 1, bbox_y: 2, bbox_width: 10, bbox_height: 8, centroid_x: 5.25, centroid_y: 6.5, area: 120.04,
    area_um2: 30.01, deformability: 0.12345, area_ratio: 1.01, ring_ratio: 0.5, laplacian_variance: null,
    youngs_modulus: 2.345, brightness_q1: 1, brightness_q2: 2, brightness_q3: 3, brightness_q4: 4,
  } satisfies ReviewRow;

  it("covers every row field the Qt model shows and formats them", () => {
    expect(METRIC_COLUMNS.length).toBeGreaterThanOrEqual(23);
    const byId = Object.fromEntries(METRIC_COLUMNS.map((c) => [c.id, c.format(row)]));
    expect(byId.frame_index).toBe("42");
    expect(byId.laplacian_variance).toBe("—");
    expect(byId.bbox).toBe("1,2 10×8");
  });

  // HdfMetricsModel::data (REVIEW-020): deformability 3 dp, area (px and µm²) 2 dp,
  // area ratio 3 dp, ring ratio 3 dp, brightness 2 dp, E 3 dp, Yes / No.
  it("formats numbers like the Qt model", () => {
    const text = (id: string, r: ReviewRow = row) => columnText(id, r);
    expect(text("deformability")).toBe("0.123");
    expect(text("area")).toBe("120.04");
    expect(text("area_um2")).toBe("30.01");
    expect(text("area_ratio")).toBe("1.010");
    expect(text("ring_ratio")).toBe("0.500");
    expect(text("brightness")).toBe("1.00 / 2.00 / 3.00 / 4.00");
    expect(text("youngs_modulus")).toBe("2.345");
    expect(text("valid")).toBe("Yes");
    expect(text("target_group")).toBe("No");
    expect(text("touches_border")).toBe("No");
    expect(text("inner_contour_count")).toBe("1");
  });

  it("leaves E blank when it is NaN (JSON null or non-finite)", () => {
    for (const e of [null, NaN, Infinity, undefined]) expect(columnText("youngs_modulus", { ...row, youngs_modulus: e as never })).toBe("");
    expect(columnText("youngs_modulus", { ...row, youngs_modulus: 0 })).toBe("0.000");
  });

  it("recording files show index and timestamp only", () => {
    expect(visibleColumns(DEFAULT_COLUMNS, true).map((c) => c.id)).toEqual(["frame_index", "timestamp_ns"]);
    expect(visibleColumns(DEFAULT_COLUMNS, false).map((c) => c.id)).toEqual(DEFAULT_COLUMNS);
  });

  it("toggling keeps table order and never empties the table", () => {
    expect(toggleColumn(["area"], "area")).toEqual(["area"]);
    expect(toggleColumn(["area"], "frame_index")).toEqual(["frame_index", "area"]);
    expect(toggleColumn(["frame_index", "area"], "frame_index")).toEqual(["area"]);
  });

  it("page offset for an index", () => {
    expect(pageOffsetFor(0, 100)).toBe(0);
    expect(pageOffsetFor(250, 100)).toBe(200);
    expect(pageOffsetFor(-3, 100)).toBe(0);
  });
});

describe("thumbnail tooltip", () => {
  // HdfReviewTab.cpp setToolTip: "Frame %1\nDouble-click to view details", %1 the tile's 0-based position.
  it("is the Qt text", () => {
    expect(thumbTooltip(0)).toBe("Frame 0\nDouble-click to view details");
    expect(thumbTooltip(42)).toBe("Frame 42\nDouble-click to view details");
  });
});

describe("initial review tab", () => {
  const info = (valid: number, invalid: number, recording = false) =>
    ({
      recording_file: recording,
      valid_images: { count: String(valid) },
      invalid_images: { count: String(invalid) },
      recorded_images: { count: String(recording ? valid : 0) },
    }) as unknown as ReviewInfo;
  it("opens on the invalid set only when there are no valid frames", () => {
    expect(initialTab(info(10, 4))).toBe("valid");
    expect(initialTab(info(0, 200))).toBe("invalid");
    expect(initialTab(info(0, 0))).toBe("valid");
    expect(initialTab(info(5, 0, true))).toBe("valid");
  });
});

describe("REVIEW-018 per-tab memory", () => {
  it("keeps each set's scroll position, per file", () => {
    const m = newTabMemory();
    expect(recallScroll(m, "a", "valid")).toBeNull();
    rememberScroll(m, "a", "valid", 300);
    rememberScroll(m, "a", "invalid", 40);
    expect(recallScroll(m, "a", "valid")).toBe(300);
    expect(recallScroll(m, "a", "invalid")).toBe(40);
    // Another file starts from nothing, as Qt resets both values on load.
    expect(recallScroll(m, "b", "valid")).toBeNull();
    rememberScroll(m, "b", "invalid", 7);
    expect(recallScroll(m, "b", "valid")).toBeNull();
    expect(recallScroll(m, "a", "valid")).toBeNull();
    rememberScroll(m, "b", "valid", -5);
    expect(recallScroll(m, "b", "valid")).toBe(0);
  });

  it("returns to the selection a set was left on, else its first item", () => {
    const m = newTabMemory();
    expect(recallSelection(m, "a", "valid", 250)).toBe(0);
    expect(recallSelection(m, "a", "invalid", 0)).toBe(-1);
    leaveTab(m, "a", "valid", 57);
    leaveTab(m, "a", "invalid", 1);
    expect(recallSelection(m, "a", "valid", 250)).toBe(57);
    expect(recallSelection(m, "a", "invalid", 2)).toBe(1);
    // A remembered position past the end of a (reloaded) set, or another file, falls back to the first.
    expect(recallSelection(m, "a", "valid", 50)).toBe(0);
    expect(recallSelection(m, "b", "valid", 250)).toBe(0);
  });

  it("leaving with nothing selected remembers nothing", () => {
    const m = newTabMemory();
    leaveTab(m, "a", "valid", 12);
    leaveTab(m, "a", "valid", -1);
    expect(recallSelection(m, "a", "valid", 250)).toBe(12);
  });

  it("a cell chosen on the scatter belongs to the Valid set and drops its stale scroll position", () => {
    const m = newTabMemory();
    leaveTab(m, "a", "valid", 3);
    rememberScroll(m, "a", "valid", 900);
    leaveTab(m, "a", "charts", 3); // nothing new chosen: the scroll still says where the cell is
    expect(recallScroll(m, "a", "valid")).toBe(900);
    leaveTab(m, "a", "charts", 100);
    expect(recallSelection(m, "a", "valid", 250)).toBe(100);
    expect(recallScroll(m, "a", "valid")).toBeNull(); // the grid reveals the new selection instead
  });
});
