// Pure model of the workspace's guided sample queue (O3/O4, #716 A5; ADR 0015 items 1, 2):
// one experiment, one run and one file per sample, run in sheet order over the existing commands.
//
//   Start (row k) → running → "Next sample" → stop → drain/finalize (observed in the experiment
//   status) → the operator confirms the close → prompt to swap the tube → Start (row k+1).
//
// The machine only records what the operator pressed and what the backend reported; it never
// emits a command. Every action is an event from a button (start, stop, confirm-closed, skip, end,
// resume); the backend's answers are the start and stop replies and the experiment status. Nothing
// advances or starts on its own: after a close the next row waits for its Start, after a failure
// the row waits for the operator's retry, skip or end.
//
// A status belongs to a row's run only when its `output_path` is the row's own file (one file per
// sample) and its start_generation is newer than the read before the row's Start. A newer run on
// another file means another run took over this instrument (another client, the classic view): the
// row is "interrupted", and the queue never stops or confirms that run.
//
// Identity rules (#736): a row's start reply alone decides started / refused / unknown, by its start
// outcome code (bridge ABI 40, #741; experimentStart.ts START_OUTCOME_STATES), or by ok without one
// (an older backend). A started or unknown attempt uses its run_index for good: the next Start always
// sends `nextRunIndex`, which only grows, so a run_index is never sent twice. A refused start recorded
// nothing and keeps the index. A row with any started or unknown attempt is locked (no edit, move or
// removal). An unknown attempt with a code was answered "not started", but by a code the backend can
// return after it opened the run's file: the file may be left behind, so it locks like a lost reply.
//
// Next sample stops only the row's own run (ABI 40): usePlan sends the row's start_generation as
// expectedStartGeneration. A stop outcome RunMismatch means the backend's run is not the row's (another
// run is active, or none is): nothing was stopped, the row is "interrupted" with the backend's reason,
// and the queue sends nothing more for it. A later status that shows the row's own run ending still
// applies (its own generation and file), so a run that ended by itself is followed to its close.
//
// Repeat sample (#716 A7b, the A7 ruling 8): a closed (or finalized, awaiting its confirmation) row
// is re-queued as the next run of the same sample_ref. Its run is kept in `history`, its attempts stay
// (the row stays locked), and the row becomes the current one again, so its Start sends the same
// sample_ref with `nextRunIndex` (the index rules above are unchanged: an index is never sent twice)
// and picks a new file. Only between samples: never while a Start, a run or a stop is in flight, never
// ahead of a failure that waits for the operator's retry, skip or end, and one repeat at a time (a
// second, of a row further down, would not be the next run). It sends nothing.
//
// A lost controlling client pauses the queue (`paused`): every action is refused until the
// operator resumes it, and resuming sends nothing. Status observations still apply while paused,
// because they are backend facts (the server's safe stop, #711, may finalize the run meanwhile).
//
// Pure module: no React or transport imports.

import { EXPERIMENT_STATES, EXPERIMENT_STOP_OUTCOMES, OPERATION_STATES } from "../bridgeContract";
import type { ExperimentStatus } from "../eventAdapter";
import { describeRunOutcome, type OutcomeSeverity } from "../runOutcome";
import { CALIBRATION_REFUSAL_TITLE, calibrationChanged, calibrationChangedText, invalidRequestReason, type PlanDraft } from "./planModel";
import type { PlanStarted, PlanStartResult } from "./usePlan";

export type RowPhase =
  | "planned"   // nothing sent for this row (or a Start that was cancelled)
  | "starting"  // the Start flow runs: file dialog, readiness with the file, the start request
  | "refused"   // the Start was not accepted (refused, not ready, an error before sending): nothing recorded
  | "unknown"   // the start request was sent but its reply was lost: the run may exist
  | "running"   // the start reply said started
  | "stopping"  // Next sample sent the stop, or the status reports the run stopping (drain, finalize)
  | "finalized" // the backend reports the run terminal and its file closed; awaiting the operator's confirmation
  | "failed"    // the run ended failed or its finalization did not complete
  | "interrupted" // another run (not this row's file) was reported: this run's end is not known
  | "closed"    // the operator confirmed the close
  | "skipped";  // the operator skipped the row

/** One Start of a row that used a run index (its reply said started, or was lost). */
export interface RunAttempt {
  runIndex: number;
  outcome: "started" | "unknown";
  /** The file chosen at Start ("" when not known). */
  path: string;
  /** start_generation of the last status read before the Start: the run is the first newer one. */
  generationBefore: string;
  /** The run's start_generation once a status reports it. */
  generation: string | null;
  /** An unknown attempt's start outcome code when its reply was read (not started, but its file
   *  may exist); absent when the reply was lost, and for a started attempt. */
  code?: number;
}

/** How the row's run ended, as the backend reported it (terminal status). */
export interface RowEnd {
  ok: boolean;
  cancelled: boolean;
  /** The file the backend wrote and closed (status.output_path). */
  outputPath: string;
  /** describeRunOutcome's headline ("" when the status says nothing more). */
  headline: string;
  severity: OutcomeSeverity | null;
}

/** Why the flow stopped at this row, shown with the operator's retry / skip / end choice. */
export interface QueueFailure {
  step: "start" | "stop" | "run";
  tone: "blocked" | "unknown" | "warn";
  title: string;
  text: string;
}

/** A run of the row the operator re-queued with Repeat sample: its file was closed and confirmed. */
export interface ClosedRun {
  runIndex: number;
  /** The file the backend wrote and closed (status.output_path). */
  path: string;
  end: RowEnd;
  /** The run's start_generation, as the status reported it. */
  generation: string | null;
}

export interface QueueRow {
  phase: RowPhase;
  attempts: RunAttempt[];
  failure: QueueFailure | null;
  end: RowEnd | null;
  /** The file the backend reports for the running run (status.output_path), once read. */
  outputPath: string;
  /** The stop request is in flight. */
  stopPending: boolean;
  /** Where a cancelled Start goes back to. */
  before: { phase: RowPhase; failure: QueueFailure | null } | null;
  /** The row's earlier closed runs, oldest first, kept by Repeat sample (absent before one). */
  history?: readonly ClosedRun[];
}

export interface QueueState {
  /** Rows by sample ref; a ref without an entry is planned. */
  rows: Readonly<Record<string, QueueRow>>;
  /** The run_index the next Start sends; it only grows. */
  nextRunIndex: number;
  /** Set when the controlling client was lost: the reason. Cleared only by the operator. */
  paused: string | null;
  /** The operator ended the queue: no row runs any more. */
  ended: boolean;
}

export const PLANNED_ROW: QueueRow = Object.freeze({
  phase: "planned", attempts: [], failure: null, end: null, outputPath: "", stopPending: false, before: null,
}) as QueueRow;

export function newQueue(): QueueState {
  return { rows: {}, nextRunIndex: 1, paused: null, ended: false };
}

export const rowState = (q: QueueState, ref: string): QueueRow => q.rows[ref] ?? PLANNED_ROW;

const DONE: ReadonlySet<RowPhase> = new Set(["closed", "skipped"]);
/** Phases from which the current row may be started (a retry from unknown or failed uses a new index). */
const STARTABLE: ReadonlySet<RowPhase> = new Set(["planned", "refused", "unknown", "failed", "interrupted"]);

/** The position of the row the queue is at: the first row not closed or skipped; -1 when the queue
 *  is complete or ended. */
export function currentIndex(order: readonly string[], q: QueueState): number {
  if (q.ended) return -1;
  return order.findIndex((ref) => !DONE.has(rowState(q, ref).phase));
}

/** True once anything was sent for the queue (a Start pressed). */
export const queueBegun = (q: QueueState): boolean => Object.values(q.rows).some((r) => r.phase !== "planned" || r.attempts.length > 0);

/** True while the queue has begun and is neither complete nor ended. */
export const queueInProgress = (order: readonly string[], q: QueueState): boolean => queueBegun(q) && currentIndex(order, q) >= 0;

/** A row the operator may still edit, move or remove: nothing of it was recorded. */
export function rowEditable(q: QueueState, ref: string): boolean {
  const r = rowState(q, ref);
  return (r.phase === "planned" || r.phase === "refused") && r.attempts.length === 0 && !q.ended;
}

/** Whether row i may move one place up (-1) or down (+1): both rows must be editable, so started
 *  rows keep their place and order. */
export function canMove(order: readonly string[], q: QueueState, i: number, by: -1 | 1): boolean {
  const j = i + by;
  return j >= 0 && j < order.length && rowEditable(q, order[i]) && rowEditable(q, order[j]);
}

export const canRemove = (order: readonly string[], q: QueueState, i: number): boolean =>
  order.length > 1 && i >= 0 && i < order.length && rowEditable(q, order[i]);

/** The run index row i would get if every Start from now on is accepted (shown as "planned"). */
export function plannedRunIndex(order: readonly string[], q: QueueState, i: number): number | null {
  const cur = currentIndex(order, q);
  if (cur < 0 || i < cur || !STARTABLE.has(rowState(q, order[i]).phase)) return null;
  const pendingBefore = order.slice(cur, i).filter((ref) => STARTABLE.has(rowState(q, ref).phase)).length;
  return q.nextRunIndex + pendingBefore;
}

// ---- events --------------------------------------------------------------------------------------

export type QueueEvent =
  /** The operator pressed Start (or Retry) for the current row. */
  | { type: "start"; ref: string; runIndex: number; generationBefore: string }
  /** The Start flow's result (decided by the start reply alone). */
  | { type: "start-result"; ref: string; runIndex: number; generationBefore: string; result: PlanStartResult }
  /** The operator pressed Next sample (or Stop last sample). */
  | { type: "stop"; ref: string }
  /** The stop request's reply: accepted, refused (with the backend's message and its stop outcome
   *  code, absent from an older backend), or none (threw). */
  | { type: "stop-result"; ref: string; ok: boolean; message: string; code?: number }
  /** A fresh experiment status read. */
  | { type: "status"; status: ExperimentStatus | null }
  /** The operator confirmed the finalized run's close. */
  | { type: "confirm-closed"; ref: string }
  /** The operator skipped the current row after a failure. */
  | { type: "skip"; ref: string }
  /** The operator ended the queue. */
  | { type: "end" }
  /** Repeat sample: re-queue a closed (or finalized) row as the next run of its sample_ref (sends nothing). */
  | { type: "repeat"; ref: string }
  /** The controlling client was lost. */
  | { type: "pause"; reason: string }
  /** The operator resumed the paused queue (sends nothing). */
  | { type: "resume" };

const big = (s: string): bigint | null => {
  try { return /^\d+$/.test(s) ? BigInt(s) : null; } catch { return null; }
};

function withRow(q: QueueState, ref: string, row: QueueRow): QueueState {
  return { ...q, rows: { ...q.rows, [ref]: row } };
}

/** The failure a Start result stands for; null when it is not one. */
function startFailure(ref: string, runIndex: number, r: PlanStartResult): QueueFailure | null {
  switch (r.kind) {
    case "refused": {
      const invalid = invalidRequestReason(r.message, r.code);
      if (invalid && calibrationChanged(invalid))
        return { step: "start", tone: "blocked", title: CALIBRATION_REFUSAL_TITLE, text: calibrationChangedText(invalid) };
      return invalid
        ? { step: "start", tone: "blocked", title: `Not started · the backend refused ${ref}'s sheet row`, text: `${invalid.label}: ${invalid.text}. No file was created. Correct the row and press Start again.` }
        : { step: "start", tone: "blocked", title: `Not started · the backend refused the Start of ${ref}`, text: `${r.path ? `${r.path}: ` : ""}${r.message}` };
    }
    case "not-ready": return { step: "start", tone: "warn", title: `Not started · readiness with the chosen file`, text: r.message };
    case "error": return { step: "start", tone: "blocked", title: `Not started · an error before the start was sent`, text: r.message };
    case "busy": return { step: "start", tone: "warn", title: `Not started · a start request is already pending`, text: "Wait for it to finish, then press Start again." };
    case "withdrawn": return { step: "start", tone: "warn", title: `Not started · control was lost while the Start of ${ref} was pending`, text: "Nothing was sent. Press Start again once the queue is resumed." };
    case "unknown": return r.code !== undefined ? {
      step: "start", tone: "unknown", title: `Not started · ${ref}, run ${runIndex} · its file may exist${r.path ? ` · ${r.path}` : ""}`,
      text: `The backend did not start the run (${r.message}), but it can answer this way after it opened the run's file, so run ${runIndex} is not sent again. Skip the sample, or retry it as run ${runIndex + 1}.`,
    } : {
      step: "start", tone: "unknown", title: `Start outcome not known · ${ref}, run ${runIndex}${r.path ? ` · ${r.path}` : ""}`,
      text: `The start request was sent but no reply was read (${r.message}). The run may have started, so run ${runIndex} is not sent again. Skip the sample, or retry it as run ${runIndex + 1}.`,
    };
    default: return null;
  }
}

/** The file the backend records for a chosen path: it appends ".h5" unless the path ends in ".h5"
 *  or ".hdf5" (ExperimentCoordinator::start), so the status's output_path is compared with this. */
export function recordedPath(chosen: string): string {
  return chosen.endsWith(".h5") || chosen.endsWith(".hdf5") ? chosen : `${chosen}.h5`;
}

/** The row taken over by another run: its own run's end is not known, and that run is not the
 *  queue's to stop or confirm. */
function interrupted(ref: string, row: QueueRow, last: RunAttempt, s: ExperimentStatus): QueueRow {
  return {
    ...row, phase: "interrupted", end: null,
    failure: { step: "run", tone: "unknown", title: `${ref} · interrupted · another run started on this instrument`,
      text: `The backend now reports another run${s.output_path ? ` (${s.output_path})` : ""}, not ${ref}'s file ${recordedPath(last.path) || "(unknown)"}. `
        + `The queue does not stop or confirm that run, and does not know how run ${last.runIndex} ended: check its file before going on.` },
  };
}

/** A row whose Next sample the backend refused as RunMismatch: it still watches for its own run. */
const stopMismatched = (row: QueueRow): boolean => row.phase === "interrupted" && row.failure?.step === "stop";

/** The status read applied to the row's started run (see the header: own file, newer generation). */
function observe(ref: string, row: QueueRow, s: ExperimentStatus): QueueRow {
  if (row.phase !== "running" && row.phase !== "stopping" && !stopMismatched(row)) return row;
  const last = row.attempts[row.attempts.length - 1];
  if (!last || last.outcome !== "started") return row;
  const gen = big(s.start_generation);
  if (gen === null) return row;
  const mine = last.path !== "" && s.output_path === recordedPath(last.path);
  let attempt = last;
  if (last.generation === null) {
    const before = big(last.generationBefore);
    if (before === null || gen <= before) return row; // an earlier run's status, or a read from before the start
    if (!mine) return interrupted(ref, row, last, s); // a newer run on another file took over before this one was seen
    attempt = { ...last, generation: s.start_generation };
  } else {
    const seen = big(last.generation);
    if (seen === null || gen < seen) return row; // an older read answering late
    // A later run: this run's end was not seen. After a refused stop the row already says so, with the backend's reason.
    if (gen > seen || !mine) return stopMismatched(row) ? row : interrupted(ref, row, last, s);
  }
  const attempts = [...row.attempts.slice(0, -1), attempt];
  const outputPath = s.output_path;
  const terminal = s.terminal && (s.state === EXPERIMENT_STATES.Idle || s.state === EXPERIMENT_STATES.Failed);
  if (terminal) {
    const ok = s.finalization_ok && s.state !== EXPERIMENT_STATES.Failed && !s.fault_code;
    const outcome = describeRunOutcome(s);
    const end: RowEnd = { ok, cancelled: s.cancelled, outputPath, headline: outcome?.headline ?? "", severity: outcome?.severity ?? null };
    const why = s.fault_message || s.completion_reason || s.message || "its finalization did not complete";
    return {
      ...row, attempts, outputPath, end, phase: ok ? "finalized" : "failed",
      failure: ok ? null : { step: "run", tone: "blocked", title: `${ref} · run ${attempt.runIndex} failed`, text: why },
    };
  }
  if (stopMismatched(row) && !(s.state === EXPERIMENT_STATES.Stopping || s.flushing)) return row; // an older read: the backend said this run is not active
  if (s.state === EXPERIMENT_STATES.Stopping || s.flushing) {
    // Stopping, whoever stopped it (Next sample, the classic Stop, the server's safe stop): a stop
    // that was said to fail is moot now.
    return { ...row, attempts, outputPath, phase: "stopping", failure: row.failure?.step === "stop" ? null : row.failure };
  }
  return { ...row, attempts, outputPath };
}

/** Whether a status shows the row's own run active, seen before (generation and file): the only
 *  state in which Next sample may send the stop, since `experiment_stop` stops whatever run is
 *  active. */
export function ownRunActive(row: QueueRow, s: ExperimentStatus | null): boolean {
  const last = row.attempts[row.attempts.length - 1];
  return !!s?.valid && row.phase === "running" && !!last && last.outcome === "started" && last.generation !== null
    && s.start_generation === last.generation && s.output_path === recordedPath(last.path)
    && s.state === EXPERIMENT_STATES.Active && !s.terminal;
}

/** The queue after one event. Events that do not apply (a guard fails) leave it unchanged, so a
 *  stale or repeated button press does nothing. `order` is the sheet's refs in run order. */
export function reduceQueue(q: QueueState, e: QueueEvent, order: readonly string[]): QueueState {
  const cur = currentIndex(order, q);
  const curRef = cur >= 0 ? order[cur] : null;
  switch (e.type) {
    case "start": {
      if (q.paused || e.ref !== curRef || e.runIndex !== q.nextRunIndex) return q;
      if (Object.values(q.rows).some((r) => r.phase === "starting")) return q;
      const row = rowState(q, e.ref);
      if (!STARTABLE.has(row.phase)) return q;
      return withRow(q, e.ref, { ...row, phase: "starting", failure: null, before: { phase: row.phase, failure: row.failure } });
    }
    case "start-result": {
      const row = rowState(q, e.ref);
      if (row.phase !== "starting") return q;
      const r = e.result;
      if (r.kind === "started" || r.kind === "unknown") {
        const attempt: RunAttempt = { runIndex: e.runIndex, outcome: r.kind, path: r.path, generationBefore: e.generationBefore, generation: null };
        if (r.kind === "unknown" && r.code !== undefined) attempt.code = r.code;
        const next = { ...q, nextRunIndex: Math.max(q.nextRunIndex, e.runIndex + 1) };
        return withRow(next, e.ref, {
          ...row, attempts: [...row.attempts, attempt], before: null, end: null, outputPath: "",
          phase: r.kind === "started" ? "running" : "unknown",
          failure: r.kind === "started" ? null : startFailure(e.ref, e.runIndex, r),
        });
      }
      if (r.kind === "cancelled") {
        const back = row.before ?? { phase: "planned" as RowPhase, failure: null };
        return withRow(q, e.ref, { ...row, phase: back.phase, failure: back.failure, before: null });
      }
      return withRow(q, e.ref, { ...row, phase: "refused", failure: startFailure(e.ref, e.runIndex, r), before: null });
    }
    case "stop": {
      if (q.paused || e.ref !== curRef) return q;
      const row = rowState(q, e.ref);
      const last = row.attempts[row.attempts.length - 1];
      if (row.phase !== "running" || row.stopPending || !last || last.generation === null) return q; // not before its run was seen
      return withRow(q, e.ref, { ...row, phase: "stopping", stopPending: true, failure: null });
    }
    case "stop-result": {
      const row = rowState(q, e.ref);
      if (!row.stopPending) return q;
      if (e.ok || row.phase !== "stopping") return withRow(q, e.ref, { ...row, stopPending: false });
      if (e.code === EXPERIMENT_STOP_OUTCOMES.RunMismatch) {
        // The backend's run is not this row's (another run is active, or none is): nothing was
        // stopped. The queue never sends the stop again for this row, with or without the generation.
        const last = row.attempts[row.attempts.length - 1];
        return withRow(q, e.ref, {
          ...row, stopPending: false, phase: "interrupted",
          failure: { step: "stop", tone: "unknown", title: `${e.ref} · not stopped · run ${last?.runIndex ?? "?"} is not the backend's active run`,
            text: `The backend refused Next sample for this row's run: ${e.message}. The queue stops only its own run, so nothing was stopped and nothing more is sent. `
              + `Check how run ${last?.runIndex ?? "?"} ended (its file ${last ? recordedPath(last.path) : "(unknown)"}) before going on.` },
        });
      }
      // Not accepted (or no reply): the run is taken as still running until a status says otherwise.
      return withRow(q, e.ref, {
        ...row, stopPending: false, phase: "running",
        failure: { step: "stop", tone: "blocked", title: `${e.ref} · the stop was not accepted`, text: `${e.message}. The run is shown as running until the status says otherwise. Press Next sample to send the stop again.` },
      });
    }
    case "status": {
      // The row whose run is running or stopping (at most one; it can outlive an ended queue).
      const status = e.status;
      if (!status?.valid) return q;
      const ref = order.find((r) => { const row = rowState(q, r); return row.phase === "running" || row.phase === "stopping" || stopMismatched(row); });
      if (ref === undefined) return q;
      const row = rowState(q, ref);
      const next = observe(ref, row, status);
      return next === row ? q : withRow(q, ref, next);
    }
    case "confirm-closed": {
      if (q.paused || e.ref !== curRef) return q;
      const row = rowState(q, e.ref);
      if (row.phase !== "finalized" || row.stopPending) return q;
      return withRow(q, e.ref, { ...row, phase: "closed", failure: null });
    }
    case "skip": {
      if (q.paused || e.ref !== curRef) return q;
      const row = rowState(q, e.ref);
      if (row.phase !== "refused" && row.phase !== "unknown" && row.phase !== "failed" && row.phase !== "interrupted") return q;
      return withRow(q, e.ref, { ...row, phase: "skipped", failure: null });
    }
    case "repeat": {
      if (repeatReason(order, q, e.ref) !== "") return q;
      const row = rowState(q, e.ref);
      const last = row.attempts[row.attempts.length - 1];
      const closed: ClosedRun = { runIndex: last.runIndex, path: row.end!.outputPath, end: row.end!, generation: last.generation };
      return withRow(q, e.ref, {
        ...row, phase: "planned", end: null, outputPath: "", failure: null, before: null, stopPending: false,
        history: [...(row.history ?? []), closed],
      });
    }
    case "end": {
      if (q.paused || q.ended || !canEnd(order, q)) return q;
      return { ...q, ended: true };
    }
    case "pause": {
      if (q.paused || !queueInProgress(order, q)) return q;
      return { ...q, paused: e.reason };
    }
    case "resume":
      return q.paused ? { ...q, paused: null } : q;
    default:
      return q;
  }
}

/** Whether the operator may end the queue now: between samples, at a failure (including a stop
 *  that was not accepted, where the run keeps running), never while a Start is pending, a run runs
 *  or a finalized run waits for its close confirmation. */
export function canEnd(order: readonly string[], q: QueueState): boolean {
  const cur = currentIndex(order, q);
  if (cur < 0) return false;
  const row = rowState(q, order[cur]);
  if (row.stopPending) return false;
  return row.phase === "planned" || row.phase === "refused" || row.phase === "unknown" || row.phase === "failed" || row.phase === "interrupted"
    || (row.phase === "running" && row.failure?.step === "stop");
}

/** Why Repeat sample is unavailable for this row ("" when it may be pressed): the row's run must be
 *  closed by the backend (finalized, the current row, or closed) and the queue between samples. */
export function repeatReason(order: readonly string[], q: QueueState, ref: string): string {
  if (q.ended) return "the queue was ended: a new plan mints a new experiment id";
  if (q.paused) return "the queue is paused: resume it first";
  if (!order.includes(ref)) return `${ref} is not in the sheet`;
  const row = rowState(q, ref);
  const last = row.attempts[row.attempts.length - 1];
  if (!row.end || !last || last.outcome !== "started" || (row.phase !== "finalized" && row.phase !== "closed"))
    return `${ref}'s run is not closed`;
  if (row.stopPending || Object.values(q.rows).some((r) => r.phase === "starting")) return "a Start or a stop is pending";
  const cur = currentIndex(order, q);
  if (row.phase === "finalized") return cur >= 0 && order[cur] === ref ? "" : `${ref}'s run is not the queue's current row`;
  if (cur < 0) return "";
  const at = rowState(q, order[cur]);
  // One repeat at a time: a second would not be the next run (the sheet order decides which row is next).
  if (at.history?.length) return `the repeat of ${order[cur]} is queued: start it, or end the queue, first`;
  if (at.phase === "planned" || at.phase === "refused") return "";
  return `${order[cur]} is ${rowChip(at, q.ended).text.toLowerCase()}: repeat a sample between samples`;
}

/** The queue's closed runs in run order: each row's earlier runs (Repeat sample) and its own run
 *  once the backend finalized it (awaiting the operator's confirmation, or closed). */
export function queueClosedRuns(order: readonly string[], q: QueueState): { ref: string; run: ClosedRun; confirmed: boolean }[] {
  const out: { ref: string; run: ClosedRun; confirmed: boolean }[] = [];
  for (const ref of order) {
    const row = rowState(q, ref);
    for (const run of row.history ?? []) out.push({ ref, run, confirmed: true });
    const last = row.attempts[row.attempts.length - 1];
    if ((row.phase === "finalized" || row.phase === "closed") && row.end && last)
      out.push({ ref, run: { runIndex: last.runIndex, path: row.end.outputPath, end: row.end, generation: last.generation }, confirmed: row.phase === "closed" });
  }
  return out.sort((a, b) => a.run.runIndex - b.run.runIndex);
}

// ---- presentation (pure) -------------------------------------------------------------------------

export type ChipTone = "confirmed" | "warn" | "blocked" | "unknown" | "unsupported" | "info" | "neutral";

/** An unknown row whose last start reply was read (a code that locks, START_OUTCOME_STATES), not lost. */
export function notStarted(row: QueueRow): boolean {
  const last = row.attempts[row.attempts.length - 1];
  return row.phase === "unknown" && last?.outcome === "unknown" && last.code !== undefined;
}

/** An interrupted row: another run was reported, or the backend refused its Next sample as RunMismatch
 *  (its run was not the active one: another run was, or none). */
const interruptedText = (row: QueueRow): string => (stopMismatched(row) ? "Interrupted · not stopped" : "Interrupted · another run");

/** A row's state as the queue table shows it. */
export function rowChip(row: QueueRow, ended: boolean): { tone: ChipTone; text: string } {
  switch (row.phase) {
    case "planned": return ended ? { tone: "neutral", text: row.history?.length ? "Repeat not run" : "Not run" }
      : { tone: "neutral", text: row.history?.length ? "Repeat planned" : "Planned" };
    case "starting": return { tone: "info", text: "Starting" };
    case "refused": return { tone: "blocked", text: "Not started" };
    case "unknown": return notStarted(row) ? { tone: "unknown", text: "Not started · file may exist" } : { tone: "unknown", text: "Unknown · reply lost" };
    case "running": return row.failure ? { tone: "warn", text: "Running · stop not accepted" } : { tone: "info", text: "Running" };
    case "stopping": return { tone: "info", text: "Stopping · draining" };
    case "finalized": return { tone: "info", text: "Finalized · confirm close" };
    case "failed": return { tone: "blocked", text: "Failed" };
    case "interrupted": return { tone: "unknown", text: interruptedText(row) };
    case "closed": return row.end?.severity && row.end.severity !== "ok" && row.end.severity !== "partial"
      ? { tone: "warn", text: "Closed · see outcome" } : { tone: "confirmed", text: "Closed" };
    case "skipped": return { tone: "neutral", text: row.attempts.length ? "Skipped · after a run" : "Skipped" };
  }
}

export interface LifecycleStep {
  key: "planned" | "starting" | "running" | "stopping" | "finalized" | "closed";
  label: string;
  state: "done" | "current" | "pending" | "failed";
  /** A kit tone for a refused, unknown or failed step (shown as a chip in the label). */
  flag?: { tone: ChipTone; text: string };
}

const STEP_LABELS: Readonly<Record<LifecycleStep["key"], string>> = {
  planned: "Planned",
  starting: "Starting",
  running: "Running",
  stopping: "Stopping · drain",
  finalized: "Finalized",
  closed: "Closed",
};
const STEP_KEYS = ["planned", "starting", "running", "stopping", "finalized", "closed"] as const;

/** The current row's lifecycle for the kit Stepper: Planned → Starting → Running → Stopping →
 *  Finalized → Closed. A step is done only when the backend (or, for Closed, the operator)
 *  confirmed it; refused, unknown and failed show their own tone. */
export function lifecycle(row: QueueRow): LifecycleStep[] {
  // How far the row got: index of the current step, and its state.
  const at: Record<RowPhase, [number, LifecycleStep["state"], LifecycleStep["flag"]?]> = {
    planned: [0, "current"],
    starting: [1, "current"],
    refused: [1, "failed", { tone: "blocked", text: "Not started" }],
    unknown: [1, "current", notStarted(row) ? { tone: "unknown", text: "Not started · file may exist" } : { tone: "unknown", text: "Unknown · reply lost" }],
    running: [2, "current", row.failure ? { tone: "warn", text: "Stop not accepted" } : undefined],
    stopping: [3, "current"],
    finalized: [5, "current"],
    failed: [4, "failed", { tone: "blocked", text: "Failed" }],
    interrupted: [2, "failed", { tone: "unknown", text: interruptedText(row) }],
    closed: [5, "done"],
    skipped: [0, "pending", { tone: "neutral", text: "Skipped" }],
  };
  const [index, state, flag] = at[row.phase];
  return STEP_KEYS.map((key, i) => {
    const step: LifecycleStep = { key, label: STEP_LABELS[key], state: i < index ? "done" : i === index ? state : "pending" };
    if (i === index && flag) step.flag = flag;
    return step;
  });
}

// ---- editing the sheet ---------------------------------------------------------------------------


/** Whether an edit of the plan keeps everything already recorded: once the queue has begun, the
 *  experiment id, the format and the operator are fixed, and every locked row (one with a started
 *  or unknown attempt, or past its Start) keeps its place and its values. */
export function editKeepsRecorded(prev: PlanDraft, next: PlanDraft, q: QueueState): boolean {
  if (!queueBegun(q) && !q.ended) return true;
  if (next.experimentId !== prev.experimentId || next.format !== prev.format) return false;
  if (next.operator.name !== prev.operator.name || next.operator.id !== prev.operator.id) return false;
  for (let i = 0; i < prev.rows.length; i++) {
    const row = prev.rows[i];
    if (rowEditable(q, row.ref)) continue;
    if (JSON.stringify(next.rows[i]) !== JSON.stringify(row)) return false;
  }
  return true;
}

// ---- the single-sample run (#763 F14) ------------------------------------------------------------

/** An experiment operation's end: its terminal OperationStatus event (BE-1), with the state
 *  (OPERATION_STATES Completed, Failed, Cancelled or TimedOut) and the event's message. */
export interface OperationEnd { state: number; message: string }

/** What App has seen of the experiment operations: their ends by operation id, and how many times
 *  events may have been lost since the page loaded (a notification overflow, a failed poll, a
 *  dropped connection to the server). */
export interface OperationTrack { ends: ReadonlyMap<string, OperationEnd>; loss: number }

export const NO_OPERATIONS: OperationTrack = { ends: new Map(), loss: 0 };

/** How many operation ends App keeps (the most recent): a sample keeps its own end once read. */
export const OPERATION_ENDS_KEPT = 64;

/** `ends` with one more operation's end (the newest last, the oldest dropped past the bound). */
export function withOperationEnd(ends: ReadonlyMap<string, OperationEnd>, id: string, end: OperationEnd): ReadonlyMap<string, OperationEnd> {
  const next = new Map(ends);
  next.delete(id);
  next.set(id, end);
  for (const old of next.keys()) {
    if (next.size <= OPERATION_ENDS_KEPT) break;
    next.delete(old);
  }
  return next;
}

/** The single-sample plan's run, followed only through the identity its start reply returned: the
 *  experiment operation the backend tracks that run under (StartResult.operation_id). The backend
 *  finishes that operation when the run's status turns terminal (Failed when the run failed or
 *  did not finalize, else Cancelled or Completed), so its terminal event is this run's end and no
 *  other's. The experiment status is never used: it names a start generation, which the start
 *  reply does not return, and a status on the same file, or with a newer generation, can be
 *  another client's run (Codex #766 review). Without an operation in the reply, or once events may
 *  have been lost since the Start and its end was not seen, the outcome is not known
 *  (`endLost`); an end that still arrives is taken. Returns `st` itself when nothing is new; the
 *  end, once read, stays. */
export function followSingleRun(st: PlanStarted, ops: OperationTrack): PlanStarted {
  if (st.outcome !== "started" || st.end || !st.operationId) return st;
  const e = ops.ends.get(st.operationId);
  if (e && e.state >= OPERATION_STATES.Completed) { // only a terminal state ends it
    const cancelled = e.state === OPERATION_STATES.Cancelled;
    return { ...st, end: { ok: e.state === OPERATION_STATES.Completed || cancelled, cancelled, message: e.message } };
  }
  if (!st.endLost && ops.loss > (st.lossBefore ?? 0)) return { ...st, endLost: true };
  return st;
}

/** The single sample's outcome when no end can be read for it. */
export const SINGLE_OUTCOME_UNKNOWN = "Outcome unknown · check the file";

/** The single sample's state on Live's queue card: its end once its operation reported it, else
 *  what its start reply said. The run's progress (running, draining) is the status's, shown by
 *  the run card, not attributed to the sample. */
export function singleRunChip(st: PlanStarted | null): { tone: ChipTone; text: string; title?: string } {
  if (!st) return { tone: "neutral", text: "Planned" };
  if (st.end) {
    if (!st.end.ok) return { tone: "blocked", text: "Failed", title: st.end.message || undefined };
    return { tone: "confirmed", text: st.end.cancelled ? "Finalized · cancelled" : "Finalized · file closed", title: st.end.message || undefined };
  }
  if (st.outcome !== "started") return { tone: "unknown", text: "Start outcome not known" };
  if (!st.operationId) return { tone: "unknown", text: SINGLE_OUTCOME_UNKNOWN, title: "The start reply named no operation to follow the run by: open its file to see how it ended." };
  if (st.endLost) return { tone: "unknown", text: SINGLE_OUTCOME_UNKNOWN, title: "Notifications may have been lost since the Start, the run's end among them: open its file to see how it ended." };
  return { tone: "info", text: "Started" };
}
