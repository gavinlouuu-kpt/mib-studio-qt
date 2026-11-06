// @vitest-environment jsdom
// Start / Stop Camera in the tab header and the Configure Mock dialog (epic #863, B2): the phase text beside the buttons
// (CONNECT-034), the reasons they are blocked (CONNECT-033/038), the dialog's frame rate and folder defaults
// (CONNECT-014/016), with a studio context built by hand.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { EXPERIMENT_STATES } from "../../bridgeContract";
import { StudioContext } from "../../studio/StudioContext";
import { CameraActions } from "./CameraActions";
import { cameraPhase } from "./connectFlow";
import { MockCameraDialog } from "./MockCameraDialog";

vi.mock("../../bridge", () => ({ bridge: {} }));
const dialogOpen = vi.fn();
vi.mock("../../transport/dialogs", () => ({ open: (...a: unknown[]) => dialogOpen(...a) }));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { vi.clearAllMocks(); host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const lifecycle = (p: object = {}) => ({ valid: true, state: "idle", generation: "3", camera_ready: false, failure: "none", message: "", failure_generation: "0", ...p }) as never;
const render = async (value: Record<string, unknown>, ui: JSX.Element) => {
  await act(async () => root.render(<StudioContext.Provider value={value as never}>{ui}</StudioContext.Provider>));
};
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text) as HTMLButtonElement;

describe("Start / Stop Camera and the phase text", () => {
  const actions = (over: Record<string, unknown> = {}) => {
    const calls = { start: vi.fn(), stop: vi.fn() };
    const running = (over.running as boolean) ?? false;
    const value = {
      onStartCamera: calls.start, onStopCamera: calls.stop, startCameraReason: undefined, running, expState: EXPERIMENT_STATES.Idle, cameraCommand: null,
      cameraPhase: cameraPhase(null, true, running), ...over,
    };
    return { value, calls };
  };

  it("shows the camera's phase beside the buttons (Qt: Camera starting... / running / stopping... / failed / not configured)", async () => {
    const text = () => host.querySelector(".camera-phase")?.textContent;
    await render(actions().value, <CameraActions />);
    expect(text()).toBe("Camera idle");
    await render(actions({ cameraPhase: cameraPhase(lifecycle({ state: "starting" }), true, false) }).value, <CameraActions />);
    expect(text()).toBe("Camera starting…");
    expect(host.querySelector(".camera-phase")?.className).toContain("starting");
    await render(actions({ running: true, cameraPhase: cameraPhase(lifecycle({ state: "running" }), true, true) }).value, <CameraActions />);
    expect(text()).toBe("Camera running");
    await render(actions({ cameraPhase: cameraPhase(lifecycle({ state: "stopping" }), true, false) }).value, <CameraActions />);
    expect(text()).toBe("Camera stopping…");
    await render(actions({ cameraPhase: cameraPhase(null, false, false) }).value, <CameraActions />);
    expect(text()).toBe("Camera not configured");
    await render(actions({ cameraPhase: cameraPhase(lifecycle({ state: "faulted", failure: "streamEnded", message: "camera ended its stream" }), true, false) }).value, <CameraActions />);
    expect(text()).toBe("Camera failed: camera ended its stream");
    expect(host.querySelector(".camera-phase")?.getAttribute("role")).toBe("status");
  });

  it("Start is blocked with the reason as tooltip; Stop only while the camera runs and nothing holds it", async () => {
    await render(actions({ startCameraReason: "No camera is configured. Please connect to a camera or configure a mock camera first." }).value, <CameraActions />);
    expect(button("Start Camera").disabled).toBe(true);
    expect(button("Start Camera").title).toMatch(/^No camera is configured/);
    expect(button("Stop Camera").disabled).toBe(true);
    expect(button("Stop Camera").title).toBe("Camera is not running");

    const s = actions({ running: true, startCameraReason: "Camera is already running" });
    await render(s.value, <CameraActions />);
    expect(button("Stop Camera").disabled).toBe(false);
    await act(async () => { button("Stop Camera").click(); });
    expect(s.calls.stop).toHaveBeenCalledOnce();

    await render(actions({ running: true, expState: EXPERIMENT_STATES.Active }).value, <CameraActions />);
    expect(button("Stop Camera").disabled).toBe(true);
    expect(button("Stop Camera").title).toMatch(/while experiment is active/);
    await render(actions({ running: true, expState: EXPERIMENT_STATES.Stopping }).value, <CameraActions />);
    expect(button("Stop Camera").title).toMatch(/data is being saved/);
    await render(actions({ running: true, cameraCommand: "stop" }).value, <CameraActions />);
    expect(button("Stop Camera").title).toBe("A camera command is already in progress.");
  });
});

describe("the Configure Mock dialog", () => {
  const dialog = (over: Record<string, unknown> = {}) => {
    const calls = { cancel: vi.fn(), apply: vi.fn(), dir: vi.fn(), fps: vi.fn(), loop: vi.fn() };
    const value = {
      showMockConfig: true, setShowMockConfig: calls.cancel, frameDir: "/frames", setFrameDir: calls.dir, mockFps: "5000", setMockFps: calls.fps,
      loopFiles: true, setLoopFiles: calls.loop, onConfigureMock: calls.apply, ...over,
    };
    return { value, calls };
  };
  const field = (label: string) => host.querySelector(`input[aria-label="${label}"]`) as HTMLInputElement;

  it("asks for a frame rate in fps, 1 to 10000, the rig's 5000 by default; Apply is allowed", async () => {
    const s = dialog();
    await render(s.value, <MockCameraDialog />);
    const fps = field("Mock frame rate (fps)");
    expect(fps.value).toBe("5000");
    expect(fps.min).toBe("1"); expect(fps.max).toBe("10000");
    expect(host.textContent).toContain("fps");
    expect(button("Apply").disabled).toBe(false);
    await act(async () => { button("Apply").click(); });
    expect(s.calls.apply).toHaveBeenCalledOnce();
  });

  it("a rate outside 1 to 10000 or a blank folder blocks Apply and says why", async () => {
    await render(dialog({ mockFps: "20000" }).value, <MockCameraDialog />);
    expect(button("Apply").disabled).toBe(true);
    expect(button("Apply").title).toBe("Frame rate must be 1 to 10000 fps");
    expect(host.querySelector('[role="alert"]')?.textContent).toBe("Frame rate must be 1 to 10000 fps");
    await render(dialog({ frameDir: "   " }).value, <MockCameraDialog />);
    expect(button("Apply").disabled).toBe(true);
    expect(button("Apply").title).toBe("Pick a frame directory first");
  });

  it("typing edits the fields; Cancel closes", async () => {
    const s = dialog();
    await render(s.value, <MockCameraDialog />);
    const input = field("Mock frame rate (fps)");
    const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!;
    await act(async () => { setter.call(input, "250"); input.dispatchEvent(new Event("input", { bubbles: true })); });
    expect(s.calls.fps).toHaveBeenCalledWith("250");
    await act(async () => { button("Cancel").click(); });
    expect(s.calls.cancel).toHaveBeenCalledWith(false);
  });

  it("Browse starts in the folder typed so far and stores the pick trimmed", async () => {
    const s = dialog({ frameDir: " /frames " });
    dialogOpen.mockResolvedValue("  /picked/folder  ");
    await render(s.value, <MockCameraDialog />);
    await act(async () => { button("Browse…").click(); });
    expect(dialogOpen).toHaveBeenCalledWith(expect.objectContaining({ directory: true, defaultPath: "/frames" }));
    expect(s.calls.dir).toHaveBeenCalledWith("/picked/folder");
    dialogOpen.mockResolvedValue(null);
    s.calls.dir.mockClear();
    await act(async () => { button("Browse…").click(); });
    expect(s.calls.dir).not.toHaveBeenCalled();
  });

  it("renders nothing while closed", async () => {
    await render(dialog({ showMockConfig: false }).value, <MockCameraDialog />);
    expect(host.querySelector(".modal")).toBeNull();
  });
});
