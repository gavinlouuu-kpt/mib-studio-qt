// PZ7035 frame-ring playback (#649 v1): what Studio shows after Stop in Run. The PL keeps the newest N frames with their
// own mask and cell results; the server freezes the ring and serves one frame at a time as a 'MIBR' packet. Pure module:
// unit-testable in plain Node. The packet layout is in PzFrameRing.h (`buildRingPacket`); the cell words are the
// 'MIBC' run-preview layout (runPreview.ts), so the same fields drive the cell table.
import type { RunPreviewCell } from "./runPreview";

/** `fetch_ring_status` (bridge ABI 34). Sequences are the ring's own: frame 0 is the first after the run was armed. */
export interface RingStatus {
  available: boolean;
  /** Why there is no ring or no playback (placement, image, not armed, invalid). */
  reason?: string;
  /** STATE = IDLE after STOP, FINAL = HEAD + 1, neither sticky bit: the buffered frames can be played back. */
  frozen: boolean;
  /** RING_STALLED, STOP_STUCK or FAULT: re-arm needed, no playback. */
  invalid: boolean;
  /** Run stopped by the operator (the backend holds it). */
  run_frozen: boolean;
  capacity_frames: number;
  /** First and last readable sequence (inclusive); count 0 when empty. */
  first_seq: number;
  last_seq: number;
  count: number;
  sensor_fps: number;
}

export interface RingFrame {
  seq: number;
  frameId: number;
  timestampTicks: number;
  tickHz: number;
  flags: number;
  width: number;
  height: number;
  maskPresent: boolean;
  resultsTruncated: boolean;
  gray: Uint8Array;
  /** 1 bit per pixel, LSB first, row-major; all zero when `maskPresent` is false. */
  mask: Uint8Array;
  cells: RunPreviewCell[];
}

const HEADER = 48;
const CELL_WORDS = 18;

export function decodeRingFrame(buf: ArrayBuffer): RingFrame {
  if (buf.byteLength < HEADER) throw new Error("ring frame: short packet");
  const v = new DataView(buf);
  if (String.fromCharCode(v.getUint8(0), v.getUint8(1), v.getUint8(2), v.getUint8(3)) !== "MIBR")
    throw new Error("ring frame: bad magic");
  if (v.getUint16(4, true) !== 1 || v.getUint16(6, true) !== HEADER) throw new Error("ring frame: unsupported version");
  const width = v.getUint16(40, true), height = v.getUint16(42, true), listed = v.getUint16(44, true);
  const bits = v.getUint16(46, true);
  const grayBytes = width * height, maskBytes = grayBytes / 8;
  if (width === 0 || height === 0 || width % 8 !== 0) throw new Error("ring frame: bad geometry");
  const expected = HEADER + grayBytes + maskBytes + listed * CELL_WORDS * 4;
  if (buf.byteLength !== expected) throw new Error(`ring frame: ${buf.byteLength} bytes, expected ${expected}`);
  const cells: RunPreviewCell[] = [];
  for (let c = 0; c < listed; c++) {
    const at = HEADER + grayBytes + maskBytes + c * CELL_WORDS * 4;
    const w = (k: number) => v.getUint32(at + 4 * k, true);
    cells.push({
      x: w(15) & 0xffff, y: w(15) >>> 16, width: w(16) & 0xffff, height: w(16) >>> 16,
      valid: w(17) !== 0,
      payload: Array.from({ length: 15 }, (_, k) => w(k)),
    });
  }
  return {
    seq: Number(v.getBigUint64(8, true)),
    frameId: Number(v.getBigUint64(16, true)),
    timestampTicks: Number(v.getBigUint64(24, true)),
    tickHz: v.getUint32(32, true),
    flags: v.getUint32(36, true),
    width, height,
    maskPresent: (bits & 1) !== 0,
    resultsTruncated: (bits & 2) !== 0,
    gray: new Uint8Array(buf, HEADER, grayBytes),
    mask: new Uint8Array(buf, HEADER + grayBytes, maskBytes),
    cells,
  };
}

// ---- capacity and range ------------------------------------------------------------------------------------------

/** Frames and seconds at the sensor rate, e.g. "5,000 frames · 1.0 s at 5,001 fps". */
export function capacityText(s: Pick<RingStatus, "capacity_frames" | "sensor_fps">): string {
  const frames = s.capacity_frames.toLocaleString("en-US");
  if (!(s.sensor_fps > 0)) return `${frames} frames`;
  return `${frames} frames · ${secondsText(s.capacity_frames / s.sensor_fps)} at ${Math.round(s.sensor_fps).toLocaleString("en-US")} fps`;
}

export function secondsText(seconds: number): string {
  if (!(seconds >= 0) || !Number.isFinite(seconds)) return "—";
  if (seconds < 10) return `${seconds.toFixed(2)} s`;
  if (seconds < 100) return `${seconds.toFixed(1)} s`;
  return `${Math.round(seconds)} s`;
}

export interface PlaybackAvailability {
  ok: boolean;
  /** Shown instead of the controls when !ok. */
  reason: string;
}

/** Playback is offered only for a frozen, valid ring with frames in it. */
export function playbackAvailability(s: RingStatus | null | undefined): PlaybackAvailability {
  if (!s) return { ok: false, reason: "No frame ring status yet." };
  if (!s.available) return { ok: false, reason: s.reason || "This instrument keeps no frame ring." };
  if (s.invalid) return { ok: false, reason: s.reason || "The ring is not valid: resume Run to re-arm it." };
  if (!s.run_frozen) return { ok: false, reason: "Stop Run to review the buffered frames." };
  if (!s.frozen) return { ok: false, reason: s.reason || "The ring is not frozen yet." };
  if (s.count <= 0) return { ok: false, reason: "The ring is empty: nothing was buffered before Stop." };
  return { ok: true, reason: "" };
}

/** "Frames 120 to 5,119 · 5,000 frames · 1.00 s" */
export function rangeText(s: Pick<RingStatus, "first_seq" | "last_seq" | "count" | "sensor_fps">): string {
  if (s.count <= 0) return "No buffered frames";
  const seconds = s.sensor_fps > 0 ? ` · ${secondsText(s.count / s.sensor_fps)}` : "";
  return `Frames ${s.first_seq.toLocaleString("en-US")} to ${s.last_seq.toLocaleString("en-US")} · ${s.count.toLocaleString("en-US")} frames${seconds}`;
}

/** Elapsed time of a frame since the first buffered one, from the packets' tick stamps ("+0.0123 s"). */
export function elapsedText(frame: Pick<RingFrame, "timestampTicks" | "tickHz">, first: Pick<RingFrame, "timestampTicks"> | null): string {
  if (!first || !(frame.tickHz > 0)) return "";
  const seconds = (frame.timestampTicks - first.timestampTicks) / frame.tickHz;
  return `+${seconds.toFixed(4)} s`;
}

// ---- scrub, step, play -------------------------------------------------------------------------------------------

export interface SeqRange {
  first: number;
  last: number;
}

export function clampSeq(seq: number, r: SeqRange): number {
  if (!Number.isFinite(seq)) return r.first;
  return Math.min(r.last, Math.max(r.first, Math.round(seq)));
}

export function stepSeq(seq: number, delta: number, r: SeqRange): number {
  return clampSeq(seq + delta, r);
}

/** Display rates offered for play: frames shown per second (the sensor ran at 5,000). */
export const DISPLAY_FPS = [1, 5, 10, 30, 60] as const;
export const DEFAULT_DISPLAY_FPS = 30;

/**
 * Playback clock: frames to advance after `elapsedMs` at `displayFps`, carrying the fractional remainder so a
 * slow tick does not slow the playback. Returns the new sequence, the remainder, and whether the end was reached.
 */
export function advancePlayback(
  seq: number, carry: number, elapsedMs: number, displayFps: number, r: SeqRange, loop = false,
): { seq: number; carry: number; ended: boolean } {
  const wanted = carry + (Math.max(0, elapsedMs) * displayFps) / 1000;
  const whole = Math.floor(wanted);
  const rest = wanted - whole;
  if (whole <= 0) return { seq, carry: rest, ended: false };
  let next = seq + whole;
  if (next <= r.last) return { seq: next, carry: rest, ended: false };
  if (loop && r.last >= r.first) {
    const span = r.last - r.first + 1;
    next = r.first + ((next - r.first) % span);
    return { seq: next, carry: rest, ended: false };
  }
  return { seq: r.last, carry: 0, ended: true };
}

// ---- overlays ------------------------------------------------------------------------------------------------------

export type OverlayMode = "off" | "mask" | "contours" | "both";
export const OVERLAY_MODES: readonly OverlayMode[] = ["off", "mask", "contours", "both"];
export const OVERLAY_LABEL: Record<OverlayMode, string> = { off: "Off", mask: "Mask", contours: "Contours", both: "Both" };

export function nextOverlay(m: OverlayMode): OverlayMode {
  return OVERLAY_MODES[(OVERLAY_MODES.indexOf(m) + 1) % OVERLAY_MODES.length];
}

/** Pixels of the mask that have an unset 4-neighbour or touch the border: the outline of each region. */
export function contourPixels(mask: Uint8Array, width: number, height: number): Uint8Array {
  const bit = (x: number, y: number) => (mask[(y * width + x) >> 3] >> ((y * width + x) & 7)) & 1;
  const out = new Uint8Array(width * height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      if (!bit(x, y)) continue;
      const edge = x === 0 || y === 0 || x === width - 1 || y === height - 1 ||
        !bit(x - 1, y) || !bit(x + 1, y) || !bit(x, y - 1) || !bit(x, y + 1);
      if (edge) out[y * width + x] = 1;
    }
  }
  return out;
}

/** RGBA of the frame under the chosen overlay (the mask tint of the Run preview; contours in amber). */
export function ringFrameRgba(f: RingFrame, overlay: OverlayMode): Uint8ClampedArray<ArrayBuffer> {
  const n = f.width * f.height;
  const out = new Uint8ClampedArray(new ArrayBuffer(n * 4));
  const showMask = overlay === "mask" || overlay === "both";
  const outline = overlay === "contours" || overlay === "both" ? contourPixels(f.mask, f.width, f.height) : null;
  for (let i = 0; i < n; i++) {
    const g = f.gray[i];
    const masked = showMask && ((f.mask[i >> 3] >> (i & 7)) & 1) === 1;
    if (outline && outline[i]) {
      out[4 * i] = 255; out[4 * i + 1] = 176; out[4 * i + 2] = 0;
    } else if (masked) {
      out[4 * i] = Math.min(255, g * 0.5 + 110); out[4 * i + 1] = g * 0.6; out[4 * i + 2] = Math.min(255, g * 0.5 + 110);
    } else {
      out[4 * i] = g; out[4 * i + 1] = g; out[4 * i + 2] = g;
    }
    out[4 * i + 3] = 255;
  }
  return out;
}

/** Why a frame has no mask, shown on the canvas. */
export function noMaskNote(f: Pick<RingFrame, "maskPresent">): string {
  return f.maskPresent ? "" : "No result for this frame: the U-Net did not deliver a mask.";
}

// ---- Save clip (arrives with the SSD path, #667) ------------------------------------------------------------------

export function saveClipState(ssdPathAvailable: boolean): { enabled: boolean; reason: string } {
  return ssdPathAvailable
    ? { enabled: true, reason: "" }
    : { enabled: false, reason: "Saving needs the SSD (not available yet)." };
}
