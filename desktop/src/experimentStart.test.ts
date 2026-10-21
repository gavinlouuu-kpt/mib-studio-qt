import { describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { bridge } from "./bridge";
import { COMMAND_TYPES, EXPERIMENT_START_OUTCOMES as O, JSON_TRANSPORT } from "./bridgeContract";
import { START_OUTCOME_STATES, startExperiment, startReplyState, type StartDeps, type StartReadiness, type StartReplyState } from "./experimentStart";
import type { RingStatus } from "./ringPlayback";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));

const ring = (over: Partial<RingStatus> = {}): RingStatus => ({
  available: true, frozen: true, invalid: false, stop_incomplete: false, restore_needed: false, run_frozen: true, capacity_frames: 5000, first_seq: 0, last_seq: 4999,
  count: 5000, sensor_fps: 5000, ...over,
} as RingStatus);
const ready: StartReadiness = { valid: true, ready: true, gates: [{ id: "run.frozen", status: 1, reason: "Run is stopped to review the buffered frames" }] };

function deps(over: Partial<StartDeps> = {}) {
  const d = {
    fetchRingStatus: vi.fn(async () => ring()),
    confirm: vi.fn(async () => true),
    pickFile: vi.fn(async () => "/data/run.h5"),
    fetchReadiness: vi.fn(async () => ready),
    start: vi.fn(async () => ({ ok: true, message: "started" })),
    ...over,
  };
  return d;
}

describe("starting an experiment from a stopped Run", () => {
  it("asks first, with the number of frames, and tells the backend that the operator agreed", async () => {
    const d = deps();
    const out = await startExperiment(d);
    expect(d.confirm).toHaveBeenCalledWith("Starting an experiment discards the 5,000 buffered frames of the stopped Run. Continue?");
    expect(d.start).toHaveBeenCalledWith("/data/run.h5", true);
    expect(out).toEqual({ kind: "started", path: "/data/run.h5", notice: "" });
  });
  it("a refused confirm starts nothing, and does not even open the file dialog", async () => {
    const d = deps({ confirm: vi.fn(async () => false) });
    expect(await startExperiment(d)).toEqual({ kind: "cancelled" });
    expect(d.pickFile).not.toHaveBeenCalled();
    expect(d.start).not.toHaveBeenCalled();
  });
  it("uses a fresh ring status, not a polled snapshot: it asks even when the page never saw the stopped Run", async () => {
    const d = deps();
    await startExperiment(d);
    expect(d.fetchRingStatus).toHaveBeenCalledTimes(1);
  });
  it("asks nothing, and does not acknowledge, when no Run is stopped", async () => {
    const d = deps({ fetchRingStatus: vi.fn(async () => ring({ run_frozen: false })) });
    await startExperiment(d);
    expect(d.confirm).not.toHaveBeenCalled();
    expect(d.start).toHaveBeenCalledWith("/data/run.h5", false);
  });
  it("a ring status that cannot be fetched does not block a start the backend would accept (it enforces the acknowledgement itself)", async () => {
    const d = deps({ fetchRingStatus: vi.fn(async () => { throw new Error("offline"); }) });
    const out = await startExperiment(d);
    expect(d.start).toHaveBeenCalledWith("/data/run.h5", false);
    expect(out.kind).toBe("started");
  });
  it("the backend's refusal of an unacknowledged discard comes back as the refusal", async () => {
    const d = deps({ fetchRingStatus: vi.fn(async () => ring({ run_frozen: false })),
                     start: vi.fn(async () => ({ ok: false, message: "Run is stopped to review 5000 buffered frames: starting an experiment discards them; acknowledge the discard to start (acknowledgeDiscardRing)" })) });
    const out = await startExperiment(d);
    expect(out).toMatchObject({ kind: "refused", path: "/data/run.h5" });
    expect((out as { message: string }).message).toContain("acknowledgeDiscardRing");
  });
  it("a cancelled file dialog and a readiness failure start nothing", async () => {
    expect(await startExperiment(deps({ pickFile: vi.fn(async () => null) }))).toEqual({ kind: "cancelled" });
    const d = deps({ fetchReadiness: vi.fn(async () => ({ valid: true, ready: false, gates: [{ id: "storage.ssd", status: 2, reason: "export in progress", remediation: "wait" }] })) });
    expect(await startExperiment(d)).toEqual({ kind: "not-ready", message: "/data/run.h5: storage.ssd: export in progress — wait" });
    expect(d.start).not.toHaveBeenCalled();
  });
  it("carries the persistence warning of the readiness gates", async () => {
    const d = deps({ fetchReadiness: vi.fn(async () => ({ ...ready, gates: [{ id: "storage.persistent", status: 1, reason: "recording to RAM" }] })) });
    expect(await startExperiment(d)).toEqual({ kind: "started", path: "/data/run.h5", notice: "recording to RAM" });
  });
});

// ABI 39 (#712, #716 A5): the workspace Plan's startRequestJson goes through the same flow, unchanged.
describe("the start request of a plan", () => {
  const REQUEST = '{"experiment_id":"3f2b8c1e-9a4d-4e6f-8b21-0c5d7e9f1a2b","run_index":1,"sample_ref":"S1","annotation":{"notes":"a\\nb  ","condition":"µ"}}';
  it("is passed to the start byte for byte", async () => {
    const d = deps();
    expect(await startExperiment(d, REQUEST)).toEqual({ kind: "started", path: "/data/run.h5", notice: "" });
    expect(d.start).toHaveBeenCalledTimes(1);
    expect(vi.mocked(d.start).mock.calls[0]).toEqual(["/data/run.h5", true, REQUEST]);
    expect(vi.mocked(d.start).mock.calls[0][2]).toBe(REQUEST);
  });
  it("an old caller without a plan sends no third argument", async () => {
    const d = deps();
    await startExperiment(d);
    expect(vi.mocked(d.start).mock.calls[0]).toHaveLength(2);
  });
  it("a cancelled dialog or a failed readiness sends nothing, request or not", async () => {
    const cancelled = deps({ pickFile: vi.fn(async () => null) });
    expect(await startExperiment(cancelled, REQUEST)).toEqual({ kind: "cancelled" });
    expect(cancelled.start).not.toHaveBeenCalled();
    const notReady = deps({ fetchReadiness: vi.fn(async () => ({ valid: true, ready: false, gates: [] })) });
    expect((await startExperiment(notReady, REQUEST)).kind).toBe("not-ready");
    expect(notReady.start).not.toHaveBeenCalled();
  });
  it("a start call that throws is an unknown outcome, not an error before sending: the request may have been accepted", async () => {
    const d = deps({ start: vi.fn(async () => { throw new Error("TRANSPORT_LOST"); }) });
    expect(await startExperiment(d, REQUEST)).toEqual({ kind: "unknown", path: "/data/run.h5", message: "Error: TRANSPORT_LOST" });
  });
  it("an error before the start call (dialog, readiness) is thrown to the caller: nothing was sent", async () => {
    const d = deps({ fetchReadiness: vi.fn(async () => { throw new Error("offline"); }) });
    await expect(startExperiment(d, REQUEST)).rejects.toThrow("offline");
    expect(d.start).not.toHaveBeenCalled();
  });
  it("the backend's InvalidRequest comes back as the refusal, with its message", async () => {
    const message = "invalid start request: annotation.condition: 300 bytes, at most 256";
    const d = deps({ start: vi.fn(async () => ({ ok: false, message })) });
    expect(await startExperiment(d, REQUEST)).toEqual({ kind: "refused", path: "/data/run.h5", message });
  });
});

// ABI 40 (#741): the start reply's start_outcome decides started / refused / unknown, not its message.
// Written out here, not read from START_OUTCOME_STATES, so a change of the mapping fails a test.
const EXPECTED: Record<keyof typeof O, StartReplyState> = {
  Started: "started",
  NotReady: "unknown",         // also after the file and snapshot (the PZ7035 provider did not start)
  StaleReadiness: "refused",
  AlreadyActive: "refused",
  StorageFailed: "unknown",    // the file was opened: initializeDatasets failing leaves it
  ProvenanceFailed: "unknown", // the file was opened: removed best effort
  Busy: "refused",
  InvalidRequest: "refused",
};
const CODES = Object.entries(O) as [keyof typeof O, number][];

describe("the start reply's outcome code (ABI 40)", () => {
  it("every code of the contract has a state, and no other code does", () => {
    expect(Object.keys(EXPECTED).sort()).toEqual(Object.keys(O).sort());
    expect(Object.keys(START_OUTCOME_STATES).map(Number).sort()).toEqual(Object.values(O).sort());
  });
  it.each(CODES)("%s (%i) maps to its state through the Start flow", async (name, code) => {
    const ok = code === O.Started;
    const d = deps({ start: vi.fn(async () => ({ ok, message: `reply ${name}`, start_outcome: code })) });
    const out = await startExperiment(d, REQUEST_TEXT);
    expect(out.kind).toBe(EXPECTED[name]);
    if (out.kind === "started") expect(out).toEqual({ kind: "started", path: "/data/run.h5", notice: "" });
    else expect(out).toEqual({ kind: EXPECTED[name], path: "/data/run.h5", message: `reply ${name}`, code });
  });
  it("the message does not decide when a code is present: a refusal code with the InvalidRequest prefix is still its own code", async () => {
    const message = "invalid start request: looks like a sheet problem";
    const d = deps({ start: vi.fn(async () => ({ ok: false, message, start_outcome: O.StorageFailed })) });
    expect(await startExperiment(d, REQUEST_TEXT)).toEqual({ kind: "unknown", path: "/data/run.h5", message, code: O.StorageFailed });
  });
  it("a code that contradicts ok, or a code not in this table, is unknown: the run may exist", () => {
    expect(startReplyState({ ok: true, message: "", start_outcome: O.StorageFailed })).toBe("unknown");
    expect(startReplyState({ ok: true, message: "", start_outcome: O.InvalidRequest })).toBe("unknown");
    expect(startReplyState({ ok: false, message: "", start_outcome: O.Started })).toBe("unknown");
    expect(startReplyState({ ok: false, message: "", start_outcome: 99 })).toBe("unknown");
    expect(startReplyState({ ok: true, message: "", start_outcome: 99 })).toBe("unknown");
  });
  it("without a code (a backend before ABI 40) ok decides, as before", async () => {
    expect(startReplyState({ ok: true, message: "" })).toBe("started");
    expect(startReplyState({ ok: false, message: "" })).toBe("refused");
    const d = deps({ start: vi.fn(async () => ({ ok: false, message: "already active" })) });
    expect(await startExperiment(d, REQUEST_TEXT)).toEqual({ kind: "refused", path: "/data/run.h5", message: "already active" });
  });
  it("bridge.experimentStart decodes the code from the transport reply", async () => {
    vi.mocked(invoke).mockReset();
    vi.mocked(invoke).mockResolvedValue({ transport_version: JSON_TRANSPORT.version, ok: false, command: COMMAND_TYPES.Experiment, message: "storage", operation_id: "0",
      start_outcome: O.StorageFailed });
    const res = await bridge.experimentStart("/data/a.h5", false, REQUEST_TEXT);
    expect(res.start_outcome).toBe(O.StorageFailed);
    expect(startReplyState(res)).toBe("unknown");
  });
});

describe("bridge.experimentStart on the transport", () => {
  const reply = { transport_version: JSON_TRANSPORT.version, ok: true, command: COMMAND_TYPES.Experiment, message: "started", operation_id: "1" };
  it("a started outcome names the reply's operation, the run's identity in the reply (#763 F14); \"0\" or none names none", async () => {
    vi.mocked(invoke).mockReset();
    vi.mocked(invoke).mockResolvedValue({ ...reply, operation_id: "9007199254740993", start_outcome: O.Started });
    const started = deps({ fetchRingStatus: vi.fn(async () => ring({ run_frozen: false })), start: (p, a, j) => bridge.experimentStart(p, a, j) });
    expect(await startExperiment(started, REQUEST_TEXT)).toEqual({ kind: "started", path: "/data/run.h5", notice: "", operationId: "9007199254740993" });
    vi.mocked(invoke).mockResolvedValue({ ...reply, operation_id: "0" });
    expect(await startExperiment(started, REQUEST_TEXT)).toEqual({ kind: "started", path: "/data/run.h5", notice: "" });
    expect(await startExperiment(deps({ fetchRingStatus: vi.fn(async () => ring({ run_frozen: false })) }))).toEqual({ kind: "started", path: "/data/run.h5", notice: "" });
  });
  it("sends startRequestJson only when given, and then exactly the text", async () => {
    vi.mocked(invoke).mockReset();
    vi.mocked(invoke).mockResolvedValue(reply);
    await bridge.experimentStart("/data/a.h5", false);
    await bridge.experimentStart("/data/b.h5", true, REQUEST_TEXT);
    expect(vi.mocked(invoke).mock.calls).toEqual([
      ["experiment_start", { outputPath: "/data/a.h5", acknowledgeDiscardRing: false }],
      ["experiment_start", { outputPath: "/data/b.h5", acknowledgeDiscardRing: true, startRequestJson: REQUEST_TEXT }],
    ]);
    expect(Object.keys(vi.mocked(invoke).mock.calls[0][1] as object)).not.toContain("startRequestJson");
  });
});
const REQUEST_TEXT = '{"experiment_id":"3f2b8c1e-9a4d-4e6f-8b21-0c5d7e9f1a2b","run_index":1,"sample_ref":"S1"}';

// #739 review: a Start that is no longer wanted (the workspace lost control while it was pending)
// sends nothing, wherever it is: the discard question, the file dialog, readiness.
describe("a Start that is no longer wanted", () => {
  const until = (step: "ring" | "confirm" | "pick" | "readiness") => {
    let wanted = true;
    const d = deps({
      fetchRingStatus: vi.fn(async () => { if (step === "ring") wanted = false; return ring(); }),
      confirm: vi.fn(async () => { if (step === "confirm") wanted = false; return true; }),
      pickFile: vi.fn(async () => { if (step === "pick") wanted = false; return "/data/run.h5"; }),
      fetchReadiness: vi.fn(async () => { if (step === "readiness") wanted = false; return ready; }),
    });
    return { d, stillWanted: () => wanted };
  };
  it.each(["ring", "confirm", "pick", "readiness"] as const)("withdrawn while %s is pending: nothing is sent, the outcome says so", async (step) => {
    const { d, stillWanted } = until(step);
    expect(await startExperiment(d, '{"run_index":1}', stillWanted)).toEqual({ kind: "withdrawn" });
    expect(d.start).not.toHaveBeenCalled();
  });
  it("the file dialog's answer is not used once the Start is withdrawn (not even its readiness is read)", async () => {
    const { d, stillWanted } = until("pick");
    await startExperiment(d, '{"run_index":1}', stillWanted);
    expect(d.fetchReadiness).not.toHaveBeenCalled();
  });
  it("still wanted at every step: started, with the request", async () => {
    const d = deps();
    const stillWanted = vi.fn(() => true);
    expect(await startExperiment(d, '{"run_index":1}', stillWanted)).toEqual({ kind: "started", path: "/data/run.h5", notice: "" });
    expect(d.start).toHaveBeenCalledWith("/data/run.h5", true, '{"run_index":1}');
    expect(stillWanted.mock.calls.length).toBeGreaterThanOrEqual(4);
  });
});
