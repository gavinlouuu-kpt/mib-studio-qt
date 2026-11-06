// @vitest-environment jsdom
// The #866 behaviour of the monitoring hooks, against a mocked bridge and dialogs: the Monitoring polls say when
// they keep failing, the standing Start readiness, the Start / Stop flow and the end of a run.
import { act, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { EXPERIMENT_START_OUTCOMES as O, EXPERIMENT_STATES, READINESS_GATE_STATUSES as S, RUN_COMPLETION_STATES as C } from "../../bridgeContract";
import type { ExperimentStatus } from "../../eventAdapter";

const bridge = vi.hoisted(() => ({
  monitoringSetActive: vi.fn(), fetchMonitoringSnapshot: vi.fn(), fetchTriggerStatus: vi.fn(), fetchExperimentReadiness: vi.fn(),
  fetchRingStatus: vi.fn(), experimentStart: vi.fn(), fetchExperimentStatus: vi.fn(), experimentStop: vi.fn(), fetchRunAccounting: vi.fn(),
  fetchProcessingConfigJson: vi.fn(),
}));
const dialogs = vi.hoisted(() => ({ confirm: vi.fn(), save: vi.fn(), message: vi.fn(), ask: vi.fn(), choose: vi.fn() }));
vi.mock("../../bridge", () => ({ bridge }));
vi.mock("../../transport/dialogs", () => dialogs);
vi.mock("../../transport", () => ({ isRemote: false }));

import { useExperimentLifecycle, useMonitoringPolling, useMonitoringState, useRunLockedNavigation, useStartReadiness, type MonitoringState } from "./useMonitoring";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => {
  vi.useFakeTimers();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  for (const fn of [...Object.values(bridge), ...Object.values(dialogs)]) fn.mockReset();
  bridge.monitoringSetActive.mockResolvedValue({ ok: true, message: "" });
  bridge.fetchMonitoringSnapshot.mockResolvedValue({ valid: true, rows: [] });
  bridge.fetchTriggerStatus.mockResolvedValue({ trigger_count: 0 });
  bridge.fetchExperimentReadiness.mockResolvedValue({ valid: true, ready: false, generation: "1", gates: [], method_calibration: null });
  bridge.fetchRingStatus.mockResolvedValue({ run_frozen: false });
  bridge.fetchExperimentStatus.mockResolvedValue(status({ state: EXPERIMENT_STATES.Active, output_path: "/d/run.h5", start_generation: "4" }));
  bridge.fetchRunAccounting.mockResolvedValue({ available: true, source: "last_run", start_generation: 4 });
  bridge.fetchProcessingConfigJson.mockResolvedValue({ valid: true, json: "{}" });
  dialogs.confirm.mockResolvedValue(true); dialogs.save.mockResolvedValue("/d/run.h5"); dialogs.message.mockResolvedValue(undefined);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

const status = (over: Partial<ExperimentStatus> = {}): ExperimentStatus => ({
  valid: true, state: EXPERIMENT_STATES.Idle, start_time_ns: "0", end_time_ns: "0", valid_buffered: "0", invalid_buffered: "0", valid_saved: "0",
  invalid_saved: "0", dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false, output_path: "", message: "", start_generation: "0",
  readiness_generation: "0", capture_generation: "0", persistence_admitted: "0", persistence_committed: "0", persistence_failed: "0", terminal: false,
  finalization_ok: false, completion: C.Unknown, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "", ...over,
});
const tick = async (ms: number) => { await act(async () => { await vi.advanceTimersByTimeAsync(ms); }); };

interface Probe { state: MonitoringState; log: string[]; lifecycle: ReturnType<typeof useExperimentLifecycle> }
function Harness({ probe, expTab = "monitoring", tab = "experiment", hostProcessing = true, running = true }:
  { probe: Probe; expTab?: "preview" | "monitoring"; tab?: string; hostProcessing?: boolean; running?: boolean }) {
  const state = useMonitoringState();
  const [, setTrig] = useState<unknown>(null);
  const append = (line: string) => { probe.log.push(line); };
  useMonitoringPolling({ ...state, ready: true, tab: tab as never, expTab, append, caps: { trigger: true } as never, setTrigStatus: setTrig as never });
  useStartReadiness({ ...state, ready: true, tab: tab as never });
  probe.lifecycle = useExperimentLifecycle({ ...state, append, hostProcessing, running });
  probe.state = state;
  return null;
}
const mount = async (props: Partial<Parameters<typeof Harness>[0]> = {}) => {
  const probe = { log: [] } as unknown as Probe;
  await act(async () => root.render(<Harness probe={probe} {...props} />));
  return probe;
};

describe("Monitoring polls (App.tsx:533, 542-544 at 5f906ac2)", () => {
  it("tells the operator when the backend refuses to switch Monitoring on, and logs it", async () => {
    bridge.monitoringSetActive.mockResolvedValue({ ok: false, message: "monitoring is busy" });
    const probe = await mount();
    await tick(0);
    expect(probe.state.monitoringIssue).toContain("monitoring is busy");
    expect(probe.log.some((l) => l.includes("monitoring is busy"))).toBe(true);
    bridge.monitoringSetActive.mockResolvedValue({ ok: true, message: "" });
  });
  it("tells the operator when the call itself fails, and clears the notice once it works", async () => {
    bridge.monitoringSetActive.mockRejectedValue(new Error("backend gone"));
    const probe = await mount({ expTab: "monitoring" });
    await tick(0);
    expect(probe.state.monitoringIssue).toContain("backend gone");
    // Leaving the view and coming back with a working backend clears it.
    bridge.monitoringSetActive.mockResolvedValue({ ok: true, message: "" });
    await act(async () => root.render(<Harness probe={probe} expTab="preview" />));
    await act(async () => root.render(<Harness probe={probe} expTab="monitoring" />));
    await tick(0);
    expect(probe.state.monitoringIssue).toBe("");
  });
  it("does not nag about one lost snapshot, but raises a notice after three in a row and clears it on success", async () => {
    const probe = await mount();
    bridge.fetchMonitoringSnapshot.mockRejectedValue(new Error("timeout"));
    await tick(500);
    await tick(500);
    expect(probe.state.snapshotHealth.failures).toBe(2);
    await tick(500);
    expect(probe.state.snapshotHealth.failures).toBe(3);
    expect(probe.state.snapshotHealth.lastError).toBe("timeout");
    bridge.fetchMonitoringSnapshot.mockResolvedValue({ valid: true, rows: [] });
    await tick(500);
    expect(probe.state.snapshotHealth.failures).toBe(0);
  });
  it("counts the snapshot and the trigger status on their own: a failing trigger does not stop the table", async () => {
    const probe = await mount();
    bridge.fetchTriggerStatus.mockRejectedValue(new Error("trigger service gone"));
    const snapshot = { valid: true, rows: [{ frame_index: 1 }] };
    bridge.fetchMonitoringSnapshot.mockResolvedValue(snapshot);
    await tick(1500);
    expect(probe.state.triggerHealth.failures).toBe(3);
    expect(probe.state.snapshotHealth.failures).toBe(0);
    expect(probe.state.monSnapshot).toBe(snapshot);
  });
  it("polls nothing while Monitoring is not shown", async () => {
    await mount({ expTab: "preview" });
    await tick(2000);
    expect(bridge.fetchMonitoringSnapshot).not.toHaveBeenCalled();
    expect(bridge.monitoringSetActive).toHaveBeenCalledWith(false);
  });
});

describe("the standing Start readiness (MON-028)", () => {
  const gates = [
    { id: "camera.rate", status: S.Fail, reason: "too fast", remediation: "lower it" },
    { id: "storage.buffer", status: S.Warn, reason: "short buffer", remediation: "" },
  ];
  it("reads the gates with no destination on the Experiment stage and keeps the same object while nothing changes", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue({ valid: true, ready: false, generation: "1", gates, method_calibration: null });
    const probe = await mount({ expTab: "preview" });
    await tick(0);
    expect(bridge.fetchExperimentReadiness).toHaveBeenCalledWith("");
    const first = probe.state.startReadiness;
    expect(first?.gates.map((g) => g.id)).toEqual(["camera.rate", "storage.buffer"]);
    bridge.fetchExperimentReadiness.mockResolvedValue({ valid: true, ready: false, generation: "2", gates: gates.map((g) => ({ ...g })), method_calibration: null });
    await tick(2000);
    expect(probe.state.startReadiness).toBe(first);
  });
  it("reads once at a time, every 2 s, and not on other stages", async () => {
    await mount({ tab: "connect", expTab: "preview" });
    await tick(5000);
    expect(bridge.fetchExperimentReadiness).not.toHaveBeenCalled();
  });
  it("raises a notice after three failed reads", async () => {
    bridge.fetchExperimentReadiness.mockRejectedValue(new Error("backend gone"));
    const probe = await mount({ expTab: "preview" });
    await tick(4100);
    expect(probe.state.readinessHealth.failures).toBe(3);
  });
});

describe("Start Experiment (MON-003, 008, 009, 010)", () => {
  const ready = { valid: true, ready: true, generation: "1", gates: [], method_calibration: null };
  it("opens the file dialog with Qt's filters and no default name, then starts", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue(ready);
    bridge.experimentStart.mockResolvedValue({ ok: true, message: "started", start_outcome: O.Started });
    const probe = await mount({ expTab: "preview" });
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(dialogs.save).toHaveBeenCalledWith({
      title: "Save Experiment Data", filters: [{ name: "HDF5 Files", extensions: ["h5", "hdf5"] }, { name: "All Files", extensions: ["*"] }], defaultPath: undefined,
    });
    expect(bridge.experimentStart).toHaveBeenCalledWith("/d/run.h5", false, undefined, false);
    expect(dialogs.choose).not.toHaveBeenCalled();
    expect(probe.log.some((l) => l.startsWith("experiment started"))).toBe(true);
    expect(probe.state.startInFlight).toBe(false);
  });
  it("explains a blocking gate in a dialog before it asks for a file", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue({ ...ready, ready: false, gates: [{ id: "camera.rate", status: S.Fail, reason: "too fast", remediation: "lower it" }] });
    const probe = await mount({ expTab: "preview" });
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(dialogs.save).not.toHaveBeenCalled();
    expect(dialogs.message).toHaveBeenCalledWith(expect.stringContaining("Blocked — camera.rate: too fast\n    → lower it"), { title: "Start Experiment", kind: "warning" });
    expect(probe.state.readinessMessage).toBe("camera.rate: too fast — lower it");
    expect(bridge.experimentStart).not.toHaveBeenCalled();
  });
  it("tells the operator about a multi-image series after the file is chosen, only on a host-processing instrument", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue(ready);
    bridge.experimentStart.mockResolvedValue({ ok: true, message: "", start_outcome: O.Started });
    bridge.fetchProcessingConfigJson.mockResolvedValue({ valid: true, json: JSON.stringify({ image_processing: { multi_image: { enabled: true, count: 3 } }, realtime_processing: { enabled: true, mode: "async_batch" } }) });
    let probe = await mount({ expTab: "preview" });
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(dialogs.message).toHaveBeenCalledWith(expect.stringContaining("Multi-image series capture requires inline realtime processing."), { title: "Start Experiment", kind: "info" });
    dialogs.message.mockClear();
    await act(async () => root.unmount()); root = createRoot(host);
    probe = await mount({ expTab: "preview", hostProcessing: false });
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(dialogs.message).not.toHaveBeenCalled();
  });
  it("words a refusal by its typed outcome: a dialog, the inline message and the log", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue(ready);
    bridge.experimentStart.mockResolvedValue({ ok: false, message: "disk full", start_outcome: O.StorageFailed });
    const probe = await mount({ expTab: "preview" });
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(dialogs.message).toHaveBeenCalledWith("Failed to prepare the experiment file:\n/d/run.h5\n\ndisk full", { title: "Error", kind: "error" });
    expect(probe.state.readinessMessage).toBe("/d/run.h5: Experiment start failed: disk full");
    expect(probe.log).toContain("experiment start failed: Experiment start failed: disk full");
  });
  it("is no longer Starting while the refusal dialog of a refused reply is open (#891 review)", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue(ready);
    bridge.experimentStart.mockResolvedValue({ ok: false, message: "disk full", start_outcome: O.StorageFailed });
    let close!: () => void;
    dialogs.message.mockImplementation(() => new Promise<void>((resolve) => { close = resolve; }));
    const probe = await mount({ expTab: "preview" });
    let started!: Promise<void>;
    await act(async () => { started = probe.lifecycle.onStartExperiment(); });
    await vi.waitFor(() => expect(dialogs.message).toHaveBeenCalledWith(expect.stringContaining("disk full"), { title: "Error", kind: "error" }));
    expect(probe.state.startInFlight).toBe(false); // the dialog is still open
    await act(async () => { close(); await started; });
    expect(probe.state.startInFlight).toBe(false);
  });
  // #858 (SHELL-054): Latest Frame recording was impossible in Tauri (parity run S7): nothing could send the acknowledgement.
  it("asks Qt's Latest Frame question before the file dialog and starts with the acknowledgement", async () => {
    const latest = { id: "camera.deliveryMode", status: S.Warn, reason: "Latest Frame intentionally discards frames; the recording may be incomplete", remediation: "" };
    bridge.fetchExperimentReadiness.mockResolvedValue({ ...ready, gates: [latest] });
    bridge.experimentStart.mockResolvedValue({ ok: true, message: "", start_outcome: O.Started });
    dialogs.choose.mockResolvedValue("yes");
    const probe = await mount({ expTab: "preview" });
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(dialogs.choose).toHaveBeenCalledWith(
      "Latest Frame prioritizes low latency and may intentionally discard frames. The recorded sequence may be incomplete.",
      { title: "Start Experiment", kind: "warning", yes: "Continue with Latest Frame", cancel: "Cancel" });
    expect(dialogs.choose.mock.invocationCallOrder[0]).toBeLessThan(dialogs.save.mock.invocationCallOrder[0]);
    expect(bridge.experimentStart).toHaveBeenCalledWith("/d/run.h5", false, undefined, true);
  });
  it("Cancel on the Latest Frame question starts nothing", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue({ ...ready, gates: [{ id: "camera.deliveryMode", status: S.Warn, reason: "r", remediation: "" }] });
    dialogs.choose.mockResolvedValue("cancel");
    const probe = await mount({ expTab: "preview" });
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(dialogs.save).not.toHaveBeenCalled();
    expect(bridge.experimentStart).not.toHaveBeenCalled();
  });
  it("shows every warning of a started run, not only the persistence one", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue({ ...ready, gates: [{ id: "camera.rate", status: S.Warn, reason: "rate cannot be checked", remediation: "" }] });
    bridge.experimentStart.mockResolvedValue({ ok: true, message: "", start_outcome: O.Started });
    const probe = await mount({ expTab: "preview" });
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(probe.state.startNotice).toBe("rate cannot be checked");
  });
});

describe("Stop Experiment (MON-025)", () => {
  it("shows Stopping while the request is out and mirrors the backend's status after it, also after a refusal", async () => {
    let release: (v: unknown) => void = () => undefined;
    bridge.experimentStop.mockReturnValue(new Promise((resolve) => { release = resolve; }));
    const probe = await mount({ expTab: "preview" });
    let pending: Promise<void> = Promise.resolve();
    await act(async () => { pending = probe.lifecycle.onStopExperiment(); });
    expect(probe.state.stopRequested).toBe(true);
    await act(async () => { release({ ok: false, message: "notActive", stop_outcome: 1 }); await pending; });
    expect(probe.state.stopRequested).toBe(false);
    expect(probe.log).toContain("experiment stop failed: notActive");
    expect(bridge.fetchExperimentStatus).toHaveBeenCalled();
    expect(probe.state.expStatus?.state).toBe(EXPERIMENT_STATES.Active);
  });
});

describe("the end of a run (MON-023, App.tsx:1031 at 5f906ac2)", () => {
  const runStatus = (over: Partial<ExperimentStatus> = {}) => status({ output_path: "/d/run.h5", start_generation: "4", ...over });
  const finish = (over: Partial<ExperimentStatus> = {}) => runStatus({
    terminal: true, finalization_ok: true, end_time_ns: "99", completion: C.IncompleteLoss, completion_reason: "undeclared loss: sequenceGaps=2",
    persistence_admitted: "120", persistence_committed: "118", ...over,
  });

  it("shows the accounting modal once for a run this client watched, and logs the saved count", async () => {
    bridge.fetchRunAccounting.mockResolvedValue({ available: true, source: "last_run", start_generation: 4, empty: 1, processed: 100, scientifically_rejected: 2, processing_failed: 0, store_overwritten: 2, store_not_committed: 0, store_malformed: 0 });
    const probe = await mount({ expTab: "preview" });
    await act(async () => probe.state.setExpStatus(runStatus({ state: EXPERIMENT_STATES.Active })));
    await act(async () => probe.state.setExpStatus(finish()));
    await tick(0);
    expect(dialogs.message).toHaveBeenCalledTimes(1);
    expect(dialogs.message.mock.calls[0][0]).toContain("The run is recorded as 'IncompleteLoss': undeclared loss: sequenceGaps=2");
    expect(dialogs.message.mock.calls[0][0]).toContain("Empty 1 · processed 100 · rejected 2 · processing failed 0 · store loss 2 · persisted 118/120");
    expect(dialogs.message.mock.calls[0][1]).toEqual({ title: "Experiment Accounting", kind: "warning" });
    expect(probe.log).toContain("Experiment saved: 118 frames persisted");
    // Another status of the same finished run does not repeat it.
    await act(async () => probe.state.setExpStatus(finish({ fault_revision: "5" })));
    await tick(0);
    expect(dialogs.message).toHaveBeenCalledTimes(1);
  });
  it("opens no modal for a clean run or for a run this client never saw running (a recovered status)", async () => {
    const clean = await mount({ expTab: "preview" });
    await act(async () => clean.state.setExpStatus(runStatus({ state: EXPERIMENT_STATES.Active })));
    await act(async () => clean.state.setExpStatus(finish({ completion: C.Complete, completion_reason: "all reconciled" })));
    await tick(0);
    expect(dialogs.message).not.toHaveBeenCalled();
    await act(async () => root.unmount()); root = createRoot(host);
    const recovered = await mount({ expTab: "preview" });
    await act(async () => recovered.state.setExpStatus(finish()));
    await tick(0);
    expect(dialogs.message).not.toHaveBeenCalled();
    expect(recovered.state.runDismissed).not.toBe("");
  });
  it("says so when the run's accounting cannot be read, instead of dropping the failure", async () => {
    bridge.fetchRunAccounting.mockRejectedValue(new Error("accounting store offline"));
    const probe = await mount({ expTab: "preview" });
    await act(async () => probe.state.setExpStatus(runStatus({ state: EXPERIMENT_STATES.Active })));
    await act(async () => probe.state.setExpStatus(finish({ completion: C.Complete })));
    await tick(0);
    expect(probe.state.accountingError).toContain("accounting store offline");
    expect(probe.log.some((l) => l.includes("run accounting unavailable: Error: accounting store offline"))).toBe(true);
    expect(probe.state.runAccounting).toBeNull();
  });
  it("drops Complete from the run state once the camera state changes after the run", async () => {
    const probe = await mount({ expTab: "preview", running: true });
    await act(async () => probe.state.setExpStatus(runStatus({ state: EXPERIMENT_STATES.Active })));
    await act(async () => probe.state.setExpStatus(finish({ completion: C.Complete })));
    await tick(0);
    expect(probe.state.runDismissed).toBe("");
    await act(async () => root.render(<Harness probe={probe} expTab="preview" running={false} />));
    expect(probe.state.runDismissed).toBe(`4:99`);
  });
});

// Findings of the #877 bench run (handover/tauri-w1-monitoring/BENCH-REPORT.md, B3, B5, B6).
describe("#877 bench findings", () => {
  it("B3: a Start refusal stays while its readiness stands and clears when the readiness changes", async () => {
    const blocked = { valid: true, ready: false, generation: "1", method_calibration: null,
      gates: [{ id: "camera.rate", status: S.Fail, reason: "too fast", remediation: "lower it" }] };
    bridge.fetchExperimentReadiness.mockResolvedValue(blocked);
    const probe = await mount({ expTab: "preview" });
    await tick(0);
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(probe.state.readinessMessage).toBe("camera.rate: too fast — lower it");
    await tick(4100); // two more standing reads, nothing changed
    expect(probe.state.readinessMessage).toBe("camera.rate: too fast — lower it");
    bridge.fetchExperimentReadiness.mockResolvedValue({ ...blocked, ready: true, gates: [] });
    await tick(2100);
    expect(probe.state.readinessMessage).toBe("");
  });
  it("B3: a refusal keeps its alert even when the standing read was stale when Start was pressed", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue({ valid: true, ready: true, generation: "1", gates: [], method_calibration: null });
    const probe = await mount({ expTab: "preview" });
    await tick(0);
    // The cause appears between two standing reads: the flow's own preflight sees it first.
    bridge.fetchExperimentReadiness.mockResolvedValue({ valid: true, ready: false, generation: "2", method_calibration: null,
      gates: [{ id: "storage.buffer", status: S.Fail, reason: "too small", remediation: "" }] });
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    await tick(2100);
    expect(probe.state.readinessMessage).toBe("storage.buffer: too small");
  });
  it("B3: a reason text that only carries a new live figure does not clear the alert", async () => {
    const blocked = (gib: string) => ({ valid: true, ready: false, generation: "1", method_calibration: null,
      gates: [{ id: "storage.buffer", status: S.Fail, reason: `does not fit 75% of the available memory (${gib} GiB)`, remediation: "" }] });
    bridge.fetchExperimentReadiness.mockResolvedValue(blocked("11.9"));
    const probe = await mount({ expTab: "preview" });
    await tick(0);
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(probe.state.readinessMessage).toContain("storage.buffer");
    bridge.fetchExperimentReadiness.mockResolvedValue(blocked("12.0"));
    await tick(2100);
    expect(probe.state.startReadiness?.gates[0].reason).toContain("12.0 GiB");
    expect(probe.state.readinessMessage).toContain("storage.buffer");
  });
  it("a Latest Frame question that cannot be shown is logged, and nothing starts", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue({ valid: true, ready: true, generation: "1", method_calibration: null,
      gates: [{ id: "camera.deliveryMode", status: S.Warn, reason: "r", remediation: "" }] });
    dialogs.choose.mockRejectedValue(new Error("dialog:allow-message denied"));
    const probe = await mount({ expTab: "preview" });
    await act(async () => { await probe.lifecycle.onStartExperiment(); });
    expect(probe.log).toContain("experiment start error: Error: dialog:allow-message denied");
    expect(bridge.experimentStart).not.toHaveBeenCalled();
  });
  it("B5: Starting lasts until the status after the reply is set, so no old end state shows in between", async () => {
    bridge.fetchExperimentReadiness.mockResolvedValue({ valid: true, ready: true, generation: "1", gates: [], method_calibration: null });
    bridge.experimentStart.mockResolvedValue({ ok: true, message: "", start_outcome: O.Started });
    let release: (v: ExperimentStatus) => void = () => undefined;
    bridge.fetchExperimentStatus.mockReturnValue(new Promise<ExperimentStatus>((resolve) => { release = resolve; }));
    const probe = await mount({ expTab: "preview" });
    let pending: Promise<void> = Promise.resolve();
    await act(async () => { pending = probe.lifecycle.onStartExperiment(); });
    await tick(0);
    expect(bridge.experimentStart).toHaveBeenCalled();
    expect(probe.state.startInFlight).toBe(true); // the reply is back, the status is not
    await act(async () => { release(status({ state: EXPERIMENT_STATES.Active, output_path: "/d/run.h5", start_generation: "5" })); await pending; });
    expect(probe.state.startInFlight).toBe(false);
    expect(probe.state.expStatus?.state).toBe(EXPERIMENT_STATES.Active);
  });
  it("B6: no outcome until the run's accounting is back, then the admitted count is the accounting's", async () => {
    let release: (v: unknown) => void = () => undefined;
    bridge.fetchRunAccounting.mockReturnValue(new Promise((resolve) => { release = resolve; }));
    const probe = await mount({ expTab: "preview" });
    await act(async () => probe.state.setExpStatus(status({ output_path: "/d/run.h5", start_generation: "4", state: EXPERIMENT_STATES.Active })));
    await act(async () => probe.state.setExpStatus(status({
      output_path: "/d/run.h5", start_generation: "4", terminal: true, finalization_ok: true, end_time_ns: "99", completion: C.Complete,
      completion_reason: "all reconciled", persistence_admitted: "938", persistence_committed: "938" })));
    await tick(0);
    expect(probe.state.runOutcome).toBeNull();
    await act(async () => { release({ available: true, source: "last_run", start_generation: 4, completion: C.Complete, admitted: "66107" }); });
    await tick(0);
    expect(probe.state.runOutcome?.headline).toBe("Run complete: all 66107 admitted frames reconciled.");
  });
  it("B6: an accounting read that never answers hides the outcome for 5 s at most", async () => {
    bridge.fetchRunAccounting.mockReturnValue(new Promise(() => undefined));
    const probe = await mount({ expTab: "preview" });
    await act(async () => probe.state.setExpStatus(status({
      output_path: "/d/run.h5", start_generation: "4", terminal: true, end_time_ns: "99", completion: C.Complete,
      completion_reason: "all reconciled", persistence_admitted: "938" })));
    await tick(4900);
    expect(probe.state.runOutcome).toBeNull();
    await tick(200);
    expect(probe.state.runOutcome?.headline).toBe("Run complete: all 938 admitted frames reconciled.");
  });
  it("B6: an accounting read cut short by a remount is read again, so the outcome shows", async () => {
    bridge.fetchRunAccounting.mockResolvedValue({ available: true, source: "last_run", start_generation: 4, completion: C.Complete, admitted: "500" });
    const probe = { log: [] } as unknown as Probe;
    const done = status({ output_path: "/d/run.h5", start_generation: "4", terminal: true, end_time_ns: "99", completion: C.Complete, completion_reason: "ok" });
    bridge.fetchExperimentStatus.mockResolvedValue(done);
    // StrictMode mounts, unmounts and mounts again: the first effect's read is dropped, the second must read again.
    const { StrictMode } = await import("react");
    function Recovered() {
      const state = useMonitoringState();
      probe.lifecycle = useExperimentLifecycle({ ...state, append: (l: string) => { probe.log.push(l); }, hostProcessing: true, running: true });
      probe.state = state;
      useState(() => { state.setExpStatus(done); return 0; });
      return null;
    }
    await act(async () => root.render(<StrictMode><Recovered /></StrictMode>));
    await tick(0);
    expect(probe.state.runOutcome?.headline).toBe("Run complete: all 500 admitted frames reconciled.");
  });
});

describe("Clear Buffer's hook point (MON-029)", () => {
  it("calls the registered listeners until they unsubscribe", async () => {
    const probe = await mount({ expTab: "preview" });
    const listener = vi.fn();
    let off = () => undefined as void;
    await act(async () => { off = probe.state.onMonitoringCleared(listener); });
    probe.state.notifyMonitoringCleared();
    expect(listener).toHaveBeenCalledTimes(1);
    off();
    probe.state.notifyMonitoringCleared();
    expect(listener).toHaveBeenCalledTimes(1);
  });
});

describe("the run lock on navigation (MON-013, #877 review)", () => {
  type Tab = "connect" | "overview" | "experiment" | "review";
  let nav: { tab: Tab; setTab: (t: Tab | ((cur: Tab) => Tab)) => void; lock: (on: boolean) => void };
  function Nav() {
    const [tab, rawSetTab] = useState<Tab>("experiment");
    const { setTab, runLockRef } = useRunLockedNavigation(rawSetTab);
    nav = { tab, setTab, lock: (on) => { runLockRef.current = on; } };
    return null;
  }
  it("keeps the stage while a run is locked, for a value and for an updater, and lets go afterwards", async () => {
    await act(async () => root.render(<Nav />));
    nav.lock(true);
    await act(async () => nav.setTab("overview"));
    expect(nav.tab).toBe("experiment");
    await act(async () => nav.setTab(() => "review"));
    expect(nav.tab).toBe("experiment");
    await act(async () => nav.setTab("connect"));
    expect(nav.tab).toBe("connect");
    nav.lock(false);
    await act(async () => nav.setTab("overview"));
    expect(nav.tab).toBe("overview");
  });
});
