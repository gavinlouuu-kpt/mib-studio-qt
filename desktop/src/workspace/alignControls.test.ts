// O2 Align (A3): which quick controls each instrument offers, capability-gated (desktop camera
// with geometry, desktop without, PZ7035), and the explicit camera mode switch.
import { describe, expect, it } from "vitest";
import type { CameraGeometry } from "../bridge";
import { alignMode, exposureReadback, readbackRows, roiOverlay, roiRow, RUN_WINDOW_BOUNDS, type AlignFacts } from "./alignControls";

const ARAVIS: CameraGeometry = {
  supported: true, overview: false, camera: "aravis", sensor_width: 2048, sensor_height: 512,
  roi: { x: 768, y: 208, width: 512, height: 96 }, width_increment: 16, height_increment: 4, offset_x_increment: 16, offset_y_increment: 4,
  min_width: 64, min_height: 4,
  session: { frame_rate_hz: 2000, frame_rate_max_hz: 4512.3, frame_rate_limit: "SensorRate", exposure_us: 2.5, delivered_frame_rate_hz: 30.2 },
};
const MINDVISION: CameraGeometry = { ...ARAVIS, camera: "mindvision", width_increment: 0, height_increment: 0, offset_x_increment: 0, offset_y_increment: 0,
  min_width: 1, min_height: 1, session: {} };
const DESKTOP: AlignFacts = {
  pz7035: false, instrumentModes: false, hostProcessing: true, geometry: ARAVIS, instrumentMode: null, runFrozen: false, modeError: "",
  appliedRunWindow: null, placedRunWindow: null, processingRoi: null, runFps: null, experimentActive: false, running: true,
};
const PZ: AlignFacts = {
  ...DESKTOP, pz7035: true, instrumentModes: true, hostProcessing: false, instrumentMode: "run",
  geometry: { ...ARAVIS, sensor_width: 816, sensor_height: 624, session: { frame_rate_hz: 120, exposure_us: 900 } },
  runFps: { available: true, fps: 5000, exposure_mode: "auto", exposure_us: 150, shs: 64, actual_fps: 5000.2, min_fps: 1000, max_fps: 5000, default_fps: 5000, qualified_fps: 5000 },
};
const byId = (f: AlignFacts) => Object.fromEntries(readbackRows(f).map((r) => [r.id, r]));

describe("quick controls by instrument", () => {
  it("desktop camera with geometry: exposure and frame rate are readbacks, gain is unavailable, the camera ROI is writable", () => {
    const rows = byId(DESKTOP);
    expect(rows.exposure).toMatchObject({ access: "ro", value: "2.5 µs" }); // short exposures keep their decimals
    expect(rows.gain).toMatchObject({ access: "unavailable", value: null });
    expect(rows.gain.why).toContain("not exposed through the bridge");
    expect(rows.frameRate).toMatchObject({ access: "ro", value: "2,000 Hz", detail: "max 4,512 Hz · ≥ 30.2 images/s here", chip: "camera script" });
    expect(rows.gain).toMatchObject({ chip: "not bridged" });
    expect(rows.runTiming).toBeUndefined(); // PZ7035 only
    const roi = roiRow(DESKTOP, null);
    expect(roi).toMatchObject({ kind: "camera", access: "rw", readback: ARAVIS.roi, label: "Camera ROI (sensor)" });
    expect(roi.bounds).toEqual({ areaWidth: 2048, areaHeight: 512, minWidth: 64, minHeight: 4, widthIncrement: 16, heightIncrement: 4, xIncrement: 16, yIncrement: 4 });
  });

  it("exposure readbacks keep short values", () => {
    expect([exposureReadback(2), exposureReadback(2.25), exposureReadback(99.5), exposureReadback(950), exposureReadback(1200)])
      .toEqual(["2 µs", "2.25 µs", "99.5 µs", "950 µs", "1.20 ms"]);
  });

  it("a camera that reports no session says so instead of guessing", () => {
    const rows = byId({ ...DESKTOP, geometry: MINDVISION });
    expect(rows.exposure).toMatchObject({ access: "unavailable", value: null });
    expect(rows.exposure.why).toBe("not reported by this camera; set in the camera script or configuration (classic view)");
    expect(rows.frameRate.access).toBe("unavailable");
    // Steps of 0 are treated as 1, the minimum size as at least 1.
    expect(roiRow({ ...DESKTOP, geometry: MINDVISION }, null).bounds).toMatchObject({ minWidth: 1, widthIncrement: 1, xIncrement: 1 });
    // No ROI saved yet: no readback, the default window is the draft.
    const none = roiRow({ ...DESKTOP, geometry: { ...ARAVIS, roi: { x: 0, y: 0, width: 0, height: 0 } } }, null);
    expect(none.readback).toBeNull();
    expect(none.initial).toEqual({ x: 768, y: 208, width: 512, height: 96 });
  });

  it("geometry without a sensor size yet is unavailable, not a guess", () => {
    expect(roiRow({ ...DESKTOP, geometry: { ...ARAVIS, sensor_width: 0, sensor_height: 0 } }, null)).toMatchObject({ access: "unavailable", bounds: null });
  });

  it("desktop without camera geometry (EGrabber, mock): the processing ROI, bounded by the frame", () => {
    const f = { ...DESKTOP, geometry: { ...ARAVIS, supported: false }, processingRoi: { x: 0, y: 0, width: 0, height: 0 } };
    const roi = roiRow(f, { width: 512, height: 96 });
    expect(roi).toMatchObject({ kind: "processing", access: "rw", readback: { x: 0, y: 0, width: 0, height: 0 } });
    expect(roi.bounds).toMatchObject({ areaWidth: 512, areaHeight: 96, minWidth: 0 });
    expect(byId(f).exposure.access).toBe("unavailable");
    expect(roiRow({ ...f, hostProcessing: false }, null)).toMatchObject({ kind: null, access: "unavailable" });
  });

  it("PZ7035: the 512 × 96 Run window is placed here and applied by the switch to Run; the Run timing is read-only", () => {
    const roi = roiRow(PZ, null);
    expect(roi).toMatchObject({ kind: "run", access: "rw", readback: null, bounds: RUN_WINDOW_BOUNDS, why: "no Run entry yet" });
    expect(roi.initial).toEqual({ x: 152, y: 264, width: 512, height: 96 }); // the centre, on the 8 / 4 steps
    expect(roi.deferred).toBe("applied when Experiment opens (the switch to Run)");
    const placed = roiRow({ ...PZ, placedRunWindow: { x: 40, y: 100, width: 512, height: 96 }, appliedRunWindow: { x: 8, y: 4, width: 512, height: 96 } }, null);
    expect(placed).toMatchObject({ readback: { x: 8, y: 4 }, initial: { x: 40, y: 100 }, why: "applied at the last Run entry" });
    const rows = byId(PZ);
    expect(rows.runTiming).toMatchObject({ access: "ro", chip: "camera document", why: "set in Config → Camera document (classic view)" });
    expect(rows.runTiming.value).toBe("5,000 fps");
    expect(rows.runTiming.detail).toBe("exposure auto 150 µs (SHS 64) · the sensor runs 5,000.2 fps");
    expect(rows.exposure).toMatchObject({ access: "ro", value: "900 µs", chip: "Align producer", why: "set by the Align producer" });
    expect(byId({ ...PZ, runFps: { available: false, reason: "no PZ7035 camera", min_fps: 0, max_fps: 0, default_fps: 0, qualified_fps: 0 } }).runTiming)
      .toMatchObject({ access: "unavailable", why: "no PZ7035 camera" });
  });

  it("outlines the ROI only on the area it is placed on", () => {
    const roi = roiRow(DESKTOP, null);
    expect(roiOverlay(roi, ARAVIS.roi, { width: 2048, height: 512 })).toEqual(ARAVIS.roi);
    expect(roiOverlay(roi, ARAVIS.roi, { width: 512, height: 96 })).toBeNull(); // the experiment window itself
    const proc = roiRow({ ...DESKTOP, geometry: null }, { width: 512, height: 96 });
    expect(roiOverlay(proc, { x: 10, y: 10, width: 100, height: 20 }, { width: 512, height: 96 })).toEqual({ x: 10, y: 10, width: 100, height: 20 });
    expect(roiOverlay(proc, { x: 0, y: 0, width: 0, height: 0 }, { width: 512, height: 96 })).toBeNull(); // whole frame
  });
});

describe("the camera mode switch is offered, never made", () => {
  it("desktop: the experiment window offers 'Show the full sensor'; the full sensor needs nothing", () => {
    expect(alignMode(DESKTOP)).toMatchObject({ label: "Experiment window", switchNeeded: true, action: "Show the full sensor", blocked: "" });
    expect(alignMode({ ...DESKTOP, geometry: { ...ARAVIS, overview: true } })).toMatchObject({ label: "Full sensor 2048 × 512", tone: "confirmed", switchNeeded: false });
    expect(alignMode({ ...DESKTOP, experimentActive: true }).blocked).toBe("a run is active");
    expect(alignMode({ ...DESKTOP, modeError: "camera busy" })).toMatchObject({ label: "Full sensor refused · camera busy", tone: "blocked" });
    expect(alignMode({ ...DESKTOP, geometry: { ...ARAVIS, supported: false } })).toMatchObject({ switchNeeded: false, label: "Camera window · as configured" });
    expect(alignMode({ ...DESKTOP, geometry: null })).toMatchObject({ switchNeeded: false, tone: "unknown" });
  });

  it("PZ7035: anything but Align offers 'Switch to Align'; a held Run ring blocks it", () => {
    expect(alignMode(PZ)).toMatchObject({ label: "Run · no live camera image", switchNeeded: true, action: "Switch to Align", blocked: "" });
    expect(alignMode({ ...PZ, instrumentMode: "align" })).toMatchObject({ label: "Align · full sensor", switchNeeded: false });
    expect(alignMode({ ...PZ, instrumentMode: null })).toMatchObject({ label: "Mode · not read yet", tone: "unknown" });
    expect(alignMode({ ...PZ, runFrozen: true }).blocked).toContain("Resume Run");
    expect(alignMode({ ...PZ, modeError: "LED guard" })).toMatchObject({ label: "Align refused · LED guard", tone: "blocked" });
  });
});
