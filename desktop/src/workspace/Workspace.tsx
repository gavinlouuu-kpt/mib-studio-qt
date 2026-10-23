// The four-stage workspace (P1, #713; track A steps A1 and A3): Prepare / Align /
// Experiment / Review on the @mib/ui kit. On the workspace branch the app opens here (landing.ts),
// with the classic view one click away ("Classic view"); the classic shell keeps running
// underneath, unchanged: the workspace never changes the classic tab, because a classic tab change
// can act on hardware (Camera & Alignment switches the PZ7035 to Align and its LED on; Experiment
// to Run). Selecting a stage here only changes what is shown; it never starts capture, switches a
// camera mode, moves a motor or stage, enables light, runs a pump or installs an update. Prepare
// (O1), Align (O2), Experiment · Plan (O3, A5), Experiment · Live (O4, A6b), Experiment · Run summary
// (O5) and Experiment · Quick look (O6) (A7b, A7c) have content; in Align
// a mode switch or a write happens only on a button, in Plan a run starts, stops or switches to Run
// only on a button ("Start run", the sample queue's steps, "Switch to Run" / "Show the experiment
// window"), and in Live a pin, a run stop or a camera stop happens only on a button (showing Live
// reads, and turns monitoring on as the classic Monitoring tab does), and in Run summary and Quick look
// a decision acts only on its button (showing them reads the run quick look; never applies calibration). In Prepare the calibration
// factor changes only on the Calibration card's Apply / Re-apply button (#716 A4b).
//
// The kit stylesheets are imported once, here, at the workspace entry.
import "@mib/ui/fonts.css";
import "@mib/ui/style.css";
import "./workspace.css";
import { AppShell, Button, Notice, StateChip, Toolbar } from "@mib/ui";
import { derivePreflight } from "../preflight";
import { AlignView, type AlignViewProps } from "./AlignView";
import { ExperimentView, type ExperimentSubView, type ExperimentViewProps } from "./ExperimentView";
import { PrepareView, type PrepareViewProps } from "./PrepareView";
import { confirmBlockers, confirmationState, prepareVerdict } from "./prepareModel";
import type { PlanCameraMode } from "./planModel";
import type { PlanController } from "./usePlan";
import { usePrepareReadiness } from "./usePrepareReadiness";
import { calibrationGate } from "./calibrationModel";
import type { PlanCalibration } from "./planParts";
import { useCalibration } from "./useCalibration";

/** The band under the brand: what the workspace is, and its one rule. */
export const WORKSPACE_BAND = {
  left: "Workspace preview",
  right: "Navigation never acts: only an explicit button switches or writes. The classic view is one click away (Classic view).",
} as const;

export const WORKSPACE_STAGES = ["Prepare", "Align", "Experiment", "Review"] as const;

/** Whether the workspace shows Prepare's verdict, and so polls Prepare's checklist and gates:
 *  in Prepare and in Experiment's Plan, not in its Live, Run summary or Quick look views (#736 review). */
export const workspaceReadsPrepare = (stage: number, experimentView: ExperimentSubView): boolean =>
  stage === 0 || (stage === 2 && experimentView === "Plan");

/** What each stage still uses the classic view for, until it moves into the workspace. */
const CLASSIC_FOR: Readonly<Record<number, string>> = {
  3: "opening and reviewing recordings (Review)",
};

/** Without App's classic Review (a host that gives none), Open in Review is unavailable. */
const NO_REVIEW = { review: { reason: "the classic Review is not available here", open: () => {} } };

export interface WorkspaceProps extends Omit<PrepareViewProps, "readiness" | "report" | "verdict" | "calibration" | "calibrationGate" | "now"> {
  brand: string;
  /** Header context: the camera source and the applied method. "" when there is none. */
  source: string;
  method: string;
  stage: number;
  onStage: (stage: number) => void;
  /** Back to the classic view as it was (no classic tab change); App remembers it as the
   *  operator's choice for the next start. */
  onExit: () => void;
  serviceMode: boolean;
  /** O2 Align: the live image, image tools and quick controls. */
  align: Omit<AlignViewProps, "onClassic">;
  /** O3 Experiment · Plan: the plan (held by App, with its sample queue), why Start is locked apart
   *  from it and Prepare (the classic Start button's reasons; "" when none), the sub-view shown (held
   *  by App, which polls the checklist only while the Plan is shown), and the explicit switch to the
   *  mode a run records in (App's applyCameraMode: the PZ7035's Run, a desktop camera's experiment
   *  window; null when none is needed). */
  experiment: {
    plan: PlanController; lockedReason: string; view: ExperimentSubView; onView: (view: ExperimentSubView) => void;
    cameraMode?: PlanCameraMode | null; onCameraMode?: () => Promise<unknown>;
    /** O4 Experiment · Live (A6b): what App reads and does for it (monitoring is on while it is shown). */
    live: ExperimentViewProps["live"];
    /** O5 Run summary and O6 Quick look (A7b/A7c): Open in Review (App's classic Review tab with a
     *  file) and the reads (tests). Adjust next's Align is this workspace's stage 1; a calibration
     *  run's Apply / Dismiss lead to Prepare's Calibration card (stage 0). */
    summary?: Omit<ExperimentViewProps["summary"], "onAlign" | "onPrepare">;
    /** The recording this page has open in the classic Review ("" or absent: none): the Plan's Start
     *  is unavailable meanwhile (#716 A7b). */
    reviewOpen?: string;
    onOpenSource?: (path:string)=>Promise<void>;
  };
}

export function Workspace({ brand, source, method, stage, onStage, onExit, serviceMode, align, experiment, ...prepare }: WorkspaceProps) {
  // Read only while Prepare or the Experiment plan (which takes Prepare's verdict) is shown and no
  // run is active (the gates would only say "running"). Always without a path.
  const showsVerdict = workspaceReadsPrepare(stage, experiment.view);
  const readiness = usePrepareReadiness(showsVerdict && !prepare.experimentActive);
  // The calibration records (#716 A4b), read with the gates and for the same views: Prepare's
  // Calibration card, and the record the Plan declares at Start. Only its Apply button acts.
  const calibration = useCalibration(showsVerdict && !prepare.experimentActive,
    { controlHeld: prepare.controlHeld, experimentActive: prepare.experimentActive, afterApply: readiness.refresh });
  const calGate = calibrationGate(readiness.snapshot);
  // A store the backend cannot read lists no records: what is in effect is then not known (never "none").
  const storeError = calibration.list?.valid ? calibration.list.store?.error ?? null : null;
  const calList = calibration.list?.valid && !storeError ? calibration.list : null;
  const planCalibration: PlanCalibration = {
    read: calList !== null, inEffectId: calList?.in_effect_record_id ?? null,
    factor: calList?.in_effect_record_id && typeof calList.current_factor === "number" ? calList.current_factor : null,
    error: prepare.experimentActive ? "not read while a run is active" : storeError ? "the calibration store cannot be read" : calibration.error,
    gate: calGate, refresh: calibration.refresh,
  };
  // O5's calibration block leads here: Prepare, its Calibration card, and the record selected (its
  // versions are a read). Nothing is applied: Apply is the card's own button.
  const showCalibration = (recordId: string | null) => {
    onStage(0);
    if (recordId) calibration.select(recordId);
    window.setTimeout(() => document.querySelector<HTMLElement>("[data-testid=workspace-calibration]")?.scrollIntoView?.({ block: "start" }), 0);
  };
  // One verdict from the checklist, the gates and the operator's confirmation, the same words in
  // the header, in Prepare and beside Start.
  const report = derivePreflight(prepare.preflightInput);
  const confirmation = confirmationState(prepare.signature, prepare.confirmedFor, report.criticalPassed);
  const verdict = prepareVerdict(report, readiness.snapshot, readiness.error, prepare.experimentActive, confirmation === "confirmed");
  return (
    <AppShell
      className="ws-shell"
      data-testid="workspace"
      brand={brand}
      tag="Workspace preview"
      context={[
        { label: "Source", value: source || "None selected" },
        { label: "Method", value: method || "None applied" },
      ]}
      stages={[...WORKSPACE_STAGES]}
      activeStage={stage}
      onStageSelect={onStage}
      band={WORKSPACE_BAND}
      status={<>
        {showsVerdict ? <StateChip tone={verdict.tone} title={verdict.detail || undefined} data-testid="workspace-verdict">{verdict.title}</StateChip> : null}
        <StateChip tone="neutral" data-testid="workspace-preview-chip">{stage === 1 || stage === 2 ? "Preview · acts only on a button" : "Preview · read-only"}</StateChip>
      </>}
      headerActions={<Button onClick={onExit}>Classic view</Button>}
    >
      {serviceMode && (
        <div className="ws-body ws-body--banner">
          <Notice tone="warn" title="Service / Commissioning mode · hardware-actuating controls are enabled">
            Set in the classic view. Not for routine operation.
          </Notice>
        </div>
      )}
      {stage === 0 ? (
        <PrepareView {...prepare} report={report} readiness={readiness} verdict={verdict} calibration={calibration} calibrationGate={calGate} now={Date.now()} />
      ) : stage === 1 ? (
        <AlignView {...align} onClassic={onExit} />
      ) : stage === 2 ? (
        <ExperimentView onOpenSource={experiment.onOpenSource} plan={experiment.plan} lockedReason={experiment.lockedReason} view={experiment.view} onView={experiment.onView}
          cameraMode={experiment.cameraMode} onCameraMode={experiment.onCameraMode} live={experiment.live}
          summary={{ ...(experiment.summary ?? NO_REVIEW), onAlign: () => onStage(1), onPrepare: showCalibration }} reviewOpen={experiment.reviewOpen}
          verdict={verdict} blockers={confirmBlockers(report)} confirmation={confirmation} calibration={planCalibration} />
      ) : (
        <div className="ws-view" data-testid={`workspace-${WORKSPACE_STAGES[stage]?.toLowerCase() ?? "stage"}`}>
          <Toolbar title={WORKSPACE_STAGES[stage]}>
            <Button onClick={onExit}>Back to the classic view</Button>
          </Toolbar>
          <div className="ws-body">
            <Notice tone="info" title={`Not yet in the workspace · ${WORKSPACE_STAGES[stage]}`}>
              Use the classic view for {CLASSIC_FOR[stage] ?? "this stage"}. Opening a stage here starts, moves or lights nothing.
            </Notice>
          </div>
        </div>
      )}
    </AppShell>
  );
}
