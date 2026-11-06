// @vitest-environment jsdom
// #863 A2: what the Overview shows for each camera: the ROI Overlay toggle (CONNECT-047), the MindVision mode label and
// W/H limits (CONNECT-055, 051), and the Overview script area, hidden for MindVision (CONNECT-061).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraGeometry } from "../../bridge";
import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";

vi.mock("../../transport/useControlRole", () => ({ useControlRole: () => ({ viewerOnly: false }) }));
import { OverviewTab } from "./OverviewTab";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const mindvision = (over: Partial<CameraGeometry> = {}): CameraGeometry => ({
  supported: true, overview: true, camera: "mindvision", sensor_width: 1280, sensor_height: 1024, roi: { x: 16, y: 8, width: 640, height: 128 },
  width_increment: 1, height_increment: 1, offset_x_increment: 1, offset_y_increment: 1, min_width: 16, min_height: 4, session: {},
  illuminated_live: false, profile_error: "", flip_horizontal: false, flip_vertical: false, capture_fps: 187, ...over,
});
const egrabber = (over: Partial<CameraGeometry> = {}): CameraGeometry => ({
  supported: true, overview: true, camera: "egrabber", sensor_width: 1920, sensor_height: 1080, roi: { x: 368, y: 616, width: 1216, height: 208 },
  width_increment: 16, height_increment: 4, offset_x_increment: 16, offset_y_increment: 4, min_width: 64, min_height: 16, session: {},
  window_source: "profile", script_role: "overview", experiment_script_path: "", window_apply: null, ...over,
});

const toggle = vi.fn();
const studioFor = (over: Record<string, unknown>) => ({
  instrumentModes: false, cameraWin: { placed: true, edit: vi.fn(), flush: vi.fn() }, modeError: null, setTab: vi.fn(), setModeRetry: vi.fn(),
  runFrozen: false, ring: null, resumeFromBanner: vi.fn(), experimentRequestBusy: false, viewerOnly: false, setFitWindow: vi.fn(), fitWindow: true,
  cameraGeometry: null, cameraWindow: null, expActive: false, saveCameraWindow: vi.fn(), ready: true, capacity: null, hostProcessing: false,
  roiFields: { x: "0", y: "0", w: "0", h: "0" }, setRoiFields: vi.fn(), onApplyRoi: vi.fn(), lastMeta: null, liveCanvasRef: { current: null },
  onWindowPointerDown: vi.fn(), onWindowPointerMove: vi.fn(), onWindowPointerUp: vi.fn(),
  quality: { pass: 0, warn: 0, fail: 0, unknown: 0, gates: [] }, qualityInput: { image: null }, alignBestRef: { current: { best: null, box: null } }, setAlignImage: vi.fn(),
  caps: { egrabber_script: true }, overviewScript: { visible: false }, roiOverlay: false, toggleRoiOverlay: toggle, previewFpsLimit: 30, stageNotice: null, geometryError: "",
  ...over,
}) as unknown as Studio;
const show = async (over: Record<string, unknown>) => {
  await act(async () => root.render(<StudioContext.Provider value={studioFor(over)}><OverviewTab /></StudioContext.Provider>));
};
const button = (text: string) => Array.from(host.querySelectorAll("button")).find((b) => b.textContent === text) as HTMLButtonElement | undefined;
const input = (label: string) => host.querySelector(`input[aria-label="${label}"]`) as HTMLInputElement;

describe("the ROI Overlay button (CONNECT-047)", () => {
  beforeEach(() => toggle.mockClear());

  it("reads 'ROI Overlay: Off' by default and 'ROI Overlay: On' when switched on, and toggles on a click", async () => {
    await show({ cameraGeometry: mindvision(), cameraWindow: { x: 16, y: 8, width: 640, height: 128 }, roiOverlay: false });
    expect(button("ROI Overlay: Off")?.getAttribute("aria-pressed")).toBe("false");
    expect(button("ROI Overlay: Off")?.title).toBe("Toggle ROI overlay rectangle");
    await act(async () => button("ROI Overlay: Off")!.click());
    expect(toggle).toHaveBeenCalledTimes(1);
    await show({ cameraGeometry: mindvision(), cameraWindow: { x: 16, y: 8, width: 640, height: 128 }, roiOverlay: true });
    expect(button("ROI Overlay: On")?.getAttribute("aria-pressed")).toBe("true");
    expect(button("ROI Overlay: Off")).toBeUndefined();
  });

  it("tells the operator why the box is not there while it is off", async () => {
    await show({ cameraGeometry: mindvision(), cameraWindow: { x: 16, y: 8, width: 640, height: 128 }, roiOverlay: false });
    expect(host.querySelector('[data-testid="roi-overlay-hint"]')?.textContent).toContain("switch ROI Overlay on");
    await show({ cameraGeometry: mindvision(), cameraWindow: { x: 16, y: 8, width: 640, height: 128 }, roiOverlay: true });
    expect(host.querySelector('[data-testid="roi-overlay-hint"]')).toBeNull();
  });

  it("has no toggle where there is no box to show (no window geometry) or on the PZ7035", async () => {
    await show({ cameraGeometry: null });
    expect(host.textContent).not.toContain("ROI Overlay");
    await show({ cameraGeometry: { ...mindvision(), supported: false } });
    expect(host.textContent).not.toContain("ROI Overlay");
    await show({ cameraGeometry: { ...mindvision(), camera: "aravis" }, cameraWindow: { x: 0, y: 0, width: 512, height: 96 }, instrumentModes: true });
    expect(host.textContent).not.toContain("ROI Overlay");
    expect(host.querySelector('[data-testid="roi-overlay-hint"]')).toBeNull();
  });
});

describe("a MindVision camera on the Overview", () => {
  const window = { x: 16, y: 8, width: 640, height: 128 };

  it("shows Qt's mode label with the measured capture rate and the display limit (CONNECT-055)", async () => {
    await show({ cameraGeometry: mindvision(), cameraWindow: window, roiOverlay: true, previewFpsLimit: 30 });
    expect(host.querySelector('[data-testid="mindvision-mode-label"]')?.textContent)
      .toBe("Full sensor overview | Capture: 187 fps | Display: up to 30 fps. Drag the ROI for the experiment.");
    expect(host.querySelector(".camera-alignment-status")?.textContent).toContain("Sensor 1280×1024.");
    await show({ cameraGeometry: mindvision({ illuminated_live: true, capture_fps: 400 }), cameraWindow: window, roiOverlay: true });
    expect(host.querySelector('[data-testid="mindvision-mode-label"]')?.textContent).toMatch(/^Trigger: 400 Hz \| Full sensor overview \| Capture: 400 fps/);
  });

  it("shows the reason when the profile cannot be read", async () => {
    await show({ cameraGeometry: mindvision({ profile_error: "Cannot read MindVision experiment profile: C:\\p\\camera.json" }), cameraWindow: null, roiOverlay: true });
    expect(host.querySelector('[data-testid="mindvision-mode-label"]')?.textContent).toBe("Cannot read MindVision experiment profile: C:\\p\\camera.json");
  });

  it("limits W and H to the sensor in steps of 1 (CONNECT-051)", async () => {
    await show({ cameraGeometry: mindvision(), cameraWindow: window, roiOverlay: true });
    expect(input("Camera window width")).toMatchObject({ min: "16", max: "1280", step: "1" });
    expect(input("Camera window height")).toMatchObject({ min: "4", max: "1024", step: "1" });
    expect(input("Camera window x")).toMatchObject({ min: "0", max: "640", step: "1" });
    expect(input("Camera window y")).toMatchObject({ min: "0", max: "896", step: "1" });
  });

  it("has no script area and no pointer to a JSON editor that is not there (CONNECT-061)", async () => {
    await show({ cameraGeometry: mindvision(), cameraWindow: window, roiOverlay: true, overviewScript: { visible: false } });
    expect(host.querySelector('section[aria-label="Overview camera script"]')).toBeNull();
    expect(host.textContent).not.toContain("JSON editor below");
    expect(host.textContent).not.toContain("Apply to Camera");
  });
});

describe("an eGrabber camera on the Overview", () => {
  it("shows the Overview script area under the canvas, in place of the Experiment-role script controls", async () => {
    const model = {
      visible: true, path: "/p/overviewConfig.js", shownPath: "/p/overviewConfig.js", defaultPath: "/p/overviewConfig.js", external: "", text: "// script", dirty: false, busy: false,
      message: null, loaded: true, applyBlock: "", edit: vi.fn(), reset: vi.fn(), save: vi.fn(), apply: vi.fn(), browse: vi.fn(), clear: vi.fn(),
    };
    await show({ cameraGeometry: egrabber(), cameraWindow: { x: 368, y: 616, width: 1216, height: 208 }, roiOverlay: true, overviewScript: model });
    const area = host.querySelector('section[aria-label="Overview camera script"]')!;
    expect(area.textContent).toContain("overviewConfig.js");
    expect(["Reset", "Save", "Apply to Camera", "Browse...", "Clear"].map((t) => !!button(t))).toEqual([true, true, true, true, true]);
    expect(host.querySelector('section[aria-label="Camera script controls"]')).toBeNull();
    expect(host.textContent).not.toContain("Browse Script…");
    await act(async () => button("Apply to Camera")!.click());
    expect(model.apply).toHaveBeenCalledTimes(1);
    // the mode line still names the window and its source
    expect(host.querySelector(".camera-alignment-status")?.textContent).toContain("Full sensor 1920×1080");
    expect(host.querySelector(".camera-alignment-status")?.textContent).toContain("the applied profile's camera.window");
  });
});
