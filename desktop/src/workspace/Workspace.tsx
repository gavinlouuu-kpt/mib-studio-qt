// The opt-in four-stage workspace (P1, #713; track A steps A1 and A3): Prepare / Align /
// Experiment / Review on the @mib/ui kit. The classic shell stays the default and keeps running
// underneath, unchanged: the workspace never changes the classic tab, because a classic tab change
// can act on hardware (Camera & Alignment switches the PZ7035 to Align and its LED on; Experiment
// to Run). Selecting a stage here only changes what is shown; it never starts capture, switches a
// camera mode, moves a motor or stage, enables light, runs a pump or installs an update. Prepare
// (O1), Align (O2) and Experiment · Plan (O3, A5) have content; in Align a mode switch or a write
// happens only on a button, and in Plan a run starts only on "Start run".
//
// The kit stylesheets are imported once, here, at the workspace entry.
import "@mib/ui/fonts.css";
import "@mib/ui/style.css";
import "./workspace.css";
import { AppShell, Button, Notice, StateChip, Toolbar } from "@mib/ui";
import { derivePreflight } from "../preflight";
import { AlignView, type AlignViewProps } from "./AlignView";
import { ExperimentView, type ExperimentSubView } from "./ExperimentView";
import { PrepareView, type PrepareViewProps } from "./PrepareView";
import { confirmBlockers, prepareVerdict } from "./prepareModel";
import type { PlanController } from "./usePlan";
import { usePrepareReadiness } from "./usePrepareReadiness";

export const WORKSPACE_STAGES = ["Prepare", "Align", "Experiment", "Review"] as const;

/** Whether the workspace shows Prepare's verdict, and so polls Prepare's checklist and gates:
 *  in Prepare and in Experiment's Plan, not in its Live or Summary placeholders (#736 review). */
export const workspaceReadsPrepare = (stage: number, experimentView: ExperimentSubView): boolean =>
  stage === 0 || (stage === 2 && experimentView === "Plan");

/** What each stage still uses the classic view for, until it moves into the workspace. */
const CLASSIC_FOR: Readonly<Record<number, string>> = {
  3: "opening and reviewing recordings (Review)",
};

export interface WorkspaceProps extends Omit<PrepareViewProps, "readiness" | "report" | "verdict"> {
  brand: string;
  /** Header context: the camera source and the applied method. "" when there is none. */
  source: string;
  method: string;
  stage: number;
  onStage: (stage: number) => void;
  /** Back to the classic view as it was (no classic tab change). */
  onExit: () => void;
  serviceMode: boolean;
  /** O2 Align: the live image, image tools and quick controls. */
  align: Omit<AlignViewProps, "onClassic">;
  /** O3 Experiment · Plan: the plan (held by App), why Start is locked apart from it and
   *  Prepare (the classic Start button's reasons; "" when none), and the sub-view shown (held by
   *  App, which polls the checklist only while the Plan is shown). */
  experiment: { plan: PlanController; lockedReason: string; view: ExperimentSubView; onView: (view: ExperimentSubView) => void };
}

export function Workspace({ brand, source, method, stage, onStage, onExit, serviceMode, align, experiment, ...prepare }: WorkspaceProps) {
  // Read only while Prepare or the Experiment plan (which takes Prepare's verdict) is shown and no
  // run is active (the gates would only say "running"). Always without a path.
  const showsVerdict = workspaceReadsPrepare(stage, experiment.view);
  const readiness = usePrepareReadiness(showsVerdict && !prepare.experimentActive);
  // One verdict from the checklist and the gates, the same words in the header and in Prepare.
  const report = derivePreflight(prepare.preflightInput);
  const verdict = prepareVerdict(report, readiness.snapshot, readiness.error, prepare.experimentActive);
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
      band={{ left: "Workspace preview · opt-in", right: "Navigation never acts: only an explicit button switches or writes. The classic view stays the default." }}
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
        <PrepareView {...prepare} report={report} readiness={readiness} verdict={verdict} />
      ) : stage === 1 ? (
        <AlignView {...align} onClassic={onExit} />
      ) : stage === 2 ? (
        <ExperimentView plan={experiment.plan} lockedReason={experiment.lockedReason} view={experiment.view} onView={experiment.onView}
          verdict={verdict} blockers={confirmBlockers(report)} onClassic={onExit} />
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
