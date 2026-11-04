import { describe, expect, it } from "vitest";
import { EXPERIMENT_START_OUTCOMES, READINESS_GATE_STATUSES as S } from "../../bridgeContract";
import {
  INLINE_SERIES_NOTICE, blockersLine, blockingGates, blocksStart, explainReadiness, gateLine, gateStatusLabel, seriesForcesInline,
  sameReadiness, startOutcomeWording, summarizeReadiness, warningGates, warningNotice, type StartGate,
} from "./startReadiness";

const gate = (id: string, status: number, reason = "", remediation = ""): StartGate => ({ id, status, reason, remediation });
const RATE = gate("camera.rate", S.Fail, "the camera is driven at 600 fps but allows at most 500 fps", "use a smaller camera window or a lower frame rate");
const BUFFER = gate("storage.buffer", S.Warn, "the buffer holds 0.80 s", "use a smaller window, or free memory");
const OUTPUT = gate("storage.output", S.Unavailable, "no output path chosen yet", "choose an HDF5 destination");

describe("gate wording (Qt explainReadiness)", () => {
  it("uses Qt's status words", () => {
    expect([S.Pass, S.Warn, S.Fail, S.Unavailable, S.NotRequired, 99].map(gateStatusLabel)).toEqual(["OK", "Warning", "Blocked", "Unknown", "Not required", "?"]);
  });
  it("blocks on Fail, Unavailable and any status it does not know, never on Warn or Pass", () => {
    expect([S.Pass, S.Warn, S.NotRequired].map((s) => blocksStart(gate("x", s)))).toEqual([false, false, false]);
    expect([S.Fail, S.Unavailable, 42].map((s) => blocksStart(gate("x", s)))).toEqual([true, true, true]);
  });
  it("writes a gate as 'Blocked — id: reason' with the remediation on the next line", () => {
    expect(gateLine(RATE)).toBe("Blocked — camera.rate: the camera is driven at 600 fps but allows at most 500 fps\n    → use a smaller camera window or a lower frame rate");
    expect(gateLine(gate("a.b", S.Unavailable, "unknown"))).toBe("Unknown — a.b: unknown");
  });
});

describe("the refusal dialog (MON-004)", () => {
  it("lists every blocking gate and then the warnings", () => {
    const e = explainReadiness([RATE, gate("lifecycle.fault", S.Fail, "the last run did not save", "acknowledge the fault")], [BUFFER])!;
    expect(e.title).toBe("Start Experiment");
    expect(e.text.startsWith("The experiment cannot start until every readiness check passes.\n\nBlocked — camera.rate:")).toBe(true);
    expect(e.text).toContain("\n\nBlocked — lifecycle.fault: the last run did not save\n    → acknowledge the fault");
    expect(e.text).toContain("Also worth knowing (these do not block Start):\n\nWarning — storage.buffer: the buffer holds 0.80 s\n    → use a smaller window, or free memory");
  });
  it("has nothing to say when nothing blocks", () => {
    expect(explainReadiness([], [BUFFER])).toBeNull();
  });
  it("keeps the one-line inline form, with or without a path", () => {
    expect(blockersLine("/data/run.h5", [RATE])).toBe("/data/run.h5: camera.rate: the camera is driven at 600 fps but allows at most 500 fps — use a smaller camera window or a lower frame rate");
    expect(blockersLine(null, [gate("a", S.Fail, "r1"), gate("b", S.Fail, "r2")])).toBe("a: r1; b: r2");
    expect(blockersLine("/data/run.h5", [])).toBe("/data/run.h5: Backend readiness unavailable; experiment was not started.");
  });
});

describe("blocking and warning gates (MON-028)", () => {
  const gates = [RATE, BUFFER, OUTPUT, gate("run.frozen", S.Warn, "Run is stopped"), gate("storage.throughput", S.Warn, "rate unknown", "choose the destination at Start"), gate("camera.window", S.Pass)];
  it("leaves out the destination for the preflight", () => {
    expect(blockingGates(gates, ["storage.output"]).map((g) => g.id)).toEqual(["camera.rate"]);
    expect(blockingGates(gates).map((g) => g.id)).toEqual(["camera.rate", "storage.output"]);
  });
  it("keeps the ring-discard gate out of the warnings (it has its own confirmation) and the throughput warning only while a destination is open", () => {
    expect(warningGates(gates).map((g) => g.id)).toEqual(["storage.buffer", "storage.throughput"]);
    expect(warningGates(gates, false).map((g) => g.id)).toEqual(["storage.buffer"]);
  });
  it("summarizes the standing readiness: blocked, warnings, ready", () => {
    expect(summarizeReadiness(gates)).toMatchObject({ tone: "blocked", line: "Blocked (1), 1 warning" });
    expect(summarizeReadiness([BUFFER, OUTPUT])).toMatchObject({ tone: "warn", line: "Ready, 1 warning" });
    expect(summarizeReadiness([OUTPUT, gate("camera.window", S.Pass)])).toMatchObject({ tone: "ok", line: "Ready to start" });
    expect(summarizeReadiness([BUFFER, gate("storage.buffer", S.Warn, "second"), OUTPUT]).line).toBe("Ready, 2 warnings");
  });
  it("turns the warnings of a started run into notice lines", () => {
    expect(warningNotice([gate("storage.persistent", S.Warn, "recording to RAM")])).toBe("recording to RAM");
    expect(warningNotice([BUFFER, gate("storage.persistent", S.Warn, "recording to RAM", "record to the SATA disk")])).toBe("the buffer holds 0.80 s (use a smaller window, or free memory)\nrecording to RAM (record to the SATA disk)");
    expect(warningNotice([RATE, gate("run.frozen", S.Warn, "Run is stopped")])).toBe("");
  });
});

describe("Start outcomes (MON-010)", () => {
  const O = EXPERIMENT_START_OUTCOMES;
  it("warns that a stale readiness needs another start", () => {
    const w = startOutcomeWording(O.StaleReadiness, "readiness generation changed", "/d/r.h5");
    expect(w.summary).toBe("Experiment start refused: readiness changed");
    expect(w.dialog).toEqual({ kind: "warning", title: "Start Experiment", text: "The configuration changed while the experiment was being prepared, so the readiness check is no longer valid.\n\nreadiness generation changed\n\nPlease start again." });
  });
  it("shows a critical dialog for a file that could not be prepared", () => {
    for (const outcome of [O.StorageFailed, O.ProvenanceFailed]) {
      const w = startOutcomeWording(outcome, "disk full", "/d/r.h5");
      expect(w.dialog).toEqual({ kind: "error", title: "Error", text: "Failed to prepare the experiment file:\n/d/r.h5\n\ndisk full" });
      expect(w.summary).toBe("Experiment start failed: disk full");
    }
  });
  it("informs for busy, already active and an invalid request", () => {
    for (const outcome of [O.AlreadyActive, O.Busy, O.InvalidRequest]) {
      expect(startOutcomeWording(outcome, "invalid start request: operator: unknown key", "/d/r.h5")).toMatchObject({
        dialog: { kind: "info", title: "Experiment", text: "invalid start request: operator: unknown key" }, summary: "invalid start request: operator: unknown key",
      });
    }
  });
  it("re-explains the gates for NotReady, and has only the message without a typed outcome", () => {
    expect(startOutcomeWording(O.NotReady, "not ready: camera.rate", "/d/r.h5")).toMatchObject({ explainGates: true, dialog: null });
    expect(startOutcomeWording(undefined, "boom", "/d/r.h5")).toEqual({ explainGates: false, dialog: null, summary: "boom" });
  });
});

describe("multi-image series (MON-008)", () => {
  it("is asked for by the processing configuration's multi_image block", () => {
    expect(seriesForcesInline(JSON.stringify({ multi_image: { enabled: true, count: 3 } }))).toBe(true);
    expect(seriesForcesInline(JSON.stringify({ multi_image: { enabled: true, count: 1 } }))).toBe(false);
    expect(seriesForcesInline(JSON.stringify({ multi_image: { enabled: false, count: 3 } }))).toBe(false);
    expect(seriesForcesInline(JSON.stringify({}))).toBe(false);
    expect(seriesForcesInline("not json")).toBe(false);
  });
  it("uses Qt's words", () => {
    expect(INLINE_SERIES_NOTICE).toBe("Multi-image series capture requires inline realtime processing.\nThis experiment will run in inline mode so series images remain reviewable.\nYour previous realtime mode will be restored when the experiment stops.");
  });
});

describe("sameReadiness", () => {
  const r = (gates: StartGate[], ready = false) => ({ valid: true, ready, gates });
  it("sees a repeated read as the same and any changed gate as different", () => {
    expect(sameReadiness(null, r([]))).toBe(false);
    expect(sameReadiness(r([RATE, BUFFER]), r([{ ...RATE }, { ...BUFFER }]))).toBe(true);
    expect(sameReadiness(r([RATE]), r([{ ...RATE, status: S.Warn }]))).toBe(false);
    expect(sameReadiness(r([RATE]), r([{ ...RATE, reason: "other" }]))).toBe(false);
    expect(sameReadiness(r([RATE]), r([RATE, BUFFER]))).toBe(false);
    expect(sameReadiness(r([RATE]), r([RATE], true))).toBe(false);
  });
});
