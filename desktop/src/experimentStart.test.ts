import { describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { bridge } from "./bridge";
import { COMMAND_TYPES, JSON_TRANSPORT } from "./bridgeContract";
import { startExperiment, type StartDeps, type StartReadiness } from "./experimentStart";
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

describe("bridge.experimentStart on the transport", () => {
  const reply = { transport_version: JSON_TRANSPORT.version, ok: true, command: COMMAND_TYPES.Experiment, message: "started", operation_id: "1" };
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
