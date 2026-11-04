// Live cell strips (#866 MON-052 .. MON-055): the pure parts (cellStrips.ts).
import { expect, it } from "vitest";
import type { MonitoringCell, MonitoringCellReason } from "../../monitoringCells";
import {
  CACHE_CELLS, CellImageCache, GRID_COLUMNS, REASON_LABELS, STRIP_CELLS, THUMBNAIL_SIZE, TINT_INVALID, TINT_TARGET,
  TINT_VALID, cellTint, composeThumbnail, fitThumbnail, listedKeys, overlayRegions, planFetches, reasonTooltip,
  reasonsLine, reasonsTooltip, stripCount,
} from "./cellStrips";

const reason = (code: MonitoringCellReason["code"], value: number | null = null, min: number | null = null, max: number | null = null): MonitoringCellReason =>
  ({ code, value, min, max });

const cell = (frame: number, extra: Partial<MonitoringCell> = {}): MonitoringCell => ({
  start_generation: "0", capture_epoch: "3", frame_index: String(frame), object_id: 0, timestamp_ns: "0", valid: true,
  target_group: false, object_count: 1, track_id: -1, centroid_x: 5, centroid_y: 5, bbox: { x: 1, y: 1, w: 8, h: 8 },
  area: 100, deformability: 0.1, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 0.5, reasons: [],
  image: { width: 10, height: 10, crop_x: 0, crop_y: 0, source_width: 100, source_height: 50, has_mask: true, bytes: "200" },
  no_image_reason: null, ...extra,
});

it("matches Qt's geometry: 100 px thumbnails, five per row, 25 cells", () => {
  expect([THUMBNAIL_SIZE, GRID_COLUMNS, STRIP_CELLS]).toEqual([100, 5, 25]);
  expect(stripCount(3)).toBe("3 of last 25");
});

it("labels every reason code as Qt's short text, joined by ' | '", () => {
  expect(REASON_LABELS).toEqual({
    no_contour: "No contour", border: "Border", area: "Area", ring: "Ring", deform: "Deform", area_ratio: "Ratio",
    focus: "Focus", wall: "Wall",
  });
  const all = (Object.keys(REASON_LABELS) as (keyof typeof REASON_LABELS)[]).map(code => reason(code));
  expect(reasonsLine(all)).toBe("No contour | Border | Area | Ring | Deform | Ratio | Focus | Wall");
  expect(reasonsLine([])).toBe("");
  expect(reasonsLine([reason("area", 5, 60, 290), reason("ring", 5, 15, 25)])).toBe("Area | Ring");
});

it("words each tooltip line as Qt does, with the measured value and the range", () => {
  expect(reasonTooltip(reason("no_contour"))).toBe("No inner contour found");
  expect(reasonTooltip(reason("border"))).toBe("Contour touches ROI border");
  expect(reasonTooltip(reason("area", 154.2, 60, 290))).toBe("Area: 154 μm² (range: 60-290)");
  expect(reasonTooltip(reason("area", 5, 60, 290))).toBe("Area: 5 μm² (range: 60-290)");
  expect(reasonTooltip(reason("ring", 5.04, 15, 25))).toBe("Ring ratio: 5.0 (range: 15.0-25.0)");
  expect(reasonTooltip(reason("deform", 0.5, 0, 0.4))).toBe("Deformability: 0.500 (range: 0.000-0.400)");
  expect(reasonTooltip(reason("area_ratio", 2.25, null, 1.5))).toBe("Area ratio: 2.25 (max: 1.50)");
  expect(reasonTooltip(reason("focus", 3, 5, 50))).toBe("Laplacian variance: 3.0 (range: 5.0-50.0)");
  expect(reasonTooltip(reason("focus", null, 5, 50))).toBe("Laplacian variance: n/a (range: 5.0-50.0)");
  expect(reasonTooltip(reason("wall", 7.5))).toBe("Centroid row 7.5 outside the channel band");
  expect(reasonTooltip(reason("wall", 7.5, 10, 30))).toBe("Centroid row 7.5 outside the channel band (band: 10-30)");
  expect(reasonsTooltip([reason("area", 5, 60, 290), reason("border")])).toBe("Area: 5 μm² (range: 60-290)\nContour touches ROI border");
  expect(reasonsTooltip([])).toBe("");
});

it("fits a crop into the thumbnail box keeping its aspect ratio, small crops scaled up", () => {
  expect(fitThumbnail(40, 36)).toEqual({ width: 100, height: 90 });
  expect(fitThumbnail(256, 128)).toEqual({ width: 100, height: 50 });
  expect(fitThumbnail(10, 400)).toEqual({ width: 3, height: 100 });
  expect(fitThumbnail(0, 0)).toEqual({ width: 100, height: 100 });
  expect(fitThumbnail(512, 1)).toEqual({ width: 100, height: 1 });
});

it("tints blue for the target group, green for valid, red for invalid", () => {
  expect(cellTint({ valid: true, target_group: true })).toBe(TINT_TARGET);
  expect(cellTint({ valid: true, target_group: false })).toBe(TINT_VALID);
  expect(cellTint({ valid: false, target_group: false })).toBe(TINT_INVALID);
  expect([TINT_TARGET, TINT_VALID, TINT_INVALID]).toEqual([[0, 120, 255], [0, 255, 0], [255, 0, 0]]);
});

// A w x h mask from rows of '#' (foreground) and '.' (background).
const maskOf = (rows: string[]) => Uint8Array.from(rows.join("").split("").map(c => (c === "#" ? 255 : 0)));
const picture = (flags: Uint8Array, w: number) =>
  Array.from({ length: flags.length / w }, (_, y) => Array.from(flags.slice(y * w, (y + 1) * w), v => (v ? "x" : ".")).join(""));

it("tints the hole an outline encloses, and outlines every contour", () => {
  const ring = [
    ".........",
    ".#######.",
    ".#.....#.",
    ".#.....#.",
    ".#######.",
    ".........",
  ];
  const { tint, outline } = overlayRegions(maskOf(ring), 9, 6);
  // The inner region: the hole and the foreground pixels along it.
  expect(picture(tint, 9)).toEqual([
    ".........",
    "..xxxxx..",
    ".xxxxxxx.",
    ".xxxxxxx.",
    "..xxxxx..",
    ".........",
  ]);
  // Every contour: the outer outline and the hole's, all on foreground pixels.
  expect(picture(outline, 9)).toEqual([
    ".........",
    ".xxxxxxx.",
    ".x.....x.",
    ".x.....x.",
    ".xxxxxxx.",
    ".........",
  ]);
});

it("tints the whole mask when it has no hole, and a mask cut by the crop's edge is no hole", () => {
  const blob = ["....", ".##.", ".##.", "...."];
  const { tint, outline } = overlayRegions(maskOf(blob), 4, 4);
  expect(picture(tint, 4)).toEqual(["....", ".xx.", ".xx.", "...."]);
  expect(picture(outline, 4)).toEqual(["....", ".xx.", ".xx.", "...."]);
  // A "C" open to the crop's right edge: its inside reaches the edge, so it is outside, not a hole.
  const c = ["#####", "#....", "#####"];
  const open = overlayRegions(maskOf(c), 5, 3);
  expect(picture(open.tint, 5)).toEqual(["xxxxx", "x....", "xxxxx"]);
  // An object touching the crop's edge is outlined along the edge too.
  const edge = overlayRegions(maskOf(["##", "##"]), 2, 2);
  expect(picture(edge.outline, 2)).toEqual(["xx", "xx"]);
  // Empty mask: nothing.
  const none = overlayRegions(new Uint8Array(6), 3, 2);
  expect(Array.from(none.tint).concat(Array.from(none.outline)).every(v => v === 0)).toBe(true);
});

it("composes the grey crop, then Qt's 70 / 30 tint and the full-colour contour", () => {
  const w = 5, h = 5;
  const image = new Uint8Array(w * h).fill(100);
  const mask = maskOf([".....", ".###.", ".###.", ".###.", "....."]);
  const plain = composeThumbnail(image, mask, w, h, TINT_VALID, false);
  expect(plain.length).toBe(w * h * 4);
  expect(Array.from(plain.slice(0, 4))).toEqual([100, 100, 100, 255]);
  expect(Array.from(plain)).toEqual(Array.from(composeThumbnail(image, null, w, h, TINT_VALID, true))); // no mask: no overlay
  const shown = composeThumbnail(image, mask, w, h, TINT_VALID, true);
  const px = (x: number, y: number) => Array.from(shown.slice((y * w + x) * 4, (y * w + x) * 4 + 4));
  expect(px(0, 0)).toEqual([100, 100, 100, 255]); // outside the mask: untouched
  expect(px(2, 2)).toEqual([70, 146, 70, 255]); // inside, no hole: green 0.7 * 100 + 0.3 * 255 = 146.5, truncated as Qt's uchar cast does
  expect(px(1, 1)).toEqual([0, 255, 0, 255]); // the outline, in the full colour
  // The target colour blends too.
  const target = composeThumbnail(image, mask, w, h, TINT_TARGET, true);
  expect(Array.from(target.slice((2 * w + 2) * 4, (2 * w + 2) * 4 + 3))).toEqual([70, 70 + 36, 70 + 76]);
  // A mask of another size draws no overlay, never a wrong one.
  expect(Array.from(composeThumbnail(image, new Uint8Array(4), w, h, TINT_VALID, true))).toEqual(Array.from(plain));
});

it("bounds the pixel cache, prunes to the listing and never evicts what the listing names", () => {
  const cache = new CellImageCache(3);
  const loading = { kind: "loading" } as const;
  for (const k of ["a", "b", "c"]) cache.set(k, loading);
  expect(cache.keys()).toEqual(["a", "b", "c"]);
  cache.set("d", loading); // over the cap: the oldest goes
  expect(cache.keys()).toEqual(["b", "c", "d"]);
  cache.set("e", loading, new Set(["b", "c", "d", "e"])); // all named: the cap yields to nothing but the newest is kept
  expect(cache.has("e") && cache.size >= 3).toBe(true);
  expect(cache.prune(new Set(["e"]))).toBeGreaterThan(0);
  expect(cache.keys()).toEqual(["e"]);
  cache.clear();
  expect(cache.size).toBe(0);
  expect(CACHE_CELLS).toBeGreaterThanOrEqual(2 * STRIP_CELLS);
});

it("plans pulls for uncached cells only, newest first, the two strips interleaved", () => {
  const strip = [cell(1), cell(2), cell(3)];
  const invalid = [cell(11, { valid: false }), cell(12, { valid: false })];
  const cache = new CellImageCache();
  expect(planFetches({ strip, invalid_strip: invalid }, cache).map(c => c.frame_index)).toEqual(["3", "12", "2", "11", "1"]);
  // Held (loading, ready or gone) cells are not pulled again.
  cache.set("0:3:2:0", { kind: "loading" });
  cache.set("0:3:12:0", { kind: "none", reason: "gone" });
  expect(planFetches({ strip, invalid_strip: invalid }, cache).map(c => c.frame_index)).toEqual(["3", "11", "1"]);
  // A cell without an image has nothing to pull.
  const noImage = cell(4, { image: null, no_image_reason: "no image: this instrument reports cell measurements without images" });
  expect(planFetches({ strip: [noImage], invalid_strip: [] }, new CellImageCache())).toEqual([]);
  // The cut.
  expect(planFetches({ strip, invalid_strip: invalid }, new CellImageCache(), 2).map(c => c.frame_index)).toEqual(["3", "12"]);
  expect(listedKeys({ strip, invalid_strip: invalid })).toEqual(new Set(["0:3:1:0", "0:3:2:0", "0:3:3:0", "0:3:11:0", "0:3:12:0"]));
});
