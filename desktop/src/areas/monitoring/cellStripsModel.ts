// Live cell strips of the Monitoring sub-tab (epic #866 MON-052 .. MON-055): the pure parts.
//
// The Qt tab shows the last 25 valid and the last 25 invalid detections as 100 px thumbnails, five
// per row (ExperimentMonitoringTab.h THUMBNAIL_SIZE / GRID_COLUMNS / MAX_FRAMES_TO_SHOW), the invalid
// ones with the short reason of each failed gate under the image and a tooltip with the measured
// value and range, and optionally the processed mask drawn over the image (Show Overlay). The
// backend owns the cells (bridge ABI 41 `monitoring_cells`, ABI next (#880, set at merge) `invalid_strip` and `reasons`):
// a crop around each object, cut by the backend from the ROI, and the same crop of the mask.
// Nothing here touches the DOM or the bridge; the hook (useMonitoringCells.ts) and the component
// (CellStrips.tsx) use it.
import { MONITORING_CELLS } from "../../bridgeContract";
import {
  cellKeyString, type MonitoringCell, type MonitoringCellReason, type MonitoringCellReasonCode,
  type MonitoringCellsListing,
} from "../../monitoringCells";

/** Qt's thumbnail edge in pixels. */
export const THUMBNAIL_SIZE = 100;
/** Thumbnails per row. */
export const GRID_COLUMNS = 5;
/** Cells per strip (Qt shows the last 25 of each kind; the backend caps both strips at this). */
export const STRIP_CELLS = MONITORING_CELLS.strip_max_cells;
/** Image and mask pulls in flight at once. */
export const CONCURRENT_FETCHES = 4;
/** Hard bound of the decoded-pixel cache (cells): both strips plus the headroom of one refresh. */
export const CACHE_CELLS = 2 * STRIP_CELLS + 14;

// ---------------------------------------------------------------------------------------------
// Rejection reasons (MON-053). Qt's getInvalidReasons: a short text under the image and a tooltip
// line with the measured value and the range, from the shared classifier's codes.

/** The short text under an invalid cell's image (Qt `InvalidReason::shortText`). */
export const REASON_LABELS: Readonly<Record<MonitoringCellReasonCode, string>> = {
  no_contour: "No contour",
  border: "Border",
  area: "Area",
  ring: "Ring",
  deform: "Deform",
  area_ratio: "Ratio",
  focus: "Focus",
  wall: "Wall",
};

const fixed = (v: number | null, digits: number): string => (v === null ? "n/a" : v.toFixed(digits));
/** Qt prints the integer gates (area, in um^2) as integers. */
const whole = (v: number | null): string => (v === null ? "n/a" : String(Number.isInteger(v) ? v : Number(v.toFixed(3))));

/** The tooltip line of one reason (Qt `InvalidReason::longText`). */
export function reasonTooltip(r: MonitoringCellReason): string {
  switch (r.code) {
    case "no_contour": return "No inner contour found";
    case "border": return "Contour touches ROI border";
    case "area": return `Area: ${fixed(r.value, 0)} μm² (range: ${whole(r.min)}-${whole(r.max)})`;
    case "ring": return `Ring ratio: ${fixed(r.value, 1)} (range: ${fixed(r.min, 1)}-${fixed(r.max, 1)})`;
    case "deform": return `Deformability: ${fixed(r.value, 3)} (range: ${fixed(r.min, 3)}-${fixed(r.max, 3)})`;
    case "area_ratio": return `Area ratio: ${fixed(r.value, 2)} (max: ${fixed(r.max, 2)})`;
    case "focus": return `Laplacian variance: ${fixed(r.value, 1)} (range: ${fixed(r.min, 1)}-${fixed(r.max, 1)})`;
    case "wall": {
      // Qt names the row only; the band is added when one is configured.
      const band = r.min !== null && r.max !== null ? ` (band: ${whole(r.min)}-${whole(r.max)})` : "";
      return `Centroid row ${fixed(r.value, 1)} outside the channel band${band}`;
    }
  }
}

/** The red text under the image: every short reason joined by " | ". Empty when none. */
export const reasonsLine = (reasons: readonly MonitoringCellReason[]): string =>
  reasons.map(r => REASON_LABELS[r.code]).join(" | ");

/** The tooltip of an invalid cell: one line per reason. Empty when none. */
export const reasonsTooltip = (reasons: readonly MonitoringCellReason[]): string =>
  reasons.map(reasonTooltip).join("\n");

// ---------------------------------------------------------------------------------------------
// Thumbnails.

/** The size a crop is drawn at: scaled to fit the thumbnail box keeping its aspect ratio, smaller
 *  crops scaled up as well (Qt: `scaled(100, 100, KeepAspectRatio)`). */
export function fitThumbnail(width: number, height: number, box = THUMBNAIL_SIZE): { width: number; height: number } {
  if (!(width > 0) || !(height > 0)) return { width: box, height: box };
  const scale = Math.min(box / width, box / height);
  return { width: Math.max(1, Math.round(width * scale)), height: Math.max(1, Math.round(height * scale)) };
}

export type Rgb = readonly [number, number, number];
/** Qt's classification colours: blue = target group, green = valid, red = invalid. */
export const TINT_TARGET: Rgb = [0, 120, 255];
export const TINT_VALID: Rgb = [0, 255, 0];
export const TINT_INVALID: Rgb = [255, 0, 0];

export function cellTint(cell: Pick<MonitoringCell, "valid" | "target_group">): Rgb {
  if (cell.target_group) return TINT_TARGET;
  return cell.valid ? TINT_VALID : TINT_INVALID;
}

/** The regions the overlay paints, in the crop's coordinates (`Uint8Array` of 0 / 1). */
export interface OverlayRegions {
  /** Tinted at 30 %: the inner contours' filled regions (the holes the object's outline encloses and
   *  the outline pixels bordering them), or the whole mask when it has none. */
  tint: Uint8Array;
  /** Outlined in the full colour: every contour (outer and inner) of the mask. */
  outline: Uint8Array;
}

/**
 * Qt's overlay, from the mask crop. `createOverlayImage` tints the filled INNER contours (the holes
 * of the mask, the object inside its halo ring) and falls back to the whole mask when there is no
 * hole, then draws every contour (outer and inner) one pixel wide. Without OpenCV: a hole is a
 * background component (4-connected) that does not touch the crop's edge; the filled hole contour
 * covers the hole and the foreground pixels it runs along; a contour pixel is a foreground pixel with
 * a background 4-neighbour (the crop's edge counts as background).
 */
export function overlayRegions(mask: Uint8Array, width: number, height: number): OverlayRegions {
  const n = width * height;
  const fg = new Uint8Array(n);
  for (let i = 0; i < n; i++) fg[i] = mask[i] > 0 ? 1 : 0;
  // Background reachable from the crop's edge is "outside"; the rest is a hole.
  const outside = new Uint8Array(n);
  const stack: number[] = [];
  const seed = (x: number, y: number) => {
    const i = y * width + x;
    if (!fg[i] && !outside[i]) { outside[i] = 1; stack.push(i); }
  };
  for (let x = 0; x < width; x++) { seed(x, 0); seed(x, height - 1); }
  for (let y = 0; y < height; y++) { seed(0, y); seed(width - 1, y); }
  while (stack.length) {
    const i = stack.pop()!;
    const x = i % width, y = (i - x) / width;
    if (x > 0) seed(x - 1, y);
    if (x < width - 1) seed(x + 1, y);
    if (y > 0) seed(x, y - 1);
    if (y < height - 1) seed(x, y + 1);
  }
  const hole = new Uint8Array(n);
  let holes = 0;
  for (let i = 0; i < n; i++) if (!fg[i] && !outside[i]) { hole[i] = 1; holes++; }

  const outline = new Uint8Array(n);
  const inner = new Uint8Array(n);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = y * width + x;
      if (hole[i]) { inner[i] = 1; continue; }
      if (!fg[i]) continue;
      const left = x > 0 ? i - 1 : -1, right = x < width - 1 ? i + 1 : -1;
      const up = y > 0 ? i - width : -1, down = y < height - 1 ? i + width : -1;
      for (const j of [left, right, up, down]) {
        if (j < 0 || !fg[j]) outline[i] = 1;
        if (j >= 0 && hole[j]) inner[i] = 1;
      }
    }
  }
  return { tint: holes > 0 ? inner : fg, outline };
}

/**
 * The RGBA pixels of a thumbnail: the Mono8 crop as grey, and with `overlay` and a mask the tint
 * (70 % image + 30 % colour, truncated as Qt's uchar cast does) over the tint region and the
 * contours in the full colour. A mask of another size than the image draws no overlay.
 */
export function composeThumbnail(
  image: Uint8Array, mask: Uint8Array | null, width: number, height: number, tint: Rgb, overlay: boolean,
): Uint8ClampedArray {
  const n = width * height;
  const out = new Uint8ClampedArray(n * 4);
  for (let i = 0; i < n; i++) {
    const g = image[i];
    out[4 * i] = g; out[4 * i + 1] = g; out[4 * i + 2] = g; out[4 * i + 3] = 255;
  }
  if (!overlay || !mask || mask.length !== n || image.length !== n) return out;
  const { tint: region, outline } = overlayRegions(mask, width, height);
  for (let i = 0; i < n; i++) {
    if (region[i]) {
      for (let c = 0; c < 3; c++) out[4 * i + c] = Math.floor(Math.min(255, out[4 * i + c] * 0.7 + tint[c] * 0.3));
    }
  }
  for (let i = 0; i < n; i++) {
    if (outline[i]) { out[4 * i] = tint[0]; out[4 * i + 1] = tint[1]; out[4 * i + 2] = tint[2]; }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// The decoded-pixel cache: a cell's crop never changes, so it is fetched once and kept while the
// listing names it. Bounded by the strips (at most two strips of STRIP_CELLS) and a hard cap.

export interface CellPixels {
  width: number;
  height: number;
  /** Mono8 crop, width x height. */
  image: Uint8Array;
  /** The same crop of the processed mask (0 / 255), or null when the row has none. */
  mask: Uint8Array | null;
}

export type CellImageState =
  | { kind: "loading" }
  | { kind: "ready"; pixels: CellPixels }
  /** The backend holds no pixels for it now (the capture changed, it was evicted) or the pull failed;
   *  `retryable`: a failure worth one more try (not "the backend no longer holds it"). */
  | { kind: "none"; reason: string; retryable?: boolean };

export class CellImageCache {
  private readonly map = new Map<string, CellImageState>();
  constructor(private readonly capacity = CACHE_CELLS) {}

  get size(): number { return this.map.size; }
  get(key: string): CellImageState | undefined { return this.map.get(key); }
  has(key: string): boolean { return this.map.has(key); }
  keys(): string[] { return [...this.map.keys()]; }
  /** Sets or replaces a cell's state; the oldest entries outside `keep` go when the cap is exceeded. */
  set(key: string, state: CellImageState, keep?: ReadonlySet<string>): void {
    this.map.delete(key); // re-insert: newest last
    this.map.set(key, state);
    if (this.map.size <= this.capacity) return;
    for (const k of this.map.keys()) {
      if (this.map.size <= this.capacity) break;
      if (k !== key && !keep?.has(k)) this.map.delete(k);
    }
  }
  /** Drops every entry the listing no longer names. Returns how many. */
  prune(keep: ReadonlySet<string>): number {
    let dropped = 0;
    for (const k of [...this.map.keys()]) if (!keep.has(k)) { this.map.delete(k); dropped++; }
    return dropped;
  }
  delete(key: string): boolean { return this.map.delete(key); }
  clear(): void { this.map.clear(); }
}

/** The cells a listing names that have an image to pull: both strips, newest first. */
export function listedCells(listing: Pick<MonitoringCellsListing, "strip" | "invalid_strip">): MonitoringCell[] {
  return [...listing.invalid_strip, ...listing.strip];
}

export const listedKeys = (listing: Pick<MonitoringCellsListing, "strip" | "invalid_strip">): Set<string> =>
  new Set(listedCells(listing).map(cellKeyString));

/**
 * What to pull after a listing: the cells with an image that the cache has no entry for (not
 * loaded, not loading, not known to be gone), newest first of each strip, at most `limit` (both
 * strips together never exceed 2 x STRIP_CELLS, so the default is no cut in practice).
 */
export function planFetches(
  listing: Pick<MonitoringCellsListing, "strip" | "invalid_strip">, cache: Pick<CellImageCache, "has">,
  limit = 2 * STRIP_CELLS,
): MonitoringCell[] {
  const plan: MonitoringCell[] = [];
  const newestFirst = (cells: MonitoringCell[]) => [...cells].reverse();
  const streams = [newestFirst(listing.strip), newestFirst(listing.invalid_strip)];
  // Interleave the two strips so neither starves the other while the queue drains.
  for (let i = 0; plan.length < limit && streams.some(s => i < s.length); i++) {
    for (const s of streams) {
      const cell = s[i];
      if (cell && cell.image !== null && !cache.has(cellKeyString(cell))) plan.push(cell);
      if (plan.length >= limit) break;
    }
  }
  return plan;
}

/** The strip's status line: how many of the capped count it shows. */
export const stripCount = (n: number): string => `${n} of last ${STRIP_CELLS}`;
