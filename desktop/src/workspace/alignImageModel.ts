// O2 Align image tools (P3, track A step A3): the pure maths behind the persistent image area.
// Fit / 1:1 / zoom / pan are one view transform (screen = image × scale + offset); the pixel
// probe, the histogram and the line profile read the frame the preview already delivered
// (Mono8, the only live format: framePacket.ts). Every computation is bounded so inspection
// never competes with capture or polling: the histogram reads at most HISTOGRAM_MAX_SAMPLES
// pixels, the profile at most PROFILE_MAX_POINTS, and FrameInspector runs them at most every
// INSPECT_INTERVAL_MS on the newest frame only, never inside the delivery callback.

export interface Size { width: number; height: number }
export interface Point { x: number; y: number }
/** screen = image × scale + offset (CSS pixels of the viewport). */
export interface ViewTransform { scale: number; offsetX: number; offsetY: number }
export type ViewMode = "fit" | "1:1" | "zoom";

/** The pixels the probe, the histogram and the profile read: a delivered Mono8 frame. */
export interface InspectFrame {
  readonly data: Uint8Array;
  readonly width: number;
  readonly height: number;
  /** Bytes per row (≥ width). */
  readonly stride_bytes: number;
  readonly frame_index: string;
}

export const MIN_SCALE = 1 / 32;
export const MAX_SCALE = 32;
/** One zoom step (a wheel notch or the + / − buttons). */
export const ZOOM_STEP = 1.25;
export const HISTOGRAM_MAX_SAMPLES = 65_536;
export const PROFILE_MAX_POINTS = 1024;
/** How often the histogram and profile follow the live image (4 per second). */
export const INSPECT_INTERVAL_MS = 250;

const clampScale = (s: number) => Math.min(MAX_SCALE, Math.max(MIN_SCALE, s));
const valid = (s: Size) => s.width > 0 && s.height > 0;

/** Keep the image on screen: a larger image always covers the viewport, a smaller one stays inside it. */
export function clampOffset(t: ViewTransform, image: Size, view: Size): ViewTransform {
  const free = (viewLen: number, imageLen: number) => viewLen - imageLen * t.scale;
  const clamp = (v: number, f: number) => Math.min(Math.max(f, 0), Math.max(Math.min(f, 0), v));
  return { scale: t.scale, offsetX: clamp(t.offsetX, free(view.width, image.width)), offsetY: clamp(t.offsetY, free(view.height, image.height)) };
}

/** The whole image in the viewport, centred (small images are enlarged to fit). */
export function fitTransform(image: Size, view: Size): ViewTransform {
  if (!valid(image) || !valid(view)) return { scale: 1, offsetX: 0, offsetY: 0 };
  const scale = clampScale(Math.min(view.width / image.width, view.height / image.height));
  return { scale, offsetX: (view.width - image.width * scale) / 2, offsetY: (view.height - image.height * scale) / 2 };
}

/** One image pixel per screen pixel, centred; pan to see the rest of a larger image. */
export function oneToOneTransform(image: Size, view: Size): ViewTransform {
  return { scale: 1, offsetX: Math.round((view.width - image.width) / 2), offsetY: Math.round((view.height - image.height) / 2) };
}

/** Zoom by `factor` about a screen point: the image pixel under it stays under it. */
export function zoomAt(t: ViewTransform, factor: number, anchor: Point, image: Size, view: Size): ViewTransform {
  const scale = clampScale(t.scale * factor);
  const k = scale / t.scale;
  return clampOffset({ scale, offsetX: anchor.x - (anchor.x - t.offsetX) * k, offsetY: anchor.y - (anchor.y - t.offsetY) * k }, image, view);
}

export function panBy(t: ViewTransform, dx: number, dy: number, image: Size, view: Size): ViewTransform {
  return clampOffset({ scale: t.scale, offsetX: t.offsetX + dx, offsetY: t.offsetY + dy }, image, view);
}

/** The transform for a mode; "zoom" keeps the operator's transform (re-clamped to a new size). */
export function transformFor(mode: ViewMode, current: ViewTransform, image: Size, view: Size): ViewTransform {
  if (mode === "fit") return fitTransform(image, view);
  if (mode === "1:1") return oneToOneTransform(image, view);
  return clampOffset(current, image, view);
}

/** The image pixel under a screen point, or null outside the image. */
export function screenToImage(t: ViewTransform, p: Point, image: Size): Point | null {
  const x = Math.floor((p.x - t.offsetX) / t.scale), y = Math.floor((p.y - t.offsetY) / t.scale);
  return x >= 0 && y >= 0 && x < image.width && y < image.height ? { x, y } : null;
}

export interface Probe { x: number; y: number; value: number }

/** Coordinates and the raw Mono8 value under the cursor (null outside the frame). */
export function probeAt(frame: InspectFrame, x: number, y: number): Probe | null {
  if (!Number.isInteger(x) || !Number.isInteger(y) || x < 0 || y < 0 || x >= frame.width || y >= frame.height) return null;
  const value = frame.data[y * Math.max(frame.stride_bytes, frame.width) + x];
  return value === undefined ? null : { x, y, value };
}

export interface Histogram {
  /** 256 counts, one per Mono8 value. */
  bins: Uint32Array;
  /** Pixels read, and the grid step between them (1 = every pixel). */
  samples: number;
  step: number;
  min: number;
  max: number;
  mean: number;
  /** Fraction of the read pixels at 255 and at 0. */
  saturated: number;
  black: number;
}

/** The histogram of the frame, on a regular grid of at most `maxSamples` pixels. */
export function histogram(frame: InspectFrame, maxSamples = HISTOGRAM_MAX_SAMPLES): Histogram {
  const bins = new Uint32Array(256);
  const { width, height } = frame, stride = Math.max(frame.stride_bytes, width);
  if (width <= 0 || height <= 0) return { bins, samples: 0, step: 1, min: 0, max: 0, mean: 0, saturated: 0, black: 0 };
  const step = Math.max(1, Math.ceil(Math.sqrt((width * height) / Math.max(1, maxSamples))));
  let samples = 0, sum = 0, min = 255, max = 0;
  for (let y = 0; y < height; y += step) {
    const row = y * stride;
    for (let x = 0; x < width; x += step) {
      const v = frame.data[row + x] ?? 0;
      bins[v]++;
      sum += v;
      samples++;
      if (v < min) min = v;
      if (v > max) max = v;
    }
  }
  return { bins, samples, step, min, max, mean: sum / samples, saturated: bins[255] / samples, black: bins[0] / samples };
}

export type ProfileAxis = "row" | "column";
export interface LineProfile {
  axis: ProfileAxis;
  /** The row (axis "row") or column (axis "column") the profile runs along. */
  index: number;
  /** Image coordinate along the line of each value. */
  positions: number[];
  values: number[];
  min: number;
  max: number;
}

/** The raw values along one row or column through the frame, at most `maxPoints` of them. */
export function lineProfile(frame: InspectFrame, axis: ProfileAxis, index: number, maxPoints = PROFILE_MAX_POINTS): LineProfile | null {
  const length = axis === "row" ? frame.width : frame.height;
  const across = axis === "row" ? frame.height : frame.width;
  if (length <= 0 || !Number.isInteger(index) || index < 0 || index >= across) return null;
  const step = Math.max(1, Math.ceil(length / Math.max(1, maxPoints)));
  const stride = Math.max(frame.stride_bytes, frame.width);
  const positions: number[] = [], values: number[] = [];
  let min = 255, max = 0;
  for (let i = 0; i < length; i += step) {
    const v = frame.data[axis === "row" ? index * stride + i : i * stride + index] ?? 0;
    positions.push(i);
    values.push(v);
    if (v < min) min = v;
    if (v > max) max = v;
  }
  return { axis, index, positions, values, min, max };
}

export interface Inspection { frameIndex: string; size: Size; histogram: Histogram; profile: LineProfile | null }

/** Schedules work on a timer; the browser's in production, fake timers in tests. */
export interface InspectTimer { set: (fn: () => void, ms: number) => unknown; clear: (handle: unknown) => void }

/**
 * Holds the newest delivered frame and computes the histogram and profile from it at most every
 * `intervalMs`. `deliver` is O(1): it only keeps a reference, so the frame pull path never waits
 * for inspection. Frames that arrive between two computations are skipped, not queued.
 */
export class FrameInspector {
  private frame: InspectFrame | null = null;
  private handle: unknown = null;
  private lastRun = -Infinity;
  private frozen = false;
  private profileAt: { axis: ProfileAxis; index: number | null } = { axis: "row", index: null };
  /** Computations so far (tests check the bound). */
  computations = 0;

  constructor(
    private readonly publish: (inspection: Inspection) => void,
    private readonly timer: InspectTimer,
    private readonly now: () => number,
    private readonly intervalMs = INSPECT_INTERVAL_MS,
  ) {}

  /** The newest frame from the preview. False while the display is frozen: the frame is not
   *  taken, so nothing derived from it (its size included) may change either. */
  deliver(frame: InspectFrame): boolean {
    if (this.frozen) return false;
    this.frame = frame;
    this.schedule();
    return true;
  }

  latest(): InspectFrame | null { return this.frame; }

  /** Freeze keeps the shown frame for inspection; capture and polling go on. */
  setFrozen(frozen: boolean): void { this.frozen = frozen; }

  /** The row or column for the profile; null = the middle of the frame. Recomputes soon. */
  setProfile(axis: ProfileAxis, index: number | null): void {
    this.profileAt = { axis, index };
    if (this.frame) this.schedule(true);
  }

  dispose(): void {
    if (this.handle !== null) this.timer.clear(this.handle);
    this.handle = null;
    this.frame = null;
  }

  private schedule(soon = false): void {
    if (this.handle !== null) return;
    const wait = soon ? 0 : Math.max(0, this.lastRun + this.intervalMs - this.now());
    this.handle = this.timer.set(() => { this.handle = null; this.run(); }, wait);
  }

  private run(): void {
    const frame = this.frame;
    if (!frame) return;
    this.lastRun = this.now();
    this.computations++;
    const { axis, index } = this.profileAt;
    const across = axis === "row" ? frame.height : frame.width;
    const at = index !== null && index < across ? index : Math.floor(across / 2);
    this.publish({ frameIndex: frame.frame_index, size: { width: frame.width, height: frame.height }, histogram: histogram(frame), profile: lineProfile(frame, axis, at) });
  }
}

/** "min 3 · max 255 · mean 121.4 · saturated 0.2 % · 49,152 px (every 2nd)" */
export function histogramSummary(h: Histogram): string {
  if (h.samples === 0) return "No pixels";
  const pct = (f: number) => `${(f * 100).toFixed(f > 0 && f < 0.001 ? 2 : 1)} %`;
  const grid = h.step === 1 ? "every pixel" : `every ${ordinal(h.step)} pixel`;
  return `min ${h.min} · max ${h.max} · mean ${h.mean.toFixed(1)} · saturated ${pct(h.saturated)} · ${h.samples.toLocaleString("en-US")} px (${grid})`;
}

function ordinal(n: number): string {
  const s = n % 100 >= 11 && n % 100 <= 13 ? "th" : ({ 1: "st", 2: "nd", 3: "rd" } as Record<number, string>)[n % 10] ?? "th";
  return `${n}${s}`;
}

/** An SVG path of the histogram bars in a w × h box (linear counts, tallest bin = full height). */
export function histogramPath(h: Histogram, w: number, hgt: number): string {
  let top = 0;
  for (const c of h.bins) if (c > top) top = c;
  if (top === 0) return "";
  const bw = w / h.bins.length;
  const parts: string[] = [];
  h.bins.forEach((c, i) => {
    if (c === 0) return;
    const bh = (c / top) * hgt;
    parts.push(`M${(i * bw).toFixed(2)} ${hgt.toFixed(2)}h${bw.toFixed(2)}v${(-bh).toFixed(2)}h${(-bw).toFixed(2)}z`);
  });
  return parts.join("");
}

/** SVG polyline points of the profile in a w × h box (value 0 at the bottom, 255 at the top). */
export function profilePoints(p: LineProfile, length: number, w: number, hgt: number): string {
  const span = Math.max(1, length - 1);
  return p.values.map((v, i) => `${((p.positions[i] / span) * w).toFixed(2)},${(hgt - (v / 255) * hgt).toFixed(2)}`).join(" ");
}
