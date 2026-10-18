// The opt-in four-stage workspace (P1, #713; track A step A1): Prepare / Align / Experiment /
// Review on the @mib/ui kit. The classic shell stays the default and keeps running underneath,
// unchanged: the workspace never changes the classic tab, because a classic tab change can act
// on hardware (Camera & Alignment switches the PZ7035 to Align and its LED on; Experiment to Run).
// Selecting a stage here only changes what is shown; it never starts capture, moves a motor or
// stage, enables light, runs a pump or installs an update. Only Prepare has content in A1.
//
// The kit stylesheets are imported once, here, at the workspace entry.
import "@mib/ui/fonts.css";
import "@mib/ui/style.css";
import "./workspace.css";
import { AppShell, Button, Notice, StateChip, Toolbar } from "@mib/ui";
import { derivePreflight } from "../preflight";
import { PrepareView, type PrepareViewProps } from "./PrepareView";
import { prepareVerdict } from "./prepareModel";
import { usePrepareReadiness } from "./usePrepareReadiness";

export const WORKSPACE_STAGES = ["Prepare", "Align", "Experiment", "Review"] as const;

/** What each stage still uses the classic view for, until it moves into the workspace. */
const CLASSIC_FOR: Readonly<Record<number, string>> = {
  1: "the live image, focus, ROI and camera window (Camera & Alignment)",
  2: "starting, monitoring and stopping a run (Experiment)",
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
}

export function Workspace({ brand, source, method, stage, onStage, onExit, serviceMode, ...prepare }: WorkspaceProps) {
  // Read only while Prepare is shown and no run is active (the gates would only say "running").
  const readiness = usePrepareReadiness(stage === 0 && !prepare.experimentActive);
  // One verdict from the checklist and the gates, the same words in the header and in Prepare.
  const report = derivePreflight(prepare.preflightInput);
  const verdict = prepareVerdict(report, readiness.snapshot, readiness.error);
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
      band={{ left: "Workspace preview · opt-in", right: "Reads the backend only. The classic view stays the default." }}
      status={<>
        {stage === 0 ? <StateChip tone={verdict.tone} title={verdict.detail || undefined} data-testid="workspace-verdict">{verdict.title}</StateChip> : null}
        <StateChip tone="neutral">Preview · read-only</StateChip>
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
