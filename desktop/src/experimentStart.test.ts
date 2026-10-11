import { describe, expect, it, vi } from "vitest";
import { startExperiment, type StartDeps, type StartReadiness } from "./experimentStart";
import type { RingStatus } from "./ringPlayback";

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
