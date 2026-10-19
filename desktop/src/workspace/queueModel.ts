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
// Identity rules (#736): a row's start reply alone decides started / refused / unknown. A started
// or unknown attempt uses its run_index for good: the next Start always sends `nextRunIndex`, which
// only grows, so a run_index is never sent twice. A refused start recorded nothing and keeps the
// index. A row with any started or unknown attempt is locked (no edit, move or removal).
//
// A lost controlling client pauses the queue (`paused`): every action is refused until the
// operator resumes it, and resuming sends nothing. Status observations still apply while paused,
// because they are backend facts (the server's safe stop, #711, may finalize the run meanwhile).
//
// Pure module: no React or transport imports.

import { EXPERIMENT_STATES } from "../bridgeContract";
import type { ExperimentStatus } from "../eventAdapter";
import { describeRunOutcome, type OutcomeSeverity } from "../runOutcome";
import { invalidRequestReason, type PlanDraft } from "./planModel";
import type { PlanStartResult } from "./usePlan";

export type RowPhase =
  | "planned"   // nothing sent for this row (or a Start that was cancelled)
  | "starting"  // the Start flow runs: file dialog, readiness with the file, the start request
  | "refused"   // the Start was not accepted (refused, not ready, an error before sending): nothing recorded
  | "unknown"   // the start request was sent but its reply was lost: the run may exist
  | "running"   // the start reply said started
  | "stopping"  // Next sample sent the stop, or the status reports the run stopping (drain, finalize)
  | "finalized" // the backend reports the run terminal and its file closed; awaiting the operator's confirmation
  | "failed"    // the run ended failed or its finalization did not complete
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
const STARTABLE: ReadonlySet<RowPhase> = new Set(["planned", "refused", "unknown", "failed"]);

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
  /** The stop request's reply: accepted, refused (with the backend's message), or none (threw). */
  | { type: "stop-result"; ref: string; ok: boolean; message: string }
  /** A fresh experiment status read. */
  | { type: "status"; status: ExperimentStatus | null }
  /** The operator confirmed the finalized run's close. */
  | { type: "confirm-closed"; ref: string }
  /** The operator skipped the current row after a failure. */
  | { type: "skip"; ref: string }
  /** The operator ended the queue. */
  | { type: "end" }
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
      const invalid = invalidRequestReason(r.message);
      return invalid
        ? { step: "start", tone: "blocked", title: `Not started · the backend refused ${ref}'s sheet row`, text: `${invalid.label}: ${invalid.text}. No file was created. Correct the row and press Start again.` }
        : { step: "start", tone: "blocked", title: `Not started · the backend refused the Start of ${ref}`, text: `${r.path ? `${r.path}: ` : ""}${r.message}` };
    }
    case "not-ready": return { step: "start", tone: "warn", title: `Not started · readiness with the chosen file`, text: r.message };
    case "error": return { step: "start", tone: "blocked", title: `Not started · an error before the start was sent`, text: r.message };
    case "busy": return { step: "start", tone: "warn", title: `Not started · a start request is already pending`, text: "Wait for it to finish, then press Start again." };
    case "unknown": return {
      step: "start", tone: "unknown", title: `Start outcome not known · ${ref}, run ${runIndex}${r.path ? ` · ${r.path}` : ""}`,
      text: `The start request was sent but no reply was read (${r.message}). The run may have started, so run ${runIndex} is not sent again. Skip the sample, or retry it as run ${runIndex + 1}.`,
    };
    default: return null;
  }
}

/** The status read applied to the current row's started run. */
function observe(ref: string, row: QueueRow, s: ExperimentStatus): QueueRow {
  if (row.phase !== "running" && row.phase !== "stopping") return row;
  const last = row.attempts[row.attempts.length - 1];
  if (!last || last.outcome !== "started") return row;
  const gen = big(s.start_generation);
  if (gen === null) return row;
  let attempt = last;
  if (last.generation === null) {
    const before = big(last.generationBefore);
    if (before !== null && gen <= before) return row; // an earlier run's status, or a read from before the start
    attempt = { ...last, generation: s.start_generation };
  } else {
    const mine = big(last.generation);
    if (mine === null || gen < mine) return row; // an older read answering late
    if (gen > mine) {
      return {
        ...row, phase: "failed", stopPending: false, end: null,
        failure: { step: "run", tone: "unknown", title: `${ref} · the end of run ${last.runIndex} was not seen`,
          text: `The backend now reports a later run${s.output_path ? ` (${s.output_path})` : ""}. Check run ${last.runIndex}'s file before going on.` },
      };
    }
  }
  const attempts = [...row.attempts.slice(0, -1), attempt];
  const outputPath = s.output_path || row.outputPath;
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
  if (s.state === EXPERIMENT_STATES.Stopping || s.flushing) {
    // Stopping, whoever stopped it (Next sample, the classic Stop, the server's safe stop): a stop
    // that was said to fail is moot now.
    return { ...row, attempts, outputPath, phase: "stopping", failure: row.failure?.step === "stop" ? null : row.failure };
  }
  return { ...row, attempts, outputPath };
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
      if (row.phase !== "running" || row.stopPending) return q;
      return withRow(q, e.ref, { ...row, phase: "stopping", stopPending: true, failure: null });
    }
    case "stop-result": {
      const row = rowState(q, e.ref);
      if (!row.stopPending) return q;
      if (e.ok || row.phase !== "stopping") return withRow(q, e.ref, { ...row, stopPending: false });
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
      const ref = order.find((r) => { const p = rowState(q, r).phase; return p === "running" || p === "stopping"; });
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
      if (row.phase !== "refused" && row.phase !== "unknown" && row.phase !== "failed") return q;
      return withRow(q, e.ref, { ...row, phase: "skipped", failure: null });
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
  return row.phase === "planned" || row.phase === "refused" || row.phase === "unknown" || row.phase === "failed"
    || (row.phase === "running" && row.failure?.step === "stop");
}

// ---- presentation (pure) -------------------------------------------------------------------------

export type ChipTone = "confirmed" | "warn" | "blocked" | "unknown" | "unsupported" | "info" | "neutral";

/** A row's state as the queue table shows it. */
export function rowChip(row: QueueRow, ended: boolean): { tone: ChipTone; text: string } {
  switch (row.phase) {
    case "planned": return ended ? { tone: "neutral", text: "Not run" } : { tone: "neutral", text: "Planned" };
    case "starting": return { tone: "info", text: "Starting" };
    case "refused": return { tone: "blocked", text: "Not started" };
    case "unknown": return { tone: "unknown", text: "Unknown · reply lost" };
    case "running": return row.failure ? { tone: "warn", text: "Running · stop not accepted" } : { tone: "info", text: "Running" };
    case "stopping": return { tone: "info", text: "Stopping · draining" };
    case "finalized": return { tone: "info", text: "Finalized · confirm close" };
    case "failed": return { tone: "blocked", text: "Failed" };
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
    unknown: [1, "current", { tone: "unknown", text: "Unknown · reply lost" }],
    running: [2, "current", row.failure ? { tone: "warn", text: "Stop not accepted" } : undefined],
    stopping: [3, "current"],
    finalized: [5, "current"],
    failed: [4, "failed", { tone: "blocked", text: "Failed" }],
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
