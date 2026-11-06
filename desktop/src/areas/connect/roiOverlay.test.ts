import { describe, expect, it } from "vitest";
import type { CameraSelection } from "../../bridge";
import { connectedCameraKey, overlaySwitchesOn, ROI_OVERLAY_TITLE, roiOverlayLabel } from "./roiOverlay";

const sel = (over: Partial<CameraSelection> = {}): CameraSelection => ({
  valid: true, configured: true, mode: 2, running: false, interface_index: 0, device_index: 0, label: "MC2066", mindvision_index: -1,
  mindvision_config_path: "", camera_script_path: "", mock_frame_dir: "", mock_interval_ms: 0, mock_loop: false, ...over,
});

describe("ROI Overlay toggle (CONNECT-047)", () => {
  it("uses Qt's button texts", () => {
    expect(roiOverlayLabel(true)).toBe("ROI Overlay: On");
    expect(roiOverlayLabel(false)).toBe("ROI Overlay: Off");
    expect(ROI_OVERLAY_TITLE).toBe("Toggle ROI overlay rectangle");
  });

  it("has no connected camera until one is valid and configured", () => {
    expect(connectedCameraKey(null)).toBe("");
    expect(connectedCameraKey(sel({ configured: false }))).toBe("");
    expect(connectedCameraKey(sel({ valid: false }))).toBe("");
    expect(connectedCameraKey(sel())).not.toBe("");
  });

  it("switches on when a camera gets connected, once per camera, not on the next poll of the same one", () => {
    const first = connectedCameraKey(sel());
    expect(overlaySwitchesOn("", first)).toBe(true);
    expect(overlaySwitchesOn(first, connectedCameraKey(sel({ running: true })))).toBe(false); // a poll after Start
    expect(overlaySwitchesOn(first, connectedCameraKey(sel()))).toBe(false);
    // another camera, another source, or the mock: connected again
    expect(overlaySwitchesOn(first, connectedCameraKey(sel({ device_index: 1, label: "MC2067" })))).toBe(true);
    expect(overlaySwitchesOn(first, connectedCameraKey(sel({ mode: 3, mindvision_index: 0, label: "MV", mindvision_config_path: "C:\\p\\camera.json" })))).toBe(true);
    expect(overlaySwitchesOn(first, connectedCameraKey(sel({ mode: 1, label: "", mock_frame_dir: "/frames" })))).toBe(true);
  });

  it("leaves the operator's choice alone when the camera disconnects", () => {
    expect(overlaySwitchesOn(connectedCameraKey(sel()), "")).toBe(false);
    expect(overlaySwitchesOn("", "")).toBe(false);
  });
});
