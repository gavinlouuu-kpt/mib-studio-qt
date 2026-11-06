// @vitest-environment jsdom
// The startup camera step's outcome on the Connect stage (epic #863, B1; CONNECT-022/024/025/026/027/028, bug #852).
import { act, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { bridge, type DiscoveredDevice, type DiscoverySnapshot, type StartupDiscoveryStatus } from "../../bridge";
import { CAMERA_SELECTION_MODES, DISCOVERY_ERROR_KINDS, DISCOVERY_JOB_STATES } from "../../bridgeContract";
import { useConnectState } from "./useConnect";
import { useStartupSelection } from "./useStartupSelection";

vi.mock("../../bridge", () => ({
  bridge: { startupDiscoveryStatus: vi.fn(), startupDiscoveryRun: vi.fn(), fetchDeviceDiscovery: vi.fn(), fetchCameraSelection: vi.fn() },
}));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
let store: Map<string, string>;
beforeEach(() => {
  vi.useFakeTimers(); vi.clearAllMocks();
  store = new Map();
  vi.stubGlobal("localStorage", { getItem: (k: string) => store.get(k) ?? null, setItem: (k: string, v: string) => store.set(k, v) });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  vi.mocked(bridge.startupDiscoveryRun).mockResolvedValue({ accepted: true, message: "Auto-selection scheduled" });
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); vi.unstubAllGlobals(); });

const append = vi.fn();
let cap: { connect: ReturnType<typeof useConnectState>; startup: ReturnType<typeof useStartupSelection>; tab: string };
interface Props { ready?: boolean; platform?: object | null; autofocus?: boolean; pz7035?: boolean; expActive?: boolean }
function Harness({ ready = true, platform = {}, autofocus = false, pz7035 = false, expActive = false }: Props) {
  const connect = useConnectState();
  const [tab, setTab] = useState("connect");
  const startup = useStartupSelection({
    ready, platform, caps: { autofocus }, pz7035, append, setTab: ((t: unknown) => { setTab(t as never); }) as never, expActive,
    ...connect,
  } as never);
  cap = { connect, startup, tab };
  return null;
}
const mount = (props: Props = {}) => act(async () => root.render(<Harness {...props} />));
const tick = (ms = 500) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });

const status = (p: Partial<StartupDiscoveryStatus> & { job?: string; state?: number; running?: boolean } = {}): StartupDiscoveryStatus => ({
  valid: true, camera_running: p.running ?? false, nanopositioner_running: false, camera_configured: p.camera_configured ?? false, nanopositioner_connected: false,
  camera: { job_id: p.job ?? "0", state: p.state, errors: [] }, nanopositioner: { job_id: "0", errors: [] },
});
const device = (p: Partial<DiscoveredDevice> = {}): DiscoveredDevice => ({
  kind: 0, provider_id: "egrabber", display_name: "", system_path: "", persistent_id: "", sdk_index: 0, interface_index: 0, device_index: 0, stream_index: -1,
  bus_address: -1, stable_identity: "", identity_strength: 1, identification: 0, claimed_by: [], capabilities: [], synthetic: false, camera_type: 0,
  interface_id: "", device_id: "", stream_id: "", model_name: "", firmware_version: "", label: "EGrabber 0", ...p,
});
const snapshot = (p: Partial<DiscoverySnapshot> = {}): DiscoverySnapshot => ({
  valid: true, job_id: "7", generation: "7", state: DISCOVERY_JOB_STATES.Completed, complete: true, overflow: false, attempt: 1, max_attempts: 1,
  kinds: [0, 1], candidates: [], errors: [], providers_run: [], origin: "startup-camera", ...p,
});
const sel = (p: object = {}) => ({ valid: true, configured: false, mode: 0, label: "", interface_index: -1, device_index: -1, mindvision_index: -1, ...p }) as never;
const ENDED = { job: "7", state: DISCOVERY_JOB_STATES.Completed };

/** Poll 1 primes (nothing yet), poll 2 sees the job running, poll 3 sees it ended. */
async function runJob(ended: StartupDiscoveryStatus) {
  vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValueOnce(status());
  await mount();
  vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValueOnce(status({ job: "7", running: true, state: DISCOVERY_JOB_STATES.Running }));
  await tick();
  vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValue(ended);
  await tick();
}

describe("the startup camera step's outcome", () => {
  it("highlights the auto-selected camera, opens its sub-tab and moves to Overview (CONNECT-027)", async () => {
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot({
      candidates: [device({ label: "EGrabber 0" }), device({ camera_type: 1, sdk_index: 2, label: "MV 2", identification: 1 })],
    }));
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(sel({ configured: true, mode: CAMERA_SELECTION_MODES.Hardware, label: "EGrabber 0" }));
    await runJob(status({ ...ENDED, camera_configured: true }));
    expect(cap.tab).toBe("overview");
    expect(cap.connect.connectTab).toBe("cameras");
    expect(cap.connect.pickedDevices.cameras).toBe("0:0:0:0");
    expect(cap.connect.actionNote).toBe("Connected to EGrabber 0 (not capturing)");
    expect(cap.connect.discovery?.cameras.map((c) => c.label)).toContain("EGrabber 0");
    expect(cap.connect.noCameraDialog).toBe(false);
  });

  it("opens the MindVision sub-tab for a MindVision camera", async () => {
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot({ candidates: [device({ camera_type: 1, sdk_index: 2, label: "MV 2", interface_index: -1, device_index: -1 })] }));
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(sel({ configured: true, mode: CAMERA_SELECTION_MODES.MindVision, mindvision_index: 2, label: "MV 2" }));
    await runJob(status({ ...ENDED, camera_configured: true }));
    expect(cap.connect.connectTab).toBe("mindvision");
    expect(cap.tab).toBe("overview");
  });

  it("refreshes the lists once per camera job, however many polls see it end", async () => {
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot());
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(sel());
    await runJob(status(ENDED));
    await tick(); await tick(); await tick();
    expect(bridge.fetchDeviceDiscovery).toHaveBeenCalledTimes(1);
    expect(bridge.fetchDeviceDiscovery).toHaveBeenCalledWith("7");
    // a second job (Try again) is reported again
    vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValue(status({ job: "9", state: DISCOVERY_JOB_STATES.Completed }));
    await tick();
    expect(bridge.fetchDeviceDiscovery).toHaveBeenCalledTimes(2);
  });

  it("no camera: 'No cameras found.', the Connect stage, and the dialog (CONNECT-024)", async () => {
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot());
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(sel());
    await runJob(status(ENDED));
    expect(cap.connect.actionNote).toBe("No cameras found.");
    expect(cap.connect.noCameraDialog).toBe(true);
    expect(cap.tab).toBe("connect");
  });

  it("several cameras: asks the operator to select one; no dialog, no tab change (CONNECT-025)", async () => {
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot({ candidates: [device({}), device({ device_index: 1, label: "EGrabber 1" })] }));
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(sel());
    await runJob(status(ENDED));
    expect(cap.connect.actionNote).toBe("Multiple cameras found; select one and click Connect.");
    expect(cap.connect.noCameraDialog).toBe(false);
    expect(cap.tab).toBe("connect");
    expect(cap.connect.discovery?.cameras).toHaveLength(2);
    expect(cap.connect.pickedDevices.cameras).toBe("0:0:0:0"); // row 0 preselected
  });

  it("an incomplete scan is 'Camera discovery incomplete: <reason>', never 'no cameras' (CONNECT-026)", async () => {
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot({ complete: false, errors: [{ provider_id: "egrabber", kind: DISCOVERY_ERROR_KINDS.Busy, message: "", endpoint: "" }] }));
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(sel());
    await runJob(status(ENDED));
    expect(cap.connect.actionNote).toBe("Camera discovery incomplete: egrabber Busy");
    expect(cap.connect.noCameraDialog).toBe(false);
    expect(cap.connect.scanNote).toContain("(incomplete: egrabber Busy)");
  });

  it("does not report a job that ended before this view looked (a webview reload)", async () => {
    vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValue(status(ENDED));
    await mount(); await tick(); await tick();
    expect(bridge.fetchDeviceDiscovery).not.toHaveBeenCalled();
    expect(cap.connect.noCameraDialog).toBe(false);
  });

  it("Try again closes the dialog and runs the immediate camera step", async () => {
    vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValue(status());
    await mount();
    await act(async () => { cap.connect.setNoCameraDialog(true); });
    await act(async () => { await cap.startup.onTryCameraAgain(); });
    expect(bridge.startupDiscoveryRun).toHaveBeenCalledWith("camera");
    expect(cap.connect.noCameraDialog).toBe(false);
  });
});

describe("the startup selection is on by default (#852)", () => {
  beforeEach(() => { vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValue(status()); });

  it("starts once when the backend is ready and the instrument has no Hardware controls", async () => {
    await mount();
    expect(bridge.startupDiscoveryRun).toHaveBeenCalledOnce();
    expect(bridge.startupDiscoveryRun).toHaveBeenCalledWith("start");
    await mount({ ready: true }); await tick();
    expect(bridge.startupDiscoveryRun).toHaveBeenCalledOnce();
  });
  it("starts after the first status reading, so a scan that ends at once is reported, not taken for an old one", async () => {
    let firstReading!: (s: StartupDiscoveryStatus) => void;
    vi.mocked(bridge.startupDiscoveryStatus).mockImplementationOnce(() => new Promise((resolve) => { firstReading = resolve; }));
    vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValue(status(ENDED));
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot());
    vi.mocked(bridge.fetchCameraSelection).mockResolvedValue(sel());
    await mount();
    expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled();
    await act(async () => firstReading(status()));
    expect(bridge.startupDiscoveryRun).toHaveBeenCalledWith("start");
    await tick();
    expect(bridge.fetchDeviceDiscovery).toHaveBeenCalledWith("7");
    expect(cap.connect.noCameraDialog).toBe(true);
  });
  it("does not wait for a status reading that never comes", async () => {
    vi.mocked(bridge.startupDiscoveryStatus).mockImplementation(() => new Promise(() => {}));
    await mount();
    expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled();
    await tick(2100);
    expect(bridge.startupDiscoveryRun).toHaveBeenCalledWith("start");
  });
  it("waits for the platform report", async () => {
    await mount({ platform: null });
    expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled();
    await mount({ platform: {} });
    expect(bridge.startupDiscoveryRun).toHaveBeenCalledOnce();
  });
  it("leaves it to the Hardware controls where they exist (they install the nanopositioner preference first)", async () => {
    await mount({ autofocus: true });
    expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled();
  });
  it("stays off after the operator opted out", async () => {
    store.set("mib.startup.auto-select-hardware", "false");
    await mount();
    expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled();
    expect(cap.startup.autoSelect).toBe(false);
  });
  it("is off on the PZ7035 unless the operator opted in", async () => {
    await mount({ pz7035: true });
    expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled();
    expect(cap.startup.autoSelect).toBe(false);
  });
  it("never starts during an experiment", async () => {
    await mount({ expActive: true });
    expect(bridge.startupDiscoveryRun).not.toHaveBeenCalled();
  });
  it("the Connect stage's checkbox stores the choice", async () => {
    await mount({ pz7035: true });
    await act(async () => { cap.startup.onAutoSelectChange(true); });
    expect(store.get("mib.startup.auto-select-hardware")).toBe("true");
    expect(cap.startup.autoSelect).toBe(true);
    await act(async () => { cap.startup.onAutoSelectChange(false); });
    expect(store.get("mib.startup.auto-select-hardware")).toBe("false");
  });
});
