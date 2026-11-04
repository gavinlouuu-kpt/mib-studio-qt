// O2 Align quick controls (P3, track A step A3): requested value → readback, as a pure state
// machine. A value is current only after readback (ui-language.md, "Truth rules"):
//
//   idle ──edit──▶ edited ──write──▶ pending ──reply ok──▶ idle, readback from after the write
//                                         ├──reply refused──▶ rejected (reason; draft = readback)
//                                         └──no reply in time──▶ timedOut (draft = readback)
//
// A readback observed while a write is pending never settles it: only the write's own reply
// does, with the readback read after the write. A reply that arrives after its timeout (or after
// a newer write) carries a stale token and is ignored. Bounds and increments are checked before
// anything is sent: a value off the increment is snapped (and the note says so), a value out of
// range is refused with the range.

/** What the device lets the operator do with a feature. */
export type Access = "rw" | "ro" | "wo" | "unavailable";
export type Phase = "idle" | "invalid" | "pending" | "rejected" | "timedOut";

/** A write gives up waiting after this long and shows the readback again. */
export const WRITE_TIMEOUT_MS = 5000;

export interface NumberBounds { min: number; max: number; increment: number; unit?: string }
export type Checked<V> = { ok: true; value: V; note: string } | { ok: false; reason: string };

const fmt = (v: number) => (Number.isInteger(v) ? String(v) : v.toFixed(3).replace(/0+$/, "").replace(/\.$/, ""));

/** One typed number: must parse, must lie in [min, max]; snapped to the increment from min. */
export function checkNumber(text: string, b: NumberBounds, label = "Value"): Checked<number> {
  const unit = b.unit ? ` ${b.unit}` : "";
  const trimmed = text.trim();
  const v = trimmed === "" ? NaN : Number(trimmed);
  if (!Number.isFinite(v)) return { ok: false, reason: `${label}: not a number` };
  if (b.max < b.min) return { ok: false, reason: `${label}: no valid value (the range is empty)` };
  if (v < b.min || v > b.max) return { ok: false, reason: `${label}: out of range · ${fmt(b.min)}–${fmt(b.max)}${unit}` };
  const inc = b.increment > 0 ? b.increment : 0;
  if (!inc) return { ok: true, value: v, note: "" };
  let snapped = b.min + Math.round((v - b.min) / inc) * inc;
  if (snapped > b.max) snapped -= inc;
  snapped = Number(snapped.toFixed(9));
  return { ok: true, value: snapped, note: snapped === v ? "" : `${label} snapped to ${fmt(snapped)}${unit} (step ${fmt(inc)})` };
}

export interface Rect { x: number; y: number; width: number; height: number }
export type RectDraft = Record<keyof Rect, string>;
export const RECT_KEYS = ["x", "y", "width", "height"] as const;
const RECT_LABEL: Record<keyof Rect, string> = { x: "X", y: "Y", width: "W", height: "H" };

/** What a window may be: its size range and steps, then the offset steps on the sensor. */
export interface RectBounds {
  /** The area the window must stay on (the sensor, or the frame). */
  areaWidth: number;
  areaHeight: number;
  minWidth: number;
  minHeight: number;
  widthIncrement: number;
  heightIncrement: number;
  xIncrement: number;
  yIncrement: number;
  /** A fixed size (the PZ7035 512×96 Run window): only the offset can change. */
  fixedSize?: { width: number; height: number };
}

/** Size first (it bounds the offset), then the offset; every field reports its own problem. */
export function checkRect(draft: RectDraft, b: RectBounds): Checked<Rect> {
  const notes: string[] = [];
  const one = (key: keyof Rect, bounds: NumberBounds): number | string => {
    const r = checkNumber(draft[key], bounds, RECT_LABEL[key]);
    if (!r.ok) return r.reason;
    if (r.note) notes.push(r.note);
    return r.value;
  };
  const floorTo = (v: number, step: number) => (step > 1 ? Math.floor(v / step) * step : v);
  const width = b.fixedSize ? b.fixedSize.width
    : one("width", { min: b.minWidth, max: floorTo(b.areaWidth, b.widthIncrement), increment: b.widthIncrement, unit: "px" });
  const height = b.fixedSize ? b.fixedSize.height
    : one("height", { min: b.minHeight, max: floorTo(b.areaHeight, b.heightIncrement), increment: b.heightIncrement, unit: "px" });
  if (typeof width === "string") return { ok: false, reason: width };
  if (typeof height === "string") return { ok: false, reason: height };
  const x = one("x", { min: 0, max: floorTo(b.areaWidth - width, b.xIncrement), increment: b.xIncrement, unit: "px" });
  const y = one("y", { min: 0, max: floorTo(b.areaHeight - height, b.yIncrement), increment: b.yIncrement, unit: "px" });
  if (typeof x === "string") return { ok: false, reason: x };
  if (typeof y === "string") return { ok: false, reason: y };
  return { ok: true, value: { x, y, width, height }, note: notes.join(" · ") };
}

export const rectDraft = (r: Rect): RectDraft => ({ x: String(r.x), y: String(r.y), width: String(r.width), height: String(r.height) });
export const rectEqual = (a: Rect | null, b: Rect | null) =>
  !!a && !!b && a.x === b.x && a.y === b.y && a.width === b.width && a.height === b.height;
export const rectText = (r: Rect) => `${r.width} × ${r.height} @ ${r.x}, ${r.y}`;

/** What a write reports back. `readback` is read after the write (null: the device has none). */
export type WriteResult<V> =
  | { ok: true; readback: V | null; message?: string; deferred?: string }
  | { ok: false; reason: string };

export interface ControlState<V, D> {
  phase: Phase;
  draft: D;
  /** The draft was typed by the operator (it no longer follows the readback). */
  edited: boolean;
  /** The last value written, after snapping. */
  requested: V | null;
  readback: V | null;
  /** A write was answered; its readback is shown against the request. */
  written: boolean;
  /** Set by the last write: the value takes effect later (e.g. "applied when Experiment opens"). */
  deferred: string;
  /** Why the draft was not written, why it was refused, or the snap note of the last write. */
  reason: string;
  /** The current write; replies with another token are stale. */
  token: number;
}

export type ControlEvent<V, D> =
  | { type: "observe"; readback: V | null }
  | { type: "edit"; draft: D }
  | { type: "invalid"; reason: string }
  | { type: "submit"; value: V; note: string }
  | { type: "settled"; token: number; result: WriteResult<V> }
  | { type: "timeout"; token: number; ms: number }
  | { type: "revert" };

export interface Codec<V, D> { format: (v: V) => D; equal: (a: V | null, b: V | null) => boolean }

export function initialControl<V, D>(readback: V | null, initial: V | null, codec: Codec<V, D>, empty: D): ControlState<V, D> {
  const shown = readback ?? initial;
  return { phase: "idle", draft: shown === null ? empty : codec.format(shown), edited: false, requested: null, readback,
    written: false, deferred: "", reason: "", token: 0 };
}

export function stepControl<V, D>(s: ControlState<V, D>, e: ControlEvent<V, D>, codec: Codec<V, D>): ControlState<V, D> {
  const restored = (readback: V | null) => (readback === null ? s.draft : codec.format(readback));
  switch (e.type) {
    case "observe": {
      const same = e.readback === null ? s.readback === null : codec.equal(e.readback, s.readback);
      if (same) return s;
      // While a write is pending the readback column follows the device, but the draft (the
      // value being written) stays; an edited draft is the operator's and is never overwritten.
      const follow = s.phase !== "pending" && !s.edited && e.readback !== null;
      return { ...s, readback: e.readback, draft: follow ? codec.format(e.readback!) : s.draft };
    }
    case "edit":
      if (s.phase === "pending") return s;
      return { ...s, draft: e.draft, edited: true, phase: "idle", reason: "" };
    case "invalid":
      if (s.phase === "pending") return s;
      return { ...s, phase: "invalid", reason: e.reason };
    case "submit":
      if (s.phase === "pending") return s;
      return { ...s, phase: "pending", requested: e.value, draft: codec.format(e.value), edited: true, written: false, deferred: "",
        reason: e.note, token: s.token + 1 };
    case "settled": {
      if (e.token !== s.token || s.phase !== "pending") return s; // stale: timed out or superseded
      if (!e.result.ok) return { ...s, phase: "rejected", reason: e.result.reason, draft: restored(s.readback), edited: false };
      const readback = e.result.readback;
      return { ...s, phase: "idle", readback, written: true, deferred: e.result.deferred ?? "", edited: false,
        draft: codec.format(readback ?? s.requested!) };
    }
    case "timeout":
      if (e.token !== s.token || s.phase !== "pending") return s;
      // Whether the device applied it is unknown: show the readback, and let a late reply go.
      return { ...s, phase: "timedOut", reason: `No reply within ${Math.round(e.ms / 1000)} s · showing the readback`,
        draft: restored(s.readback), edited: false, token: s.token + 1 };
    case "revert":
      if (s.phase === "pending") return s;
      return { ...s, phase: "idle", reason: "", edited: false, draft: restored(s.readback) };
  }
}

export type ControlTone = "confirmed" | "warn" | "blocked" | "unknown" | "unsupported" | "info" | "neutral";
export interface ControlStatus { tone: ControlTone; label: string }

/** The chip of a control row, in "State · detail" form. */
export function controlStatus<V, D>(s: ControlState<V, D>, access: Access, codec: Codec<V, D>, text: (v: V) => string,
  why = ""): ControlStatus {
  if (access === "unavailable") return { tone: "unsupported", label: `Unavailable${why ? ` · ${why}` : ""}` };
  if (access === "ro") return s.readback === null
    ? { tone: "unknown", label: "Unknown · not reported" }
    : { tone: "neutral", label: `Read-only${why ? ` · ${why}` : ""}` };
  switch (s.phase) {
    case "pending": return { tone: "unknown", label: "Pending · waiting for the readback" };
    case "invalid": return { tone: "warn", label: `Not written · ${s.reason}` };
    case "rejected": return { tone: "blocked", label: `Rejected · ${s.reason}` };
    case "timedOut": return { tone: "warn", label: `Timed out · ${s.reason.replace(/^No reply/, "no reply")}` };
    case "idle": break;
  }
  if (s.edited) return { tone: "neutral", label: "Edited · not written" };
  if (s.written) {
    if (s.readback !== null && codec.equal(s.requested, s.readback)) return { tone: "confirmed", label: "Readback matches the request" };
    if (s.deferred) return { tone: "info", label: `Written · ${s.deferred}` };
    if (s.readback === null) return { tone: "unknown", label: "Written · no readback" };
    return { tone: "warn", label: `Readback differs · device reports ${text(s.readback)}` };
  }
  if (s.readback === null) return { tone: "unknown", label: access === "wo" ? "Write-only · no readback" : "Unknown · not reported" };
  return { tone: "neutral", label: "Current" };
}
