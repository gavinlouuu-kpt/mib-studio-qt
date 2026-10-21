// The workspace's Experiment stage (#716): sub-views Plan (O3, A5), Live (O4, A6b) and Summary (O5 Run
// summary and O6 Quick look, A7b). Choosing a sub-view only changes what is shown. The sub-view is
// held by App, because what is read depends on it: Prepare's checklist and gates are polled only
// while the Plan is shown (workspaceReadsPrepare), monitoring runs only while Live is shown, and the
// run quick look is read only while the Summary is shown (useQuickLook: on showing, on the run and
// status transitions, and on Refresh; never polled).
import { Button, Toolbar } from "@mib/ui";
import { LiveView, type LiveViewProps } from "./LiveView";
import { PlanView, type PlanViewProps } from "./PlanView";
import { SummaryView, type SummaryViewProps } from "./SummaryView";

export const EXPERIMENT_SUBVIEWS = ["Plan", "Live", "Summary"] as const;
export type ExperimentSubView = (typeof EXPERIMENT_SUBVIEWS)[number];

export interface ExperimentViewProps extends PlanViewProps {
  /** The sub-view shown, and the operator's choice of another. */
  view: ExperimentSubView;
  onView: (view: ExperimentSubView) => void;
  /** O4 Live: what App reads and does for it (the plan is the Plan's). */
  live: Omit<LiveViewProps, "tabs" | "plan" | "onView">;
  /** O5/O6 Summary: what App gives it (the plan is the Plan's; the transition to idle comes from
   *  Live's experimentActive). */
  summary: Pick<SummaryViewProps, "onAlign" | "review" | "read">;
}

export function ExperimentView({ view: sub, onView: setSub, live, summary, ...plan }: ExperimentViewProps) {
  const tabs = EXPERIMENT_SUBVIEWS.map((s) => (
    <Button key={s} variant={s === sub ? "selected" : "secondary"} aria-current={s === sub ? "page" : undefined} onClick={() => setSub(s)}>{s}</Button>
  ));
  if (sub === "Live") return <LiveView {...live} tabs={tabs} plan={plan.plan} onView={setSub} />;
  if (sub === "Summary") return <SummaryView {...summary} tabs={tabs} plan={plan.plan} experimentActive={live.experimentActive} onView={setSub} />;
  return (
    <div className="ws-view" data-testid="workspace-experiment">
      <Toolbar title="Experiment plan">{tabs}</Toolbar>
      <PlanView {...plan} onShowLive={() => setSub("Live")} />
    </div>
  );
}
