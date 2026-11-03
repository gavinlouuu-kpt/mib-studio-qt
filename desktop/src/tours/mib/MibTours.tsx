// MIB Studio tours: the one component App renders. It gives the generic TourHost this app's views, conditions and idle rule:
// - navigate/currentView speak the TourView vocabulary of views.ts and only ever change view state (a shown stage, a sub-view, a
//   classic tab, an open disclosure). They never send a bridge command, never write the landing preference (landing.ts) and never
//   move to the classic Overview or Experiment tab, which can switch the camera mode.
// - the conditions are the facts the existing components report after a backend reply (facts.ts). The tour calls no bridge command.
// - action steps are blocked while an experiment runs, something records or the camera acquires (except a step marked duringRun;
//   a step marked needsCamera is not blocked by the camera acquiring).
// - the "what's new" notice is quiet under the same rule. Nothing starts a tour by itself: the Help menu entry and the workspace
//   header's Tours button only open the list.
import { useMemo, useRef } from "react";
import { TourHost } from "../TourHost";
import type { TourAction, TourEntry, TourView } from "../registry";
import type { ConditionState } from "../runner";
import { MIB_TOURS } from "./entries";
import { CONDITION_NAMES, facts, type FactName } from "./facts";
import {
  classicView, EXPERIMENT_SUB_NAMES, NAVIGABLE_CLASSIC_TABS, parseView, revealCollapsed, SUB_TITLES, workspaceView, WORKSPACE_STAGE_NAMES,
  type ClassicTab, type ExperimentSubName,
} from "./views";
import "./mibTours.css";

/** localStorage key prefix: `.seen` (the "what's new" marker) and `.feedback` (the checkpoint answers). */
export const TOURS_STORAGE_KEY = "yofo.studio.tours";

/** What App tells the tours about itself. */
export interface MibToursApp {
  /** The workspace stage shown (0..3), or null for the classic shell. */
  workspaceStage: number | null;
  /** The Experiment stage's sub-view title ("Plan", "Live", "Run summary", "Quick look"). */
  experimentView: string;
  classicTab: ClassicTab;
  /** An experiment is Starting, Active or Stopping. */
  experimentActive: boolean;
  /** A frame recording clip is running. */
  recording: boolean;
  /** The camera is acquiring. */
  acquiring: boolean;
}

/** The only things navigation may do. App implements them with plain state setters. */
export interface MibToursNav {
  /** Show a workspace stage (and, when given, the Experiment sub-view). Must not write the landing preference. */
  showWorkspace(stage: number, sub: ExperimentSubName | null): void;
  /** Show the classic shell on a tab. Must not write the landing preference. */
  showClassic(tab: ClassicTab): void;
}

const subName = (title: string): ExperimentSubName =>
  EXPERIMENT_SUB_NAMES.find((n) => SUB_TITLES[n] === title) ?? "plan";

export function currentViewOf(app: MibToursApp): TourView {
  if (app.workspaceStage !== null) return workspaceView(WORKSPACE_STAGE_NAMES[app.workspaceStage] ?? "prepare", subName(app.experimentView));
  return classicView(app.classicTab);
}

/** Show a view. Returns what it did, for the tests: "ignored" for a view it does not know or must not move to. */
export function navigateTo(view: TourView, app: MibToursApp, nav: MibToursNav): "workspace" | "classic" | "ignored" {
  const parsed = parseView(view);
  if (!parsed) return "ignored";
  if (parsed.shell === "workspace") {
    nav.showWorkspace(parsed.stage, parsed.sub);
    return "workspace";
  }
  // A classic tab change can act (the Overview and Experiment tabs switch the camera mode): only the tab already shown, or a tab
  // that only reads, is ever shown.
  if (parsed.tab !== app.classicTab && !NAVIGABLE_CLASSIC_TABS.includes(parsed.tab)) return "ignored";
  nav.showClassic(parsed.tab);
  return "classic";
}

/** Why an action step cannot be done now, or null. A step marked `needsCamera` (a camera feature write works only on an open
 *  camera session) is not blocked by the camera acquiring, only by an experiment or a recording: the backend's own gate for feature
 *  writes (FeatureControl.md) is the same. */
export function actionBlockedReason(app: MibToursApp, action?: Pick<TourAction, "needsCamera">): string | null {
  if (app.experimentActive) return "an experiment is running";
  if (app.recording) return "a recording is in progress";
  if (app.acquiring && !action?.needsCamera) return "the camera is acquiring";
  return null;
}

/** True while the "what's new" notice must stay hidden. */
export const toursQuiet = (app: MibToursApp) => actionBlockedReason(app) !== null;

const ENTRIES: TourEntry<unknown>[] = MIB_TOURS.filter((e) => e.when({}));

export function MibTours({ listOpen, onListClose, app, nav }: { listOpen: boolean; onListClose: () => void; app: MibToursApp; nav: MibToursNav }) {
  const live = useRef({ app, nav });
  live.current = { app, nav };
  const bound = useMemo(() => ({
    navigate: (v: TourView) => void navigateTo(v, live.current.app, live.current.nav),
    currentView: () => currentViewOf(live.current.app),
    actionBlocked: (action?: TourAction) => actionBlockedReason(live.current.app, action),
    conditions: Object.fromEntries(CONDITION_NAMES.map((n) => [n, () => facts.condition(n)])) as Record<string, () => ConditionState>,
    detail: (name: string) => facts.detail(name as FactName),
    onTourStart: () => facts.reset(),
  }), []);
  return (
    <TourHost
      entries={ENTRIES}
      storageKey={TOURS_STORAGE_KEY}
      {...bound}
      reveal={revealCollapsed}
      confirmedBy="the backend"
      quiet={toursQuiet(app)}
      listOpen={listOpen}
      onListClose={onListClose}
      exportMeta={{ app: "MIB Studio" }}
    />
  );
}
