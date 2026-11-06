// @vitest-environment jsdom
// The Connect stage's view (epic #863, B1): the lists, the buttons and their tooltips, the delivery mode control, the
// status line and the dialogs, with a studio context built by hand.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { CameraDiscovery } from "../../bridge";
import { DISCOVERY_JOB_STATES } from "../../bridgeContract";
import { StudioContext } from "../../studio/StudioContext";
import { CameraDialogs } from "./CameraDialogs";
import { ConnectTab } from "./ConnectTab";
import { cameraKey, framegrabberKey, NO_PICK } from "./connectFlow";

vi.mock("../../bridge", () => ({ bridge: {} }));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const cam = (label: string, p: object = {}) => ({ camera_type: 0, camera_index: -1, interface_index: 0, device_index: 0, interface_id: "", device_id: "", model_name: "", firmware_version: "", label, ...p });
const fg = { interface_index: 0, device_index: 0, stream_index: 0, interface_id: "", device_id: "", stream_id: "", model_name: "", label: "Coaxlink 0" };
const discovery = (p: Partial<CameraDiscovery> = {}): CameraDiscovery => ({ valid: true, complete: true, job_id: "1", cameras: [], framegrabbers: [], state: DISCOVERY_JOB_STATES.Completed, errors: [], ...p });
const preflight = { passed: 0, warning: 0, failed: 1, notRequired: 0, criticalPassed: false, checks: [{ id: "camera", label: "Camera", requirement: "required", status: "failed", detail: "No camera selected", expected: "", detected: "—", recovery: [{ kind: "refresh", label: "Refresh" }] }] };

function studio(over: Record<string, unknown> = {}) {
  const calls = { refresh: vi.fn(async () => {}), preflight: vi.fn(async () => {}), connect: vi.fn(), mock: vi.fn(), delivery: vi.fn(), autoSelect: vi.fn(), pick: vi.fn(), tab: vi.fn() };
  const value = {
    connectTab: "cameras", setConnectTab: calls.tab, pz7035: false, caps: { egrabber_script: true, autofocus: true }, discovery: discovery(),
    pickedDevices: NO_PICK, setPickedDevices: calls.pick, refreshCameraState: calls.refresh, ready: true, onConnectPicked: calls.connect,
    cameraScript: { busy: false }, running: false, expActive: false, setShowMockConfig: calls.mock, camSelection: { configured: false }, preflight,
    refreshPreflight: calls.preflight, scanning: false, scanNote: "", actionNote: "", deliveryModeStatus: { valid: true, mode: "everyFrame", active_mode: "everyFrame", confirmed: false, capturing: false },
    onDeliveryModeChange: calls.delivery, autoSelect: true, onAutoSelectChange: calls.autoSelect, ...over,
  };
  return { value, calls };
}
const render = async (over: Record<string, unknown> = {}, ui = <ConnectTab />) => {
  const s = studio(over);
  await act(async () => root.render(<StudioContext.Provider value={s.value as never}>{ui}</StudioContext.Provider>));
  return s;
};
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text) as HTMLButtonElement;

describe("buttons and their reasons", () => {
  it("Connect names why it is blocked while the camera runs, during an experiment, and with nothing selected", async () => {
    const d = discovery({ cameras: [cam("Cam A")] });
    await render({ discovery: d, pickedDevices: { ...NO_PICK, cameras: cameraKey(d.cameras[0]) } });
    expect(button("Connect").disabled).toBe(false);
    expect(button("Connect").title).toBe("");
    await render({ discovery: d, pickedDevices: { ...NO_PICK, cameras: cameraKey(d.cameras[0]) }, running: true });
    expect(button("Connect").disabled).toBe(true);
    expect(button("Connect").title).toMatch(/^Cannot change camera connection while camera is running/);
    expect(button("Configure Mock…").disabled).toBe(true);
    expect(button("Configure Mock…").title).toMatch(/camera is running/);
    await render({ discovery: d, expActive: true });
    expect(button("Connect").title).toMatch(/experiment is active/);
    expect(button("Configure Mock…").title).toMatch(/experiment is active/);
    await render({ discovery: d });
    expect(button("Connect").disabled).toBe(true);
    expect(button("Connect").title).toBe("Please select a eGrabber camera from the list.");
  });

  it("Refresh is disabled while scanning; the status line shows the scan", async () => {
    await render({ scanning: true, actionNote: "ignored while scanning" });
    expect(button("Refresh").disabled).toBe(true);
    expect(host.querySelector('[role="status"]')?.textContent).toBe("Scanning for cameras and framegrabbers...");
  });

  it("Refresh stays enabled while the camera runs: the backend's refusal reaches the status line", async () => {
    const s = await render({ running: true, scanNote: "Camera discovery incomplete: refused while capturing" });
    expect(button("Refresh").disabled).toBe(false);
    await act(async () => { button("Refresh").click(); });
    expect(s.calls.refresh).toHaveBeenCalled();
    expect(host.querySelector('[role="status"]')?.textContent).toBe("Camera discovery incomplete: refused while capturing · no camera selected");
  });

  it("the preflight's 'No camera selected' Refresh rescans the devices as well (App.tsx:1802-1811)", async () => {
    const s = await render();
    const recovery = host.querySelector(".check-recovery button") as HTMLButtonElement;
    await act(async () => { recovery.click(); });
    expect(s.calls.preflight).toHaveBeenCalled();
    expect(s.calls.refresh).toHaveBeenCalled();
  });
});

describe("the lists", () => {
  it("framegrabber rows are selectable", async () => {
    const d = discovery({ framegrabbers: [fg] });
    const s = await render({ connectTab: "framegrabbers", discovery: d, pickedDevices: { ...NO_PICK, framegrabbers: framegrabberKey(fg) } });
    const radio = host.querySelector('input[type="radio"]') as HTMLInputElement;
    expect(radio.checked).toBe(true);
    expect(button("Connect").disabled).toBe(false);
    expect(host.textContent).toContain("Coaxlink 0");
    expect(s.value.pickedDevices.framegrabbers).toBe(framegrabberKey(fg));
  });
  it("picking a row records it for the shown sub-tab only", async () => {
    const d = discovery({ cameras: [cam("Cam A"), cam("Cam B", { device_index: 1 })] });
    const s = await render({ discovery: d });
    await act(async () => { (host.querySelectorAll('input[type="radio"]')[1] as HTMLInputElement).click(); });
    expect(s.calls.pick).toHaveBeenCalledOnce();
    const update = s.calls.pick.mock.calls[0][0] as (p: typeof NO_PICK) => typeof NO_PICK;
    expect(update(NO_PICK)).toEqual({ ...NO_PICK, cameras: cameraKey(d.cameras[1]) });
  });
  it("says when a sub-tab has no devices", async () => {
    await render({ connectTab: "framegrabbers" });
    expect(host.textContent).toContain("No framegrabbers found");
    await render({ connectTab: "mindvision" });
    expect(host.textContent).toContain("No MindVision cameras found");
  });
});

describe("delivery mode (CONNECT-019)", () => {
  it("offers Every Frame and Latest Frame, shows the current mode and sends the change", async () => {
    const s = await render({ deliveryModeStatus: { valid: true, mode: "latestFrame", active_mode: "latestFrame", confirmed: false, capturing: false } });
    const select = host.querySelector('select[aria-label="Delivery mode"]') as HTMLSelectElement;
    expect([...select.options].map((o) => o.textContent)).toEqual(["Every Frame (preserves sequence)", "Latest Frame (lowest latency, may drop)"]);
    expect(select.value).toBe("latestFrame");
    expect(select.closest("label")?.title).toMatch(/^Applies at the next capture start\. For this session only/);
    await act(async () => { select.value = "everyFrame"; select.dispatchEvent(new Event("change", { bubbles: true })); });
    expect(s.calls.delivery).toHaveBeenCalledWith("everyFrame");
  });
  it("is blocked during an experiment and when the bridge has no delivery mode", async () => {
    await render({ expActive: true });
    expect((host.querySelector('select[aria-label="Delivery mode"]') as HTMLSelectElement).disabled).toBe(true);
    await render({ deliveryModeStatus: null });
    expect((host.querySelector('select[aria-label="Delivery mode"]') as HTMLSelectElement).disabled).toBe(true);
  });
  it("tells a running capture that the new mode applies at the next start", async () => {
    await render({ deliveryModeStatus: { valid: true, mode: "latestFrame", active_mode: "everyFrame", confirmed: true, capturing: true }, running: true });
    expect(host.textContent).toContain("Running with Every Frame (preserves sequence); the selected mode applies at the next capture start.");
  });
});

describe("startup auto-select (#852)", () => {
  it("shows the opt-out on instruments without the Hardware controls only", async () => {
    const s = await render({ caps: { egrabber_script: true, autofocus: false } });
    const box = host.querySelector(".auto-select-startup input") as HTMLInputElement;
    expect(box.checked).toBe(true);
    await act(async () => { box.click(); });
    expect(s.calls.autoSelect).toHaveBeenCalledWith(false);
    await render({ caps: { egrabber_script: true, autofocus: true } });
    expect(host.querySelector(".auto-select-startup")).toBeNull();
  });
});

describe("the dialogs", () => {
  it("No camera found: the check-power text, Try again and OK", async () => {
    const onTry = vi.fn(), close = vi.fn();
    await render({ noCameraDialog: true, setNoCameraDialog: close, onTryCameraAgain: onTry, cameraNotice: null, setCameraNotice: vi.fn() }, <CameraDialogs />);
    expect(host.textContent).toContain("camera LED is green (ready)");
    expect(host.querySelector('[role="alertdialog"]')?.getAttribute("aria-label")).toBe("No camera found");
    await act(async () => { button("Try again").click(); });
    expect(onTry).toHaveBeenCalled();
    await act(async () => { button("OK").click(); });
    expect(close).toHaveBeenCalledWith(false);
  });
  it("a refused camera command raises a modal warning with its title", async () => {
    const close = vi.fn();
    await render({ noCameraDialog: false, setNoCameraDialog: vi.fn(), onTryCameraAgain: vi.fn(), cameraNotice: { title: "Camera", message: "Cannot stop camera while experiment is active." }, setCameraNotice: close }, <CameraDialogs />);
    expect(host.querySelector("h3")?.textContent).toBe("Camera");
    expect(host.textContent).toContain("Cannot stop camera while experiment is active.");
    await act(async () => { button("OK").click(); });
    expect(close).toHaveBeenCalledWith(null);
  });
});
