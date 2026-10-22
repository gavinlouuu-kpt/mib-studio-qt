// What the Experiment's Run summary (O5) and Quick look (O6) share (#716 A7b, split in A7c): the
// sub-view toolbar actions, the quick look's per-run states and tiles, and the decisions (the A7
// ruling 8), each one explicit step:
//
// - Next sample (Continue): on a finalized row, the queue's Confirm closed (sends nothing), then the
//   Plan shows the next Start; otherwise it only shows the Plan. Disabled while a row runs (stopping
//   is Next sample in the Plan and Live).
// - Repeat sample: re-queues the run's row as the next run of its sample_ref (queue event `repeat`,
//   sends nothing).
// - End queue: the queue's own `end` (sends nothing); on a finalized row it confirms the close first
//   (the queue's Confirm closed), since the queue never ends past an unconfirmed close.
// - Adjust next: navigation only (the Plan, or Align).
// - Open in Review: the classic Review tab with the file (`load_recording`, a control command).
//
// A viewer's acting decisions are disabled with the shared control block; reads and navigation stay.
// Nothing here assesses a run: the helper text describes the step, never the data.
import type { ReactNode } from "react";
import { Notice, StatTile, StateChip } from "@mib/ui";
import type { RunQuickLookRun, RunStatistics } from "../runQuickLook";
import type { ExperimentSubView } from "./ExperimentView";
import { controlBlock } from "./planParts";
import { DEFERRED_TEXT, differsChips, flagChips, NOT_RECORDED, quickTiles, type PlanRun } from "./quickLookModel";
import { currentIndex, repeatReason, rowState, upcomingIndex } from "./queueModel";
import { endQueueState, nextSample, sampleName } from "./runSummaryModel";
import type { SampleRow } from "./planModel";
import type { PlanController } from "./usePlan";

export const basename = (path: string) => path.split(/[\\/]/).pop() || path;
export const runName = (r: PlanRun) => `Run ${r.runIndex} · ${r.ref || "sample"}`;
export const what = (r: PlanRun) => (r.row ? (r.row.sample.trim() || r.row.condition.trim()) : "");

/** Who controls the decisions that act: a viewer's are disabled, and nothing is sent. */
export const DECISIONS_CONTROL = controlBlock("Each of Next sample, Repeat sample, End queue and Open in Review");

/** Open in Review: App's classic Review tab with the file; `reason` says why it is unavailable. */
export interface ReviewAction { reason: string; open: (path: string) => void }

export interface Decision { reason: string; title: string; act: () => void }

export interface Decisions {
  controlHeld: boolean;
  /** Next sample / Continue, and the sample its Start would be for (null when none is left). */
  next: Decision & { row: SampleRow | null };
  repeat: Decision;
  end: Decision;
  review: Decision;
}

/** The decisions for a closed run (null: none is shown), each with why it is unavailable ("" when it
 *  may be pressed) and what it does. */
export function decisionsFor(run: PlanRun | null, plan: PlanController, review: ReviewAction, onView: (v: ExperimentSubView) => void): Decisions {
  const queue = plan.queue, q = queue.state;
  const queueMode = plan.draft.format === "queue";
  const order = plan.draft.rows.map((r) => r.ref);
  const controlHeld = queue.controlHeld;
  const busy = queue.pending !== null;
  const control = controlHeld ? "" : DECISIONS_CONTROL.title;
  const nothing = () => {};

  // Repeat sample: the run's own row, its latest run only.
  let repeat = "";
  if (!run) repeat = "no closed run is shown";
  else if (!queueMode) repeat = "a single-sample plan has one run: a sample queue repeats a sample";
  else {
    const row = rowState(q, run.ref);
    const last = row.attempts[row.attempts.length - 1];
    const requeued = row.history?.some((h) => h.runIndex === run.runIndex) ?? false;
    repeat = control || (busy ? "a Start or a stop is pending" : "")
      || (last && last.runIndex !== run.runIndex ? `${run.ref} was re-queued after this run (run ${last.runIndex})`
        : requeued ? `${run.ref} is already re-queued: its next run waits for its Start in the Plan` : repeatReason(order, q, run.ref));
  }

  // Next sample: the queue's next step from here.
  const cur = queueMode ? currentIndex(order, q) : -1;
  const curRef = cur >= 0 ? order[cur] : null;
  const curState = curRef ? rowState(q, curRef) : null;
  const upcoming = nextSample(plan.draft, queueMode ? upcomingIndex(order, q) : -1);
  const upcomingText = upcoming ? sampleName(upcoming) : null;
  let next: Decision;
  if (queueMode && curState?.phase === "finalized") {
    next = { reason: control || (q.paused ? "the queue is paused: resume it in the Plan or Live" : busy ? "a Start or a stop is pending" : ""),
      title: `Confirms ${curRef}'s close (sends nothing), then the Plan shows ${upcomingText ? `${upcomingText}'s Start` : "that the queue is complete"}`,
      act: () => { queue.confirmClosed(); onView("Plan"); } };
  } else if (queueMode && curState && ["starting", "running", "stopping"].includes(curState.phase)) {
    next = { reason: `${curRef} is ${curState.phase}: Next sample (its stop) is in the Plan and Live`, title: "", act: nothing };
  } else {
    next = { reason: "", act: () => onView("Plan"),
      title: queueMode ? (curRef ? `Shows the Plan, where ${curRef} waits for its Start (navigation only)` : "Shows the Plan: add a sample or start a new plan (navigation only)")
        : "Shows the Plan: a new plan runs the next sample (navigation only)" };
  }

  // End queue: the queue's own end, after confirming a finalized run's close.
  const e = endQueueState(order, q, queueMode, busy);
  const end: Decision = { reason: (queueMode && control) || e.reason, title: e.title,
    act: () => { if (e.confirmFirst) queue.confirmClosed(); queue.end(); } };

  const reviewReason = !run ? "no closed run is shown" : control || review.reason;
  return {
    controlHeld,
    next: { ...next, row: upcoming },
    repeat: { reason: repeat, act: () => { if (run) queue.repeat(run.ref); },
      title: run ? `Re-queues ${run.ref} as the next run (run ${q.nextRunIndex}) with the same sample ref; Start picks a new file. Sends nothing.` : "" },
    end,
    review: { reason: reviewReason, act: () => { if (run) review.open(run.path); },
      title: "Opens this file in the classic Review tab (it loads the file there). While a file is open in Review, Start is refused: close it in Review first." },
  };
}

/** The reasons of the unavailable decisions, one line each (a viewer's are the control notice). */
export function DecisionReasons({ d, items }: { d: Decisions; items: [string, Decision][] }) {
  const lines = items.filter(([, x]) => x.reason !== "" && (d.controlHeld || x.reason !== DECISIONS_CONTROL.title)).map(([name, x]) => `${name}: ${x.reason}.`);
  return <>{lines.map((t) => <p key={t} className="ws-note ws-ql__reason">{t}</p>)}</>;
}

/** The control notice a viewer sees above the decisions. */
export function ControlNotice({ d }: { d: Decisions }) {
  return d.controlHeld ? null : <Notice tone={DECISIONS_CONTROL.tone} title={DECISIONS_CONTROL.title} data-testid="summary-control">{DECISIONS_CONTROL.text}</Notice>;
}

/** What the backend says when it has no numbers for a run (deferred, raw, error). */
export function NoNumbers({ q, testId }: { q: RunQuickLookRun | null; testId: string }) {
  if (!q) return null;
  if (q.status === "deferred") return (
    <Notice tone="unknown" title={`Quick look ${DEFERRED_TEXT}`} data-testid={`${testId}-deferred`}>
      {"The backend reads a file it has not cached only while no run or review job is in progress. It is read again here when the run in progress ends; Refresh reads it again now."}
    </Notice>
  );
  if (q.status === "not_available") return (
    <Notice tone="info" title="Not available until exported to HDF5" data-testid={`${testId}-not-available`}>
      {`${q.message || "A raw recording has no quick look"}. Export it to HDF5 to see its numbers.`}
    </Notice>
  );
  if (q.status === "error") return <Notice tone="unknown" title="Quick look not read for this file" data-testid={`${testId}-error`}>{q.message || NOT_RECORDED}</Notice>;
  return null;
}

/** A run's quick look tiles: valid rows, the medians with the backend's deltas, the target group, the rate. */
export function Tiles({ s, q, hideDeltas, testId }: { s: RunStatistics; q: RunQuickLookRun; hideDeltas?: boolean; testId: string }) {
  return (
    <div className="ws-live__stats ws-ql__tiles" data-testid={testId}>
      {quickTiles(s, hideDeltas ? null : q.delta).map((t) => (
        <StatTile key={t.key} label={t.label} value={t.value} missing={t.missing} delta={t.delta} title={t.detail} data-testid={`${testId}-${t.key}`} />
      ))}
    </div>
  );
}

/** The flags and what differs from the reference, as descriptive chips. */
export function Chips({ s, q }: { s: RunStatistics | null; q: RunQuickLookRun | null }) {
  return <>
    {flagChips(s).map((c) => <StateChip key={c.key} size="small" tone={c.tone} data-testid={`flag-${c.key}`}>{c.text}</StateChip>)}
    {differsChips(q?.delta ?? null).map((c) => <StateChip key={`d-${c.key}`} size="small" tone={c.tone} data-testid={`differs-${c.key}`}>{c.text}</StateChip>)}
  </>;
}

/** The read error and the truncation notices both sub-views show. */
export function ReadNotices({ error, left, read }: { error: string; left: number; read: number }): ReactNode {
  return <>
    {error ? <Notice tone="unknown" title="Quick look not read" data-testid="summary-read-error">{error}</Notice> : null}
    {left > 0 ? (
      <Notice tone="info" title={`The newest ${read} runs are shown`} data-testid="summary-truncated">
        {`${left} earlier ${left === 1 ? "run is" : "runs are"} not read here: one read covers at most ${read} files. Review opens them all.`}
      </Notice>
    ) : null}
  </>;
}
