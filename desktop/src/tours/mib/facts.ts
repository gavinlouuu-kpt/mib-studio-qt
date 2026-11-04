// MIB Studio tours: the facts store. A tiny, module-level store the existing components report to, and only after a backend
// reply confirmed something (a finished discovery job, a feature readback, a run's accounting, a saved revision ...).
// The tour's action steps wait on these as named conditions (CONDITION_NAMES). The store computes nothing and never calls the
// bridge: it keeps what a component was told, a short detail string the component formatted from the reply, and the reply's
// refusal text. It is forgotten when a tour starts, so a step waits for what the operator does during the tour.
//
// Conditions (the names an action step awaits):
//   discovery-finished         a discovery job reached a terminal state and its reply named a backend instance (E01).
//   feature-write-readback     a camera feature write came back with the camera's readback (E02).
//   run-start-accepted         the backend accepted experiment_start (E04). Separate from the next one.
//   run-first-frame-observed   a status of that run showed a nonzero frame counter (E04). Observed from polled counters.
//   run-accounting-reconciled  the stopped run's last_run accounting arrived and says reconciled (E06).
//   recipe-created             recipe_create answered ok (I02).
//   review-recording-opened    the classic Review opened a stored recording (F04).
//   population-evaluated       analyze_population answered ok (F04, F05, F08).
//   workspace-revision-saved   workspace_write committed a revision (I12, F12).
import type { ConditionState } from "../runner";

export const FACTS = {
  discovery: "discovery-finished",
  featureWrite: "feature-write-readback",
  runStart: "run-start-accepted",
  runFirstFrame: "run-first-frame-observed",
  runAccounting: "run-accounting-reconciled",
  recipeCreated: "recipe-created",
  reviewOpened: "review-recording-opened",
  populationEvaluated: "population-evaluated",
  workspaceSaved: "workspace-revision-saved",
} as const;

export type FactName = (typeof FACTS)[keyof typeof FACTS];
/** The condition names an entry's action may await. */
export const CONDITION_NAMES: readonly FactName[] = Object.values(FACTS);

/** What a component reports: `ok` with a short detail it formatted from the reply, or the backend's refusal text. */
export type FactReport = { ok: true; detail: string } | { ok: false; reason: string };

export interface FactsStore {
  report(name: FactName, report: FactReport): void;
  /** `met` once a success was reported; a refusal is handed out once (`failed`) and then forgotten, so Try again waits afresh. */
  condition(name: FactName): ConditionState;
  detail(name: FactName): string | undefined;
  /** Forget everything (a tour starts). */
  reset(): void;
}

export function createFactsStore(): FactsStore {
  const reports = new Map<FactName, FactReport>();
  return {
    report(name, report) { reports.set(name, report); },
    condition(name) {
      const r = reports.get(name);
      if (!r) return "pending";
      if (r.ok) return "met";
      reports.delete(name);
      return { failed: r.reason };
    },
    detail(name) {
      const r = reports.get(name);
      return r?.ok ? r.detail : undefined;
    },
    reset() { reports.clear(); },
  };
}

/** The app's one store. */
export const facts = createFactsStore();

const clip = (text: string, max = 160) => (text.length > max ? `${text.slice(0, max - 1)}…` : text);
/** Components call these after a backend reply. A reply is never inspected beyond what the caller already decoded. */
export const reportFact = (name: FactName, detail: string) => facts.report(name, { ok: true, detail: clip(detail) });
export const reportFactRefused = (name: FactName, reason: string) => facts.report(name, { ok: false, reason: clip(reason) });
