// The workspace's Experiment stage (#716): sub-views Plan (O3, A5), Live (O4, A6) and Summary
// (O5/O6, A7). Plan has content; Live and Summary are not in the workspace yet and send the
// operator to the classic view. Choosing a sub-view only changes what is shown. The sub-view is
// held by App, because what is read depends on it: Prepare's checklist and gates are polled only
// while the Plan is shown (workspaceReadsPrepare).
import { Button, Notice, Toolbar } from "@mib/ui";
import { PlanView, type PlanViewProps } from "./PlanView";

export const EXPERIMENT_SUBVIEWS = ["Plan", "Live", "Summary"] as const;
export type ExperimentSubView = (typeof EXPERIMENT_SUBVIEWS)[number];

const CLASSIC_FOR: Readonly<Record<Exclude<ExperimentSubView, "Plan">, string>> = {
  Live: "monitoring and stopping a run",
  Summary: "a run's outcome and counts",
};

export interface ExperimentViewProps extends PlanViewProps {
  /** The sub-view shown, and the operator's choice of another. */
  view: ExperimentSubView;
  onView: (view: ExperimentSubView) => void;
  /** Back to the classic view as it was (no classic tab change). */
  onClassic: () => void;
}

export function ExperimentView({ view: sub, onView: setSub, onClassic, ...plan }: ExperimentViewProps) {
  return (
    <div className="ws-view" data-testid="workspace-experiment">
      <Toolbar title="Experiment plan">
        {EXPERIMENT_SUBVIEWS.map((s) => (
          <Button key={s} variant={s === sub ? "selected" : "secondary"} aria-current={s === sub ? "page" : undefined} onClick={() => setSub(s)}>{s}</Button>
        ))}
      </Toolbar>
      {sub === "Plan" ? <PlanView {...plan} /> : (
        <div className="ws-body">
          <Notice tone="info" title={`Not yet in the workspace · ${sub}`} data-testid={`workspace-experiment-${sub.toLowerCase()}`}
            actions={<Button onClick={onClassic}>Back to the classic view</Button>}>
            {`Use the classic view for ${CLASSIC_FOR[sub]}. Opening it here starts, moves or lights nothing.`}
          </Notice>
        </div>
      )}
    </div>
  );
}
