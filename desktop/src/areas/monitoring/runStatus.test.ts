import { describe, expect, it } from "vitest";
import { EXPERIMENT_STATES, RUN_COMPLETION_STATES } from "../../bridgeContract";
import type { ExperimentStatus } from "../../eventAdapter";
import { runKey } from "../../runOutcome";
import {
  RUN_PHASE_GLYPH, RUN_PHASE_LABEL, cameraStopBlock, deriveRunPresentation, failureReason, recordBlock, runProgressText, isRunLockedTab, runLockedTabChange,
} from "./runStatus";

const status = (over: Partial<ExperimentStatus> = {}): ExperimentStatus => ({
  valid: true, state: EXPERIMENT_STATES.Idle, start_time_ns: "0", end_time_ns: "0", valid_buffered: "0", invalid_buffered: "0",
  valid_saved: "0", invalid_saved: "0", dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false,
  output_path: "", message: "", start_generation: "0", readiness_generation: "0", capture_generation: "0",
  persistence_admitted: "0", persistence_committed: "0", persistence_failed: "0", terminal: false, finalization_ok: false,
  completion: RUN_COMPLETION_STATES.Unknown, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "", ...over,
});
const active = (over: Partial<ExperimentStatus> = {}) => status({ state: EXPERIMENT_STATES.Active, output_path: "/data/run.h5", start_generation: "4", ...over });
const finished = (over: Partial<ExperimentStatus> = {}) => status({
  state: EXPERIMENT_STATES.Idle, terminal: true, finalization_ok: true, output_path: "/data/run.h5", start_generation: "4", end_time_ns: "99",
  persistence_admitted: "120", persistence_committed: "120", completion: RUN_COMPLETION_STATES.Complete, ...over,
});

describe("the run phases (Qt RunStatusModel)", () => {
  it("uses Qt's glyphs and words for every phase", () => {
    expect(RUN_PHASE_LABEL).toEqual({
      idle: "Idle", cameraRunning: "Camera running", starting: "Starting", running: "Running", stopping: "Stopping",
      saving: "Saving", complete: "Complete", failed: "Failed – recovery required",
    });
    expect(Object.values(RUN_PHASE_GLYPH)).toEqual(["○", "◉", "◔", "●", "◑", "⇩", "✔", "✕"]);
  });

  it("is Idle with no run and Camera running with the camera on", () => {
    expect(deriveRunPresentation({ cameraRunning: false, status: null })).toMatchObject({ phase: "idle", text: "Idle", glyph: "○", locked: false });
    expect(deriveRunPresentation({ cameraRunning: true, status: status() })).toMatchObject({ phase: "cameraRunning", text: "Camera running", glyph: "◉" });
    expect(deriveRunPresentation({ cameraRunning: true, status: status({ valid: false }) }).phase).toBe("cameraRunning");
  });

  it("follows the backend through Starting, Running, Stopping and Saving with the file name", () => {
    const at = (s: ExperimentStatus, extra = {}) => deriveRunPresentation({ cameraRunning: true, status: s, ...extra });
    expect(at(status({ state: EXPERIMENT_STATES.Starting, output_path: "/data/run.h5" }))).toMatchObject({ phase: "starting", text: "Starting: run.h5", locked: true });
    expect(at(active())).toMatchObject({ phase: "running", text: "Running: run.h5", tone: "ok", locked: true, title: "Run state: Running: run.h5 (running)" });
    // Stop pressed, the backend has not said so yet.
    expect(at(active(), { stopRequested: true })).toMatchObject({ phase: "stopping", text: "Stopping: run.h5", locked: true });
    // The worker drains the write queue and closes the file.
    expect(at(active({ state: EXPERIMENT_STATES.Stopping, flushing: true }))).toMatchObject({ phase: "saving", text: "Saving: run.h5", locked: true });
  });

  it("shows Starting while a start request is in flight, without locking anything yet", () => {
    expect(deriveRunPresentation({ cameraRunning: true, status: status(), startInFlight: true })).toMatchObject({ phase: "starting", locked: false });
    expect(deriveRunPresentation({ cameraRunning: true, status: null, startInFlight: true }).phase).toBe("starting");
  });

  it("ends in Complete for a clean run and in Failed - recovery required for a run that lost data (Qt latchFailure)", () => {
    expect(deriveRunPresentation({ cameraRunning: true, status: finished() })).toMatchObject({ phase: "complete", text: "Complete: run.h5", glyph: "✔", locked: false });
    expect(deriveRunPresentation({ cameraRunning: true, status: finished({ completion: RUN_COMPLETION_STATES.IntentionallyPartial }) }).phase).toBe("complete");
    expect(deriveRunPresentation({ cameraRunning: true, status: finished({ completion: RUN_COMPLETION_STATES.IncompleteLoss }) }))
      .toMatchObject({ phase: "failed", text: "Failed – recovery required: IncompleteLoss", tone: "err", glyph: "✕", locked: false });
    expect(deriveRunPresentation({ cameraRunning: true, status: finished({ completion: RUN_COMPLETION_STATES.Failed }) }).detail).toBe("Failed");
  });

  it("latches a save fault first, whatever the completion says", () => {
    expect(failureReason(finished({ fault_code: "experiment.flushFailed" }))).toBe("flush failed");
    expect(failureReason(finished({ fault_code: "experiment.provenanceFailed" }))).toBe("metadata/provenance write failed");
    expect(failureReason(finished({ fault_code: "experiment.saveFailed", fault_message: "disk full" }))).toBe("disk full");
    expect(failureReason(finished())).toBe("");
    const o = deriveRunPresentation({ cameraRunning: true, status: finished({ state: EXPERIMENT_STATES.Failed, fault_code: "experiment.saveFailed", fault_message: "disk full" }) });
    expect(o).toMatchObject({ phase: "failed", detail: "disk full" });
  });

  it("is Failed and still locked while a save error is being finalized, and unlocks at the terminal status", () => {
    const saving = status({ state: EXPERIMENT_STATES.Failed, output_path: "/data/run.h5", fault_code: "experiment.saveFailed", fault_message: "disk full" });
    expect(deriveRunPresentation({ cameraRunning: true, status: saving })).toMatchObject({ phase: "failed", detail: "disk full", locked: true });
    expect(deriveRunPresentation({ cameraRunning: true, status: { ...saving, terminal: true } }).locked).toBe(false);
  });

  it("drops Complete once the operator moved past the run, but never a failure", () => {
    const done = finished();
    expect(deriveRunPresentation({ cameraRunning: true, status: done, dismissedRun: runKey(done) }).phase).toBe("cameraRunning");
    expect(deriveRunPresentation({ cameraRunning: false, status: done, dismissedRun: runKey(done) }).phase).toBe("idle");
    const lost = finished({ completion: RUN_COMPLETION_STATES.IncompleteLoss });
    expect(deriveRunPresentation({ cameraRunning: true, status: lost, dismissedRun: runKey(lost) }).phase).toBe("failed");
  });

  it("does not present a cancelled run as an outcome", () => {
    expect(deriveRunPresentation({ cameraRunning: true, status: finished({ cancelled: true }) }).phase).toBe("cameraRunning");
  });
});

describe("what a run locks", () => {
  it("refuses Stop Camera with Qt's reasons, in Qt's order", () => {
    expect(cameraStopBlock({ expActive: true, flushing: true, recording: true })).toBe("Cannot stop camera while an experiment is active. Stop the experiment first.");
    expect(cameraStopBlock({ expActive: false, flushing: true, recording: true })).toBe("Cannot stop camera while experiment data is being saved. Wait for the save to finish.");
    expect(cameraStopBlock({ expActive: false, flushing: false, recording: true })).toBe("Cannot stop camera while frame recording is active. Stop recording first.");
    expect(cameraStopBlock({ expActive: false, flushing: false, recording: false })).toBeUndefined();
  });

  it("disables Raw Record while an experiment is not idle or an HDF5 file is open, but never Stop Recording", () => {
    expect(recordBlock({ recording: false, experimentIdle: false, hdf5Open: false })).toBe("Stop or reset the experiment and close the existing HDF5 file before recording.");
    expect(recordBlock({ recording: false, experimentIdle: true, hdf5Open: true })).toBe("Close the HDF5 file open in Review before recording.");
    expect(recordBlock({ recording: false, experimentIdle: true, hdf5Open: false })).toBeUndefined();
    expect(recordBlock({ recording: true, experimentIdle: false, hdf5Open: true })).toBeUndefined();
  });

  it("keeps the current stage when a navigation asks for a shut one during the run (MON-013, #877 review)", () => {
    expect(isRunLockedTab("overview")).toBe(true);
    expect(isRunLockedTab("review")).toBe(true);
    expect(isRunLockedTab("experiment")).toBe(false);
    expect(isRunLockedTab(undefined)).toBe(false);
    expect(runLockedTabChange("experiment", "overview", true)).toBe("experiment");
    expect(runLockedTabChange("experiment", "review", true)).toBe("experiment");
    expect(runLockedTabChange("experiment", "connect", true)).toBe("connect");
    expect(runLockedTabChange("overview", "overview", true)).toBe("overview"); // a run started elsewhere: stay put
    expect(runLockedTabChange("experiment", "overview", false)).toBe("overview");
  });
});

describe("stop and finalization progress (Qt status label)", () => {
  const text = (s: ExperimentStatus | null, extra = {}) => runProgressText(deriveRunPresentation({ cameraRunning: true, status: s, ...extra }), s);
  it("says what is happening while the run stops and is saved", () => {
    expect(text(active(), { stopRequested: true })).toBe("Stopping experiment…");
    expect(text(active({ state: EXPERIMENT_STATES.Stopping }))).toBe("Saving experiment data…");
    expect(text(status({ state: EXPERIMENT_STATES.Failed, fault_code: "experiment.saveFailed" }))).toBe("Saving experiment data after a save error…");
  });
  it("ends with the persisted frame count, or with why the save is incomplete", () => {
    expect(text(finished())).toBe("Experiment saved: 120 frames persisted");
    expect(text(finished({ finalization_ok: false, completion: RUN_COMPLETION_STATES.Failed, completion_reason: "metadata write failed" }))).toBe("Experiment save incomplete: metadata write failed");
  });
  it("is silent while idle, running or after a cancelled run", () => {
    expect(text(null)).toBe("");
    expect(text(active())).toBe("");
    expect(text(finished({ cancelled: true }))).toBe("");
  });
});
