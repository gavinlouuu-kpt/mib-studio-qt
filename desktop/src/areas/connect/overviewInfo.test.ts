import { describe, expect, it } from "vitest";
import type { CameraGeometry } from "../../bridge";
import { cameraModeLead, mindVisionModeLabel, roiSummary, TRIGGER_PREFIX } from "./overviewInfo";

const mv = (over: Partial<CameraGeometry> = {}): CameraGeometry => ({
  supported: true, overview: true, camera: "mindvision", sensor_width: 1280, sensor_height: 1024, roi: { x: 16, y: 8, width: 640, height: 128 },
  width_increment: 1, height_increment: 1, offset_x_increment: 1, offset_y_increment: 1, min_width: 16, min_height: 4, session: {},
  illuminated_live: false, profile_error: "", flip_horizontal: false, flip_vertical: false, capture_fps: 0, ...over,
});

describe("the MindVision mode label (CONNECT-055, Qt OverviewTab::onTick)", () => {
  it("says Full sensor overview with the measured capture rate and the display limit", () => {
    expect(mindVisionModeLabel(mv({ capture_fps: 187 }), 30))
      .toBe("Full sensor overview | Capture: 187 fps | Display: up to 30 fps. Drag the ROI for the experiment.");
    expect(mindVisionModeLabel(mv(), 30)).toContain("Capture: 0 fps"); // stopped
    expect(mindVisionModeLabel(mv({ capture_fps: undefined }), 30)).toContain("Capture: 0 fps"); // a backend without the member
    expect(mindVisionModeLabel(mv({ capture_fps: 99.6 }), 29.97)).toContain("Capture: 100 fps | Display: up to 30 fps");
  });

  it("puts the 400 Hz trigger marker in front for an illuminated-live profile", () => {
    expect(TRIGGER_PREFIX).toBe("Trigger: 400 Hz | ");
    expect(mindVisionModeLabel(mv({ illuminated_live: true, capture_fps: 400 }), 50))
      .toBe("Trigger: 400 Hz | Full sensor overview | Capture: 400 fps | Display: up to 50 fps. Drag the ROI for the experiment.");
  });

  it("shows why the profile cannot be read instead of a mode", () => {
    const reason = "Cannot read MindVision experiment profile: C:\\MIBTest\\camera.json";
    expect(mindVisionModeLabel(mv({ profile_error: reason, illuminated_live: true }), 30)).toBe(reason);
    expect(mindVisionModeLabel(mv({ profile_error: "width must be positive" }), 30)).toBe("width must be positive");
  });

  it("names the experiment ROI when the camera is in its Experiment window", () => {
    expect(mindVisionModeLabel(mv({ overview: false }), 30)).toBe("Experiment ROI: 640x128.");
    expect(mindVisionModeLabel(mv({ overview: false, illuminated_live: true }), 30)).toBe("Trigger: 400 Hz | Experiment ROI: 640x128.");
  });
});

describe("the mode lead of the other cameras", () => {
  const eg = (over: Partial<CameraGeometry> = {}) => mv({ camera: "egrabber", script_role: "overview", ...over });
  it("names the full sensor in the Overview, the Experiment window otherwise", () => {
    expect(cameraModeLead(eg({ sensor_width: 1920, sensor_height: 1080 }))).toBe("Full sensor 1920×1080");
    expect(cameraModeLead(eg({ overview: false, script_role: "experiment" }))).toBe("Experiment window");
    expect(cameraModeLead(mv({ camera: "aravis", sensor_width: 0, sensor_height: 0 }))).toBe("Full sensor ?×?");
  });
  it("does not call an eGrabber camera that ran no script yet the Experiment window", () => {
    expect(cameraModeLead(eg({ overview: false, script_role: "" }))).toBe("No camera script applied yet");
  });
});

describe("the ROI summary (CONNECT-064, Qt MainWindow roiLabel_)", () => {
  const processing = { x: "0", y: "0", w: "512", h: "96" };
  it("follows the Overview rectangle: W × H @ (x, y)", () => {
    const s = roiSummary({ geometry: mv(), window: { x: 368, y: 616, width: 1216, height: 208 }, instrumentModes: false, processing });
    expect(s.text).toBe("ROI: 1216 × 208 @ (368, 616)");
    expect(s.title).toBe("Camera window on the Overview. Processing ROI: 512 × 96 @ (0, 0)");
  });
  it("tells a processing ROI that is not set in the tooltip", () => {
    const s = roiSummary({ geometry: mv(), window: { x: 0, y: 0, width: 512, height: 96 }, instrumentModes: false, processing: { x: "0", y: "0", w: "0", h: "0" } });
    expect(s.title).toContain("Processing ROI: full frame");
  });
  it("falls back to the processing ROI when no camera window is placed, the camera has no window, or on the PZ7035", () => {
    expect(roiSummary({ geometry: mv(), window: null, instrumentModes: false, processing })).toEqual({ text: "ROI: 512 × 96 @ (0, 0)" });
    expect(roiSummary({ geometry: null, window: { x: 1, y: 2, width: 3, height: 4 }, instrumentModes: false, processing })).toEqual({ text: "ROI: 512 × 96 @ (0, 0)" });
    expect(roiSummary({ geometry: mv({ supported: false }), window: { x: 1, y: 2, width: 3, height: 4 }, instrumentModes: false, processing }).text).toBe("ROI: 512 × 96 @ (0, 0)");
    expect(roiSummary({ geometry: mv({ camera: "aravis" }), window: { x: 152, y: 264, width: 512, height: 96 }, instrumentModes: true, processing }).text).toBe("ROI: 512 × 96 @ (0, 0)");
    expect(roiSummary({ geometry: mv(), window: null, instrumentModes: false, processing: { x: "0", y: "0", w: "0", h: "0" } })).toEqual({ text: "ROI: full frame" });
    expect(roiSummary({ geometry: mv(), window: { x: 0, y: 0, width: 0, height: 0 }, instrumentModes: false, processing }).text).toBe("ROI: 512 × 96 @ (0, 0)");
  });
});
