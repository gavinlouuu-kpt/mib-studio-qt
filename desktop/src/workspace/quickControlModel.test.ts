// O2 Align quick controls (A3): bounds and increments, and the requested → readback machine:
// pending, readback, rejection, timeout, stale replies and readbacks observed mid-write.
import { describe, expect, it } from "vitest";
import {
  checkNumber,
  checkRect,
  controlStatus,
  initialControl,
  rectDraft,
  rectEqual,
  rectText,
  stepControl,
  type Codec,
  type ControlEvent,
  type ControlState,
  type Rect,
  type RectBounds,
  type RectDraft,
} from "./quickControlModel";

const CODEC: Codec<Rect, RectDraft> = { format: rectDraft, equal: rectEqual };
const EMPTY: RectDraft = { x: "", y: "", width: "", height: "" };
const SENSOR: RectBounds = { areaWidth: 2048, areaHeight: 512, minWidth: 64, minHeight: 4, widthIncrement: 16, heightIncrement: 4, xIncrement: 16, yIncrement: 4 };
const RUN: RectBounds = { areaWidth: 816, areaHeight: 624, minWidth: 512, minHeight: 96, widthIncrement: 1, heightIncrement: 1, xIncrement: 8, yIncrement: 4,
  fixedSize: { width: 512, height: 96 } };
const A: Rect = { x: 0, y: 0, width: 512, height: 96 };
const B: Rect = { x: 32, y: 8, width: 512, height: 96 };
const draft = (r: Partial<RectDraft>): RectDraft => ({ x: "0", y: "0", width: "512", height: "96", ...r });

const run = (s: ControlState<Rect, RectDraft>, ...events: ControlEvent<Rect, RectDraft>[]) => events.reduce((acc, e) => stepControl(acc, e, CODEC), s);
const status = (s: ControlState<Rect, RectDraft>, access: "rw" | "ro" | "wo" | "unavailable" = "rw", why = "") => controlStatus(s, access, CODEC, rectText, why);

describe("bounds and increments are checked before a write", () => {
  it("a number must parse and lie in range; the range is named", () => {
    expect(checkNumber("", { min: 0, max: 10, increment: 1 })).toEqual({ ok: false, reason: "Value: not a number" });
    expect(checkNumber("abc", { min: 0, max: 10, increment: 1 }, "X")).toEqual({ ok: false, reason: "X: not a number" });
    expect(checkNumber("11", { min: 0, max: 10, increment: 1, unit: "px" }, "X")).toEqual({ ok: false, reason: "X: out of range · 0–10 px" });
    expect(checkNumber("-1", { min: 0, max: 10, increment: 1 })).toMatchObject({ ok: false });
    expect(checkNumber("5", { min: 6, max: 4, increment: 1 }, "X")).toEqual({ ok: false, reason: "X: no valid value (the range is empty)" });
  });

  it("a value off the increment is snapped from min, and the note says so", () => {
    expect(checkNumber("40", { min: 0, max: 2048, increment: 16, unit: "px" }, "X")).toEqual({ ok: true, value: 48, note: "X snapped to 48 px (step 16)" });
    expect(checkNumber("48", { min: 0, max: 2048, increment: 16 })).toEqual({ ok: true, value: 48, note: "" });
    expect(checkNumber("7", { min: 4, max: 100, increment: 4 })).toEqual({ ok: true, value: 8, note: "Value snapped to 8 (step 4)" });
    // Snapping never leaves the range.
    expect(checkNumber("101", { min: 0, max: 101, increment: 4 })).toMatchObject({ ok: true, value: 100 });
    expect(checkNumber("2.5", { min: 0, max: 10, increment: 0 })).toEqual({ ok: true, value: 2.5, note: "" });
  });

  it("a window: size first (it bounds the offset), each field names its problem", () => {
    expect(checkRect(draft({ x: "1536" }), SENSOR)).toEqual({ ok: true, value: { x: 1536, y: 0, width: 512, height: 96 }, note: "" });
    expect(checkRect(draft({ x: "1552" }), SENSOR)).toEqual({ ok: false, reason: "X: out of range · 0–1536 px" });
    expect(checkRect(draft({ width: "500", x: "40" }), SENSOR)).toEqual({ ok: true, value: { x: 48, y: 0, width: 496, height: 96 },
      note: "W snapped to 496 px (step 16) · X snapped to 48 px (step 16)" });
    expect(checkRect(draft({ width: "32" }), SENSOR)).toEqual({ ok: false, reason: "W: out of range · 64–2048 px" });
    expect(checkRect(draft({ height: "x" }), SENSOR)).toEqual({ ok: false, reason: "H: not a number" });
  });

  it("the PZ7035 Run window keeps its 512 × 96 size and its 8 / 4 steps", () => {
    expect(checkRect(draft({ x: "300", y: "530", width: "9", height: "9" }), RUN)).toEqual({ ok: false, reason: "Y: out of range · 0–528 px" });
    expect(checkRect(draft({ x: "301", y: "526", width: "9" }), RUN)).toEqual({ ok: true, value: { x: 304, y: 528, width: 512, height: 96 },
      note: "X snapped to 304 px (step 8) · Y snapped to 528 px (step 4)" });
  });
});

describe("requested → readback", () => {
  const start = () => initialControl<Rect, RectDraft>(A, null, CODEC, EMPTY);

  it("the draft follows the readback until the operator edits it", () => {
    let s = start();
    expect(s.draft).toEqual(rectDraft(A));
    expect(status(s)).toEqual({ tone: "neutral", label: "Current" });
    s = run(s, { type: "observe", readback: B });
    expect(s.draft).toEqual(rectDraft(B));
    s = run(s, { type: "edit", draft: draft({ x: "16" }) }, { type: "observe", readback: A });
    expect(s.draft.x).toBe("16"); // an edit is never overwritten by a poll
    expect(s.readback).toEqual(A);
    expect(status(s)).toEqual({ tone: "neutral", label: "Edited · not written" });
  });

  it("pending until the write's own reply; a readback seen meanwhile does not settle it", () => {
    let s = run(start(), { type: "submit", value: B, note: "" });
    expect(s.phase).toBe("pending");
    expect(s.token).toBe(1);
    expect(status(s)).toEqual({ tone: "unknown", label: "Pending · waiting for the readback" });
    s = run(s, { type: "observe", readback: B }); // a poll that happens to show the value: still pending
    expect(s.phase).toBe("pending");
    s = run(s, { type: "settled", token: 1, result: { ok: true, readback: B } });
    expect(s.phase).toBe("idle");
    expect(status(s)).toEqual({ tone: "confirmed", label: "Readback matches the request" });
  });

  it("a readback that differs from the request is shown, never hidden", () => {
    const adjusted = { ...B, x: 48 };
    const s = run(start(), { type: "submit", value: B, note: "" }, { type: "settled", token: 1, result: { ok: true, readback: adjusted } });
    expect(s.draft).toEqual(rectDraft(adjusted));
    expect(status(s)).toEqual({ tone: "warn", label: "Readback differs · device reports 512 × 96 @ 48, 8" });
  });

  it("a rejected write shows its reason and restores the readback", () => {
    const s = run(start(), { type: "edit", draft: draft({ x: "32" }) }, { type: "submit", value: B, note: "" },
      { type: "settled", token: 1, result: { ok: false, reason: "camera busy" } });
    expect(s.phase).toBe("rejected");
    expect(s.draft).toEqual(rectDraft(A));
    expect(s.edited).toBe(false);
    expect(status(s)).toEqual({ tone: "blocked", label: "Rejected · camera busy" });
    expect(run(s, { type: "edit", draft: draft({ x: "1" }) }).reason).toBe(""); // the next edit clears it
  });

  it("a timeout restores the readback, and the late reply is ignored", () => {
    let s = run(start(), { type: "submit", value: B, note: "" }, { type: "timeout", token: 1, ms: 5000 });
    expect(s.phase).toBe("timedOut");
    expect(s.draft).toEqual(rectDraft(A));
    expect(status(s)).toEqual({ tone: "warn", label: "Timed out · no reply within 5 s · showing the readback" });
    s = run(s, { type: "settled", token: 1, result: { ok: true, readback: B } });
    expect(s.phase).toBe("timedOut"); // stale
    // The next poll shows what the device really did.
    s = run(s, { type: "observe", readback: B });
    expect(s.readback).toEqual(B);
    expect(s.draft).toEqual(rectDraft(B));
  });

  it("one write at a time: a second submit or an edit while pending is ignored, an old token too", () => {
    let s = run(start(), { type: "submit", value: B, note: "" });
    expect(run(s, { type: "submit", value: A, note: "" })).toBe(s);
    expect(run(s, { type: "edit", draft: EMPTY })).toBe(s);
    expect(run(s, { type: "revert" })).toBe(s);
    s = run(s, { type: "settled", token: 1, result: { ok: true, readback: B } }, { type: "submit", value: A, note: "" });
    expect(run(s, { type: "settled", token: 1, result: { ok: false, reason: "old" } })).toBe(s);
    expect(run(s, { type: "timeout", token: 1, ms: 5000 })).toBe(s);
  });

  it("an invalid draft is not written; revert goes back to the readback", () => {
    let s = run(start(), { type: "edit", draft: draft({ x: "9999" }) }, { type: "invalid", reason: "X: out of range · 0–1536 px" });
    expect(status(s)).toEqual({ tone: "warn", label: "Not written · X: out of range · 0–1536 px" });
    expect(s.token).toBe(0);
    s = run(s, { type: "revert" });
    expect(s.draft).toEqual(rectDraft(A));
    expect(status(s).label).toBe("Current");
  });

  it("write-only and deferred writes say there is no readback (yet)", () => {
    const wo = initialControl<Rect, RectDraft>(null, A, CODEC, EMPTY);
    expect(wo.draft).toEqual(rectDraft(A)); // the initial value fills the draft
    expect(status(wo, "wo")).toEqual({ tone: "unknown", label: "Write-only · no readback" });
    const written = run(wo, { type: "submit", value: B, note: "" }, { type: "settled", token: 1, result: { ok: true, readback: null } });
    expect(status(written, "wo")).toEqual({ tone: "unknown", label: "Written · no readback" });
    expect(written.draft).toEqual(rectDraft(B));
    const deferred = run(wo, { type: "submit", value: B, note: "" },
      { type: "settled", token: 1, result: { ok: true, readback: null, deferred: "applied when Experiment opens" } });
    expect(status(deferred)).toEqual({ tone: "info", label: "Written · applied when Experiment opens" });
    // Once the device reports the requested value, it is confirmed.
    expect(status(run(deferred, { type: "observe", readback: B }))).toEqual({ tone: "confirmed", label: "Readback matches the request" });
  });

  it("read-only and unavailable features never offer a write", () => {
    const s = start();
    expect(status(s, "ro", "set in the camera script")).toEqual({ tone: "neutral", label: "Read-only · set in the camera script" });
    expect(status(initialControl<Rect, RectDraft>(null, null, CODEC, EMPTY), "ro")).toEqual({ tone: "unknown", label: "Unknown · not reported" });
    expect(status(s, "unavailable", "not exposed through the bridge")).toEqual({ tone: "unsupported", label: "Unavailable · not exposed through the bridge" });
  });

  it("keeps the snap note of the write it made", () => {
    const s = run(start(), { type: "submit", value: B, note: "X snapped to 32 px (step 16)" }, { type: "settled", token: 1, result: { ok: true, readback: B } });
    expect(s.reason).toBe("X snapped to 32 px (step 16)");
  });
});
