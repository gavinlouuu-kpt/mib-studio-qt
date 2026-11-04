import { describe, expect, it, vi } from "vitest";
import { EXPERIMENT_START_OUTCOMES as O } from "./bridgeContract";
import { startExperiment, type StartDeps, type StartDialog, type StartGate, type StartReadiness } from "./experimentStart";
import type { RingStatus } from "./ringPlayback";

const ring = (over: Partial<RingStatus> = {}): RingStatus => ({
  available: true, frozen: true, invalid: false, stop_incomplete: false, restore_needed: false, run_frozen: true, capacity_frames: 5000, first_seq: 0, last_seq: 4999,
  count: 5000, sensor_fps: 5000, ...over,
} as RingStatus);
const ready: StartReadiness = { valid: true, ready: true, gates: [{ id: "run.frozen", status: 1, reason: "Run is stopped to review the buffered frames" }] };
const gate = (id: string, status: number, reason = "", remediation = ""): StartGate => ({ id, status, reason, remediation });
// With no destination chosen the backend leaves storage.output Unavailable: that alone must not stop the flow.
const preflightOk: StartReadiness = { valid: true, ready: false, gates: [...ready.gates, gate("storage.output", 3, "no output path chosen yet", "choose an HDF5 destination")] };

function deps(over: Partial<StartDeps> = {}) {
  const base = {
    fetchRingStatus: vi.fn(async () => ring()),
    confirm: vi.fn(async () => true),
    pickFile: vi.fn(async () => "/data/run.h5"),
    fetchReadiness: vi.fn(async (path: string): Promise<StartReadiness> => (path === "" ? preflightOk : ready)),
    start: vi.fn(async () => ({ ok: true, message: "started" })),
    inform: vi.fn(async (_dialog: StartDialog) => undefined),
  };
  // The mocks stay typed as mocks (assertions on .mock.calls); `over` replaces members for one test.
  return { ...base, ...over } as typeof base;
}

describe("starting an experiment from a stopped Run", () => {
  it("asks first, with the number of frames, and tells the backend that the operator agreed", async () => {
    const d = deps();
    const out = await startExperiment(d);
    expect(d.confirm).toHaveBeenCalledWith("Starting an experiment discards the 5,000 buffered frames of the stopped Run. Continue?");
    expect(d.start).toHaveBeenCalledWith("/data/run.h5", true, false);
    expect(out).toEqual({ kind: "started", path: "/data/run.h5", notice: "", warnings: [] });
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
    expect(d.start).toHaveBeenCalledWith("/data/run.h5", false, false);
  });
  it("a ring status that cannot be fetched does not block a start the backend would accept (it enforces the acknowledgement itself)", async () => {
    const d = deps({ fetchRingStatus: vi.fn(async () => { throw new Error("offline"); }) });
    const out = await startExperiment(d);
    expect(d.start).toHaveBeenCalledWith("/data/run.h5", false, false);
    expect(out.kind).toBe("started");
  });
  it("the backend's refusal of an unacknowledged discard comes back as the refusal", async () => {
    const d = deps({ fetchRingStatus: vi.fn(async () => ring({ run_frozen: false })),
                     start: vi.fn(async () => ({ ok: false, message: "Run is stopped to review 5000 buffered frames: starting an experiment discards them; acknowledge the discard to start (acknowledgeDiscardRing)" })) });
    const out = await startExperiment(d);
    expect(out).toMatchObject({ kind: "refused", path: "/data/run.h5" });
    expect((out as { message: string }).message).toContain("acknowledgeDiscardRing");
  });
});

describe("the order of checks (MON-003)", () => {
  it("explains a blocking gate before the file dialog opens, and before the ring question", async () => {
    const blocked: StartReadiness = { valid: true, ready: false, gates: [gate("camera.rate", 2, "the camera allows at most 500 fps", "use a lower frame rate"), gate("storage.output", 3, "no output path chosen yet")] };
    const d = deps({ fetchReadiness: vi.fn(async () => blocked) });
    const out = await startExperiment(d);
    expect(d.fetchReadiness).toHaveBeenCalledWith("");
    expect(d.pickFile).not.toHaveBeenCalled();
    expect(d.confirm).not.toHaveBeenCalled();
    expect(d.start).not.toHaveBeenCalled();
    expect(out).toEqual({ kind: "not-ready", message: "camera.rate: the camera allows at most 500 fps — use a lower frame rate", gates: [blocked.gates[0]] });
    expect(d.inform).toHaveBeenCalledTimes(1);
    expect(d.inform.mock.calls[0][0]).toMatchObject({ kind: "warning", title: "Start Experiment" });
    expect(d.inform.mock.calls[0][0].text).toContain("Blocked — camera.rate: the camera allows at most 500 fps\n    → use a lower frame rate");
    expect(d.inform.mock.calls[0][0].text).not.toContain("storage.output");
  });
  it("does not treat the unknown destination as a blocker", async () => {
    const d = deps();
    expect((await startExperiment(d)).kind).toBe("started");
    expect(d.inform).not.toHaveBeenCalled();
    expect(d.fetchReadiness.mock.calls.map((c) => c[0])).toEqual(["", "/data/run.h5"]);
  });
  it("an unavailable backend stops the flow before the dialog", async () => {
    const d = deps({ fetchReadiness: vi.fn(async () => ({ valid: false, ready: false, gates: [] })) });
    expect(await startExperiment(d)).toEqual({ kind: "not-ready", message: "Backend readiness unavailable; experiment was not started.", gates: [] });
    expect(d.pickFile).not.toHaveBeenCalled();
  });
  it("a cancelled file dialog starts nothing", async () => {
    expect(await startExperiment(deps({ pickFile: vi.fn(async () => null) }))).toEqual({ kind: "cancelled" });
  });
  it("a gate that fails only for the chosen destination is explained after the dialog, with the warnings", async () => {
    const blocked: StartReadiness = { valid: true, ready: false, gates: [gate("storage.ssd", 2, "export in progress", "wait"), gate("storage.buffer", 1, "the buffer holds 0.8 s", "free memory")] };
    const d = deps({ fetchReadiness: vi.fn(async (p: string) => (p === "" ? preflightOk : blocked)) });
    expect(await startExperiment(d)).toEqual({ kind: "not-ready", message: "/data/run.h5: storage.ssd: export in progress — wait", gates: [blocked.gates[0]] });
    expect(d.start).not.toHaveBeenCalled();
    expect(d.inform.mock.calls[0][0].text).toContain("Warning — storage.buffer: the buffer holds 0.8 s\n    → free memory");
  });
});

describe("what a started run carries", () => {
  it("every warning of the readiness gates, not only the persistence one (MON-004)", async () => {
    const warn = [gate("storage.persistent", 1, "recording to RAM", "record to the SATA disk"), gate("camera.rate", 1, "rate cannot be checked"), gate("run.frozen", 1, "Run is stopped")];
    const d = deps({ fetchReadiness: vi.fn(async (p: string) => (p === "" ? preflightOk : { valid: true, ready: true, gates: warn })) });
    const out = await startExperiment(d);
    expect(out).toMatchObject({ kind: "started", notice: "recording to RAM (record to the SATA disk)\nrate cannot be checked" });
    expect((out as { warnings: StartGate[] }).warnings.map((g) => g.id)).toEqual(["storage.persistent", "camera.rate"]);
  });
});

describe("multi-image series (MON-008)", () => {
  it("tells the operator after the file is chosen, before the final readiness", async () => {
    const order: string[] = [];
    const d = deps({
      needsInlineForSeries: vi.fn(async () => true),
      pickFile: vi.fn(async () => { order.push("pick"); return "/data/run.h5"; }),
      inform: vi.fn(async (m) => { order.push(`inform:${m.kind}`); }),
      fetchReadiness: vi.fn(async (p: string) => { order.push(`readiness:${p}`); return p === "" ? preflightOk : ready; }),
    });
    await startExperiment(d);
    expect(order).toEqual(["readiness:", "pick", "inform:info", "readiness:/data/run.h5"]);
    expect(d.inform.mock.calls[0][0].text).toContain("Your previous realtime mode will be restored when the experiment stops.");
  });
  it("says nothing without a series, and a failing check does not block the start", async () => {
    const quiet = deps({ needsInlineForSeries: vi.fn(async () => false) });
    await startExperiment(quiet);
    expect(quiet.inform).not.toHaveBeenCalled();
    const broken = deps({ needsInlineForSeries: vi.fn(async () => { throw new Error("config"); }) });
    expect((await startExperiment(broken)).kind).toBe("started");
  });
});

describe("a refused Start, by its typed outcome (MON-010, SHELL-058)", () => {
  const refuse = (outcome: number | undefined, message: string) => deps({ start: vi.fn(async () => ({ ok: false, message, outcome })) });

  it("StaleReadiness warns and asks for another start", async () => {
    const d = refuse(O.StaleReadiness, "readiness generation 4 is stale");
    expect(await startExperiment(d)).toMatchObject({ kind: "refused", outcome: O.StaleReadiness, summary: "Experiment start refused: readiness changed" });
    expect(d.inform).toHaveBeenCalledWith(expect.objectContaining({ kind: "warning", text: expect.stringContaining("Please start again.") }));
  });
  it("StorageFailed and ProvenanceFailed are critical and name the file", async () => {
    for (const outcome of [O.StorageFailed, O.ProvenanceFailed]) {
      const d = refuse(outcome, "cannot create the file");
      await startExperiment(d);
      expect(d.inform).toHaveBeenCalledWith({ kind: "error", title: "Error", text: "Failed to prepare the experiment file:\n/data/run.h5\n\ncannot create the file" });
    }
  });
  it("Busy, AlreadyActive and InvalidRequest inform with the backend's message", async () => {
    for (const outcome of [O.Busy, O.AlreadyActive, O.InvalidRequest]) {
      const d = refuse(outcome, "an experiment is already active");
      expect(await startExperiment(d)).toMatchObject({ kind: "refused", summary: "an experiment is already active" });
      expect(d.inform).toHaveBeenCalledWith({ kind: "info", title: "Experiment", text: "an experiment is already active" });
    }
  });
  it("NotReady re-reads the gates and explains them", async () => {
    const after: StartReadiness = { valid: true, ready: false, gates: [gate("lifecycle.fault", 2, "the last run did not save", "acknowledge the fault")] };
    let calls = 0;
    const d = deps({
      start: vi.fn(async () => ({ ok: false, message: "not ready: lifecycle.fault", outcome: O.NotReady })),
      fetchReadiness: vi.fn(async (p: string) => (p === "" ? preflightOk : ++calls === 1 ? ready : after)),
    });
    const out = await startExperiment(d);
    expect(out).toMatchObject({ kind: "not-ready", message: "/data/run.h5: lifecycle.fault: the last run did not save — acknowledge the fault" });
    expect(d.inform).toHaveBeenCalledWith(expect.objectContaining({ kind: "warning", text: expect.stringContaining("Blocked — lifecycle.fault") }));
  });
  it("without a typed outcome (an older backend) only the message is known and no dialog opens", async () => {
    const d = refuse(undefined, "backend not initialized");
    expect(await startExperiment(d)).toMatchObject({ kind: "refused", message: "backend not initialized", summary: "backend not initialized" });
    expect(d.inform).not.toHaveBeenCalled();
  });
  it("works without a dialog transport: the outcome still carries the wording", async () => {
    const d = refuse(O.Busy, "busy");
    expect(await startExperiment({ ...d, inform: undefined })).toMatchObject({ kind: "refused", summary: "busy" });
  });
  it("a dialog that fails to open does not lose the outcome", async () => {
    const d = deps({ start: vi.fn(async () => ({ ok: false, message: "busy", outcome: O.Busy })), inform: vi.fn(async () => { throw new Error("no window"); }) });
    expect(await startExperiment(d)).toMatchObject({ kind: "refused", summary: "busy" });
  });
});

// #858 (SHELL-054): a capture that runs Latest Frame. The backend warns on camera.deliveryMode and refuses a start that does
// not carry the operator's acknowledgement; Qt asks Switch to Every Frame / Continue with Latest Frame / Cancel first.
describe("starting while the capture runs Latest Frame (#858)", () => {
  const latest = gate("camera.deliveryMode", 1, "Latest Frame intentionally discards frames; the recording may be incomplete",
    "switch to Every Frame or acknowledge the policy at Start");
  const noRing = { fetchRingStatus: vi.fn(async () => ring({ run_frozen: false })) };
  const readiness = (extra: StartGate[]) => vi.fn(async (path: string): Promise<StartReadiness> =>
    (path === "" ? { valid: true, ready: false, gates: [gate("storage.output", 3, "no output path chosen yet"), ...extra] } : { valid: true, ready: true, gates: extra }));

  it("asks before the file dialog and sends the acknowledgement on Continue", async () => {
    const order: string[] = [];
    const chooseLatestFrame = vi.fn(async () => { order.push("ask"); return "continue" as const; });
    const d = deps({ ...noRing, fetchReadiness: readiness([latest]), chooseLatestFrame, pickFile: vi.fn(async () => { order.push("file"); return "/data/run.h5"; }) });
    const out = await startExperiment(d);
    expect(chooseLatestFrame).toHaveBeenCalledWith(false);
    expect(order).toEqual(["ask", "file"]);
    expect(d.start).toHaveBeenCalledWith("/data/run.h5", false, true);
    expect(out.kind).toBe("started");
  });
  it("Cancel starts nothing and opens no file dialog", async () => {
    const d = deps({ ...noRing, fetchReadiness: readiness([latest]), chooseLatestFrame: vi.fn(async () => "cancel" as const) });
    expect(await startExperiment(d)).toEqual({ kind: "cancelled" });
    expect(d.pickFile).not.toHaveBeenCalled();
    expect(d.start).not.toHaveBeenCalled();
  });
  it("Switch sets Every Frame for the next camera start, still acknowledges this run, and says so", async () => {
    const switchToEveryFrame = vi.fn(async () => ({ ok: true, message: "Delivery mode set to Every Frame." }));
    const chooseLatestFrame = vi.fn(async () => "switch" as const);
    const d = deps({ ...noRing, fetchReadiness: readiness([latest]), chooseLatestFrame, switchToEveryFrame });
    const out = await startExperiment(d);
    expect(chooseLatestFrame).toHaveBeenCalledWith(true);
    expect(switchToEveryFrame).toHaveBeenCalledTimes(1);
    expect(d.start).toHaveBeenCalledWith("/data/run.h5", false, true);
    expect(out.kind === "started" && out.notice.split("\n")[0]).toBe(
      "Delivery mode set to Every Frame; it applies at the next camera start. This recording runs with Latest Frame.");
  });
  it("a refused switch is explained and starts nothing", async () => {
    const switchToEveryFrame = vi.fn(async () => ({ ok: false, message: "refused during an experiment" }));
    const d = deps({ ...noRing, fetchReadiness: readiness([latest]), chooseLatestFrame: vi.fn(async () => "switch" as const), switchToEveryFrame });
    expect(await startExperiment(d)).toEqual({ kind: "cancelled" });
    expect(d.inform).toHaveBeenCalledWith({ kind: "warning", title: "Start Experiment",
      text: "The delivery mode could not be set to Every Frame: refused during an experiment\n\nThe experiment was not started." });
    expect(d.pickFile).not.toHaveBeenCalled();
    expect(d.start).not.toHaveBeenCalled();
  });
  it("asks nothing for Every Frame (the gate passes) or an unconfirmed mode (Unavailable blocks on its own)", async () => {
    for (const status of [0, 3]) {
      const chooseLatestFrame = vi.fn(async () => "continue" as const);
      const d = deps({ ...noRing, fetchReadiness: readiness([{ ...latest, status }]), chooseLatestFrame });
      await startExperiment(d);
      expect(chooseLatestFrame).not.toHaveBeenCalled();
      if (status === 0) expect(d.start).toHaveBeenCalledWith("/data/run.h5", false, false);
    }
  });
  it("without the question (an older shell) the start goes unacknowledged, and the backend's refusal comes back", async () => {
    const d = deps({ ...noRing, fetchReadiness: readiness([latest]), start: vi.fn(async () => ({
      ok: false, outcome: O.NotReady, message: "Latest Frame delivery discards frames; acknowledge the policy or switch to Every Frame" })) });
    const out = await startExperiment(d);
    expect(d.start).toHaveBeenCalledWith("/data/run.h5", false, false);
    expect(out).toMatchObject({ kind: "refused", summary: "Experiment not ready: Latest Frame delivery discards frames; acknowledge the policy or switch to Every Frame" });
  });
});
