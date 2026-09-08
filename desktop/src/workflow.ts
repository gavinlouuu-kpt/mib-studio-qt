// Guided four-stage operator workflow (UX-1, issue #305 / epic #304).
//
// The Qt-parity shell (#266) exposes Connect / Overview / Experiment / Review
// as *locations*. This module layers an authoritative *stage state* on top of
// them: each stage is Not started / Needs attention / Ready / Running /
// Complete, derived purely from backend facts plus explicit operator
// confirmation — never from the fact that the operator visited a tab.
//
// Design rules baked in here (UX-1 acceptance criteria):
//   - Camera detection/connection alone never completes Preflight; the
//     operator must explicitly confirm hardware readiness, and that
//     confirmation is invalidated when the device or processing core changes.
//   - deriveWorkflow() takes no notion of the active tab, so visiting a stage
//     can never mark it complete.
//   - A blocked stage carries the specific failing checks and the recovery
//     hint, so the UI can explain *why* and *what next*.
//
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.
import { EXPERIMENT_STATES } from "./bridgeContract";

export type StageId = "preflight" | "alignment" | "experiment" | "review";

/** The existing shell tab a stage maps onto. */
export type StageTab = "connect" | "overview" | "experiment" | "review";

export type StageStatus =
  | "not-started"
  | "needs-attention"
  | "ready"
  | "running"
  | "complete";

export const STAGE_STATUS_LABEL: Readonly<Record<StageStatus, string>> = {
  "not-started": "Not started",
  "needs-attention": "Needs attention",
  ready: "Ready",
  running: "Running",
  complete: "Complete",
};

/** Backend facts the workflow is derived from. Every field is an authoritative
 *  snapshot value the shell already polls — this module invents no state. */
export interface WorkflowFacts {
  backendReady: boolean;
  cameraConfigured: boolean;
  cameraRunning: boolean;
  /** Stable identity of the selected device + core the operator would be
   *  confirming (e.g. "mock|Demo|core:2.1.0"); "" when nothing is selected. */
  preflightSignature: string;
  /** Signature the operator last confirmed Preflight against; "" if never. */
  preflightConfirmedFor: string;
  /** Identity the operator would be confirming alignment against. */
  alignmentSignature: string;
  /** Signature the operator last confirmed alignment against; "" if never. */
  alignmentConfirmedFor: string;
  coreValid: boolean;
  corePinSatisfied: boolean;
  requiredCoreVersion: string;
  /** A contract EXPERIMENT_STATES value. */
  experimentState: number;
  /** True once a run finished this session with saved output (not cancelled). */
  experimentCompleted: boolean;
  reviewFileOpen: boolean;
  reviewValid: boolean;
}

export interface StageView {
  id: StageId;
  tab: StageTab;
  title: string;
  status: StageStatus;
  statusLabel: string;
  /** Short human-readable summary of the stage's current state. */
  summary: string;
  /** Specific failing checks / recovery hints for a blocked or attention
   *  stage. Empty when the stage is clear. */
  blocking: string[];
}

export type NextActionKind =
  | "navigate"
  | "confirm-preflight"
  | "confirm-alignment";

export interface NextAction {
  stageId: StageId;
  tab: StageTab;
  /** Imperative label, e.g. "Confirm hardware preflight". */
  label: string;
  kind: NextActionKind;
}

export interface WorkflowView {
  stages: StageView[];
  /** Earliest stage that is not yet Complete (a Running stage counts as the
   *  current one). Falls back to Review when everything is Complete. */
  currentStageId: StageId;
  /** The single safe next action, or null when the workflow is complete. */
  recommended: NextAction | null;
}

function confirmed(signature: string, confirmedFor: string): boolean {
  return signature !== "" && signature === confirmedFor;
}

function isExperimentActive(state: number): boolean {
  return (
    state === EXPERIMENT_STATES.Starting ||
    state === EXPERIMENT_STATES.Active ||
    state === EXPERIMENT_STATES.Stopping
  );
}

function derivePreflight(f: WorkflowFacts): StageView {
  const blocking: string[] = [];
  let status: StageStatus;

  if (!f.backendReady) {
    status = "not-started";
    blocking.push("Backend is not initialized");
  } else if (!f.cameraConfigured) {
    status = "not-started";
    blocking.push("No camera configured — select a device in the Connect tab");
  } else {
    if (!f.coreValid) {
      blocking.push("Processing core identity is unavailable");
    } else if (!f.corePinSatisfied) {
      blocking.push(
        `Processing core pin is not satisfied (requires ${f.requiredCoreVersion || "a pinned version"})`,
      );
    }
    if (blocking.length > 0) {
      status = "needs-attention";
    } else if (confirmed(f.preflightSignature, f.preflightConfirmedFor)) {
      status = "complete";
    } else {
      // Critical checks pass but the operator has not confirmed readiness.
      // Detection alone is deliberately not enough to complete Preflight.
      status = "ready";
    }
  }

  const summary =
    status === "complete"
      ? "Hardware confirmed healthy and ready"
      : status === "ready"
        ? "Checks pass — confirm the hardware is ready"
        : blocking[0] ?? "Verify required hardware";

  return {
    id: "preflight",
    tab: "connect",
    title: "Hardware Preflight",
    status,
    statusLabel: STAGE_STATUS_LABEL[status],
    summary,
    blocking,
  };
}

function deriveAlignment(f: WorkflowFacts, preflight: StageView): StageView {
  const blocking: string[] = [];
  let status: StageStatus;

  if (preflight.status !== "complete") {
    status = "not-started";
    blocking.push("Complete Hardware Preflight first");
  } else if (isExperimentActive(f.experimentState)) {
    // Alignment is locked in while a run is active.
    status = "complete";
  } else if (!f.cameraRunning) {
    status = "needs-attention";
    blocking.push("Start the camera to view the live image");
  } else if (confirmed(f.alignmentSignature, f.alignmentConfirmedFor)) {
    status = "complete";
  } else {
    status = "ready";
  }

  const summary =
    status === "complete"
      ? "Image, focus and ROI confirmed"
      : status === "ready"
        ? "Live image available — confirm focus, alignment and ROI"
        : blocking[0] ?? "Align the camera and ROI";

  return {
    id: "alignment",
    tab: "overview",
    title: "Camera & Alignment",
    status,
    statusLabel: STAGE_STATUS_LABEL[status],
    summary,
    blocking,
  };
}

function deriveExperiment(f: WorkflowFacts, alignment: StageView): StageView {
  const blocking: string[] = [];
  let status: StageStatus;

  if (isExperimentActive(f.experimentState)) {
    status = "running";
  } else if (alignment.status !== "complete") {
    status = "not-started";
    blocking.push("Complete Camera & Alignment first");
  } else if (f.experimentState === EXPERIMENT_STATES.Failed) {
    status = "needs-attention";
    blocking.push("The previous experiment failed — review the error and retry");
  } else if (f.experimentCompleted) {
    status = "complete";
  } else if (!f.cameraRunning) {
    status = "needs-attention";
    blocking.push("Start the camera before running an experiment");
  } else {
    status = "ready";
  }

  const summary =
    status === "running"
      ? "Experiment is running"
      : status === "complete"
        ? "Experiment finished and data was saved"
        : status === "ready"
          ? "Ready to start the experiment"
          : blocking[0] ?? "Set up and run the experiment";

  return {
    id: "experiment",
    tab: "experiment",
    title: "Experiment",
    status,
    statusLabel: STAGE_STATUS_LABEL[status],
    summary,
    blocking,
  };
}

function deriveReview(f: WorkflowFacts, experiment: StageView): StageView {
  const blocking: string[] = [];
  let status: StageStatus;

  if (f.reviewFileOpen && f.reviewValid) {
    status = "complete";
  } else if (f.reviewFileOpen && !f.reviewValid) {
    status = "needs-attention";
    blocking.push("The opened file could not be read — open a valid recording");
  } else if (experiment.status === "complete") {
    status = "ready";
  } else if (experiment.status === "running") {
    status = "not-started";
    blocking.push("Available after the experiment finishes");
  } else {
    status = "not-started";
    blocking.push("Run an experiment or open a recording to review");
  }

  const summary =
    status === "complete"
      ? "Reviewing recorded data"
      : status === "ready"
        ? "Results are ready to review"
        : blocking[0] ?? "Review recorded data";

  return {
    id: "review",
    tab: "review",
    title: "Review",
    status,
    statusLabel: STAGE_STATUS_LABEL[status],
    summary,
    blocking,
  };
}

function recommendedFor(stage: StageView, f: WorkflowFacts): NextAction | null {
  switch (stage.id) {
    case "preflight":
      if (stage.status === "ready") {
        return {
          stageId: "preflight",
          tab: "connect",
          label: "Confirm hardware preflight",
          kind: "confirm-preflight",
        };
      }
      return {
        stageId: "preflight",
        tab: "connect",
        label: f.cameraConfigured ? "Resolve hardware preflight" : "Select a camera",
        kind: "navigate",
      };
    case "alignment":
      if (stage.status === "ready") {
        return {
          stageId: "alignment",
          tab: "overview",
          label: "Confirm alignment & ROI",
          kind: "confirm-alignment",
        };
      }
      return {
        stageId: "alignment",
        tab: "overview",
        label: f.cameraRunning ? "Adjust camera & alignment" : "Start the camera",
        kind: "navigate",
      };
    case "experiment":
      return {
        stageId: "experiment",
        tab: "experiment",
        label:
          stage.status === "running"
            ? "Monitor the running experiment"
            : stage.status === "ready"
              ? "Start the experiment"
              : "Set up the experiment",
        kind: "navigate",
      };
    case "review":
      return {
        stageId: "review",
        tab: "review",
        label: "Review results",
        kind: "navigate",
      };
  }
}

/** Derive the full workflow view from authoritative backend facts. */
export function deriveWorkflow(f: WorkflowFacts): WorkflowView {
  const preflight = derivePreflight(f);
  const alignment = deriveAlignment(f, preflight);
  const experiment = deriveExperiment(f, alignment);
  const review = deriveReview(f, experiment);
  const stages = [preflight, alignment, experiment, review];

  // Current stage: a Running stage is where the operator is; otherwise the
  // earliest stage that is not yet Complete. Everything complete → Review.
  const running = stages.find((s) => s.status === "running");
  const firstIncomplete = stages.find((s) => s.status !== "complete");
  const current = running ?? firstIncomplete ?? review;

  const allComplete = stages.every((s) => s.status === "complete");
  const recommended = allComplete ? null : recommendedFor(current, f);

  return { stages, currentStageId: current.id, recommended };
}
