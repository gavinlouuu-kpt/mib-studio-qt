// Pure model of the O5 Run summary's hand-off parts (#716 A7c; Penpot board O5): the Outcome card's
// lifecycle checklist, stop reason, duration and writer, the experiment board's placeholder tiles
// (O6), the Next card's next sample and End queue, and the calibration record derived from a run.
//
// Truth rules:
// - Every checklist step comes from what this page observed: the start reply, the experiment status
//   of the run's own generation and file (queueModel RunSeen, the terminal status), the operator's
//   close confirmation, or a single sample's operation end. A step that was not observed reads
//   "unknown", never ticked: a status read every half second can miss a short Active or Stopping.
// - The duration comes only from the terminal status's recorded start and end (the backend's wall
//   clock); without both it is "Not recorded". This module reads no quick look value: the quick
//   look's numbers are formatted elsewhere and never computed on (quickLookModel.test.ts pins those
//   sources; runSummaryModel.test.ts checks this module imports nothing of the quick look).
// - The calibration record is matched by its source run_id, as the backend recorded it; nothing is
//   applied from here (Apply lives on Prepare's Calibration card).
//
// Pure module: no React or transport imports.

import type { CalibrationRecord, CalibrationRecordsList } from "../calibrationRecords";
import type { SampleRow } from "./planModel";
import type { PlanRun } from "./quickLookModel";
import { canEnd, currentIndex, queueBegun, rowChip, rowState, type ChipTone, type QueueState } from "./queueModel";
import type { PlanStarted } from "./usePlan";

// ---- the Outcome card --------------------------------------------------------------------------

export type CheckState = "done" | "current" | "unknown";

export interface CheckStep {
  key: "requested" | "acknowledged" | "active" | "stopping" | "drain" | "closed";
  label: string;
  state: CheckState;
  /** Where the state comes from, in plain words. */
  detail: string;
}

const LABELS: Readonly<Record<CheckStep["key"], string>> = {
  requested: "Start requested",
  acknowledged: "Acknowledged · first frame seen",
  active: "Active · recording",
  stopping: "Stopping",
  drain: "Drain · finalizing",
  closed: "Confirmed closed",
};

const NOT_FOLLOWED = "not observed: a single sample is followed by its operation's end, not the run status";
const MISSED = "not observed: the run status is read about twice a second and can miss a short state";

/** The closed run's lifecycle, each step from what this page observed (unknown when it was not). */
export function outcomeChecklist(run: PlanRun): CheckStep[] {
  const step = (key: CheckStep["key"], state: CheckState, detail: string): CheckStep => ({ key, label: LABELS[key], state, detail });
  const requested = step("requested", "done", `this plan sent the Start of run ${run.runIndex}`);
  if (run.followed === "operation") {
    return [
      requested,
      step("acknowledged", "unknown", `the start reply said started; first frame ${NOT_FOLLOWED}`),
      step("active", "unknown", NOT_FOLLOWED),
      step("stopping", "unknown", NOT_FOLLOWED),
      step("drain", "done", "the run's operation ended: the backend ends it once the run is finalized"),
      step("closed", "done", "the backend closed the file (its operation ended); a single sample has no close confirmation"),
    ];
  }
  const seen = run.seen ?? {};
  return [
    requested,
    seen.frame
      ? step("acknowledged", "done", "the start reply said started, and a status of this run showed its first frames")
      : step("acknowledged", "unknown", `the start reply said started; first frame ${MISSED}`),
    seen.active ? step("active", "done", "a status of this run showed it Active") : step("active", "unknown", MISSED),
    seen.stopAccepted ? step("stopping", "done", "the backend accepted this page's Next sample for this run")
      : seen.stopping ? step("stopping", "done", "a status of this run showed it Stopping")
      : step("stopping", "unknown", MISSED),
    step("drain", "done", "the run's terminal status reported its finalization complete"),
    run.confirmed ? step("closed", "done", "you confirmed the backend's close")
      : step("closed", "current", "finalized and closed by the backend · awaiting your close confirmation"),
  ];
}

/** Why the run stopped, as far as this page observed it; null when it did not. */
export function stopReason(run: PlanRun): string | null {
  if (run.outcome.cancelled) return "cancelled";
  if (run.seen?.stopAccepted) return "Next sample · this page's stop, accepted by the backend";
  return null;
}

/** What the stop reason reads when this page did not observe one. */
export const STOP_REASON_UNKNOWN = "not observed here · the classic view, another client or the server may have stopped it; the file's stop record names the path";

const NOT_RECORDED = "Not recorded";
const digits = /^\d{1,20}$/;

/** The run's duration from the terminal status's recorded start and end ("4 min 21 s"); "Not
 *  recorded" without both (a single sample, a row ended before this was kept, or a zero). */
export function durationText(run: PlanRun): string {
  const t = run.times;
  if (!t || !digits.test(t.startNs) || !digits.test(t.endNs)) return NOT_RECORDED;
  const start = BigInt(t.startNs), end = BigInt(t.endNs);
  if (start === 0n || end === 0n || end < start) return NOT_RECORDED;
  const tenths = (end - start) / 100_000_000n;
  const s = tenths / 10n;
  if (s < 60n) return `${s}.${tenths % 10n} s`;
  const h = s / 3600n, m = (s % 3600n) / 60n, sec = s % 60n;
  return h > 0n ? `${h} h ${m} min ${sec} s` : `${m} min ${sec} s`;
}

/** Where the duration comes from (a tooltip). */
export const DURATION_SOURCE = "From the run's terminal status: the backend's recorded start and end (wall clock)";

/** The writer's state, as observed: the file closed with its finalization complete. */
export function writerText(run: PlanRun): string {
  return run.followed === "operation"
    ? "closed · the run's operation ended (the backend finalizes first)"
    : "closed · finalization complete (terminal status)";
}

// ---- the experiment board (O6): closed runs and placeholders in plan order -----------------------

export type BoardEntry =
  | { kind: "run"; run: PlanRun }
  | { kind: "active"; ref: string | null }
  | { kind: "placeholder"; ref: string; row: SampleRow; text: string; tone: ChipTone };

const IN_FLIGHT = new Set(["starting", "running", "stopping"]);

/** The board's tiles in plan order: each row's closed runs (those shown), then its run in progress
 *  or a grey placeholder for a run not yet made ("Queued", no numbers). A single sample has its run,
 *  or one placeholder. `active`: a run is in progress (the status), shown where it belongs, or last
 *  when it is not this plan's. */
export function boardEntries(p: { draft: { format: string; rows: readonly SampleRow[] }; queue: QueueState; started: PlanStarted | null },
  shown: readonly PlanRun[], active: boolean): BoardEntry[] {
  const out: BoardEntry[] = [];
  let placedActive = false;
  if (p.draft.format !== "queue") {
    const row = p.draft.rows[0];
    out.push(...shown.map((run) => ({ kind: "run" as const, run })));
    if (shown.length === 0 && row) {
      if (active && p.started && !p.started.end) { out.push({ kind: "active", ref: row.ref }); placedActive = true; }
      else out.push({ kind: "placeholder", ref: row.ref, row, tone: "neutral",
        text: !p.started ? "Planned" : p.started.end ? (p.started.end.ok ? "Closed · not read" : "Failed · no numbers") : "Started · not closed" });
    }
    if (active && !placedActive) out.push({ kind: "active", ref: null });
    return out;
  }
  const q = p.queue;
  for (const row of p.draft.rows) {
    out.push(...shown.filter((r) => r.ref === row.ref).map((run) => ({ kind: "run" as const, run })));
    const st = rowState(q, row.ref);
    if (IN_FLIGHT.has(st.phase)) {
      if (active) { out.push({ kind: "active", ref: row.ref }); placedActive = true; }
      else out.push({ kind: "placeholder", ref: row.ref, row, tone: "neutral", text: rowChip(st, q.ended).text });
      continue;
    }
    if (st.phase === "finalized" || st.phase === "closed") continue;
    const chip = rowChip(st, q.ended);
    const queued = st.phase === "planned" && !q.ended;
    out.push({ kind: "placeholder", ref: row.ref, row, tone: "neutral", text: queued ? (st.history?.length ? "Repeat queued" : "Queued") : chip.text });
  }
  if (active && !placedActive) out.push({ kind: "active", ref: null });
  return out;
}

// ---- the Next card (O5) ------------------------------------------------------------------------

/** The sample the next Start is for, once this run's close is confirmed (the plan's row); null when
 *  none is left. */
export function nextSample(draft: { format: string; rows: readonly SampleRow[] }, upcoming: number): SampleRow | null {
  if (draft.format !== "queue" || upcoming < 0) return null;
  return draft.rows[upcoming] ?? null;
}

/** How a sheet row is named: "S2 · Donor A · Cytochalasin D" (its sample and condition, when set). */
export function sampleName(row: SampleRow): string {
  return [row.ref, row.sample.trim(), row.condition.trim()].filter((v) => v).join(" · ");
}

/** End queue from the summary: the queue's own `end` (sends nothing), after confirming a finalized
 *  run's close (the queue's Confirm closed, which also sends nothing). "" when it may be pressed. */
export function endQueueState(order: readonly string[], q: QueueState, queueMode: boolean, busy: boolean):
  { reason: string; confirmFirst: boolean; title: string } {
  const none = (reason: string) => ({ reason, confirmFirst: false, title: "" });
  if (!queueMode) return none("a single-sample plan has no queue to end");
  if (q.ended) return none("the queue was already ended");
  const cur = currentIndex(order, q);
  if (cur < 0) return none("the queue is complete: every sample has run");
  if (q.paused) return none("the queue is paused: resume it in the Plan or Live first");
  if (busy) return none("a Start or a stop is pending");
  const ref = order[cur], row = rowState(q, ref);
  if (row.phase === "finalized") {
    const left = order.some((r, i) => i > cur && !["closed", "skipped"].includes(rowState(q, r).phase));
    if (!left) return none(`${ref} is the last sample: Next sample confirms its close and completes the queue`);
    return { reason: "", confirmFirst: true,
      title: `Confirms ${ref}'s close, then ends the queue: the samples not run stay "Not run". Sends nothing.` };
  }
  if (!queueBegun(q)) return none("the queue has not begun: nothing to end yet");
  if (canEnd(order, q)) return { reason: "", confirmFirst: false, title: `Ends the queue before ${ref}: the samples not run stay "Not run". Sends nothing.` };
  return none(`${ref} is ${rowChip(row, q.ended).text.toLowerCase()}: end the queue once its run is closed (Next sample in the Plan or Live stops it)`);
}

// ---- the calibration-run block (O5) ------------------------------------------------------------

/** The record the backend derived from this run: its source run_id is the run's (the newest version
 *  when several are listed); null when none names it, or the run's id is not known. */
export function recordForRun(list: CalibrationRecordsList | null, runId: string | null): CalibrationRecord | null {
  if (!list?.valid || !list.records || !runId) return null;
  let best: CalibrationRecord | null = null;
  for (const r of list.records) {
    if (r.source?.run_id !== runId) continue;
    if (!best || r.sequence > best.sequence || (r.sequence === best.sequence && r.version > best.version)) best = r;
  }
  return best;
}

/** Why no record is shown for a calibration run, as far as it is known. */
export function noRecordReason(p: { listError: string; storeError: string | null; listRead: boolean; listValid?: boolean; runIdKnown: boolean; runId: string | null }): string {
  if (p.listError) return `the calibration records could not be read (${p.listError})`;
  if (p.storeError) return "the calibration store cannot be read, so its records are not known";
  if (!p.listRead) return "the calibration records are not read yet";
  if (p.listValid === false) return "the backend listed no records (not initialized yet), so they are not known";
  if (!p.runIdKnown) return "the run's id is not known yet: it is read from the run's quick look";
  if (p.runId === null) return "the run's file records no run id, so no record can be matched to it";
  return `none of the backend's records names this run (run id ${p.runId}) as its source`;
}

const GATE_NAMES: Readonly<Record<string, string>> = {
  min_valid_objects: "Valid objects", max_cv_percent: "Diameter CV", run_finalized: "Run finalized", file_readable: "Run file readable",
};
const gateNumber = (v: number | null) => (v === null ? "not recorded" : String(Number(v.toPrecision(6))));

/** A record's minimum-evidence gates as the backend recorded them: "Valid objects: met · 812, limit 200". */
export function gateLines(r: CalibrationRecord): { id: string; text: string; met: boolean }[] {
  return (r.gates ?? []).map((g) => {
    const numeric = g.value !== null || g.id === "min_valid_objects" || g.id === "max_cv_percent";
    return { id: g.id, met: g.pass,
      text: `${GATE_NAMES[g.id] ?? g.id}: ${g.pass ? "met" : "not met"}${numeric ? ` · ${gateNumber(g.value)}, limit ${gateNumber(g.limit)}` : ""}${g.reason ? ` · ${g.reason}` : ""}` };
  });
}

/** "3.1 % · limit 10 %": the record's diameter CV and its method's limit, as recorded. */
export function cvText(r: CalibrationRecord): string {
  const v = r.results?.cv_diameter_percent;
  const limit = r.inputs?.max_cv_percent;
  const cv = typeof v === "number" && Number.isFinite(v) ? `${Number(v.toFixed(2))} %` : "Not recorded";
  return `${cv}${typeof limit === "number" && Number.isFinite(limit) ? ` · limit ${limit} %` : ""}`;
}
