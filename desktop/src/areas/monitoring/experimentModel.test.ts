import { describe, expect, it } from "vitest";
import { EXPERIMENT_STATES, READINESS_GATE_STATUSES as S, RUN_COMPLETION_STATES as C } from "../../bridgeContract";
import type { ExperimentStatus } from "../../eventAdapter";
import { POLL_HEALTHY } from "./pollHealth";
import { STAGE_LOCK_REASON } from "./runStatus";
import { experimentModel } from "./useMonitoring";

const status = (over: Partial<ExperimentStatus> = {}) => ({
  valid: true, state: EXPERIMENT_STATES.Idle, flushing: false, terminal: false, cancelled: false, output_path: "/d/run.h5", start_generation: "4",
  end_time_ns: "0", completion: C.Unknown, completion_reason: "", fault_code: "", fault_message: "", persistence_committed: "0", finalization_ok: false, ...over,
}) as ExperimentStatus;
const input = (over: Record<string, unknown> = {}) => ({
  ready: true, instrumentModes: false, running: true, expActive: false, expState: EXPERIMENT_STATES.Idle, expStatus: status(), experimentRequestBusy: false,
  cores: { initialized: true }, runModeBlock: undefined, recording: false, reviewMeta: null, startInFlight: false, stopRequested: false, runDismissed: "",
  startReadiness: null, readinessHealth: POLL_HEALTHY, monitoringIssue: "", snapshotHealth: POLL_HEALTHY, triggerHealth: POLL_HEALTHY, accountingError: "",
  caps: { trigger: true }, ...over,
}) as unknown as Parameters<typeof experimentModel>[0];
const running = { expActive: true, expState: EXPERIMENT_STATES.Active, expStatus: status({ state: EXPERIMENT_STATES.Active }) };

describe("experimentModel (#866)", () => {
  it("keeps the Start Experiment reasons", () => {
    expect(experimentModel(input()).startExperimentReason).toBeUndefined();
    expect(experimentModel(input({ running: false })).startExperimentReason).toBe("Camera must be running before starting an experiment");
    expect(experimentModel(input(running)).startExperimentReason).toBe("Experiment is already running");
  });

  it("locks the stages, Stop Camera and Record for the whole run, with Qt's reasons", () => {
    const idle = experimentModel(input());
    expect(idle).toMatchObject({ runLocked: false, stopCameraBlock: undefined, recordReason: undefined });
    const run = experimentModel(input(running));
    expect(run.runLocked).toBe(true);
    expect(run.runPresentation).toMatchObject({ phase: "running", text: "Running: run.h5" });
    expect(run.stopCameraBlock).toBe("Cannot stop camera while an experiment is active. Stop the experiment first.");
    expect(run.recordReason).toBe("Stop or reset the experiment and close the existing HDF5 file before recording.");
    expect(STAGE_LOCK_REASON).toContain("Locked while an experiment runs");
  });

  it("stays locked while the file is being saved, and refuses Stop Camera for the save", () => {
    const saving = experimentModel(input({ expActive: true, expState: EXPERIMENT_STATES.Stopping, expStatus: status({ state: EXPERIMENT_STATES.Stopping, flushing: true }) }));
    expect(saving).toMatchObject({ runLocked: true, runProgress: "Saving experiment data…" });
    const failedSaving = experimentModel(input({ expActive: false, expState: EXPERIMENT_STATES.Failed, expStatus: status({ state: EXPERIMENT_STATES.Failed, fault_code: "experiment.saveFailed" }) }));
    expect(failedSaving.runLocked).toBe(true);
  });

  it("refuses Stop Camera while raw recording, and Record while a file is open in Review", () => {
    expect(experimentModel(input({ recording: true })).stopCameraBlock).toBe("Cannot stop camera while frame recording is active. Stop recording first.");
    expect(experimentModel(input({ recording: true })).recordReason).toBeUndefined();
    expect(experimentModel(input({ reviewMeta: { file_open: true } })).recordReason).toBe("Close the HDF5 file open in Review before recording.");
  });

  it("summarizes the standing readiness only once the backend answered", () => {
    expect(experimentModel(input()).readinessSummary).toBeNull();
    const gates = [{ id: "camera.rate", status: S.Fail, reason: "too fast", remediation: "" }, { id: "storage.output", status: S.Unavailable, reason: "no path" }];
    expect(experimentModel(input({ startReadiness: { valid: true, ready: false, gates } })).readinessSummary).toMatchObject({ tone: "blocked", line: "Blocked (1)" });
    expect(experimentModel(input({ startReadiness: { valid: false, ready: false, gates: [] } })).readinessSummary).toBeNull();
  });

  it("collects the Monitoring notices, and leaves the trigger out where the instrument has no trigger", () => {
    const failing = { failures: 3, lastError: "gone" };
    const m = experimentModel(input({ monitoringIssue: "Monitoring could not be switched on", snapshotHealth: failing, triggerHealth: failing }));
    expect(m.monitoringNotices).toEqual([
      "Monitoring could not be switched on",
      "Monitoring data could not be read from the backend (3 attempts in a row): gone",
      "Trigger status could not be read from the backend (3 attempts in a row): gone",
    ]);
    expect(experimentModel(input({ triggerHealth: failing, caps: { trigger: false } })).monitoringNotices).toEqual([]);
    expect(experimentModel(input({ snapshotHealth: { failures: 2, lastError: "x" } })).monitoringNotices).toEqual([]);
  });
});
