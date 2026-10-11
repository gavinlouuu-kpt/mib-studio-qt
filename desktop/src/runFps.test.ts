import { describe, expect, it } from "vitest";
import { applyNotice, discardQuestion, draftTiming, exposureText, parseRunFps, pendingNotice, rateSummary, type RunFpsStatus } from "./runFps";

const status = (over: Partial<RunFpsStatus> = {}): RunFpsStatus => ({
  available: true, min_fps: 100, max_fps: 5000, default_fps: 5000, qualified_fps: 5000, fps: 5000, exposure_us: 150, hmax: 58, vmax: 256, shs: 64, exposure_lines: 192,
  actual_fps: 5000.673, qualified: true, in_run: true, applied_fps: 5000, pending: false, held: false, ...over,
});

describe("the Run rate arithmetic mirrors pz::runTiming (PzRunTiming.h)", () => {
  // The same vectors as tests/backend/run_fps_test.cpp: HMAX 58, SHS 64, VMAX = floor(74.25 MHz / (58 x fps)), exposure = (VMAX - 64) lines.
  it.each([
    [5000, 256, 192, 150.0, 5000.673],
    [2000, 640, 576, 449.939, 2000.269],
    [1000, 1280, 1216, 949.872, 1000.135],
    [500, 2560, 2496, 1949.737, 500.067],
    [100, 12801, 12737, 9949.441, 100.006],
  ])("%i fps", (fps, vmax, lines, exposureUs, actual) => {
    const t = draftTiming(fps);
    expect([t.vmax, t.lines]).toEqual([vmax, lines]);
    expect(t.exposureUs).toBeCloseTo(exposureUs, 3);
    expect(t.actualFps).toBeCloseTo(actual, 3);
  });
  it("keeps the strobe on the exposure start: the shutter always opens 64 lines after the frame start", () => {
    for (const fps of [100, 250, 1000, 2500, 4999, 5000]) {
      const t = draftTiming(fps);
      expect(t.vmax - t.lines).toBe(64); // SHS
      expect(t.exposureUs).toBeLessThan(1e6 / fps); // the exposure fits in the frame
    }
  });
});

describe("the operator's input", () => {
  it("accepts a whole number within the limits, with separators", () => {
    expect(parseRunFps("1000")).toEqual({ ok: true, fps: 1000 });
    expect(parseRunFps(" 1,000 ")).toEqual({ ok: true, fps: 1000 });
    expect(parseRunFps("5000")).toEqual({ ok: true, fps: 5000 });
    expect(parseRunFps("100")).toEqual({ ok: true, fps: 100 });
  });
  it("refuses everything else with the reason", () => {
    for (const bad of ["", "abc", "99.5", "1e3", "-100", "0"]) expect(parseRunFps(bad).ok, bad).toBe(false);
    expect(parseRunFps("99")).toEqual({ ok: false, reason: "the Run rate must be from 100 to 5000 fps" });
    expect(parseRunFps("5001")).toEqual({ ok: false, reason: "the Run rate must be from 100 to 5000 fps" });
  });
});

describe("texts", () => {
  it("shows microseconds below a millisecond and milliseconds above", () => {
    expect(exposureText(150)).toBe("150 µs");
    expect(exposureText(949.872)).toBe("950 µs");
    expect(exposureText(1949.737)).toBe("1.95 ms");
  });
  it("summarises the rate with the exposure and the real sensor rate", () => {
    expect(rateSummary({ fps: 1000, exposure_us: 949.872, actual_fps: 1000.135 })).toBe("1,000 fps · exposure 950 µs · the sensor runs 1,000.1 fps");
  });
  it("tells whether applying restarts the camera and what a held ring loses", () => {
    expect(applyNotice(status({ in_run: true }), 0)).toContain("re-enters Run");
    expect(applyNotice(status({ in_run: false }), 0)).toContain("next enters Run");
    expect(applyNotice(status({ held: true }), 5000)).toContain("discards them");
    expect(discardQuestion(status({ held: true }), 5000)).toContain("discards the 5,000 buffered frames");
    expect(discardQuestion(status(), 5000)).toBe("");
    expect(applyNotice(null, 0)).toBe("");
  });
  it("says so when the camera runs at another rate than the one set", () => {
    expect(pendingNotice(status())).toBe("");
    expect(pendingNotice(status({ fps: 1000, applied_fps: 5000, pending: true }))).toBe("The camera runs at 5,000 fps; 1,000 fps is set and applies after Run is entered again.");
  });
});
