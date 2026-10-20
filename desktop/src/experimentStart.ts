// The Start Experiment flow (extracted from App.tsx so it can be tested): pick the file, ask before a stopped Run's buffered frames are discarded, evaluate readiness, start.
// `startRequestJson` (ABI 39, #712) is the run's declared identity and planned annotation as JSON text, from the workspace's Plan
// (workspace/planModel.ts buildStartRequest). It is passed on unchanged; without one (the classic Start) none is sent.
// `stillWanted` (the workspace's Plan) is asked after every step the operator answers or waits for (the discard question,
// the file dialog, readiness) and immediately before experiment_start: once it says no (the client lost control while the
// Start was pending, #716 A5 review) the flow sends nothing more and reports "withdrawn"; the operator presses Start again.
// The start reply's `start_outcome` (bridge ABI 40, #741) decides what the reply means, not its message: `startReplyState`.
import { EXPERIMENT_START_OUTCOMES as O, READINESS_GATE_STATUSES } from "./bridgeContract";
import { startDiscardNotice, type RingStatus } from "./ringPlayback";

export interface StartGate { id: string; status: number; reason: string; remediation?: string }
export interface StartReadiness { valid: boolean; ready: boolean; gates: StartGate[] }
/** The start reply: `start_outcome` is the backend's EXPERIMENT_START_OUTCOMES value (ABI 40), absent
 *  from an older backend (ABI 39 and before), which answers with ok and message only. */
export interface StartResult { ok: boolean; message: string; start_outcome?: number }

/** What a start reply means for the run's identity (#736 rules): started (the run exists), refused
 *  (nothing was recorded: the identity and the run index may be sent again) or unknown (the run, or
 *  its file, may exist: the identity and the run index are used for good). */
export type StartReplyState = "started" | "refused" | "unknown";

/** Each start outcome code's state (bridge ABI 40, #741). The codes the backend returns only before
 *  it begins the run (ExperimentCoordinator::start, before Starting: no start generation, no file) are
 *  refused. The codes it can also return after it opened the run's HDF5 file are unknown, because
 *  the file, with the identity in its snapshot, may be left behind:
 *  - StorageFailed: initializeDatasets failing closes the file without removing it.
 *  - ProvenanceFailed: the snapshot or annotation write failing removes the file, best effort.
 *  - NotReady: mostly a readiness refusal before anything, but on the PZ7035 also "execution
 *    provider did not start" after the file and snapshot were written (the file is removed best
 *    effort, a pzrec run aborted). The code alone cannot tell the two apart, so it locks. */
export const START_OUTCOME_STATES: Readonly<Record<number, StartReplyState>> = {
  [O.Started]: "started",
  [O.NotReady]: "unknown",
  [O.StaleReadiness]: "refused",
  [O.AlreadyActive]: "refused",
  [O.StorageFailed]: "unknown",
  [O.ProvenanceFailed]: "unknown",
  [O.Busy]: "refused",
  [O.InvalidRequest]: "refused",
};

/** The state of a start reply that was read. Without a code (an older backend) ok decides, as
 *  before ABI 40. A code this table does not know, or a code that contradicts ok, is unknown: the
 *  run may exist. */
export function startReplyState(res: StartResult): StartReplyState {
  if (res.start_outcome === undefined) return res.ok ? "started" : "refused";
  const state = START_OUTCOME_STATES[res.start_outcome];
  if (state === undefined) return "unknown";
  if ((state === "started") !== res.ok) return "unknown";
  return state;
}

export interface StartDeps {
  /** A fresh ring status (not the polled snapshot, which can be seconds old or missing for another client). */
  fetchRingStatus: () => Promise<RingStatus | null | undefined>;
  confirm: (message: string) => Promise<boolean>;
  pickFile: () => Promise<string | null | undefined>;
  fetchReadiness: (path: string) => Promise<StartReadiness>;
  /** Called with a third argument only when the flow was given a startRequestJson. */
  start: (path: string, acknowledgeDiscardRing: boolean, startRequestJson?: string) => Promise<StartResult>;
}

export type StartOutcome =
  | { kind: "cancelled" }
  | { kind: "not-ready"; message: string }
  /** The backend refused the start before it began the run (`code`: its start outcome, absent from
   *  an older backend): nothing was recorded. */
  | { kind: "refused"; path: string; message: string; code?: number }
  | { kind: "started"; path: string; notice: string }
  /** The run may exist, so its identity must be treated as used. Without `code`: the start request
   *  was sent but no reply was decoded (transport error). With `code`: the reply was read and says
   *  not started, but with a code the backend can return after it opened the run's file
   *  (START_OUTCOME_STATES). */
  | { kind: "unknown"; path: string; message: string; code?: number }
  /** Nothing was sent: the Start was withdrawn while it was pending (`stillWanted` said no). */
  | { kind: "withdrawn" };

export async function startExperiment(deps: StartDeps, startRequestJson?: string, stillWanted: () => boolean = () => true): Promise<StartOutcome> {
  // A stopped Run (after Stop, or after an SSD run) holds its buffered frames and the start discards them: ask first, and tell the backend that the operator agreed.
  const discard = startDiscardNotice(await deps.fetchRingStatus().catch(() => null));
  if (!stillWanted()) return { kind: "withdrawn" };
  if (discard && !(await deps.confirm(discard))) return { kind: "cancelled" };
  if (!stillWanted()) return { kind: "withdrawn" };
  const picked = await deps.pickFile();
  if (!stillWanted()) return { kind: "withdrawn" }; // the dialog's answer is not used
  if (!picked) return { kind: "cancelled" };
  const readiness = await deps.fetchReadiness(picked);
  if (!stillWanted()) return { kind: "withdrawn" };
  if (!readiness.valid || !readiness.ready) {
    const reason = readiness.gates.filter(g => g.status === READINESS_GATE_STATUSES.Fail || g.status === READINESS_GATE_STATUSES.Unavailable)
      .map(g => `${g.id}: ${g.reason}${g.remediation ? ` — ${g.remediation}` : ""}`).join("; ");
    return { kind: "not-ready", message: `${picked}: ${reason || "Backend readiness unavailable; experiment was not started."}` };
  }
  const notice = readiness.gates.filter(g => g.id === "storage.persistent" && g.status === READINESS_GATE_STATUSES.Warn).map(g => g.reason).join(" ");
  // The outcome is the start reply's alone. Anything before this point failing means nothing was
  // sent; the start call itself failing means the outcome is not known.
  let res: StartResult;
  if (!stillWanted()) return { kind: "withdrawn" }; // immediately before sending
  try {
    res = startRequestJson === undefined
      ? await deps.start(picked, discard !== "")
      : await deps.start(picked, discard !== "", startRequestJson);
  } catch (e) {
    return { kind: "unknown", path: picked, message: String(e) };
  }
  const state = startReplyState(res);
  if (state === "started") return { kind: "started", path: picked, notice };
  const code = res.start_outcome === undefined ? {} : { code: res.start_outcome };
  return { kind: state, path: picked, message: res.message, ...code };
}
