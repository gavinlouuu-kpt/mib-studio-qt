// The run state the operator sees, and what it locks (epic #866: MON-012, 013, 014, 022, 025; also SHELL-059).
//
// Qt keeps one RunStatusModel (models/RunStatusModel.cpp) and projects it as a glyph + text widget in the
// Experiment tab corner. The phase is derived here from the backend's experiment status instead of being
// driven by the UI, so another client's run or a recovered session shows the same state. Pure: no React.
import { EXPERIMENT_STATES, RUN_COMPLETION_STATES } from "../../bridgeContract";
import type { ExperimentStatus } from "../../eventAdapter";
import { runKey } from "../../runOutcome";

export type RunPhase = "idle" | "cameraRunning" | "starting" | "running" | "stopping" | "saving" | "complete" | "failed";

/** Qt runPhaseGlyph: the glyph is never the only cue, the text always says it too. */
export const RUN_PHASE_GLYPH: Record<RunPhase, string> = {
  idle: "○", cameraRunning: "◉", starting: "◔", running: "●", stopping: "◑", saving: "⇩", complete: "✔", failed: "✕",
};

/** Qt runPhaseLabel. */
export const RUN_PHASE_LABEL: Record<RunPhase, string> = {
  idle: "Idle", cameraRunning: "Camera running", starting: "Starting", running: "Running", stopping: "Stopping",
  saving: "Saving", complete: "Complete", failed: "Failed – recovery required",
};

export type RunTone = "neutral" | "ok" | "warn" | "err";
const RUN_PHASE_TONE: Record<RunPhase, RunTone> = {
  idle: "neutral", cameraRunning: "neutral", starting: "warn", running: "ok", stopping: "warn", saving: "warn", complete: "neutral", failed: "err",
};

export interface RunPresentation {
  phase: RunPhase;
  glyph: string;
  /** "Running: run.h5" (Qt RunPresentationState::text). */
  text: string;
  detail: string;
  tone: RunTone;
  /** The tooltip: "Run state: <text> (<phase>)". */
  title: string;
  /** Starting, Running, Stopping or Saving (also a failure still being saved): the stage tabs, Raw Record and
   *  Stop Camera are locked (Qt `experimentActive_`, which stays set until the terminal status). */
  locked: boolean;
}

export interface RunStatusInput {
  cameraRunning: boolean;
  status: ExperimentStatus | null;
  /** A start request is in flight (the file is chosen and the backend is opening it). */
  startInFlight?: boolean;
  /** Stop was pressed and the backend has not yet reported Stopping. */
  stopRequested?: boolean;
  /** A finished run the operator moved past (the camera started or stopped after it): Qt drops Complete then. */
  dismissedRun?: string;
}

const FAULT_LATCHES: Record<string, string> = {
  "experiment.flushFailed": "flush failed",
  "experiment.provenanceFailed": "metadata/provenance write failed",
};

function baseName(path: string): string {
  return path.split(/[\\/]/).filter(Boolean).pop() ?? "";
}

/** Why Qt latches a finished run as Failed, or "" when it ended well. Same order as MainWindow::onExperimentStatus:
 *  a save fault first, then a completion that is not Complete or IntentionallyPartial. */
export function failureReason(s: ExperimentStatus): string {
  if (FAULT_LATCHES[s.fault_code]) return FAULT_LATCHES[s.fault_code];
  if (s.fault_code === "experiment.saveFailed") return s.fault_message || "save failed";
  if (s.completion === RUN_COMPLETION_STATES.Complete || s.completion === RUN_COMPLETION_STATES.IntentionallyPartial) return "";
  return Object.entries(RUN_COMPLETION_STATES).find(([, v]) => v === s.completion)?.[0] ?? "Unknown";
}

function present(phase: RunPhase, detail: string, locked: boolean): RunPresentation {
  const label = RUN_PHASE_LABEL[phase];
  const text = detail ? `${label}: ${detail}` : label;
  return { phase, glyph: RUN_PHASE_GLYPH[phase], text, detail, tone: RUN_PHASE_TONE[phase], title: `Run state: ${text} (${phase})`, locked };
}

export function deriveRunPresentation({ cameraRunning, status, startInFlight, stopRequested, dismissedRun }: RunStatusInput): RunPresentation {
  const idle = () => present(cameraRunning ? "cameraRunning" : "idle", "", false);
  if (!status || !status.valid) return startInFlight ? present("starting", "", false) : idle();
  const file = baseName(status.output_path);
  switch (status.state) {
    case EXPERIMENT_STATES.Starting: return present("starting", file, true);
    case EXPERIMENT_STATES.Active: return stopRequested ? present("stopping", file, true) : present("running", file, true);
    case EXPERIMENT_STATES.Stopping:
      // The coordinator's worker is draining the write queue and closing the file: Qt shows Saving from here.
      return present("saving", file, true);
    case EXPERIMENT_STATES.Failed:
      // A save error: the run is still being finalized (Qt latches Failed while saving) until the terminal status.
      if (!status.terminal) return present("failed", status.fault_message || failureReason(status), true);
      break;
  }
  if (startInFlight) return present("starting", "", false);
  if (status.terminal && !status.cancelled) {
    const why = status.state === EXPERIMENT_STATES.Failed ? failureReason(status) || status.fault_message || "failed" : failureReason(status);
    if (why) return present("failed", why, false);
    if (runKey(status) !== dismissedRun) return present("complete", file, false);
  }
  return idle();
}

// ---- What a run locks ------------------------------------------------------------------------------

/** Qt MainWindow.cpp:1150-1158 (#585): the Overview and Review stages are shut for the whole run. */
export const STAGE_LOCK_REASON = "Locked while an experiment runs or is being saved. Stop the experiment and wait for the save to finish.";

/** Why Stop Camera is refused, or undefined (Qt CameraController operation guard, MainWindow.cpp:437-451). */
export function cameraStopBlock({ expActive, flushing, recording }: { expActive: boolean; flushing: boolean; recording: boolean }): string | undefined {
  if (expActive) return "Cannot stop camera while an experiment is active. Stop the experiment first.";
  if (flushing) return "Cannot stop camera while experiment data is being saved. Wait for the save to finish.";
  if (recording) return "Cannot stop camera while frame recording is active. Stop recording first.";
  return undefined;
}

/** Why Raw Record is disabled, or undefined (Qt PlaybackPanel::updateRecordingUI, #538): an experiment that is not
 *  idle or an open HDF5 file would be written to by two owners. While recording, Stop Recording stays available. */
export function recordBlock({ recording, experimentIdle, hdf5Open }: { recording: boolean; experimentIdle: boolean; hdf5Open: boolean }): string | undefined {
  if (recording) return undefined;
  if (!experimentIdle) return "Stop or reset the experiment and close the existing HDF5 file before recording.";
  if (hdf5Open) return "Close the HDF5 file open in Review before recording.";
  return undefined;
}

// ---- Stop and finalization progress (MON-025) ----------------------------------------------------------

/** The sentence Qt puts in the status label while a run stops and is saved. "" when there is nothing to say. */
export function runProgressText(p: RunPresentation, status: ExperimentStatus | null): string {
  switch (p.phase) {
    case "stopping": return "Stopping experiment…";
    case "saving": return "Saving experiment data…";
    case "failed":
      if (p.locked) return "Saving experiment data after a save error…";
      break;
    case "complete": break;
    default: return "";
  }
  if (!status?.valid || !status.terminal || status.cancelled) return "";
  return status.finalization_ok
    ? `Experiment saved: ${status.persistence_committed} frames persisted`
    : `Experiment save incomplete: ${status.completion_reason}`;
}
