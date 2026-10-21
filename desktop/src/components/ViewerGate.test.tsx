// @vitest-environment jsdom
// #763 F19: in a browser the classic view's control buttons stayed enabled for
// a viewer (another client controls the instrument); a press only logged
// VIEWER_ONLY. The gate disables them, follows a handover both ways, leaves
// the desktop shell (no session) alone, and the classic view puts every
// control command behind it.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import appSource from "../App.tsx?raw";

let state: { clientId?: number; controllerId?: number | null } = {};
let hasSession = true;
const listeners = new Set<(s: typeof state) => void>();
vi.mock("../transport/index", () => ({
  get transport() {
    return {
      kind: hasSession ? "ws" : "tauri",
      invoke: vi.fn(),
      session: hasSession
        ? { get: () => state, subscribe: (l: (s: typeof state) => void) => { listeners.add(l); return () => listeners.delete(l); } }
        : undefined,
    };
  },
}));
import { VIEWER_ONLY_REASON, ViewerGate, viewerReason } from "./ViewerGate";
import { RingPlaybackPanel } from "./RingPlaybackPanel";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
const setState = async (next: typeof state) => {
  state = next;
  await act(async () => { listeners.forEach((l) => l(state)); });
};
beforeEach(() => {
  state = {}; hasSession = true; listeners.clear();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const start = vi.fn(), fit = vi.fn();
function Toolbar() {
  return (
    <div className="toolbar">
      <ViewerGate>
        <button onClick={start}>Start Camera</button>
        <input aria-label="ROI x" defaultValue="0" />
        <select aria-label="Format"><option>TIFF</option></select>
      </ViewerGate>
      <button onClick={fit}>Fit: Window</button>
    </div>
  );
}
const byText = (t: string) => Array.from(host.querySelectorAll("button")).find((b) => b.textContent === t)!;

describe("ViewerGate", () => {
  it("disables the controls inside it for a viewer and leaves the rest alone", async () => {
    start.mockReset(); fit.mockReset();
    await act(async () => root.render(<Toolbar />));
    await setState({ clientId: 7, controllerId: 3 });
    for (const el of [byText("Start Camera"), host.querySelector("input")!, host.querySelector("select")!]) expect(el.matches(":disabled")).toBe(true);
    await act(async () => byText("Start Camera").click());
    expect(start).not.toHaveBeenCalled();
    expect(host.querySelector("fieldset")!.title).toBe(VIEWER_ONLY_REASON);
    expect(byText("Fit: Window").matches(":disabled")).toBe(false);
    await act(async () => byText("Fit: Window").click());
    expect(fit).toHaveBeenCalledOnce();
  });

  it("follows a handover both ways", async () => {
    start.mockReset();
    await act(async () => root.render(<Toolbar />));
    await setState({ clientId: 7, controllerId: 7 });
    expect(byText("Start Camera").matches(":disabled")).toBe(false);
    await setState({ clientId: 7, controllerId: 3 });
    expect(byText("Start Camera").matches(":disabled")).toBe(true);
    await setState({ clientId: 7, controllerId: 7 }); // took control back
    expect(byText("Start Camera").matches(":disabled")).toBe(false);
    await act(async () => byText("Start Camera").click());
    expect(start).toHaveBeenCalledOnce();
    expect(host.querySelector("fieldset")!.hasAttribute("title")).toBe(false);
  });

  it("is open before the server has said who controls, and in the desktop shell", async () => {
    await act(async () => root.render(<Toolbar />));
    expect(byText("Start Camera").matches(":disabled")).toBe(false);
    await act(async () => root.unmount());
    hasSession = false;
    root = createRoot(host);
    await act(async () => root.render(<Toolbar />));
    expect(byText("Start Camera").matches(":disabled")).toBe(false);
  });

  it("puts the viewer's reason first", () => {
    expect(viewerReason(true, "Camera is not running")).toBe(VIEWER_ONLY_REASON);
    expect(viewerReason(false, "Camera is not running")).toBe("Camera is not running");
    expect(viewerReason(false)).toBeUndefined();
  });
});

describe("ring playback for a viewer", () => {
  it("keeps stepping through the held frames but not Resume", async () => {
    const status = {
      available: true, frozen: true, invalid: false, stop_incomplete: false, restore_needed: false, run_frozen: true,
      capacity_frames: 5000, first_seq: 100, last_seq: 5099, count: 5000, sensor_fps: 5000,
    };
    await act(async () => root.render(
      <RingPlaybackPanel status={status as never} fetchFrame={vi.fn(async () => null) as never} onResume={vi.fn() as never} append={vi.fn()} />,
    ));
    await setState({ clientId: 7, controllerId: 3 });
    const resume = Array.from(host.querySelectorAll("button")).find((b) => b.textContent?.startsWith("Resume Run"))!;
    expect(resume.disabled).toBe(true);
    expect(resume.title).toBe(VIEWER_ONLY_REASON);
    expect((host.querySelector('button[aria-label="Previous frame"]') as HTMLButtonElement).disabled).toBe(false);
  });
});

// The classic view (App.tsx is too large to mount here): every control that sends a control
// command sits inside a <ViewerGate> or carries `viewerOnly` in its own disabled state. The list
// is the controls the walk-through found enabled for a viewer plus their siblings; a new control
// command in the classic view belongs on it.
describe("the classic view gates its control commands", () => {
  const gated = (at: number) => appSource.lastIndexOf("<ViewerGate>", at) > appSource.lastIndexOf("</ViewerGate>", at);
  const occurrences = (needle: string) => {
    const out: number[] = [];
    for (let i = appSource.indexOf(needle); i >= 0; i = appSource.indexOf(needle, i + 1)) out.push(i);
    return out;
  };
  it.each([
    "onClick={onStartCamera}", "onClick={onStopCamera}", "<CaptureRecovery", "<ExperimentRecovery", "<ExportStatus", "<ReanalysisStatus",
    "<HardwareControls", "<StageControls", "onClick={refreshCameraState}", "onClick={onConnectPicked}", "onClick={() => setShowMockConfig(true)}",
    "void saveCameraWindow(cameraWindow)", "onClick={onApplyRoi}", "<CameraScriptControls", "<CameraDocumentEditor",
    "onClick={onStartExperiment}", "onClick={onStopExperiment}", "<RunRateControl", "<ProcessingSetupControls", "onClick={onToggleRecord}",
    "<PreviewBufferControls", "<BackgroundCalibrationControls", "<CoreManagementPanel", "<ProfilesPanel", "<ConfigDocumentEditor",
    "bridge.monitoringClear()", "bridge.triggerManualPulse()", "bridge.triggerSetPulseDuration(", "bridge.triggerPeriodicStart(",
    "onClick={onSelectHdf}", "}}>Close File</button>", "reviewExport.start(", "<ReanalysisControls",
  ])("%s is inside a ViewerGate", (needle) => {
    const at = occurrences(needle);
    expect(at.length).toBeGreaterThan(0);
    for (const i of at) expect(gated(i), `${needle} at offset ${i}`).toBe(true);
  });
  it.each([
    ["onClick={() => void resumeFromBanner()}", "experimentRequestBusy || viewerOnly"],
    ["onClick={() => void onStopRun()}", "disabled={viewerOnly}"],
    ["onClick={onApplyConfigJson}", "|| viewerOnly}"],
    ["onClick={onApplyProcessing}", "|| viewerOnly}"],
    ["apply={bridge.setInstrumentLed}", "|| viewerOnly}"],
    ['label: "Open Recording…"', "pending: viewerOnly ? VIEWER_ONLY_REASON"],
  ])("%s is disabled for a viewer on its own", (needle, guard) => {
    const line = appSource.split("\n").find((l) => l.includes(needle));
    expect(line, needle).toBeDefined();
    expect(line).toContain(guard);
  });
});
