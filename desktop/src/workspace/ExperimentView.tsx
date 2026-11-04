// The workspace's Experiment stage (#716): sub-views Plan (O3, A5), Live (O4, A6b), Run summary (O5)
// and Quick look (O6) (A7b, split into their own sub-views as on the Penpot boards in A7c). Choosing a
// sub-view only changes what is shown. The sub-view is held by App, because what is read depends on
// it: Prepare's checklist and gates are polled only while the Plan is shown (workspaceReadsPrepare),
// monitoring runs only while Live is shown, and the run quick look is read only while the Run summary
// (its run) or Quick look (the plan's closed runs) is shown (useQuickLook: on showing, on the run and
// status transitions, and on Refresh; never polled). The run the two inspect is held here, so it
// stays while the operator moves between them.
import { useState } from "react";
import { Button, Toolbar } from "@mib/ui";
import { LiveView, type LiveViewProps } from "./LiveView";
import { PlanView, type PlanViewProps } from "./PlanView";
import { QuickLookView, type QuickLookViewProps } from "./QuickLookView";
import { RunSummaryView, type RunSummaryViewProps } from "./RunSummaryView";

export const EXPERIMENT_SUBVIEWS = ["Plan", "Live", "Run summary", "Quick look"] as const;
export type ExperimentSubView = (typeof EXPERIMENT_SUBVIEWS)[number];

export interface ExperimentViewProps extends PlanViewProps {
  /** The sub-view shown, and the operator's choice of another. */
  view: ExperimentSubView;
  onView: (view: ExperimentSubView) => void;
  /** O4 Live: what App reads and does for it (the plan is the Plan's). */
  live: Omit<LiveViewProps, "tabs" | "plan" | "onView">;
  /** O5 Run summary and O6 Quick look: what App gives them (the plan is the Plan's; the transition to
   *  idle comes from Live's experimentActive). */
  summary: Pick<QuickLookViewProps, "onAlign" | "review" | "read"> & Pick<RunSummaryViewProps, "onPrepare" | "readCalibration">;
}

export function ExperimentView({ view: sub, onView: setSub, live, summary, ...plan }: ExperimentViewProps) {
  const [inspected, setInspected] = useState<string | null>(null);
  const tabs = EXPERIMENT_SUBVIEWS.map((s) => (
    <Button key={s} variant={s === sub ? "selected" : "secondary"} aria-current={s === sub ? "page" : undefined} onClick={() => setSub(s)}>{s}</Button>
  ));
  if (sub === "Live") return <LiveView {...live} tabs={tabs} plan={plan.plan} onView={setSub} />;
  if (sub === "Run summary") return <RunSummaryView tabs={tabs} plan={plan.plan} experimentActive={live.experimentActive} onView={setSub}
    inspected={inspected} onInspect={setInspected} onPrepare={summary.onPrepare} review={summary.review} read={summary.read} readCalibration={summary.readCalibration} />;
  if (sub === "Quick look") return <QuickLookView tabs={tabs} plan={plan.plan} experimentActive={live.experimentActive} onView={setSub}
    inspected={inspected} onInspect={setInspected} onAlign={summary.onAlign} review={summary.review} read={summary.read} />;
  return (
    <div className="ws-view" data-testid="workspace-experiment">
      <Toolbar title="Experiment plan">{tabs}</Toolbar>
      <PlanView {...plan} onShowLive={() => setSub("Live")} />
    </div>
  );
}
