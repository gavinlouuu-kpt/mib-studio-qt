import { describe, expect, it } from "vitest";
import { applyNotice, discardQuestion, exposureText, pendingNotice, qualificationNote, QUALIFIED_NOTE, rateSummary, strobeSummary, type RunFpsStatus } from "./runFps";

const status = (over: Partial<RunFpsStatus> = {}): RunFpsStatus => ({
  available: true, min_fps: 1000, max_fps: 5000, default_fps: 5000, qualified_fps: 5000, fps: 5000, exposure_mode: "auto", exposure_us: 150, hmax: 58, vmax: 256, shs: 64, exposure_lines: 192,
  actual_fps: 5000.673, qualified: true, in_run: true, applied_fps: 5000, pending: false, held: false, ...over,
});

describe("the texts of the Run setting (the numbers come from the backend)", () => {
  it("formats exposures and summaries", () => {
    expect(exposureText(949.87)).toBe("950 µs");
    expect(exposureText(1949.737)).toBe("1.95 ms");
    expect(rateSummary({ fps: 1000, exposure_mode: "auto", exposure_us: 949.87, shs: 64, actual_fps: 1000.135 })).toBe("1,000 fps · exposure auto 950 µs (SHS 64) · the sensor runs 1,000.1 fps");
    expect(rateSummary({ fps: 2000, exposure_mode: "manual", exposure_us: 470.2, shs: 38, actual_fps: 2000.269 })).toBe("2,000 fps · exposure 470 µs (SHS 38) · the sensor runs 2,000.3 fps");
    expect(strobeSummary({ exposure_start_us: 50, exposure_end_us: 200, strobe_start_us: 7, strobe_end_us: 67 })).toBe("exposure 50 to 200 µs after XVS, LED strobe 7 to 67 µs");
  });
  it("keeps the qualification note until the setting is the qualified one", () => {
    expect(QUALIFIED_NOTE).toBe("5,000 fps is the qualified rate; others are not yet validated on the board.");
    expect(qualificationNote(true)).toBe("");
    expect(qualificationNote(false)).toBe(QUALIFIED_NOTE);
  });
  it("says what applying does", () => {
    expect(applyNotice(null, 0)).toBe("");
    expect(applyNotice(status(), 0)).toBe("Applying re-enters Run (the camera restarts for a few seconds).");
    expect(applyNotice(status({ in_run: false }), 0)).toBe("The setting applies when the instrument next enters Run.");
    expect(applyNotice(status({ held: true }), 5000)).toContain("applying discards them");
  });
  it("asks before discarding a stopped Run's frames, only then", () => {
    expect(discardQuestion(status(), 0)).toBe("");
    expect(discardQuestion(status({ held: true }), 5000)).toBe("Applying the camera document discards the 5,000 buffered frames of the stopped Run. Continue?");
    expect(discardQuestion(status({ held: true, in_run: false }), 5000)).toBe(""); // nothing is discarded outside Run
    expect(discardQuestion(status({ available: false, held: true }), 5000)).toBe("");
  });
  it("tells when the camera runs at another setting than the one set", () => {
    expect(pendingNotice(status())).toBe("");
    expect(pendingNotice(status({ fps: 1000, applied_fps: 5000, pending: true }))).toBe("The camera runs at 5,000 fps; 1,000 fps is set and applies after Run is entered again.");
  });
});
