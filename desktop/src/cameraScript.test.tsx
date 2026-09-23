// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { CameraScriptControls, useCameraScript, type CameraScriptContext } from "./cameraScript";
import { bridge, type CameraSelection } from "./bridge";
import { open } from "@tauri-apps/plugin-dialog";

vi.mock("./bridge", () => ({ bridge: {
  fetchCameraSelection: vi.fn(), applyCameraScript: vi.fn(), resetHardwareCamera: vi.fn(),
} }));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const camera: CameraSelection = { valid: true, configured: true, mode: 2, running: false,
  interface_index: 0, device_index: 1, label: "camera", mindvision_index: -1,
  mindvision_config_path: "", camera_script_path: "", mock_frame_dir: "", mock_interval_ms: 0, mock_loop: false };
let root: Root;
let host: HTMLDivElement;
let ctx: CameraScriptContext;
function Harness() { const model = useCameraScript(ctx); return <CameraScriptControls model={model} />; }
function button(text: string) { return [...host.querySelectorAll("button")].find(x => x.textContent === text)!; }
async function click(text: string) { await act(async () => { button(text).click(); }); }
beforeEach(async () => {
  vi.resetAllMocks();
  vi.mocked(open).mockResolvedValue("/tmp/camera.js");
  vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(camera);
  vi.mocked(bridge.applyCameraScript).mockResolvedValue({ok: true, message: "Applied", operation_id: "0", command: 0} as never);
  ctx = { ready: true, running: false, experimentActive: false, selection: camera, append: vi.fn(), refresh: vi.fn().mockResolvedValue(undefined) };
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  await act(async () => { root.render(<Harness />); });
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
describe("camera script operator controls", () => {
  it("selects without actuating and only applies on an explicit click", async () => {
    await click("Browse Script…");
    expect(bridge.applyCameraScript).not.toHaveBeenCalled();
    expect(host.textContent).toContain("not applied");
    await click("Apply to Camera");
    expect(bridge.applyCameraScript).toHaveBeenCalledWith("/tmp/camera.js");
    expect(ctx.refresh).toHaveBeenCalledOnce();
  });
  it("rejects stale capture state before touching hardware", async () => {
    await click("Browse Script…");
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue({...camera, running: true});
    await click("Apply to Camera");
    expect(bridge.applyCameraScript).not.toHaveBeenCalled();
    expect(host.textContent).toContain("Stop the camera");
  });
  it("does not retarget a script when the selected device changes", async () => {
    await click("Browse Script…");
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue({...camera, device_index: 2});
    await click("Apply to Camera");
    expect(bridge.applyCameraScript).not.toHaveBeenCalled();
    expect(host.textContent).toContain("selection changed");
  });
  it("keeps one pending mutation and reports backend rejection", async () => {
    await click("Browse Script…");
    let resolve!: (value: Awaited<ReturnType<typeof bridge.applyCameraScript>>) => void;
    vi.mocked(bridge.applyCameraScript).mockReturnValue(new Promise(r => { resolve = r; }));
    await click("Apply to Camera");
    await click("Apply to Camera");
    expect(bridge.applyCameraScript).toHaveBeenCalledOnce();
    expect(button("Reset Camera").disabled).toBe(true);
    await act(async () => resolve({ok:false,message:"SDK rejected script",operation_id:"0",command:0} as never));
    expect(host.textContent).toContain("SDK rejected script");
    expect(button("Reset Camera").disabled).toBe(false);
  });
  it.each([{experimentActive:true}, {running:true}, {ready:false}, {selection:{...camera, mode:1}}])("blocks unavailable or active setup: %j", async patch => {
    ctx = {...ctx, ...patch};
    await act(async () => root.render(<Harness />));
    expect(button("Browse Script…").disabled).toBe(true);
    expect(button("Reset Camera").disabled).toBe(true);
  });
});
