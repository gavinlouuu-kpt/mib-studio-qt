// Shell (epic #862): the UX-1 guided workflow (stage status, the recommended next action, the
// startup landing) and the UX-8 active-context bar. Moved from App.tsx without change (#869 wave 0).
import { useEffect } from "react";
import { EXPERIMENT_STATES } from "../../bridgeContract";
import { deriveContextBar, type ContextBarFacts } from "../../contextBar";
import { deriveWorkflow, type StageTab, type WorkflowFacts } from "../../workflow";
import type { ConfigDocumentsModel, ConfigState } from "../config/useConfig";
import type { ConnectState, ConnectModel } from "../connect/useConnect";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { ReviewState } from "../review/useReview";
import type { ShellState } from "./useShell";

type WorkflowDeps = Pick<ShellState, "ready" | "preflightConfirmedFor" | "alignmentConfirmedFor" | "instrumentModes" | "instrument">
  & Pick<ConnectState, "camSelection" | "running"> & Pick<ConfigState, "coreStatus" | "stats">
  & Pick<MonitoringState, "expState" | "expActive" | "expStatus"> & Pick<ReviewState, "reviewMeta">
  & Pick<ConnectModel, "cameraConfigured" | "preflight" | "quality"> & Pick<ConfigDocumentsModel, "profiles">;

// Render-time values: confirmation signatures, the workflow stages and the context bar.
export function shellWorkflow({
  ready, preflightConfirmedFor, alignmentConfirmedFor, instrumentModes, instrument, camSelection, running, coreStatus,
  stats, expState, expActive, expStatus, reviewMeta, cameraConfigured, preflight, quality, profiles,
}: WorkflowDeps) {
  // ---- UX-1 guided four-stage workflow (issue #305) ----
  // Signatures the operator confirms against: change the device or the pinned
  // processing core and the prior confirmation no longer matches.
  const deviceIdentity = cameraConfigured
    ? `${camSelection?.mode ?? ""}|${camSelection?.label ?? ""}`
    : "";
  const preflightSignature = cameraConfigured
    ? `${deviceIdentity}|core:${coreStatus?.valid ? coreStatus.active_version : ""}`
    : "";
  const alignmentSignature = deviceIdentity;
  // Saved rows plus Idle do not prove successful finalization. The accepted
  // backend must supply a retained exact terminal outcome before showing Complete.
  const experimentCompleted = false;

  const workflowFacts: WorkflowFacts = {
    backendReady: ready,
    cameraConfigured,
    cameraRunning: running,
    preflightSignature,
    preflightConfirmedFor,
    alignmentSignature,
    alignmentConfirmedFor,
    coreValid: coreStatus?.valid ?? false,
    corePinSatisfied: coreStatus?.pin_satisfied ?? false,
    requiredCoreVersion: coreStatus?.required_version ?? "",
    // The checklist's own rule ("Required checks must pass before Preflight can be confirmed"),
    // so a failing required check (PL core, sensor link, LED strobe) cannot be confirmed (#548).
    requiredFailures: preflight.checks
      .filter((c) => c.requirement === "required" && c.status !== "passed")
      .map((c) => `${c.label}: ${c.detail}`),
    experimentState: expState,
    experimentCompleted,
    reviewFileOpen: reviewMeta?.file_open ?? false,
    reviewValid: reviewMeta?.valid ?? false,
    // The PZ7035's camera mode (G15): Run and Align need no Preflight confirmation and the stepper follows the mode.
    instrumentMode: instrumentModes ? instrument?.mode?.name ?? "unknown" : undefined,
  };
  const workflow = deriveWorkflow(workflowFacts);
  const stageByTab = Object.fromEntries(workflow.stages.map((s) => [s.tab, s])) as Record<
    StageTab,
    (typeof workflow.stages)[number]
  >;
  const currentStage = workflow.stages.find((s) => s.id === workflow.currentStageId)!;
  // A confirmation cannot be applied while a run is active (setup is locked).
  const recNeedsConfirm = !!workflow.recommended && workflow.recommended.kind !== "navigate";
  const recDisabled = recNeedsConfirm && expActive;

  // ---- UX-8 persistent active-context bar (issue #312) ----
  // Warnings = unresolved attention items surfaced by preflight + quality.
  const warningsCount = preflight.failed + preflight.warning + quality.warn + quality.fail;
  const contextFacts: ContextBarFacts = {
    profileName: profiles.activeProfile?.profile_id || profiles.activeProfile?.name || "",
    cameraConfigured,
    cameraRunning: running,
    cameraLabel: camSelection?.label || (camSelection?.mode === 1 ? "Mock camera" : ""),
    pixelToMicron: stats?.pixel_to_micron ?? NaN,
    currentStageTitle: currentStage.title,
    currentStageStatus: currentStage.status,
    allStagesComplete: workflow.recommended === null,
    experimentActive: expActive,
    experimentFailed: expState === EXPERIMENT_STATES.Failed,
    operatorName: "", // operator identity not captured yet
    outputPath: expStatus?.output_path ?? "",
    warningsCount,
    storageWarning: instrument?.storage?.warning ?? "",
  };
  const contextBar = deriveContextBar(contextFacts);
  return {
    preflightSignature, alignmentSignature, workflow, stageByTab, currentStage, recNeedsConfirm, recDisabled,
    warningsCount, contextBar,
  };
}
export type ShellWorkflow = ReturnType<typeof shellWorkflow>;

type LandingDeps = Pick<ShellState, "ready" | "didInitStage" | "setTab" | "setPreflightConfirmedFor" | "setAlignmentConfirmedFor">
  & Pick<ConnectState, "camSelection"> & Pick<MonitoringState, "expActive" | "expState"> & Pick<ConnectModel, "preflight">
  & Pick<ShellWorkflow, "workflow" | "preflightSignature" | "alignmentSignature">;

// The workflow bar's recommended action, and the one-time startup landing on the earliest incomplete stage.
export function useWorkflowLanding({
  ready, didInitStage, setTab, setPreflightConfirmedFor, setAlignmentConfirmedFor, camSelection, expActive, expState,
  preflight, workflow, preflightSignature, alignmentSignature,
}: LandingDeps) {
  const doRecommended = () => {
    const rec = workflow.recommended;
    if (!rec) return;
    if (rec.kind === "confirm-preflight" && !expActive && preflight.criticalPassed) {
      setPreflightConfirmedFor(preflightSignature);
      setTab("connect");
    } else if (rec.kind === "confirm-alignment" && !expActive) {
      setAlignmentConfirmedFor(alignmentSignature);
      setTab("overview");
    } else {
      setTab(rec.tab);
    }
  };

  // Startup lands on the earliest incomplete stage (UX-1), unless an active or
  // recoverable experiment demands the Experiment stage. Runs once, after the
  // first camera snapshot arrives so the landing reflects real backend state.
  useEffect(() => {
    if (didInitStage.current || !ready || camSelection === null) return;
    didInitStage.current = true;
    if (expActive || expState === EXPERIMENT_STATES.Failed) {
      setTab("experiment");
      return;
    }
    const cur = workflow.stages.find((s) => s.id === workflow.currentStageId);
    if (cur) setTab(cur.tab);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, camSelection, expActive, expState]);
  return { doRecommended };
}
