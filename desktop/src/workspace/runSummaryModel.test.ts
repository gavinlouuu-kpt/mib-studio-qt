// The O5 Run summary's hand-off model (#716 A7c): the lifecycle checklist only from observations
// (unknown otherwise), the duration only from recorded timestamps, the board's placeholders in plan
// order, End queue over the queue's own events, and the calibration record matched by its source run.
import { describe, expect, it } from "vitest";
import { calList, calRecord } from "./calibrationFixtures";
import { emptyRow } from "./planModel";
import type { PlanRun } from "./quickLookModel";
import { newQueue, reduceQueue, type QueueEvent, type QueueState } from "./queueModel";
import {
  boardEntries,
  cvText,
  durationText,
  endQueueState,
  gateLines,
  noRecordReason,
  outcomeChecklist,
  recordForRun,
  stopReason,
} from "./runSummaryModel";

const run = (over: Partial<PlanRun> = {}): PlanRun => ({
  path: "/d/s1.h5", runIndex: 1, ref: "S1", row: null, confirmed: false, outcome: { cancelled: false, headline: "" },
  followed: "status", seen: {}, times: null, ...over,
});
const states = (r: PlanRun) => outcomeChecklist(r).map((s) => `${s.key}:${s.state}`);

describe("the lifecycle checklist: a step is ticked only when observed", () => {
  it("nothing observed beyond the start and the terminal status: the middle steps are unknown", () => {
    expect(states(run())).toEqual(["requested:done", "acknowledged:unknown", "active:unknown", "stopping:unknown", "drain:done", "closed:current"]);
  });
  it("every state observed and the close confirmed", () => {
    expect(states(run({ seen: { frame: true, active: true, stopping: true }, confirmed: true })))
      .toEqual(["requested:done", "acknowledged:done", "active:done", "stopping:done", "drain:done", "closed:done"]);
    expect(states(run({ seen: { stopAccepted: true } }))[3]).toBe("stopping:done");
  });
  it("a single sample: followed by its operation, so the status steps stay unknown", () => {
    expect(states(run({ followed: "operation", seen: null, confirmed: true })))
      .toEqual(["requested:done", "acknowledged:unknown", "active:unknown", "stopping:unknown", "drain:done", "closed:done"]);
  });
  it("the stop reason only as observed", () => {
    expect(stopReason(run())).toBeNull();
    expect(stopReason(run({ seen: { stopAccepted: true } }))).toContain("Next sample");
    expect(stopReason(run({ outcome: { cancelled: true, headline: "" } }))).toBe("cancelled");
  });
});

describe("the duration: only from the recorded start and end", () => {
  it("formats the span; Not recorded without both, with a zero, or reversed", () => {
    expect(durationText(run({ times: { startNs: "1000000000000", endNs: "1041500000000" } }))).toBe("41.5 s");
    expect(durationText(run({ times: { startNs: "1000000000000", endNs: "1261000000000" } }))).toBe("4 min 21 s");
    expect(durationText(run({ times: { startNs: "1", endNs: "3723000000001" } }))).toBe("1 h 2 min 3 s");
    expect(durationText(run())).toBe("Not recorded");
    expect(durationText(run({ times: { startNs: "0", endNs: "5000" } }))).toBe("Not recorded");
    expect(durationText(run({ times: { startNs: "9000", endNs: "5000" } }))).toBe("Not recorded");
    // Above 2^53 ns (≈ 104 days of epoch): exact, no float.
    expect(durationText(run({ times: { startNs: "1792000000000000000", endNs: "1792000261000000000" } }))).toBe("4 min 21 s");
  });
});

const ORDER = ["S1", "S2", "S3"];
const draft = { format: "queue", rows: ORDER.map((r) => emptyRow(r)) };
const ev = (q: QueueState, events: QueueEvent[]) => events.reduce((s, e) => reduceQueue(s, e, ORDER), q);
const status = (over: Record<string, unknown>) => ({
  valid: true, state: 0, start_time_ns: "0", end_time_ns: "0", valid_buffered: "0", invalid_buffered: "0", valid_saved: "0", invalid_saved: "0",
  dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false, output_path: "/d/s1.h5", message: "", start_generation: "1", readiness_generation: "1",
  capture_generation: "1", persistence_admitted: "0", persistence_committed: "0", persistence_failed: "0", terminal: false, finalization_ok: false,
  completion: 0, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "", ...over,
}) as never;
const startS1: QueueEvent[] = [
  { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
  { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: { kind: "started", path: "/d/s1", notice: "" } },
];
const finalizedS1 = () => ev(newQueue(), [...startS1, { type: "status", status: status({ state: 2 }) }, { type: "status", status: status({ terminal: true, finalization_ok: true }) }]);

describe("the board: closed runs and placeholders in plan order", () => {
  const kinds = (es: ReturnType<typeof boardEntries>) => es.map((e) => (e.kind === "run" ? `run ${e.run.runIndex}` : e.kind === "active" ? `active ${e.ref}` : `${e.ref}: ${e.text}`));
  it("a fresh queue is all placeholders; a closed run takes its row's place; an ended queue's rows read Not run", () => {
    expect(kinds(boardEntries({ draft, queue: newQueue(), started: null }, [], false))).toEqual(["S1: Queued", "S2: Queued", "S3: Queued"]);
    const q = finalizedS1();
    expect(kinds(boardEntries({ draft, queue: q, started: null }, [run()], false))).toEqual(["run 1", "S2: Queued", "S3: Queued"]);
    const ended = ev(q, [{ type: "confirm-closed", ref: "S1" }, { type: "end" }]);
    expect(kinds(boardEntries({ draft, queue: ended, started: null }, [run({ confirmed: true })], false))).toEqual(["run 1", "S2: Not run", "S3: Not run"]);
  });
  it("the run in progress sits in its row; a run not of this plan comes last", () => {
    const q = ev(newQueue(), [...startS1]);
    expect(kinds(boardEntries({ draft, queue: q, started: null }, [], true))).toEqual(["active S1", "S2: Queued", "S3: Queued"]);
    expect(kinds(boardEntries({ draft, queue: newQueue(), started: null }, [], true))).toEqual(["S1: Queued", "S2: Queued", "S3: Queued", "active null"]);
  });
  it("every placeholder is grey", () => {
    for (const e of boardEntries({ draft, queue: newQueue(), started: null }, [], false)) if (e.kind === "placeholder") expect(e.tone).toBe("neutral");
  });
});

describe("End queue: the queue's own end, after confirming a finalized run's close", () => {
  it("finalized with samples left: confirm then end; at the last sample, between samples, and refusals", () => {
    const q = finalizedS1();
    expect(endQueueState(ORDER, q, true, false)).toMatchObject({ reason: "", confirmFirst: true });
    expect(endQueueState(["S1"], q, true, false).reason).toContain("S1 is the last sample");
    const between = ev(q, [{ type: "confirm-closed", ref: "S1" }]);
    expect(endQueueState(ORDER, between, true, false)).toMatchObject({ reason: "", confirmFirst: false });
    expect(endQueueState(ORDER, between, true, true).reason).toBe("a Start or a stop is pending");
    expect(endQueueState(ORDER, { ...between, paused: "lost" }, true, false).reason).toContain("paused");
    expect(endQueueState(ORDER, newQueue(), true, false).reason).toContain("has not begun");
    expect(endQueueState(ORDER, ev(newQueue(), startS1), true, false).reason).toContain("S1 is running");
    expect(endQueueState(ORDER, q, false, false).reason).toBe("a single-sample plan has no queue to end");
  });
});

describe("the calibration record of a run", () => {
  it("matched by its source run_id, the newest version; none without the run's id", () => {
    const a = calRecord("cal-1", "proposed", { source: { run_id: "r-1", file: "/d/s1.h5", file_sha256: null, start_generation: 1 }, sequence: 1 });
    const b = calRecord("cal-1", "applied", { source: { run_id: "r-1", file: "/d/s1.h5", file_sha256: null, start_generation: 1 }, sequence: 2 });
    const other = calRecord("cal-2", "proposed", { source: { run_id: "r-2", file: "/d/s2.h5", file_sha256: null, start_generation: 2 } });
    const list = calList([a, other, b]);
    expect(recordForRun(list, "r-1")).toBe(b);
    expect(recordForRun(list, "r-3")).toBeNull();
    expect(recordForRun(list, null)).toBeNull();
    expect(recordForRun({ valid: false }, "r-1")).toBeNull();
  });
  it("gates as recorded (met / not met with value and limit), the CV with its limit, and why no record is shown", () => {
    const ok = calRecord("cal-1", "proposed");
    expect(gateLines(ok).map((g) => g.text)).toEqual(["Valid objects: met · 812, limit 200", "Diameter CV: met · 3.1, limit 10"]);
    expect(gateLines(calRecord("cal-x", "insufficient")).map((g) => g.met)).toEqual([false, false]);
    expect(cvText(ok)).toBe("3.1 % · limit 10 %");
    const base = { listError: "", storeError: null, listRead: true, runIdKnown: true, runId: "r-1" };
    expect(noRecordReason(base)).toContain("none of the backend's records names this run (run id r-1)");
    expect(noRecordReason({ ...base, runId: null })).toContain("records no run id");
    expect(noRecordReason({ ...base, runIdKnown: false })).toContain("not known yet");
    expect(noRecordReason({ ...base, listError: "TRANSPORT_LOST" })).toContain("could not be read (TRANSPORT_LOST)");
    expect(noRecordReason({ ...base, storeError: "corrupt" })).toContain("store cannot be read");
    expect(noRecordReason({ ...base, listValid: false })).toContain("listed no records");
  });
});

describe("this model reads no quick look value", () => {
  const src = Object.values(import.meta.glob("./runSummaryModel.ts", { query: "?raw", import: "default", eager: true }) as Record<string, string>)[0];
  it("imports nothing of the quick look and names none of its numbers", () => {
    expect(src.length).toBeGreaterThan(1000);
    expect(src).not.toMatch(/from "\.\.\/runQuickLook"|RunStatistics|RunQuickLook|\.statistics\b|median|delta/);
  });
});
