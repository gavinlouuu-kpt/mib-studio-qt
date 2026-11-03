// MIB Studio tours: the view vocabulary. A TourView is a plain string record the registry carries (`view` on a ui target and
// the runner's start view). These helpers make and read the two shapes the app has:
//   { shell: "workspace", stage: "prepare" | "align" | "experiment" | "review", sub?: "plan" | "live" | "run-summary" | "quick-look" }
//   (`sub` is the Experiment stage's sub-view; it is kept for the other stages too, so the start view comes back whole.)
//   { shell: "classic", tab: "connect" | "overview" | "experiment" | "review" }
// Navigation is view state only. `parseView` refuses what it does not know, and `classicTabMayChange` names the two classic tabs a
// tour may move to: a change to the classic Overview or Experiment tab can switch the camera mode (Desktop-Shell.md, "Navigation
// never actuates"), so a tour never moves there. Nothing in this file calls the bridge.
import type { TourView } from "../registry";

export const WORKSPACE_STAGE_NAMES = ["prepare", "align", "experiment", "review"] as const;
export type WorkspaceStageName = (typeof WORKSPACE_STAGE_NAMES)[number];
export const EXPERIMENT_SUB_NAMES = ["plan", "live", "run-summary", "quick-look"] as const;
export type ExperimentSubName = (typeof EXPERIMENT_SUB_NAMES)[number];
/** The Experiment sub-view titles the app uses (workspace/ExperimentView.tsx EXPERIMENT_SUBVIEWS), by tour name. */
export const SUB_TITLES: Readonly<Record<ExperimentSubName, "Plan" | "Live" | "Run summary" | "Quick look">> = {
  plan: "Plan", live: "Live", "run-summary": "Run summary", "quick-look": "Quick look",
};
export const CLASSIC_TABS = ["connect", "overview", "experiment", "review"] as const;
export type ClassicTab = (typeof CLASSIC_TABS)[number];
/** The classic tabs a tour may move to. */
export const NAVIGABLE_CLASSIC_TABS: readonly ClassicTab[] = ["connect", "review"];

export const workspaceView = (stage: WorkspaceStageName, sub?: ExperimentSubName): TourView =>
  sub ? { shell: "workspace", stage, sub } : { shell: "workspace", stage };
export const classicView = (tab: ClassicTab): TourView => ({ shell: "classic", tab });

export type ParsedView =
  | { shell: "workspace"; stage: number; sub: ExperimentSubName | null }
  | { shell: "classic"; tab: ClassicTab };

export function parseView(view: TourView): ParsedView | null {
  if (view.shell === "workspace") {
    const stage = WORKSPACE_STAGE_NAMES.indexOf(view.stage as WorkspaceStageName);
    if (stage < 0) return null;
    const sub = view.sub === undefined ? null : EXPERIMENT_SUB_NAMES.find((s) => s === view.sub) ?? null;
    if (view.sub !== undefined && !sub) return null;
    return { shell: "workspace", stage, sub };
  }
  if (view.shell === "classic") {
    const tab = CLASSIC_TABS.find((t) => t === view.tab);
    return tab ? { shell: "classic", tab } : null;
  }
  return null;
}

/** Open a collapsed `<details>` that holds a step's element (view state only; the disclosure is the only thing that changes).
 *  A `<summary>` never opens its own `<details>`. */
export function revealCollapsed(el: Element): void {
  let from: Element | null = el.parentElement;
  if (el.tagName === "SUMMARY") from = from?.parentElement ?? null; // its own <details> stays as the operator left it
  for (let d = from?.closest("details") ?? null; d; d = d.parentElement?.closest("details") ?? null) {
    if (!d.open) d.open = true;
  }
}
