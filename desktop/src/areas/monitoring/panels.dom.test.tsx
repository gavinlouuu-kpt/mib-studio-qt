// @vitest-environment jsdom
// The Monitoring panel (Clear Buffer's question, the failure notices) and the Experiment tab frame (run state widget,
// standing readiness, progress and accounting lines), with their heavy children replaced.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { EXPERIMENT_STATES, READINESS_GATE_STATUSES as S, RUN_COMPLETION_STATES as C } from "../../bridgeContract";
import type { ExperimentStatus } from "../../eventAdapter";

const bridge = vi.hoisted(() => ({ monitoringClear: vi.fn() }));
const dialogs = vi.hoisted(() => ({ ask: vi.fn() }));
vi.mock("../../bridge", () => ({ bridge }));
vi.mock("../../transport/dialogs", () => dialogs);
vi.mock("../hardware/TriggerTestControls", () => ({ TriggerTestControls: () => null }));
vi.mock("../preview/PreviewPanel", () => ({ PreviewPanel: () => <div data-testid="preview" /> }));
vi.mock("../../components/MonitoringCharts", () => ({ MonitoringCharts: () => <div data-testid="charts" /> }));
vi.mock("../../components/ResultMetricCells", () => ({ ResultMetricCells: () => null }));
vi.mock("../../components/RunRateControl", () => ({ RunRateControl: () => null }));
vi.mock("../../components/RunWindowNotice", () => ({ RunWindowNotice: () => null }));

import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";
import { ExperimentTab } from "./ExperimentTab";
import { MonitoringPanel } from "./MonitoringPanel";
import { deriveRunPresentation } from "./runStatus";
import { summarizeReadiness } from "./startReadiness";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); vi.clearAllMocks(); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const render = async (s: unknown, view: JSX.Element) => act(async () => root.render(<StudioContext.Provider value={s as Studio}>{view}</StudioContext.Provider>));
const button = (label: string) => Array.from(host.querySelectorAll<HTMLButtonElement>("button")).find((b) => b.textContent === label)!;

describe("MonitoringPanel (MON-029 and the swallowed polls)", () => {
  const panel = (over: Record<string, unknown> = {}) => ({
    append: vi.fn(), ready: true, caps: { trigger: false }, trigStatus: null, monSnapshot: { valid_held: 3, invalid_held: 0, valid_appended: 3, invalid_appended: 0, rows: [] },
    expActive: false, setExpTab: vi.fn(), setConfigTab: vi.fn(), setMonSnapshot: vi.fn(), notifyMonitoringCleared: vi.fn(), monitoringNotices: [], ...over,
  });

  it("asks before it clears and clears nothing on No", async () => {
    const s = panel();
    dialogs.ask.mockResolvedValue(false);
    await render(s, <MonitoringPanel />);
    await act(async () => button("Clear Buffer").click());
    expect(dialogs.ask).toHaveBeenCalledWith("Are you sure you want to clear the monitoring buffer? This will remove all accumulated frames.", { title: "Clear Buffer", kind: "warning" });
    expect(bridge.monitoringClear).not.toHaveBeenCalled();
    expect(s.notifyMonitoringCleared).not.toHaveBeenCalled();
  });
  it("clears on Yes, empties the table at once and tells whoever registered a hook", async () => {
    const s = panel();
    dialogs.ask.mockResolvedValue(true);
    bridge.monitoringClear.mockResolvedValue({ ok: true, message: "" });
    await render(s, <MonitoringPanel />);
    await act(async () => button("Clear Buffer").click());
    expect(bridge.monitoringClear).toHaveBeenCalledTimes(1);
    expect(s.setMonSnapshot).toHaveBeenCalledWith(null);
    expect(s.notifyMonitoringCleared).toHaveBeenCalledTimes(1);
    expect(s.append).toHaveBeenCalledWith("monitoring buffers cleared");
  });
  it("shows the Monitoring failures as alerts above the charts", async () => {
    await render(panel({ monitoringNotices: ["Monitoring could not be switched on in the backend (busy): rows and charts may stay empty."] }), <MonitoringPanel />);
    const alert = host.querySelector("[role='alert'][data-tour='monitoring-notice']")!;
    expect(alert.textContent).toContain("Monitoring could not be switched on");
    expect(alert.compareDocumentPosition(host.querySelector("[data-testid='charts']")!) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  });
});

describe("ExperimentTab (MON-022, 025, 028, 023)", () => {
  const status = (over: Partial<ExperimentStatus> = {}) => ({
    valid: true, state: EXPERIMENT_STATES.Idle, terminal: false, cancelled: false, output_path: "/d/run.h5", start_generation: "4", end_time_ns: "0",
    completion: C.Unknown, completion_reason: "", fault_code: "", fault_message: "", persistence_committed: "0", finalization_ok: false, ...over,
  }) as ExperimentStatus;
  const tab = (over: Record<string, unknown> = {}) => {
    const expStatus = (over.expStatus as ExperimentStatus) ?? status();
    return {
      ssdConfigured: false, ssd: null, expTab: "preview", setExpTab: vi.fn(), roiFields: { x: "0", y: "0", w: "0", h: "0" }, onStartExperiment: vi.fn(),
      startExperimentReason: undefined, onStopExperiment: vi.fn(), expState: expStatus.state, instrumentModes: false, runFpsStatus: null, cameraWin: {},
      modeError: null, setTab: vi.fn(), setModeRetry: vi.fn(), readinessMessage: "", expActive: false, runOutcome: null, expStatus, startNotice: "", tab: "experiment",
      instrument: null, runPresentation: deriveRunPresentation({ cameraRunning: true, status: expStatus }), runProgress: "", readinessSummary: null, readinessNotice: "",
      accountingNotice: "", stopRequested: false, ...over,
    };
  };

  it("lets the operator dismiss a Start refusal's alert; nothing is sent (#877 bench B3)", async () => {
    const setReadinessMessage = vi.fn();
    await render(tab({ readinessMessage: "/ro/run.h5: storage.output: not writable", setReadinessMessage }), <ExperimentTab />);
    const alert = [...host.querySelectorAll("[role=alert]")].find((e) => e.textContent?.includes("storage.output"))!;
    expect(alert.textContent).toContain("Experiment readiness: /ro/run.h5: storage.output: not writable");
    const dismiss = alert.querySelector<HTMLButtonElement>("button.readiness-dismiss")!;
    expect(dismiss.textContent).toBe("Dismiss");
    await act(async () => dismiss.click());
    expect(setReadinessMessage).toHaveBeenCalledWith("");
  });
  it("puts the run state widget in the toolbar", async () => {
    await render(tab(), <ExperimentTab />);
    expect(host.querySelector(".toolbar .right [data-tour='monitoring-run-status']")!.textContent).toBe("◉Camera running");
  });
  it("lists the gates that block Start or warn, before Start is pressed (not while a run is active)", async () => {
    const summary = summarizeReadiness([
      { id: "camera.rate", status: S.Fail, reason: "too fast", remediation: "lower it" }, { id: "storage.buffer", status: S.Warn, reason: "short buffer", remediation: "" },
    ]);
    await render(tab({ readinessSummary: summary }), <ExperimentTab />);
    expect(host.querySelectorAll(".start-readiness li")).toHaveLength(2);
    await render(tab({ readinessSummary: summary, expActive: true }), <ExperimentTab />);
    expect(host.querySelector(".start-readiness")).toBeNull();
  });
  it("says what is happening while the run stops and is saved", async () => {
    await render(tab({ runProgress: "Saving experiment data…" }), <ExperimentTab />);
    expect(host.querySelector("[data-tour='monitoring-run-progress']")!.textContent).toBe("Saving experiment data…");
  });
  it("disables Stop while the stop request is out", async () => {
    const active = status({ state: EXPERIMENT_STATES.Active });
    await render(tab({ expStatus: active, stopRequested: true }), <ExperimentTab />);
    expect(button("Stop Experiment")).toMatchObject({ disabled: true, title: "Stopping experiment…" });
    await render(tab({ expStatus: active }), <ExperimentTab />);
    expect(button("Stop Experiment").disabled).toBe(false);
  });
  it("adds Qt's remediation under a run that lost data, and says when the accounting could not be read", async () => {
    const lost = status({ terminal: true, finalization_ok: true, completion: C.IncompleteLoss, completion_reason: "undeclared loss" });
    await render(tab({ expStatus: lost, accountingNotice: "The run's frame accounting could not be read (gone); the counts below come from the saved status only." }), <ExperimentTab />);
    expect(host.querySelector("[data-tour='monitoring-run-remediation']")!.textContent).toBe("Review the frame accounting in the Review tab before using this run.");
    expect(host.querySelector("[data-tour='monitoring-accounting-error']")!.textContent).toContain("could not be read");
    await render(tab({ expStatus: status({ terminal: true, completion: C.Complete }) }), <ExperimentTab />);
    expect(host.querySelector("[data-tour='monitoring-run-remediation']")).toBeNull();
  });
});
