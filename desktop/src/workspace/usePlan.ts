// The workspace's Experiment plan (O3, #716 A5), held by App so it survives stage changes and
// leaving the workspace. The plan is local: creating or editing it sends nothing. Only `start`
// (single sample) and the queue's buttons act, and only through the caller's commands (App's
// `runExperimentStart`, the classic `experimentStart.ts` flow with the plan's startRequestJson, and
// App's stop, `experiment_stop` with the row's start generation as expectedStartGeneration, ABI 40).
// The start reply's code decides started / refused / unknown (experimentStart.ts startReplyState).
//
// - The experiment id is minted when the plan is created and kept while the plan is edited and
//   while a Start is refused or cancelled (no run was recorded). A single-sample plan is locked once
//   its run has started: its identity is recorded in that run, and it has no second run. "New plan"
//   mints a new id.
// - A sample queue (`queue`) runs its rows in order, one run and one file each, under the plan's one
//   experiment id; queueModel.ts is the state machine. Each button press is one step; the status
//   App already polls is observed; nothing advances or starts on its own. A lost controlling client
//   pauses the queue until the operator resumes it.
// - A control loss withdraws any pending Start (single or queue): an epoch, taken when Start is
//   pressed, is checked by the Start flow after every answer and immediately before
//   experiment_start. A withdrawn Start sends nothing; the operator presses Start again.
// - The selected operator (#393 phase 1) is remembered locally, in localStorage, when a run
//   starts with it. It is a selection, not authentication, and the backend records it with
//   assurance "selected". A remembered operator is only a suggestion (#763 F6): the plan App
//   creates (at every page load) starts with no operator, and one is sent only once the operator
//   typed it or picked it from the remembered list in this page's Plan. "New plan" keeps the
//   operator selected in the plan before it, which was itself typed or picked here.
// - The Quick look's reference run (#716 A7b, the A7 ruling 4, Q3 a) is held here, with the plan:
//   none until the operator chooses one, cleared by "New plan", never persisted (until the
//   workspace store exists, #393/#395). Choosing it sends nothing; the backend confirms its run_id
//   when the quick look is read.
import { useCallback, useEffect, useRef, useState } from "react";
import type { MethodPurpose } from "../calibrationRecords";
import type { ExperimentStatus } from "../eventAdapter";
import type { StartOutcome } from "../experimentStart";
import {
  buildStartRequest,
  mintExperimentId,
  newPlanDraft,
  parseOperators,
  rememberOperator,
  type OperatorChoice,
  type PlanDraft,
} from "./planModel";
import { currentIndex, editKeepsRecorded, followSingleRun, newQueue, NO_OPERATIONS, ownRunActive, queueInProgress, reduceQueue, rowState, type OperationTrack, type QueueEvent, type QueueState } from "./queueModel";

/** Where the remembered operators are kept (this machine only). */
export const OPERATORS_STORAGE_KEY = "mib.workspace.operators";

/** What a Start from the plan came to: the flow's outcome (decided by the start reply alone; a
 *  failed status refresh after it is `refreshError`, reported apart), an error before the start
 *  was sent, or nothing sent because a start request was already pending. */
export type PlanStartResult =
  | (StartOutcome & { refreshError?: string })
  | { kind: "error"; message: string }
  | { kind: "busy" };

export interface PlanStarted {
  path: string;
  notice: string;
  runIndex: number;
  /** "started": the backend accepted the start. "unknown": the run may exist, so the identity is
   *  treated as used all the same: the reply was lost, or (`code`) it said not started with a code
   *  the backend can return after it opened the run's file. */
  outcome: "started" | "unknown";
  code?: number;
  /** The experiment operation the start reply named (StartResult.operation_id): the only identity
   *  the run is followed by (queueModel followSingleRun, #763 F14). Absent when it named none. */
  operationId?: string;
  /** OperationTrack.loss when the Start was pressed. */
  lossBefore?: number;
  /** How the run ended, once its operation's terminal event was read. */
  end?: { ok: boolean; cancelled: boolean; message: string };
  /** Events may have been lost since the Start before its end was seen: the outcome is not known. */
  endLost?: boolean;
  /** The applied method's purpose at the Start's readiness check with its file (ABI 43); null when
   *  that report could not say; absent when not known (#716 A7c, O5's calibration block). */
  methodPurpose?: MethodPurpose | null;
}

/** The stop command's reply (`experiment_stop`); `stop_outcome` is its EXPERIMENT_STOP_OUTCOMES value
 *  (ABI 40), absent from an older backend. */
export interface StopReply { ok: boolean; message: string; stop_outcome?: number }

/** Why the queue pauses when this client no longer controls the instrument. */
export const CONTROL_LOST_REASON = "this client lost control of the instrument (another client took control, or the connection dropped)";

export interface QueueController {
  state: QueueState;
  /** A queue command is in flight: the Start flow or the stop. */
  pending: "start" | "stop" | null;
  /** This client controls the instrument (always on the desktop). */
  controlHeld: boolean;
  /** The last experiment status read (the run's progress and its file). */
  status: ExperimentStatus | null;
  /** Start (or retry) the current row with the next run index: the Start flow, once.
   *  `calibrationRecordId`: the applied record the page shows as in effect, declared in the start
   *  request (#716 A4b; buildStartRequest); null or absent declares none. */
  startCurrent: (calibrationRecordId?: string | null) => Promise<void>;
  /** Next sample: stop the current row's run (stop → drain → finalize, observed in the status). */
  stopCurrent: () => Promise<void>;
  /** The operator confirms the finalized run's file is closed (sends nothing). */
  confirmClosed: () => void;
  /** The operator skips the current row after a failure (sends nothing). */
  skip: () => void;
  /** The operator ends the queue (sends nothing). */
  end: () => void;
  /** The operator resumes a paused queue (sends nothing; only allowed with control held). */
  resume: () => void;
  /** Repeat sample (#716 A7b): re-queue this closed (or finalized) row as the next run of its
   *  sample_ref (sends nothing; only with control held; queueModel repeatReason). */
  repeat: (ref: string) => void;
}

/** The Quick look's reference run, as the client holds it (#716 A7b; run_quick_look `reference`):
 *  the file, and the run_id and start generation its quick look reported. */
export interface PlanReference {
  path: string;
  run_id: string | null;
  start_generation: string | null;
  /** How the plan names it: the run index and sample ref (null when not known). */
  runIndex: number | null;
  ref: string | null;
}

export interface PlanController {
  draft: PlanDraft;
  /** Edit the draft (ignored while a Start is pending, once a single sample's run has started, and
   *  when it would change anything a queue already recorded). */
  edit: (change: (draft: PlanDraft) => PlanDraft) => void;
  /** Remembered operators, most recent first. */
  operators: OperatorChoice[];
  /** The last single-sample Start's result, until the next Start or a new plan. */
  result: PlanStartResult | null;
  /** Set once the single-sample plan's run has started (or may have): the plan is locked. */
  started: PlanStarted | null;
  starting: boolean;
  /** Run the Start flow with this startRequestJson (single sample). */
  start: (startRequestJson: string, runIndex: number) => Promise<PlanStartResult | null>;
  /** A new plan: a new experiment id, an empty sheet, the operator selected in this plan (typed or
   *  picked in this page's Plan, never a remembered one by itself), the same format. Ignored while
   *  a queue is in progress. */
  newPlan: () => void;
  loadSaved?: (plan: PlanDraft) => boolean;
  /** The guided sample queue. */
  queue: QueueController;
  /** The Quick look's reference run: none until the operator chooses one; "New plan" clears it. */
  reference: PlanReference | null;
  /** Choose (or clear, with null) the reference run (sends nothing). */
  setReference: (reference: PlanReference | null) => void;
}

export interface PlanOptions {
  /** The experiment status App polls (observed by the queue). */
  status?: ExperimentStatus | null;
  /** Stop the row's run: `experiment_stop` with this start generation (canonical decimal) as
   *  expectedStartGeneration (ABI 40), so the backend stops nothing when another run is active. */
  stop?: (expectedStartGeneration: string) => Promise<StopReply>;
  /** Whether this client controls the instrument (false pauses a queue in progress, and a single
   *  sample's Start sends nothing). */
  controlHeld?: boolean;
  /** The experiment operations' ends App has read (the single sample's run ends with its own). */
  operations?: OperationTrack;
}

function readOperators(): OperatorChoice[] {
  try {
    const raw = window.localStorage.getItem(OPERATORS_STORAGE_KEY);
    return raw === null ? [] : parseOperators(JSON.parse(raw));
  } catch {
    return [];
  }
}

function writeOperators(list: OperatorChoice[]) {
  try {
    window.localStorage.setItem(OPERATORS_STORAGE_KEY, JSON.stringify(list));
  } catch {
    /* storage unavailable: the operator is just not remembered */
  }
}

const noStop = async (_expectedStartGeneration: string): Promise<StopReply> => ({ ok: false, message: "stopping is not available here" });

/** The caller's Start flow: the startRequestJson, and whether the Start is still wanted (asked
 *  after each step and immediately before experiment_start; false withdraws it). */
export type RunStart = (startRequestJson: string, stillWanted?: () => boolean) => Promise<PlanStartResult>;

export function usePlan(runStart: RunStart, options: PlanOptions = {}): PlanController {
  const status = options.status ?? null;
  const controlHeld = options.controlHeld ?? true;
  const operations = options.operations ?? NO_OPERATIONS;
  const [operators, setOperators] = useState<OperatorChoice[]>(readOperators);
  // No operator until one is typed or picked: the remembered list only suggests (#763 F6).
  const [draft, setDraft] = useState<PlanDraft>(() => newPlanDraft(mintExperimentId()));
  const [result, setResult] = useState<PlanStartResult | null>(null);
  const [started, setStarted] = useState<PlanStarted | null>(null);
  const [starting, setStarting] = useState(false);
  const [queue, setQueue] = useState<QueueState>(newQueue);
  const [queuePending, setQueuePending] = useState<"start" | "stop" | null>(null);
  const [reference, setReference] = useState<PlanReference | null>(null);
  const pending = useRef(false);
  const startedRef = useRef(false);
  // The values as last rendered, for the async actions: the operator remembered is the one the
  // Start sent (the draft cannot change while a Start is pending).
  const draftRef = useRef(draft);
  draftRef.current = draft;
  const operatorsRef = useRef(operators);
  operatorsRef.current = operators;
  const queueRef = useRef(queue);
  queueRef.current = queue;
  const statusRef = useRef(status);
  statusRef.current = status;
  const operationsRef = useRef(operations);
  operationsRef.current = operations;
  const controlRef = useRef(controlHeld);
  controlRef.current = controlHeld;
  const stopRef = useRef(options.stop ?? noStop);
  stopRef.current = options.stop ?? noStop;
  // Bumped on every control loss: a Start pressed before it is no longer wanted.
  const controlEpoch = useRef(0);

  const order = () => draftRef.current.rows.map((r) => r.ref);
  const dispatch = useCallback((e: QueueEvent) => {
    const refs = draftRef.current.rows.map((r) => r.ref);
    // Applied to the ref at once too, so the next guard in the same tick sees it.
    queueRef.current = reduceQueue(queueRef.current, e, refs);
    setQueue((q) => reduceQueue(q, e, refs));
  }, []);

  // The queue observes every status read (backend facts apply even while paused).
  useEffect(() => { dispatch({ type: "status", status }); }, [status, dispatch]);
  // The single sample's run ends with the operation its start reply named, never by a status (#763 F14).
  useEffect(() => { setStarted((st) => (st ? followSingleRun(st, operations) : st)); }, [started, operations]);
  // A lost controlling client pauses a queue in progress; it never resumes on its own.
  // A control loss also withdraws any pending Start (the epoch the Start flow checks).
  useEffect(() => {
    if (controlHeld) return;
    controlEpoch.current += 1;
    dispatch({ type: "pause", reason: CONTROL_LOST_REASON });
  }, [controlHeld, dispatch]);

  const edit = useCallback((change: (d: PlanDraft) => PlanDraft) => {
    if (startedRef.current || pending.current) return;
    const prev = draftRef.current;
    const next = change(prev);
    if (!editKeepsRecorded(prev, next, queueRef.current)) return;
    draftRef.current = next;
    setDraft(next);
  }, []);

  const remember = useCallback(() => {
    const next = rememberOperator(operatorsRef.current, draftRef.current.operator);
    writeOperators(next);
    setOperators(next);
  }, []);

  const start = useCallback(async (json: string, runIndex: number): Promise<PlanStartResult | null> => {
    // Only the controlling client starts (a viewer's button is disabled too): nothing is sent.
    if (pending.current || startedRef.current || !controlRef.current) return null;
    pending.current = true;
    setStarting(true);
    setResult(null);
    const epoch = controlEpoch.current;
    const lossBefore = operationsRef.current.loss;
    try {
      const out = await runStart(json, () => controlEpoch.current === epoch && controlRef.current);
      setResult(out);
      // Locked as soon as the backend accepted the start (or may have): whatever a later refresh
      // did, this identity is recorded in that run and is not sent again.
      if (out.kind === "started" || out.kind === "unknown") {
        startedRef.current = true;
        const code = out.kind === "unknown" && out.code !== undefined ? { code: out.code } : {};
        const op = out.kind === "started" && out.operationId ? { operationId: out.operationId } : {};
        const purpose = out.kind === "started" && out.methodPurpose !== undefined ? { methodPurpose: out.methodPurpose } : {};
        setStarted({ path: out.path, notice: out.kind === "started" ? out.notice : "", runIndex, outcome: out.kind, ...code, ...op, ...purpose, lossBefore });
      }
      if (out.kind === "started") remember();
      return out;
    } catch (e) {
      // Not expected (the Start flow reports its own errors); the request may have been sent, so
      // the identity is treated as used, as for a lost reply.
      const out: PlanStartResult = { kind: "unknown", path: "", message: String(e) };
      startedRef.current = true;
      setStarted({ path: "", notice: "", runIndex, outcome: "unknown" });
      setResult(out);
      return out;
    } finally {
      pending.current = false;
      setStarting(false);
    }
  }, [runStart, remember]);

  // ---- the queue's buttons: each sends at most one command ----

  const startCurrent = useCallback(async (calibrationRecordId: string | null = null) => {
    const q = queueRef.current, d = draftRef.current, s = statusRef.current;
    if (pending.current || q.paused || !controlRef.current || d.format !== "queue" || !s?.valid) return;
    const index = currentIndex(order(), q);
    if (index < 0) return;
    const ref = d.rows[index].ref, runIndex = q.nextRunIndex, generationBefore = s.start_generation;
    const build = buildStartRequest(d, index, runIndex, calibrationRecordId);
    if (build.json === null) return;
    dispatch({ type: "start", ref, runIndex, generationBefore });
    if (queueRef.current.rows[ref]?.phase !== "starting") return; // the machine refused it
    pending.current = true;
    setQueuePending("start");
    const epoch = controlEpoch.current;
    let out: PlanStartResult;
    try {
      out = await runStart(build.json, () => controlEpoch.current === epoch && controlRef.current && !queueRef.current.paused);
    } catch (e) {
      out = { kind: "unknown", path: "", message: String(e) }; // the request may have been sent
    } finally {
      pending.current = false;
      setQueuePending(null);
    }
    dispatch({ type: "start-result", ref, runIndex, generationBefore, result: out });
    dispatch({ type: "status", status: statusRef.current });
    if (out.kind === "started") remember();
  }, [runStart, dispatch, remember]);

  const stopCurrent = useCallback(async () => {
    const q = queueRef.current;
    if (pending.current || q.paused || !controlRef.current) return;
    const index = currentIndex(order(), q);
    if (index < 0) return;
    const ref = draftRef.current.rows[index].ref;
    // Only when the status shows this row's own run active; and the stop names that run (its start
    // generation), so the backend refuses it, stopping nothing, if another run replaced it since.
    const row = rowState(q, ref);
    if (!ownRunActive(row, statusRef.current)) return;
    const generation = row.attempts[row.attempts.length - 1].generation!;
    dispatch({ type: "stop", ref });
    if (!queueRef.current.rows[ref]?.stopPending) return; // the machine refused it
    pending.current = true;
    setQueuePending("stop");
    let reply: StopReply;
    try {
      reply = await stopRef.current(generation);
    } catch (e) {
      reply = { ok: false, message: `no reply to the stop (${String(e)})` };
    } finally {
      pending.current = false;
      setQueuePending(null);
    }
    dispatch({ type: "stop-result", ref, ok: reply.ok, message: reply.message, ...(reply.stop_outcome === undefined ? {} : { code: reply.stop_outcome }) });
    dispatch({ type: "status", status: statusRef.current });
  }, [dispatch]);

  const currentRef = () => {
    const i = currentIndex(order(), queueRef.current);
    return i < 0 ? null : draftRef.current.rows[i].ref;
  };
  const confirmClosed = useCallback(() => { const ref = currentRef(); if (ref && !pending.current) dispatch({ type: "confirm-closed", ref }); }, [dispatch]);
  const skip = useCallback(() => { const ref = currentRef(); if (ref && !pending.current) dispatch({ type: "skip", ref }); }, [dispatch]);
  const end = useCallback(() => { if (!pending.current) dispatch({ type: "end" }); }, [dispatch]);
  const resume = useCallback(() => { if (controlRef.current) dispatch({ type: "resume" }); }, [dispatch]);
  const repeat = useCallback((ref: string) => { if (!pending.current && controlRef.current) dispatch({ type: "repeat", ref }); }, [dispatch]);

  const newPlan = useCallback(() => {
    if (pending.current || queueInProgress(order(), queueRef.current)) return;
    startedRef.current = false;
    setStarted(null);
    setResult(null);
    setReference(null);
    queueRef.current = newQueue();
    setQueue(queueRef.current);
    const next = newPlanDraft(mintExperimentId(), draftRef.current.operator, draftRef.current.format);
    draftRef.current = next;
    setDraft(next);
  }, []);

  const loadSaved = (saved: PlanDraft): boolean => {
    if (!controlRef.current || pending.current || startedRef.current || queueInProgress(order(), queueRef.current)) return false;
    const next = structuredClone(saved);
    next.experimentId = mintExperimentId();
    draftRef.current = next;
    setDraft(next);
    setResult(null); setReference(null);
    queueRef.current = newQueue(); setQueue(queueRef.current);
    return true;
  };

  return {
    draft, edit, loadSaved, operators, result, started, starting, start, newPlan, reference, setReference,
    queue: { state: queue, pending: queuePending, controlHeld, status, startCurrent, stopCurrent, confirmClosed, skip, end, resume, repeat },
  };
}
