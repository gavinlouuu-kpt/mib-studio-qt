// Pure helpers for the virtualised thumbnail grid (plan
// 2026-10-01-standalone-review-app, PR 2). No DOM, no bridge: unit-tested in
// thumbnails.test.ts.
//
// The backend packs a page of `count` tiles of `size`×`size` px into one
// frame of width `size` and height `size·count` (pull kind 5; Mono8 or RGB8).

/** Tile edge in px (Qt THUMBNAIL_SIZE). */
export const THUMB_SIZE = 128;
/** Tiles fetched per backend pull (one IPC round trip per page). A page is
 * one frame packet of THUMB_SIZE × (THUMB_SIZE·count) px, so it must stay
 * within the contract's max_dimension (8192): 64 × 128 = 8192. */
export const THUMB_PAGE = 64;

export interface GridGeometry {
  columns: number;
  cell: number; // px, cell height (row pitch) including the gap
  cellWidth: number; // px, cell width (column pitch) including the gap
  rows: number;
  height: number; // total scroll height
}

/** Grid of `cellWidth`×`cell` cells (`cellWidth` defaults to square). */
export function gridGeometry(total: number, width: number, cell: number, cellWidth = cell): GridGeometry {
  const columns = Math.max(1, Math.floor(Math.max(0, width) / cellWidth));
  const rows = Math.ceil(Math.max(0, total) / columns);
  return { columns, cell, cellWidth, rows, height: rows * cell };
}

/**
 * The band of a letterboxed size×size tile that holds the image: the backend
 * scales a w×h frame to fit and centres it, so a 512×96 frame occupies a
 * 128×24 band. Returns the band's offset and extent in tile pixels.
 */
export function imageBand(size: number, width: number, height: number): { x: number; y: number; w: number; h: number } {
  if (width <= 0 || height <= 0) return { x: 0, y: 0, w: size, h: size };
  const scale = Math.min(size / width, size / height);
  const w = Math.max(1, Math.floor(width * scale));
  const h = Math.max(1, Math.floor(height * scale));
  return { x: Math.floor((size - w) / 2), y: Math.floor((size - h) / 2), w, h };
}

/** Item range [start, end) intersecting the viewport, padded by `overscanRows`. */
export function visibleRange(
  scrollTop: number,
  viewport: number,
  geometry: GridGeometry,
  total: number,
  overscanRows = 2,
): { start: number; end: number } {
  if (total <= 0) return { start: 0, end: 0 };
  const firstRow = Math.max(0, Math.floor(scrollTop / geometry.cell) - overscanRows);
  const lastRow = Math.min(geometry.rows - 1, Math.floor((scrollTop + Math.max(0, viewport)) / geometry.cell) + overscanRows);
  const start = Math.min(total, firstRow * geometry.columns);
  const end = Math.min(total, (lastRow + 1) * geometry.columns);
  return { start, end: Math.max(start, end) };
}

/** Pages (offsets, multiples of `page`) covering [start, end). */
export function pagesFor(start: number, end: number, page = THUMB_PAGE): number[] {
  if (end <= start) return [];
  const out: number[] = [];
  for (let p = Math.floor(start / page) * page; p < end; p += page) out.push(p);
  return out;
}

/** Scroll position that brings item `index` into view (null = already visible). */
export function scrollToReveal(index: number, scrollTop: number, viewport: number, geometry: GridGeometry): number | null {
  const row = Math.floor(index / geometry.columns);
  const top = row * geometry.cell;
  if (top < scrollTop) return top;
  if (top + geometry.cell > scrollTop + viewport) return Math.max(0, top + geometry.cell - viewport);
  return null;
}

/** One tile of a packed strip → RGBA pixels (size×size×4). */
export function tileRgba(data: Uint8Array, size: number, channels: 1 | 3, tile: number): Uint8ClampedArray<ArrayBuffer> {
  const px = size * size;
  const out = new Uint8ClampedArray(new ArrayBuffer(px * 4));
  const base = tile * px * channels;
  if (base + px * channels > data.length) return out;
  for (let i = 0; i < px; i++) {
    const d = i * 4;
    if (channels === 3) {
      const s = base + i * 3;
      out[d] = data[s];
      out[d + 1] = data[s + 1];
      out[d + 2] = data[s + 2];
    } else {
      const g = data[base + i];
      out[d] = g;
      out[d + 1] = g;
      out[d + 2] = g;
    }
    out[d + 3] = 255;
  }
  return out;
}

/** Bounded LRU of decoded pages (the Qt QCache rule: keep memory bounded). */
export class PageCache<T> {
  private map = new Map<number, T>();
  constructor(private readonly capacity: number) {}
  get(key: number): T | undefined {
    const v = this.map.get(key);
    if (v !== undefined) {
      this.map.delete(key);
      this.map.set(key, v);
    }
    return v;
  }
  set(key: number, value: T): void {
    this.map.delete(key);
    this.map.set(key, value);
    while (this.map.size > this.capacity) this.map.delete(this.map.keys().next().value!);
  }
  has(key: number): boolean {
    return this.map.has(key);
  }
  clear(): void {
    this.map.clear();
  }
  get size(): number {
    return this.map.size;
  }
}
