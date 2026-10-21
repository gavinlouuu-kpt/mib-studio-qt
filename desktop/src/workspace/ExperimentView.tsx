// The workspace's Experiment stage (#716): sub-views Plan (O3, A5), Live (O4, A6b) and Summary
// (O5/O6, A7). Plan and Live have content; Summary is not in the workspace yet and sends the
// operator to the classic view. Choosing a sub-view only changes what is shown. The sub-view is
// held by App, because what is read depends on it: Prepare's checklist and gates are polled only
// while the Plan is shown (workspaceReadsPrepare), and monitoring runs only while Live is shown.
import { Button, Notice, Toolbar } from "@mib/ui";
import { LiveView, type LiveViewProps } from "./LiveView";
import { PlanView, type PlanViewProps } from "./PlanView";

export const EXPERIMENT_SUBVIEWS = ["Plan", "Live", "Summary"] as const;
export type ExperimentSubView = (typeof EXPERIMENT_SUBVIEWS)[number];

export interface ExperimentViewProps extends PlanViewProps {
  /** The sub-view shown, and the operator's choice of another. */
  view: ExperimentSubView;
  onView: (view: ExperimentSubView) => void;
  /** Back to the classic view as it was (no classic tab change). */
  onClassic: () => void;
  /** O4 Live: what App reads and does for it (the plan is the Plan's). */
  live: Omit<LiveViewProps, "tabs" | "plan" | "onView">;
}

export function ExperimentView({ view: sub, onView: setSub, onClassic, live, ...plan }: ExperimentViewProps) {
  const tabs = EXPERIMENT_SUBVIEWS.map((s) => (
    <Button key={s} variant={s === sub ? "selected" : "secondary"} aria-current={s === sub ? "page" : undefined} onClick={() => setSub(s)}>{s}</Button>
  ));
  if (sub === "Live") return <LiveView {...live} tabs={tabs} plan={plan.plan} onView={setSub} />;
  return (
    <div className="ws-view" data-testid="workspace-experiment">
      <Toolbar title="Experiment plan">{tabs}</Toolbar>
      {sub === "Plan" ? <PlanView {...plan} onShowLive={() => setSub("Live")} /> : (
        <div className="ws-body">
          <Notice tone="info" title="Not yet in the workspace · Summary" data-testid="workspace-experiment-summary"
            actions={<Button onClick={onClassic}>Back to the classic view</Button>}>
            Use the classic view for a run's outcome and counts. Opening it here starts, moves or lights nothing.
          </Notice>
        </div>
      )}
    </div>
  );
}
