import { describe, expect, it } from "vitest";
import { applyNotice, discardQuestion, draftTiming, exposureText, parseRunFps, pendingNotice, rateSummary, strobeNotice, type RunFpsStatus } from "./runFps";

const status = (over: Partial<RunFpsStatus> = {}): RunFpsStatus => ({
  available: true, min_fps: 100, max_fps: 5000, default_fps: 5000, qualified_fps: 5000, fps: 5000, exposure_us: 150, hmax: 58, vmax: 256, shs: 64, exposure_lines: 192,
  actual_fps: 5000.673, qualified: true, in_run: true, applied_fps: 5000, pending: false, held: false, ...over,
});

describe("the Run rate arithmetic mirrors pz::runTiming (PzRunTiming.h)", () => {
  // The same vectors as tests/backend/instrument_modes_test.cpp: HMAX 58, exposure 150 µs = 192 lines at every rate, VMAX = floor(74.25 MHz / (58 x fps)), SHS = VMAX - 192.
  it.each([
    [5000, 256, 64, 5000.673],
    [2000, 640, 448, 2000.269],
    [1000, 1280, 1088, 1000.135],
    [500, 2560, 2368, 500.067],
    [100, 12801, 12609, 100.006],
  ])("%i fps", (fps, vmax, shs, actual) => {
    const t = draftTiming(fps);
    expect([t.vmax, t.shs, t.lines, t.exposureUs]).toEqual([vmax, shs, 192, 150]);
    expect(t.actualFps).toBeCloseTo(actual, 3);
  });
  it("the strobe (XVS + 7 to 67 µs) overlaps the exposure (the last 150 µs of the frame) only above ~4.6 kfps", () => {
    const five = draftTiming(5000);
    expect([five.exposureStartUs, five.exposureEndUs]).toEqual([expect.closeTo(50, 0), expect.closeTo(200, 0)]);
    expect(five.strobeOverlaps).toBe(true);
    expect(strobeNotice(five)).toBe("");
    expect(draftTiming(4700).strobeOverlaps).toBe(true);
    expect(draftTiming(4500).strobeOverlaps).toBe(false);
    const one = draftTiming(1000);
    expect(one.strobeOverlaps).toBe(false);
    expect(strobeNotice(one)).toMatch(/^Strobe outside the exposure at this rate: frames will be unlit \(exposure 850 to 1000 µs after XVS, strobe 7 to 67 µs\)/);
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
    expect(rateSummary({ fps: 1000, exposure_us: 150, actual_fps: 1000.135 })).toBe("1,000 fps · exposure 150 µs · the sensor runs 1,000.1 fps");
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
