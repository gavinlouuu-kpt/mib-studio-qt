// Wafer Grid on the Overview (CONNECT-054): the pure part. Qt reference: OverviewTab::updateDotGridOverlay
// (the text), SimpleImageCanvas::paintEvent (the marks), OverviewTab showEvent / hideEvent (when to decode).
import { describe, expect, it } from "vitest";
import type { DotGridPose } from "../../bridge";
import {
  applyDotGridPose, displayScale, drawWaferGrid, INITIAL_WAFER_GRID, nextSinceSequence, sameImageSize, waferGridActive,
  waferGridButtonLabel, waferGridGeometry, waferGridText, waferGridUnavailableText, waferGridView, WAFER_GRID_COLORS,
  WAFER_GRID_POLL_MS, type WaferGridView,
} from "./waferGrid";

const head = { available: true, enabled: true, paused: false, designs: 1, sequence: 7, has_pose: true, unchanged: false };
const valid = (over: Partial<DotGridPose> = {}): DotGridPose => ({
  ...head, valid: true, frame_index: 11, image_width: 800, image_height: 600, dots: 40, votes: 4, decode_ms: 3.14159, reason: "",
  centre_x_um: 39999.96, centre_y_um: 52000.05, theta_deg: 5.0106, um_per_px: 0.29298654, mirrored: true, chip: "R3C0",
  design_id: "wafer-sort-rt", design_name: "Wafer_soRT DC sorting chip (30 um channels)", agreement: 1, residual_px: 0.08,
  dots_px: [[165.7, 39.7], [233.9, 45.6]], dots_px_total: 2, ...over,
});
const failed = (over: Partial<DotGridPose> = {}): DotGridPose => ({
  ...head, valid: false, frame_index: 12, image_width: 800, image_height: 600, dots: 3, votes: 0, decode_ms: 1, reason: "too few dots", ...over,
});
const noPose = (over: Partial<DotGridPose> = {}): DotGridPose => ({ ...head, has_pose: false, ...over });

describe("pose text (Qt: OverviewTab::updateDotGridOverlay)", () => {
  it("formats a valid pose in Qt's three lines: position, rotation and scale, design / chip / counts", () => {
    expect(waferGridText(valid())).toBe(
      "Wafer X 40000.0 µm   Y 52000.1 µm\n" +
      "θ 5.01°   0.2930 µm/px   mirrored\n" +
      "Wafer_soRT DC sorting chip (30 um channels)   chip R3C0   40 dots   votes 4   3.1 ms",
    );
  });

  it("says direct for an unmirrored view, falls back to the design id, and leaves out an empty chip", () => {
    const text = waferGridText(valid({ mirrored: false, design_name: "", chip: "" }));
    expect(text.split("\n")[1]).toBe("θ 5.01°   0.2930 µm/px   direct");
    expect(text.split("\n")[2]).toBe("wafer-sort-rt   40 dots   votes 4   3.1 ms");
    expect(waferGridText(valid({ design_name: "", design_id: "", chip: "" })).split("\n")[2]).toBe("40 dots   votes 4   3.1 ms");
  });

  it("shows the failure reason in words, with the dot count, and a wait state before the first pose", () => {
    expect(waferGridText(failed())).toBe("Wafer grid: too few dots (3 dots)");
    expect(waferGridText(failed({ reason: "no consistent code window", dots: 12 }))).toBe("Wafer grid: no consistent code window (12 dots)");
    expect(waferGridText(noPose())).toBe("Wafer grid: waiting for a frame");
  });

  it("words of a refused toggle and the button follow Qt", () => {
    expect(waferGridUnavailableText("bundled dot-grid registry rejected")).toBe("Dot-grid localization is not available: bundled dot-grid registry rejected");
    expect(waferGridButtonLabel(false)).toBe("Wafer Grid: Off");
    expect(waferGridButtonLabel(true)).toBe("Wafer Grid: On");
    expect(WAFER_GRID_POLL_MS).toBe(250);
  });
});

describe("the overlay view", () => {
  it("copies the dots of a valid pose and none of a failed one (a failed pose never shows a stale position)", () => {
    const v = waferGridView(valid());
    expect(v).toMatchObject({ valid: true, imageWidth: 800, imageHeight: 600, dots: [[165.7, 39.7], [233.9, 45.6]] });
    const f = waferGridView(failed({ dots_px: [[1, 2]] }));
    expect(f).toMatchObject({ valid: false, dots: [], text: "Wafer grid: too few dots (3 dots)" });
    expect(waferGridView(noPose())).toMatchObject({ valid: false, dots: [] });
  });

  it("copies the pose only when the sequence moved", () => {
    let state = applyDotGridPose(INITIAL_WAFER_GRID, valid({ sequence: 7 }));
    expect(state).toMatchObject({ enabled: true, sequence: 7, designs: 1 });
    expect(nextSinceSequence(state)).toBe(7);
    // The backend answers a current sequence with the state block only: the copied view is kept as it is.
    const held = state.view;
    state = applyDotGridPose(state, { ...head, sequence: 7, unchanged: true });
    expect(state.view).toBe(held);
    expect(state.sequence).toBe(7);
    // A newer pose replaces it.
    state = applyDotGridPose(state, failed({ sequence: 8 }));
    expect(state.sequence).toBe(8);
    expect(state.view).toMatchObject({ valid: false });
    expect(nextSinceSequence(state)).toBe(8);
  });

  it("resets when the service is switched off (Qt resets the overlay when enabled flips) and asks for a full pose again", () => {
    const on = applyDotGridPose(INITIAL_WAFER_GRID, valid());
    const off = applyDotGridPose(on, { ...head, enabled: false, has_pose: false, sequence: 7 });
    expect(off).toMatchObject({ enabled: false, view: null, sequence: 0 });
    expect(nextSinceSequence(off)).toBe(0);
    // An unchanged reply with no copied view (just switched on) is not trusted: the full pose is used.
    const again = applyDotGridPose(off, valid({ sequence: 7 }));
    expect(again.view).not.toBeNull();
    expect(applyDotGridPose(off, { ...head, sequence: 7, unchanged: true, has_pose: true }).view).toMatchObject({ valid: false });
  });

  it("follows the service's availability and design count", () => {
    expect(applyDotGridPose(INITIAL_WAFER_GRID, { ...head, available: false, enabled: false, has_pose: false, designs: 0 }))
      .toMatchObject({ available: false, enabled: false, designs: 0 });
  });
});

describe("when to decode (Qt: showEvent / hideEvent)", () => {
  const on = { ready: true, supported: true, tab: "overview", windowVisible: true, experimentActive: false };
  it("only while the Overview is shown in a visible window with no experiment", () => {
    expect(waferGridActive(on)).toBe(true);
    for (const tab of ["connect", "experiment", "review", "config"]) expect(waferGridActive({ ...on, tab }), tab).toBe(false);
    expect(waferGridActive({ ...on, windowVisible: false })).toBe(false); // hidden or minimised
    expect(waferGridActive({ ...on, experimentActive: true })).toBe(false); // never next to an experiment
    expect(waferGridActive({ ...on, ready: false })).toBe(false);
    expect(waferGridActive({ ...on, supported: false })).toBe(false); // PZ7035 Align has no host frame store
  });
});

describe("overlay geometry (Qt: SimpleImageCanvas::paintEvent)", () => {
  const view: WaferGridView = { valid: true, text: "t", dots: [[100, 50], [300, 200]], imageWidth: 800, imageHeight: 600 };

  it("at 1:1 uses Qt's marks: radius 6, a 14 px cross at the image centre, the text box 8 px inside the corner", () => {
    const g = waferGridGeometry(view, 800, 600, 1);
    expect(g.dots).toEqual([{ x: 100, y: 50, r: 6 }, { x: 300, y: 200, r: 6 }]);
    expect(g.cross).toEqual([{ x1: 386, y1: 300, x2: 414, y2: 300 }, { x1: 400, y1: 286, x2: 400, y2: 314 }]);
    expect(g.dotLineWidth).toBe(1);
    expect(g.crossLineWidth).toBe(2);
    expect([g.textX, g.textY]).toEqual([8, 8]);
  });

  it("sizes the marks in display pixels when the page scales the canvas down (fit window)", () => {
    const g = waferGridGeometry(view, 800, 600, 0.25); // 200 px wide on screen
    expect(g.dots[0].r).toBe(12); // max(3, 6 x 0.25) = 3 display px = 12 image px
    expect(g.cross[0]).toEqual({ x1: 344, y1: 300, x2: 456, y2: 300 }); // 14 display px = 56 image px
    expect(g.crossLineWidth).toBe(8);
    expect([g.textX, g.textY, g.fontPx]).toEqual([32, 32, 52]);
    // Zoomed in the radius grows with the zoom, as Qt's 6 x scale does.
    expect(waferGridGeometry(view, 800, 600, 2).dots[0].r).toBe(6);
    expect(waferGridGeometry(view, 800, 600, 0).dots[0].r).toBe(6); // an unlaid-out canvas counts as 1:1
  });

  it("draws no marks for a failed pose or for a frame of another size, but always the text", () => {
    const failedView = waferGridView(failed());
    expect(waferGridGeometry(failedView, 800, 600, 1)).toMatchObject({ dots: [], cross: [] });
    expect(waferGridGeometry(view, 1024, 768, 1)).toMatchObject({ dots: [], cross: [] }); // the window changed since the decode
    expect(sameImageSize(view, 800, 600)).toBe(true);
    expect(sameImageSize(view, 800, 601)).toBe(false);
    expect(waferGridGeometry(failedView, 800, 600, 1).textX).toBe(8);
  });

  it("measures the display scale from the laid-out canvas", () => {
    expect(displayScale({ width: 800, clientWidth: 200 })).toBe(0.25);
    expect(displayScale({ width: 800, clientWidth: 0 })).toBe(1);
    expect(displayScale({ width: 0, clientWidth: 100 })).toBe(1);
  });
});

// A canvas context that records what is drawn, in order.
function recordingContext(width: number, height: number) {
  const calls: string[] = [];
  const style: Record<string, unknown> = {};
  const target: Record<string, unknown> = {
    canvas: { width, height },
    measureText: (t: string) => ({ width: t.length * 6 }),
  };
  const ctx = new Proxy(target, {
    get: (t, name: string) => (name in t ? t[name] : (...args: unknown[]) => { calls.push(`${name}(${args.map((a) => (typeof a === "number" ? Math.round(a * 100) / 100 : a)).join(",")})@${style.strokeStyle ?? ""}|${style.fillStyle ?? ""}`); }),
    set: (_t, name: string, value) => { style[name] = value; return true; },
  });
  return { ctx: ctx as unknown as CanvasRenderingContext2D, calls, style };
}

describe("drawing", () => {
  it("draws the dots, then the cross, then the text box last, in Qt's colours", () => {
    const { ctx, calls } = recordingContext(800, 600);
    drawWaferGrid(ctx, { valid: true, text: "line one\nline two", dots: [[100, 50], [300, 200]], imageWidth: 800, imageHeight: 600 }, 1);
    const names = calls.map((c) => c.slice(0, c.indexOf("(")));
    const firstArc = names.indexOf("arc"), firstMove = names.indexOf("moveTo"), box = names.indexOf("fillRect"), text = names.indexOf("fillText");
    expect(names.filter((n) => n === "arc")).toHaveLength(2);
    expect(names.filter((n) => n === "moveTo")).toHaveLength(2);
    expect(firstArc).toBeLessThan(firstMove);
    expect(firstMove).toBeLessThan(box);
    expect(box).toBeLessThan(text);
    expect(names.filter((n) => n === "fillText")).toHaveLength(2);
    expect(calls.find((c) => c.startsWith("arc("))).toContain(WAFER_GRID_COLORS.dot);
    expect(calls.find((c) => c.startsWith("moveTo("))).toContain(WAFER_GRID_COLORS.cross);
    expect(calls.find((c) => c.startsWith("fillText("))).toContain(WAFER_GRID_COLORS.textValid);
  });

  it("shows the failure reason as text on its own, in the failure colour", () => {
    const { ctx, calls } = recordingContext(800, 600);
    drawWaferGrid(ctx, waferGridView(failed()), 1);
    const names = calls.map((c) => c.slice(0, c.indexOf("(")));
    expect(names).not.toContain("arc");
    expect(names).not.toContain("moveTo");
    const text = calls.find((c) => c.startsWith("fillText("))!;
    expect(text).toContain("Wafer grid: too few dots (3 dots)");
    expect(text).toContain(WAFER_GRID_COLORS.textFailed);
  });
});
