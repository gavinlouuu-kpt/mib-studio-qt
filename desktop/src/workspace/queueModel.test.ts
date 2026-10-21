// The guided sample queue's state machine (#716 A5): every transition, a failure at each step, the
// operator's retry / skip / end, identity locking (a run index is never sent twice), the pause on a
// lost controlling client, and no transition that starts or advances on its own.
import { describe, expect, it } from "vitest";
import { EXPERIMENT_START_OUTCOMES as O, EXPERIMENT_STATES as S, EXPERIMENT_STOP_OUTCOMES as SO, RUN_COMPLETION_STATES as C } from "../bridgeContract";
import type { ExperimentStatus } from "../eventAdapter";
import { emptyRow, newPlanDraft, type PlanDraft } from "./planModel";
import {
  canEnd,
  canMove,
  canRemove,
  currentIndex,
  editKeepsRecorded,
  lifecycle,
  newQueue,
  notStarted,
  plannedRunIndex,
  queueBegun,
  queueInProgress,
  reduceQueue,
  rowChip,
  rowEditable,
  rowState,
  ownRunActive,
  recordedPath,
  followSingleRun,
  singleRunChip,
  type QueueEvent,
  type QueueState,
  type RowPhase,
} from "./queueModel";
import type { PlanStarted, PlanStartResult } from "./usePlan";

const ORDER = ["S1", "S2", "S3"];
const status = (over: Partial<ExperimentStatus> = {}): ExperimentStatus => ({
  valid: true, state: S.Idle, start_time_ns: "0", end_time_ns: "0", valid_buffered: "0", invalid_buffered: "0", valid_saved: "0", invalid_saved: "0",
  dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false, output_path: "", message: "", start_generation: "0", readiness_generation: "1",
  capture_generation: "1", persistence_admitted: "0", persistence_committed: "0", persistence_failed: "0", terminal: false, finalization_ok: false,
  completion: C.Complete, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "", ...over,
});
/** Statuses of a run on a file (by default the file every test row chose, "/data/run.h5"). */
const RUN = "/data/run.h5";
const active = (gen: string, path = RUN) => status({ state: S.Active, start_generation: gen, output_path: path });
const stopping = (gen: string, path = RUN) => status({ state: S.Stopping, flushing: true, start_generation: gen, output_path: path });
const closed = (gen: string, over: Partial<ExperimentStatus> = {}) =>
  status({ state: S.Idle, terminal: true, finalization_ok: true, start_generation: gen, output_path: RUN, persistence_admitted: "10", ...over });

const STARTED = (path = RUN): PlanStartResult => ({ kind: "started", path, notice: "" });

/** Apply events in order. */
function run(events: QueueEvent[], q: QueueState = newQueue(), order = ORDER): QueueState {
  return events.reduce((acc, e) => reduceQueue(acc, e, order), q);
}
const phase = (q: QueueState, ref: string): RowPhase => rowState(q, ref).phase;

/** S1 started as run 1 (generation 1, from 0), and running. */
const startedS1 = (): QueueState => run([
  { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
  { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: STARTED() },
]);
/** S1 run through to finalized. */
const finalizedS1 = (): QueueState => run([
  { type: "status", status: active("1") },
  { type: "stop", ref: "S1" },
  { type: "stop-result", ref: "S1", ok: true, message: "ok" },
  { type: "status", status: stopping("1") },
  { type: "status", status: closed("1") },
], startedS1());

describe("a new queue", () => {
  it("is at S1, planned, not begun; nothing is in progress and every row can be edited", () => {
    const q = newQueue();
    expect(currentIndex(ORDER, q)).toBe(0);
    expect(queueBegun(q)).toBe(false);
    expect(queueInProgress(ORDER, q)).toBe(false);
    expect(q.nextRunIndex).toBe(1);
    for (const ref of ORDER) expect(rowEditable(q, ref)).toBe(true);
    expect(ORDER.map((_, i) => plannedRunIndex(ORDER, q, i))).toEqual([1, 2, 3]);
  });
});

describe("Start", () => {
  it("is taken only for the current row, with the next run index, and not while another Start is pending or paused", () => {
    const q = newQueue();
    expect(run([{ type: "start", ref: "S2", runIndex: 1, generationBefore: "0" }], q)).toBe(q); // not the current row
    expect(run([{ type: "start", ref: "S1", runIndex: 2, generationBefore: "0" }], q)).toBe(q); // not the next index
    expect(run([{ type: "start", ref: "S1", runIndex: 1, generationBefore: "0" }], { ...q, paused: "lost" })).toEqual({ ...q, paused: "lost" });
    const starting = run([{ type: "start", ref: "S1", runIndex: 1, generationBefore: "0" }], q);
    expect(phase(starting, "S1")).toBe("starting");
    expect(run([{ type: "start", ref: "S1", runIndex: 1, generationBefore: "0" }], starting)).toBe(starting); // pending already
    expect(rowEditable(starting, "S1")).toBe(false);
  });

  it("started: the row runs, the index is used for good and the row is locked", () => {
    const q = startedS1();
    expect(phase(q, "S1")).toBe("running");
    expect(q.nextRunIndex).toBe(2);
    expect(rowState(q, "S1").attempts).toEqual([{ runIndex: 1, outcome: "started", path: RUN, generationBefore: "0", generation: null }]);
    expect(rowEditable(q, "S1")).toBe(false);
    expect(queueInProgress(ORDER, q)).toBe(true);
  });

  it("unknown (reply lost): the index is used, the row locked and stays visible; the operator retries with a new index or skips", () => {
    const q = run([
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: { kind: "unknown", path: "/data/s1.h5", message: "TRANSPORT_LOST" } },
    ]);
    expect(phase(q, "S1")).toBe("unknown");
    expect(q.nextRunIndex).toBe(2);
    expect(rowEditable(q, "S1")).toBe(false);
    expect(rowState(q, "S1").failure).toMatchObject({ step: "start", tone: "unknown", title: "Start outcome not known · S1, run 1 · /data/s1.h5" });
    expect(rowState(q, "S1").failure!.text).toContain("retry it as run 2");
    // Retrying with the old index is refused; with the new one it starts.
    expect(run([{ type: "start", ref: "S1", runIndex: 1, generationBefore: "0" }], q)).toBe(q);
    const retried = run([
      { type: "start", ref: "S1", runIndex: 2, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 2, generationBefore: "0", result: STARTED() },
    ], q);
    expect(rowState(retried, "S1").attempts.map((a) => [a.runIndex, a.outcome])).toEqual([[1, "unknown"], [2, "started"]]);
    expect(retried.nextRunIndex).toBe(3);
    // Or skipped: the row stays, as skipped, with its unknown attempt.
    const skipped = run([{ type: "skip", ref: "S1" }], q);
    expect(phase(skipped, "S1")).toBe("skipped");
    expect(currentIndex(ORDER, skipped)).toBe(1);
    expect(rowChip(rowState(skipped, "S1"), false)).toEqual({ tone: "neutral", text: "Skipped · after a run" });
  });

  it.each<[string, PlanStartResult, string]>([
    ["refused", { kind: "refused", path: "/data/s1.h5", message: "Experiment is already running" }, "Not started · the backend refused the Start of S1"],
    ["InvalidRequest", { kind: "refused", path: "/data/s1.h5", message: "invalid start request: annotation.condition: 300 bytes, at most 256" }, "Not started · the backend refused S1's sheet row"],
    ["not ready", { kind: "not-ready", message: "/data/s1.h5: storage.output: not writable" }, "Not started · readiness with the chosen file"],
    ["an error before sending", { kind: "error", message: "dialog unavailable" }, "Not started · an error before the start was sent"],
    ["busy", { kind: "busy" }, "Not started · a start request is already pending"],
    ["withdrawn (control lost while pending)", { kind: "withdrawn" }, "Not started · control was lost while the Start of S1 was pending"],
  ])("%s: nothing was recorded, the index is kept, the row stays editable; the flow stops there", (_, result, title) => {
    const q = run([
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result },
    ]);
    expect(phase(q, "S1")).toBe("refused");
    expect(q.nextRunIndex).toBe(1);
    expect(rowEditable(q, "S1")).toBe(true);
    expect(rowState(q, "S1").failure!.title).toBe(title);
    expect(currentIndex(ORDER, q)).toBe(0);
    // Start again with the same index (nothing was recorded), skip, or end.
    expect(phase(run([{ type: "start", ref: "S1", runIndex: 1, generationBefore: "0" }], q), "S1")).toBe("starting");
    expect(phase(run([{ type: "skip", ref: "S1" }], q), "S1")).toBe("skipped");
    expect(run([{ type: "end" }], q).ended).toBe(true);
  });

  it("the InvalidRequest refusal names the field in plain words", () => {
    const q = run([
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: { kind: "refused", path: "/x.h5", message: "invalid start request: annotation.condition: 300 bytes, at most 256" } },
    ]);
    expect(rowState(q, "S1").failure!.text).toBe("Condition: 300 bytes, at most 256. No file was created. Correct the row and press Start again.");
  });

  it("cancelled (the dialog): back to where it was, the failure shown before kept", () => {
    const refused = run([
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: { kind: "not-ready", message: "x" } },
    ]);
    const cancelled = run([
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: { kind: "cancelled" } },
    ], refused);
    expect(cancelled.rows.S1).toEqual(refused.rows.S1);
    const fresh = run([
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: { kind: "cancelled" } },
    ]);
    expect(phase(fresh, "S1")).toBe("planned");
    expect(queueBegun(fresh)).toBe(false);
  });
});

describe("the run, followed in the status", () => {
  it("a status of the run before the Start (or an older read) is not this run's", () => {
    const q = run([{ type: "start", ref: "S1", runIndex: 1, generationBefore: "4" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "4", result: STARTED() }]);
    const before = run([{ type: "status", status: closed("4") }], q);
    expect(before).toBe(q); // the previous run's terminal status does not finalize this one
    const seen = run([{ type: "status", status: active("5") }], q);
    expect(rowState(seen, "S1").attempts[0].generation).toBe("5");
    expect(rowState(seen, "S1").outputPath).toBe(RUN);
    expect(run([{ type: "status", status: closed("4") }], seen)).toBe(seen); // an older read answering late
  });

  it("stop → stopping → finalized with the file the backend closed; nothing more until the operator confirms", () => {
    let q = run([{ type: "status", status: active("1") }], startedS1());
    q = run([{ type: "stop", ref: "S1" }], q);
    expect(phase(q, "S1")).toBe("stopping");
    expect(rowState(q, "S1").stopPending).toBe(true);
    expect(run([{ type: "stop", ref: "S1" }], q)).toBe(q); // one stop at a time
    q = run([{ type: "stop-result", ref: "S1", ok: true, message: "ok" }], q);
    expect(phase(q, "S1")).toBe("stopping");
    q = run([{ type: "status", status: active("1") }], q); // a read from before the stop took effect
    expect(phase(q, "S1")).toBe("stopping");
    q = run([{ type: "status", status: stopping("1") }, { type: "status", status: closed("1") }], q);
    expect(phase(q, "S1")).toBe("finalized");
    expect(rowState(q, "S1").end).toMatchObject({ ok: true, outputPath: RUN, severity: "ok" });
    // Any number of further reads change nothing: the close waits for the operator, S2 waits for its Start.
    const again = run(Array.from({ length: 5 }, () => ({ type: "status", status: closed("1") }) as QueueEvent), q);
    expect(again).toBe(q);
    expect(currentIndex(ORDER, q)).toBe(0);
    q = run([{ type: "confirm-closed", ref: "S1" }], q);
    expect(phase(q, "S1")).toBe("closed");
    expect(currentIndex(ORDER, q)).toBe(1);
    expect(phase(q, "S2")).toBe("planned"); // not started: the operator presses Start S2
    expect(plannedRunIndex(ORDER, q, 1)).toBe(2);
  });

  it("a run that ends without Next sample (the classic Stop, the server's safe stop) is followed too", () => {
    const q = run([{ type: "status", status: active("1") }, { type: "status", status: stopping("1") }, { type: "status", status: closed("1") }], startedS1());
    expect(phase(q, "S1")).toBe("finalized");
  });

  it.each<[string, Partial<ExperimentStatus>, string]>([
    ["a failed run", { state: S.Failed, finalization_ok: false, fault_code: "experiment.saveFailed", fault_message: "disk full" }, "disk full"],
    ["a finalization that did not complete", { finalization_ok: false, completion_reason: "metadata not written" }, "metadata not written"],
    ["a fault after the close", { fault_code: "experiment.provenanceFailed", fault_message: "provenance could not be saved" }, "provenance could not be saved"],
  ])("%s: failed, with the backend's reason; the operator retries with a new index, skips or ends", (_, over, why) => {
    const q = run([{ type: "status", status: active("1") }, { type: "status", status: closed("1", over) }], startedS1());
    expect(phase(q, "S1")).toBe("failed");
    expect(rowState(q, "S1").failure).toMatchObject({ step: "run", tone: "blocked", title: "S1 · run 1 failed", text: why });
    expect(run([{ type: "confirm-closed", ref: "S1" }], q)).toBe(q); // a failed run is not "closed"
    expect(canEnd(ORDER, q)).toBe(true);
    const retry = run([{ type: "start", ref: "S1", runIndex: 2, generationBefore: "1" }], q);
    expect(phase(retry, "S1")).toBe("starting");
    expect(run([{ type: "start", ref: "S1", runIndex: 1, generationBefore: "1" }], q)).toBe(q);
    expect(phase(run([{ type: "skip", ref: "S1" }], q), "S1")).toBe("skipped");
  });

  it("another run during the row (a later generation, another file): interrupted; the queue never stops or confirms that run", () => {
    const q = run([{ type: "status", status: active("1") }, { type: "status", status: active("2", "/data/other.h5") }], startedS1());
    expect(phase(q, "S1")).toBe("interrupted");
    expect(rowState(q, "S1").failure).toMatchObject({ step: "run", tone: "unknown", title: "S1 · interrupted · another run started on this instrument" });
    expect(rowState(q, "S1").failure!.text).toContain("/data/other.h5");
    expect(rowState(q, "S1").failure!.text).toContain("does not stop or confirm that run");
    expect(run([{ type: "stop", ref: "S1" }], q)).toBe(q);
    // The other run finalizing changes nothing for S1: it is not S1's to confirm.
    const after = run([{ type: "status", status: closed("2", { output_path: "/data/other.h5" }) }], q);
    expect(after).toBe(q);
    expect(run([{ type: "confirm-closed", ref: "S1" }], after)).toBe(after);
    expect(rowChip(rowState(q, "S1"), false)).toEqual({ tone: "unknown", text: "Interrupted · another run" });
    // The operator decides: retry as a new run, skip or end.
    expect(canEnd(ORDER, q)).toBe(true);
    expect(phase(run([{ type: "start", ref: "S1", runIndex: 2, generationBefore: "2" }], q), "S1")).toBe("starting");
    expect(phase(run([{ type: "skip", ref: "S1" }], q), "S1")).toBe("skipped");
  });

  it("another run starting before the queue's first observation of the row's run is never adopted as the row's", () => {
    // S1's start was accepted (generation 1), its refresh failed; another client stopped it and
    // started its own run (generation 2, its own file) before the next good poll.
    const q = run([{ type: "status", status: active("2", "/data/theirs.h5") }], startedS1());
    expect(phase(q, "S1")).toBe("interrupted");
    expect(rowState(q, "S1").attempts[0].generation).toBeNull(); // never adopted
    expect(rowState(q, "S1").outputPath).toBe("");
    expect(run([{ type: "status", status: stopping("2", "/data/theirs.h5") }, { type: "status", status: closed("2", { output_path: "/data/theirs.h5" }) }], q)).toBe(q);
  });

  it("a status of the row's generation on another file, or of its file at an older generation, is not the row's", () => {
    const seen = run([{ type: "status", status: active("1") }], startedS1());
    expect(phase(run([{ type: "status", status: active("1", "/data/other.h5") }], seen), "S1")).toBe("interrupted");
    const unseen = startedS1();
    expect(run([{ type: "status", status: active("0") }], unseen)).toBe(unseen); // the read before the start, same file or not
  });

  it("the backend appends .h5 to a chosen file without it: that is still the row's file", () => {
    expect(recordedPath("/data/s1")).toBe("/data/s1.h5");
    expect(recordedPath("/data/s1.h5")).toBe("/data/s1.h5");
    expect(recordedPath("/data/s1.hdf5")).toBe("/data/s1.hdf5");
    const q = run([
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: STARTED("/data/s1") },
      { type: "status", status: active("1", "/data/s1.h5") },
    ]);
    expect(phase(q, "S1")).toBe("running");
    expect(rowState(q, "S1").attempts[0].generation).toBe("1");
  });

  it("Next sample needs the row's own run seen active: not before the first observation, not for another run", () => {
    const unseen = startedS1();
    expect(ownRunActive(rowState(unseen, "S1"), active("1"))).toBe(false); // the machine has not seen it yet
    expect(run([{ type: "stop", ref: "S1" }], unseen)).toBe(unseen);
    const seen = run([{ type: "status", status: active("1") }], unseen);
    expect(ownRunActive(rowState(seen, "S1"), active("1"))).toBe(true);
    expect(ownRunActive(rowState(seen, "S1"), active("1", "/data/other.h5"))).toBe(false);
    expect(ownRunActive(rowState(seen, "S1"), active("2"))).toBe(false);
    expect(ownRunActive(rowState(seen, "S1"), stopping("1"))).toBe(false);
    expect(ownRunActive(rowState(seen, "S1"), null)).toBe(false);
  });

  it("a stop that is not accepted (or gets no reply) stops the flow there: the run is shown running with the reason; Next sample sends it again", () => {
    let q = run([{ type: "status", status: active("1") }, { type: "stop", ref: "S1" }], startedS1());
    q = run([{ type: "stop-result", ref: "S1", ok: false, message: "no reply to the stop (TRANSPORT_LOST)" }], q);
    expect(phase(q, "S1")).toBe("running");
    expect(rowState(q, "S1").failure).toMatchObject({ step: "stop", title: "S1 · the stop was not accepted" });
    expect(rowChip(rowState(q, "S1"), false).text).toBe("Running · stop not accepted");
    expect(canEnd(ORDER, q)).toBe(true); // abort: the run keeps running, said in the view
    expect(phase(run([{ type: "stop", ref: "S1" }], q), "S1")).toBe("stopping"); // retry
    // The stop did take effect after all: the status says so, and the failure goes.
    const moot = run([{ type: "status", status: stopping("1") }], q);
    expect(phase(moot, "S1")).toBe("stopping");
    expect(rowState(moot, "S1").failure).toBeNull();
  });

  it("a stop reply arriving after the status already showed the end changes nothing", () => {
    let q = run([{ type: "status", status: active("1") }, { type: "stop", ref: "S1" }, { type: "status", status: closed("1") }], startedS1());
    expect(phase(q, "S1")).toBe("finalized");
    expect(run([{ type: "confirm-closed", ref: "S1" }], q)).toBe(q); // its stop reply is still awaited
    q = run([{ type: "stop-result", ref: "S1", ok: false, message: "not active" }], q);
    expect(phase(q, "S1")).toBe("finalized");
    expect(rowState(q, "S1").failure).toBeNull();
    expect(phase(run([{ type: "confirm-closed", ref: "S1" }], q), "S1")).toBe("closed");
  });

  it("an invalid status or one with an unreadable generation is ignored", () => {
    const q = startedS1();
    expect(run([{ type: "status", status: { ...active("1"), valid: false } }], q)).toBe(q);
    expect(run([{ type: "status", status: active("x") }], q)).toBe(q);
    expect(run([{ type: "status", status: null }], q)).toBe(q);
  });
});

describe("the operator's choices", () => {
  it("skip only at a failure; end only between samples or at a failure, never while a run runs or waits for its close", () => {
    expect(run([{ type: "skip", ref: "S1" }])).toEqual(newQueue());
    expect(canEnd(ORDER, newQueue())).toBe(true); // nothing begun (the view shows End queue only once begun)
    const running = run([{ type: "status", status: active("1") }], startedS1());
    expect(canEnd(ORDER, running)).toBe(false);
    expect(run([{ type: "end" }], running)).toBe(running);
    expect(run([{ type: "skip", ref: "S1" }], running)).toBe(running);
    const fin = finalizedS1();
    expect(canEnd(ORDER, fin)).toBe(false);
    const between = run([{ type: "confirm-closed", ref: "S1" }], fin);
    expect(canEnd(ORDER, between)).toBe(true);
    const ended = run([{ type: "end" }], between);
    expect(ended.ended).toBe(true);
    expect(currentIndex(ORDER, ended)).toBe(-1);
    expect(queueInProgress(ORDER, ended)).toBe(false);
    expect(rowChip(rowState(ended, "S2"), true)).toEqual({ tone: "neutral", text: "Not run" });
    expect(run([{ type: "start", ref: "S2", runIndex: 2, generationBefore: "1" }], ended)).toBe(ended);
    expect(rowEditable(ended, "S2")).toBe(false);
  });

  it("the queue completes after the last row's close; nothing more happens", () => {
    let q = finalizedS1();
    q = run([{ type: "confirm-closed", ref: "S1" }], q, ["S1"]);
    expect(currentIndex(["S1"], q)).toBe(-1);
    expect(queueInProgress(["S1"], q)).toBe(false);
    expect(queueBegun(q)).toBe(true);
  });
});

describe("a lost controlling client", () => {
  it("pauses a queue in progress; every action is refused until resumed; status facts still apply; resuming sends nothing and starts nothing", () => {
    const runningQ = run([{ type: "status", status: active("1") }], startedS1());
    const paused = run([{ type: "pause", reason: "control lost" }], runningQ);
    expect(paused.paused).toBe("control lost");
    for (const e of [{ type: "stop", ref: "S1" }, { type: "end" }, { type: "skip", ref: "S1" }] as QueueEvent[]) expect(run([e], paused)).toBe(paused);
    // The server's safe stop finalizes the run meanwhile: shown, but not confirmed for the operator.
    const fin = run([{ type: "status", status: stopping("1") }, { type: "status", status: closed("1") }], paused);
    expect(phase(fin, "S1")).toBe("finalized");
    expect(fin.paused).toBe("control lost");
    expect(run([{ type: "confirm-closed", ref: "S1" }], fin)).toBe(fin);
    const resumed = run([{ type: "resume" }], fin);
    expect(resumed.paused).toBeNull();
    expect(phase(resumed, "S1")).toBe("finalized"); // still waits for the operator's confirmation
    expect(phase(run([{ type: "confirm-closed", ref: "S1" }], resumed), "S1")).toBe("closed");
  });

  it("a Start pending when control is lost still takes its reply (the reply decides), but the queue stays paused", () => {
    const starting = run([{ type: "start", ref: "S1", runIndex: 1, generationBefore: "0" }]);
    const paused = run([{ type: "pause", reason: "control lost" }], starting);
    expect(paused.paused).toBe("control lost");
    const after = run([{ type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: STARTED() }], paused);
    expect(phase(after, "S1")).toBe("running");
    expect(after.paused).toBe("control lost");
  });

  it("does not pause a queue that has not begun, or one that is finished", () => {
    expect(run([{ type: "pause", reason: "x" }]).paused).toBeNull();
    const done = run([{ type: "confirm-closed", ref: "S1" }], finalizedS1(), ["S1"]);
    expect(run([{ type: "pause", reason: "x" }], done, ["S1"]).paused).toBeNull();
  });
});

describe("identity: a run index is never sent twice", () => {
  it("over any sequence of outcomes, the attempts' run indexes are distinct and each Start sends the next one", () => {
    const outcomes: PlanStartResult[] = [
      STARTED(), { kind: "unknown", path: "", message: "lost" }, { kind: "refused", path: "", message: "no" }, { kind: "cancelled" },
      { kind: "not-ready", message: "x" }, { kind: "error", message: "e" }, { kind: "busy" },
    ];
    let seed = 7;
    const rand = (n: number) => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed % n; };
    for (let trial = 0; trial < 50; trial++) {
      let q = newQueue(), gen = 0;
      for (let step = 0; step < 40; step++) {
        const cur = currentIndex(ORDER, q);
        if (cur < 0) break;
        const ref = ORDER[cur], p = phase(q, ref);
        if (["planned", "refused", "unknown", "failed"].includes(p)) {
          const runIndex = q.nextRunIndex;
          q = run([{ type: "start", ref, runIndex, generationBefore: String(gen) },
            { type: "start-result", ref, runIndex, generationBefore: String(gen), result: outcomes[rand(outcomes.length)] }], q);
          if (rand(4) === 0 && p !== "planned") q = run([{ type: "skip", ref }], q);
        } else if (p === "running") {
          gen += 1;
          q = run([{ type: "status", status: active(String(gen)) }, { type: "stop", ref }, { type: "stop-result", ref, ok: true, message: "" },
            { type: "status", status: rand(3) === 0 ? closed(String(gen), { finalization_ok: false }) : closed(String(gen)) }], q);
        } else if (p === "finalized") {
          q = run([{ type: "confirm-closed", ref }], q);
        }
      }
      const used = ORDER.flatMap((ref) => rowState(q, ref).attempts.map((a) => a.runIndex));
      expect(new Set(used).size).toBe(used.length);
      if (used.length) expect(q.nextRunIndex).toBe(Math.max(...used) + 1);
    }
  });
});

describe("editing", () => {
  it("started rows keep their place; only not-started rows move or go, and at least one row stays", () => {
    const q = run([{ type: "status", status: active("1") }], startedS1());
    expect(canMove(ORDER, q, 1, -1)).toBe(false); // S2 cannot move above the started S1
    expect(canMove(ORDER, q, 1, 1)).toBe(true);
    expect(canMove(ORDER, q, 2, 1)).toBe(false); // no row below
    expect(canRemove(ORDER, q, 0)).toBe(false);
    expect(canRemove(ORDER, q, 2)).toBe(true);
    expect(canRemove(["S1"], newQueue(), 0)).toBe(false);
    expect(plannedRunIndex(ORDER, q, 0)).toBeNull(); // S1 has its index
    expect(plannedRunIndex(ORDER, q, 1)).toBe(2);
    expect(plannedRunIndex(ORDER, q, 2)).toBe(3);
  });

  it("an edit that touches anything recorded is refused; edits of rows not yet started pass", () => {
    const draft: PlanDraft = { ...newPlanDraft("3f2b8c1e-9a4d-4e6f-8b21-0c5d7e9f1a2b", { name: "A", id: "" }, "queue"), rows: ORDER.map((r) => emptyRow(r)) };
    const q = startedS1();
    const set = (i: number, condition: string): PlanDraft => ({ ...draft, rows: draft.rows.map((r, j) => (j === i ? { ...r, condition } : r)) });
    expect(editKeepsRecorded(draft, set(1, "Vehicle"), q)).toBe(true);
    expect(editKeepsRecorded(draft, set(0, "Vehicle"), q)).toBe(false);
    expect(editKeepsRecorded(draft, { ...draft, rows: [draft.rows[1], draft.rows[0], draft.rows[2]] }, q)).toBe(false);
    expect(editKeepsRecorded(draft, { ...draft, operator: { name: "B", id: "" } }, q)).toBe(false);
    expect(editKeepsRecorded(draft, { ...draft, format: "single" }, q)).toBe(false);
    expect(editKeepsRecorded(draft, { ...draft, experimentId: "00000000-0000-4000-8000-000000000000" }, q)).toBe(false);
    expect(editKeepsRecorded(draft, { ...draft, rows: [...draft.rows, emptyRow("S4")] }, q)).toBe(true);
    // Before anything began, anything goes.
    expect(editKeepsRecorded(draft, set(0, "Vehicle"), newQueue())).toBe(true);
  });
});

describe("presentation", () => {
  it("the lifecycle stepper: Planned → Starting → Running → Stopping → Finalized → Closed, with refused / unknown / failed flagged", () => {
    const states = (p: RowPhase, failure = false) => lifecycle({ ...rowState(newQueue(), "S1"), phase: p, failure: failure ? { step: "stop", tone: "blocked", title: "", text: "" } : null })
      .map((s) => `${s.key}:${s.state}${s.flag ? `(${s.flag.text})` : ""}`);
    expect(lifecycle(rowState(newQueue(), "S1")).map((s) => s.label)).toEqual(["Planned", "Starting", "Running", "Stopping · drain", "Finalized", "Closed"]);
    expect(states("planned")).toEqual(["planned:current", "starting:pending", "running:pending", "stopping:pending", "finalized:pending", "closed:pending"]);
    expect(states("starting")).toEqual(["planned:done", "starting:current", "running:pending", "stopping:pending", "finalized:pending", "closed:pending"]);
    expect(states("refused")).toEqual(["planned:done", "starting:failed(Not started)", "running:pending", "stopping:pending", "finalized:pending", "closed:pending"]);
    expect(states("unknown")).toEqual(["planned:done", "starting:current(Unknown · reply lost)", "running:pending", "stopping:pending", "finalized:pending", "closed:pending"]);
    expect(states("running")).toEqual(["planned:done", "starting:done", "running:current", "stopping:pending", "finalized:pending", "closed:pending"]);
    expect(states("running", true)[2]).toBe("running:current(Stop not accepted)");
    expect(states("stopping")).toEqual(["planned:done", "starting:done", "running:done", "stopping:current", "finalized:pending", "closed:pending"]);
    expect(states("finalized")).toEqual(["planned:done", "starting:done", "running:done", "stopping:done", "finalized:done", "closed:current"]);
    expect(states("failed")).toEqual(["planned:done", "starting:done", "running:done", "stopping:done", "finalized:failed(Failed)", "closed:pending"]);
    expect(states("closed")).toEqual(["planned:done", "starting:done", "running:done", "stopping:done", "finalized:done", "closed:done"]);
    expect(states("interrupted")).toEqual(["planned:done", "starting:done", "running:failed(Interrupted · another run)", "stopping:pending", "finalized:pending", "closed:pending"]);
  });

  it("row chips use the kit tones", () => {
    const chip = (p: RowPhase) => rowChip({ ...rowState(newQueue(), "S1"), phase: p }, false);
    expect(chip("planned")).toEqual({ tone: "neutral", text: "Planned" });
    expect(chip("refused")).toEqual({ tone: "blocked", text: "Not started" });
    expect(chip("unknown")).toEqual({ tone: "unknown", text: "Unknown · reply lost" });
    expect(chip("failed")).toEqual({ tone: "blocked", text: "Failed" });
    expect(chip("closed")).toEqual({ tone: "confirmed", text: "Closed" });
    expect(rowChip({ ...rowState(newQueue(), "S1"), phase: "closed", end: { ok: true, cancelled: false, outputPath: "", headline: "", severity: "loss" } }, false))
      .toEqual({ tone: "warn", text: "Closed · see outcome" });
  });
});

// Bridge ABI 40 (#741): the stop reply's code, and a start that locks by its code.
describe("the outcome codes (ABI 40)", () => {
  const MISMATCH = "stop refused: expected run 1, but run 2 is active; nothing was stopped";
  /** S1 running as generation 1, its Next sample sent and answered with this reply. */
  const stopAnswered = (ok: boolean, message: string, code?: number) => run([
    { type: "status", status: active("1") },
    { type: "stop", ref: "S1" },
    { type: "stop-result", ref: "S1", ok, message, ...(code === undefined ? {} : { code }) },
  ], startedS1());

  it("RunMismatch: interrupted at the stop with the backend's reason; no further stop is taken", () => {
    const q = stopAnswered(false, MISMATCH, SO.RunMismatch);
    const row = rowState(q, "S1");
    expect(row.phase).toBe("interrupted");
    expect(row.stopPending).toBe(false);
    expect(row.failure).toMatchObject({ step: "stop", tone: "unknown", title: "S1 · not stopped · run 1 is not the backend's active run" });
    expect(row.failure!.text).toContain(MISMATCH);
    expect(row.failure!.text).toContain("nothing more is sent");
    expect(run([{ type: "stop", ref: "S1" }], q)).toBe(q);
    expect(ownRunActive(row, active("1"))).toBe(false);
    expect(rowChip(row, false)).toEqual({ tone: "unknown", text: "Interrupted · not stopped" });
    expect(lifecycle(row).find((st) => st.key === "running")!.flag).toEqual({ tone: "unknown", text: "Interrupted · not stopped" });
    expect(canEnd(ORDER, q)).toBe(true);
  });

  it("after RunMismatch: another run's status keeps the reason; an older Active read changes nothing; the row's own end is followed", () => {
    const q = stopAnswered(false, MISMATCH, SO.RunMismatch);
    expect(run([{ type: "status", status: active("2", "/data/other.h5") }], q)).toBe(q);
    expect(run([{ type: "status", status: closed("2", { output_path: "/data/other.h5" }) }], q)).toBe(q);
    expect(run([{ type: "status", status: active("1") }], q)).toBe(q);
    expect(phase(run([{ type: "status", status: stopping("1") }], q), "S1")).toBe("stopping");
    const done = run([{ type: "status", status: closed("1") }], q);
    expect(phase(done, "S1")).toBe("finalized");
    expect(rowState(done, "S1").failure).toBeNull();
    const failed = run([{ type: "status", status: closed("1", { state: S.Failed, finalization_ok: false, fault_message: "disk full" }) }], q);
    expect(phase(failed, "S1")).toBe("failed");
  });

  it.each([["Busy", SO.Busy], ["NotActive", SO.NotActive], ["no code", undefined]] as const)("%s: the stop was not accepted, as before ABI 40", (name, code) => {
    const q = stopAnswered(false, name, code);
    expect(phase(q, "S1")).toBe("running");
    expect(rowState(q, "S1").failure).toMatchObject({ step: "stop", tone: "blocked", title: "S1 · the stop was not accepted" });
    expect(phase(run([{ type: "stop", ref: "S1" }], q), "S1")).toBe("stopping");
  });

  it("Accepted, with or without its code: stopping until the status shows the end", () => {
    for (const code of [SO.Accepted, undefined]) {
      const q = stopAnswered(true, "Accepted", code);
      expect(phase(q, "S1")).toBe("stopping");
      expect(rowState(q, "S1").stopPending).toBe(false);
    }
  });

  it("a start that locks by its code: unknown, its index used, said as not started with its file possibly left", () => {
    const q = run([
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0",
        result: { kind: "unknown", path: RUN, message: "failed to initialize datasets", code: O.StorageFailed } },
    ]);
    const row = rowState(q, "S1");
    expect(row.phase).toBe("unknown");
    expect(row.attempts).toEqual([{ runIndex: 1, outcome: "unknown", path: RUN, generationBefore: "0", generation: null, code: O.StorageFailed }]);
    expect(q.nextRunIndex).toBe(2);
    expect(notStarted(row)).toBe(true);
    expect(rowChip(row, false)).toEqual({ tone: "unknown", text: "Not started · file may exist" });
    expect(row.failure!.title).toBe(`Not started · S1, run 1 · its file may exist · ${RUN}`);
    expect(rowEditable(q, "S1")).toBe(false);
    // A lost reply is still said as lost.
    const lost = run([
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: { kind: "unknown", path: RUN, message: "TRANSPORT_LOST" } },
    ]);
    expect(notStarted(rowState(lost, "S1"))).toBe(false);
    expect(rowChip(rowState(lost, "S1"), false)).toEqual({ tone: "unknown", text: "Unknown · reply lost" });
  });

  it("a refusal by a code other than InvalidRequest is not read as a sheet problem, whatever its message", () => {
    const q = run([
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0",
        result: { kind: "refused", path: RUN, message: "invalid start request: looks like one", code: O.AlreadyActive } },
    ]);
    expect(rowState(q, "S1").failure!.title).toBe("Not started · the backend refused the Start of S1");
    expect(q.nextRunIndex).toBe(1);
  });
});

describe("the single-sample run, followed in the status (#763 F14)", () => {
  const started = (over: Partial<PlanStarted> = {}): PlanStarted => ({ path: "/data/run", notice: "", runIndex: 1, outcome: "started", generationBefore: "4", ...over });
  const on = (over: Partial<ExperimentStatus>) => status({ output_path: RUN, ...over });

  it("takes the first newer run on its file, then follows only that generation to its end", () => {
    let st = started();
    expect(followSingleRun(st, on({ terminal: true, finalization_ok: true, start_generation: "4" }))).toBe(st); // the run before the Start
    expect(followSingleRun(st, status({ state: S.Active, start_generation: "5", output_path: "/data/other.h5" }))).toBe(st); // another file
    st = followSingleRun(st, on({ state: S.Active, start_generation: "5" }));
    expect(st.generation).toBe("5");
    expect(singleRunChip(st, on({ state: S.Active, start_generation: "5" }))).toEqual({ tone: "info", text: "Running" });
    expect(singleRunChip(st, on({ state: S.Stopping, flushing: true, start_generation: "5" }))).toEqual({ tone: "info", text: "Stopping · draining" });
    expect(followSingleRun(st, on({ terminal: true, finalization_ok: true, start_generation: "6" }))).toBe(st); // a later run on the same file
    st = followSingleRun(st, on({ terminal: true, finalization_ok: true, start_generation: "5" }));
    expect(st.end).toEqual({ ok: true, cancelled: false });
    expect(singleRunChip(st, status({ state: S.Active, start_generation: "6", output_path: "/data/other.h5" }))).toEqual({ tone: "confirmed", text: "Finalized · file closed" });
    expect(followSingleRun(st, on({ state: S.Failed, terminal: true, start_generation: "5" }))).toBe(st); // the end stays
  });

  it("a failed or cancelled end is said as such", () => {
    const failed = followSingleRun(started({ generation: "5" }), on({ state: S.Failed, terminal: true, start_generation: "5" }));
    expect(singleRunChip(failed, null)).toEqual({ tone: "blocked", text: "Failed" });
    const cancelled = followSingleRun(started({ generation: "5" }), on({ terminal: true, finalization_ok: true, cancelled: true, start_generation: "5" }));
    expect(singleRunChip(cancelled, null)).toEqual({ tone: "confirmed", text: "Finalized · cancelled" });
  });

  it("without a status read before its Start, a terminal status is never taken for it until its run was seen in progress", () => {
    let st = started({ generationBefore: null });
    expect(followSingleRun(st, on({ terminal: true, finalization_ok: true, start_generation: "3" }))).toBe(st);
    st = followSingleRun(st, on({ state: S.Active, start_generation: "5" }));
    expect(followSingleRun(st, on({ terminal: true, finalization_ok: true, start_generation: "5" })).end).toEqual({ ok: true, cancelled: false });
  });

  it("an unknown start, or none, is said as before", () => {
    expect(singleRunChip(null, null)).toEqual({ tone: "neutral", text: "Planned" });
    expect(singleRunChip(started({ outcome: "unknown" }), null)).toEqual({ tone: "unknown", text: "Start outcome not known" });
    expect(followSingleRun(started({ outcome: "unknown" }), on({ terminal: true, finalization_ok: true, start_generation: "5" })).end).toBeUndefined();
    expect(singleRunChip(started(), null)).toEqual({ tone: "info", text: "Started" });
  });
});
