import { describe, expect, it } from "vitest";
import type { StageStatus } from "../bridge";
import { READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { ExperimentReadiness, ReadinessGate } from "../eventAdapter";
import { derivePreflight, type CheckStatus, type PreflightCheck, type PreflightInput } from "../preflight";
import { DESKTOP_CAPABILITIES } from "../platformCapabilities";
import {
  CONFIRMATION_TEXT,
  GATE_LABELS,
  PREPARE_OUTPUT_PATH,
  READINESS_FOOTNOTE,
  checkFactTone,
  checkText,
  checkTone,
  confirmBlockers,
  confirmationState,
  gateBlocksStart,
  gateLabel,
  gateText,
  gateTone,
  prepareVerdict,
  startRefusalText,
  summarizeRequired,
  worstState,
  zStageFact,
} from "./prepareModel";

const gate = (id: string, status: number, reason = "", remediation = ""): ReadinessGate => ({ id, status, reason, remediation });
const snapshot = (gates: ReadinessGate[], over: Partial<ExperimentReadiness> = {}): ExperimentReadiness =>
  ({ valid: true, ready: false, generation: "7", gates, ...over });
// What the backend reports without a path (ExperimentCoordinator, outputPath.empty()).
const NO_PATH = gate("storage.output", G.Unavailable, "no output path chosen yet", "choose an HDF5 destination");

describe("backend gate statuses in the UI language (#713)", () => {
  it("maps Pass / Warn / Fail / Unavailable / NotRequired to kit tones; Unknown is never Pass", () => {
    expect([G.Pass, G.Warn, G.Fail, G.Unavailable, G.NotRequired].map(gateTone)).toEqual(["confirmed", "warn", "blocked", "unknown", "neutral"]);
    expect(gateTone(99)).toBe("unknown");
    expect(gateText(NO_PATH)).toBe("Unavailable · no output path chosen yet");
    expect(gateText(gate("camera.source", G.Pass))).toBe("Pass");
    expect(gateText(gate("x", 99, "odd"))).toBe("Unknown · odd");
  });

  it("Fail and Unavailable block Start; Warn and NotRequired do not; an unknown value blocks", () => {
    expect([G.Pass, G.Warn, G.Fail, G.Unavailable, G.NotRequired, 99].map(gateBlocksStart)).toEqual([false, false, true, true, false, true]);
  });

  it("Prepare never names a destination", () => {
    expect(PREPARE_OUTPUT_PATH).toBe("");
  });
});

const INPUT: PreflightInput = {
  backendReady: true, cameraConfigured: true, cameraRunning: false, cameraIdentity: "Cam", cameraExpected: "",
  coreValid: true, corePinSatisfied: true, coreVersion: "2.1.0", requiredCoreVersion: "2.1.0",
  autofocus: { valid: true, connected: false, identity: "" }, samplePump: { valid: true, connected: false, identity: "" },
  sheathPump: { valid: true, connected: false, identity: "" }, trigger: { valid: true, cameraAttached: false },
  storageKnown: false, storageWritable: false, storageFreeOk: false, storagePath: "",
};
const PASSING = derivePreflight(INPUT);

describe("one Prepare verdict from the checklist and the path-less gates", () => {
  it("with the gates not read it is never a pass: Unknown, and Not ready only when the checklist blocks", () => {
    expect(prepareVerdict(PASSING, null)).toMatchObject({ word: "Ready to confirm", tone: "unknown", title: "Ready to confirm · gates not read yet", startBlocked: [] });
    expect(prepareVerdict(PASSING, null, "bridge gone").detail).toContain("bridge gone");
    expect(prepareVerdict(PASSING, snapshot([], { valid: false })).title).toBe("Ready to confirm · gates not read yet");
    const notInitialized = derivePreflight({ ...INPUT, backendReady: false });
    expect(prepareVerdict(notInitialized, null)).toMatchObject({ word: "Not ready", tone: "unknown", title: "Not ready · 2 unknown" });
  });

  it("checklist passes but a gate refuses Start: Ready to confirm · Start blocked, in the warn tone, never Not ready", () => {
    const v = prepareVerdict(PASSING, snapshot([gate("camera.source", G.Pass), gate("processing.core", G.Fail, "processing core pin not satisfied", "activate the pinned core"), NO_PATH]));
    expect(v).toMatchObject({ word: "Ready to confirm", tone: "warn", title: "Ready to confirm · Start blocked: Processing core" });
    expect(v.startBlocked).toEqual(["Processing core — activate the pinned core"]);
    expect(startRefusalText(v.startBlocked)).toEqual({ title: "Start blocked · 1 gate", text: "Start will be refused until: Processing core — activate the pinned core." });
    expect(v.detail).toBe("Failed: Processing core. The destination is checked at Start, with the chosen file.");
  });

  it("Unavailable refuses Start like Fail; the reason stands in for a missing remediation; the chip names two", () => {
    const v = prepareVerdict(PASSING, snapshot([
      gate("processing.roi", G.Unavailable, "no live frame yet."), gate("processing.core", G.Fail, "pin", "activate the pinned core."), gate("clock.wall", G.Fail), NO_PATH]));
    expect(v.title).toBe("Ready to confirm · Start blocked: Region of interest, Processing core and 1 more");
    expect(v.startBlocked).toEqual(["Region of interest — no live frame yet", "Processing core — activate the pinned core", "Wall clock"]);
    expect(startRefusalText(v.startBlocked)!.title).toBe("Start blocked · 3 gates");
    expect(startRefusalText([])).toBeNull();
  });

  it("is paused, not ready to confirm, while a run is active; a blocking checklist still reads Not ready", () => {
    expect(prepareVerdict(PASSING, null, "", true)).toMatchObject({ word: "Readiness paused", tone: "info", title: "Readiness paused · a run is active" });
    expect(prepareVerdict(derivePreflight({ ...INPUT, coreKnown: false }), null, "", true).word).toBe("Not ready");
  });

  it("is Ready when the checklist passes and only the destination waits for Start; the destination is never counted", () => {
    const v = prepareVerdict(PASSING, snapshot([gate("camera.source", G.Pass), NO_PATH]));
    expect(v).toMatchObject({ word: "Ready", tone: "confirmed", title: "Ready · destination checked at Start" });
    expect(v.detail).toContain("checked at Start");
  });

  it("counts blocking checks and gates together, in plain names", () => {
    const report = derivePreflight({ ...INPUT, coreKnown: false });
    const v = prepareVerdict(report, snapshot([gate("camera.source", G.Unavailable, "no camera source configured"), gate("science.pl", G.Warn, "x"), gate("storage.hdf5", G.Fail, "open"), NO_PATH]));
    expect(v).toMatchObject({ word: "Not ready", tone: "blocked", title: "Not ready · 1 failed, 2 unknown, 1 warning" });
    expect(v.detail).toBe("Failed: Recording file. Unknown: Processing core / trust, Camera source. Warnings: PL science. The destination is checked at Start, with the chosen file.");
  });

  it("a required warning makes it Not ready even when no backend gate fails (#548)", () => {
    const warned = derivePreflight({ ...INPUT, cameraExpected: "Other camera" });
    const v = prepareVerdict(warned, snapshot([gate("camera.source", G.Pass), gate("camera.session", G.Warn, "w"), NO_PATH]));
    expect(v).toMatchObject({ word: "Not ready", tone: "warn", title: "Not ready · 2 warnings" });
  });

  it("is Ready with warnings when only optional checks or gates warn", () => {
    const optionalWarn = derivePreflight({ ...INPUT, storageKnown: true, storageWritable: true, storageFreeOk: false });
    expect(prepareVerdict(optionalWarn, snapshot([NO_PATH]))).toMatchObject({ word: "Ready with warnings", tone: "warn", title: "Ready with warnings · 1 warning" });
    expect(prepareVerdict(PASSING, snapshot([gate("processing.roi", G.Warn, "no ROI set"), NO_PATH])).detail).toContain("Warnings: Region of interest.");
  });

  it("a destination gate that failed for another reason refuses Start", () => {
    const v = prepareVerdict(PASSING, snapshot([gate("storage.output", G.Fail, "not writable")]));
    expect(v.title).toBe("Ready to confirm · Start blocked: Destination file");
    expect(v.startBlocked).toEqual(["Destination file — not writable"]);
  });

  it("keeps a fixed footnote, with no gate ids in it", () => {
    expect(READINESS_FOOTNOTE).toBe("Unknown is never Pass. Unsupported never blocks. Start re-evaluates every gate with the chosen file.");
  });
});

describe("plain gate labels", () => {
  // Every gate id the backend emits (ExperimentCoordinator.cpp, MethodProvenance.cpp).
  const BACKEND_GATES = [
    "calibration.pixelToMicron", "camera.deliveryMode", "camera.geometry", "camera.mode", "camera.session", "camera.source", "clock.wall",
    "instrument.mode", "instrument.strobe", "lifecycle.experiment", "lifecycle.fault", "lifecycle.recording", "method.revision",
    "processing.background", "processing.backgroundCalibration", "processing.config", "processing.core", "processing.profileCompile",
    "processing.roi", "rf.generator", "ring.placement", "run.frozen", "science.pl", "storage.buffer", "storage.hdf5", "storage.output",
    "storage.persistent", "storage.roundtrip", "storage.ssd", "telemetry.transportLoss", "trigger.output",
  ];
  it("names every backend gate, with distinct labels", () => {
    expect(Object.keys(GATE_LABELS).sort()).toEqual([...BACKEND_GATES].sort());
    expect(new Set(Object.values(GATE_LABELS)).size).toBe(BACKEND_GATES.length);
    expect(gateLabel("science.pl")).toBe("PL science");
    expect(gateLabel("storage.hdf5")).toBe("Recording file");
  });
  it("falls back to the raw id for a gate it does not know", () => {
    expect(gateLabel("future.gate")).toBe("future.gate");
  });
});

describe("why Confirm is unavailable", () => {
  it("names the blocking required checks in plain words", () => {
    expect(confirmBlockers(PASSING)).toBeNull();
    const pz = derivePreflight({ ...INPUT, capabilities: { ...DESKTOP_CAPABILITIES, instrument: "pz7035", autofocus: false, trigger: false, pl_identity: true, led_strobe: true }, instrument: null });
    expect(confirmBlockers(pz)).toEqual({ tone: "unknown", lines: ["3 required checks not read yet or not verified: PL core (build + weights), Sensor link, LED strobe"] });
    const mixed = derivePreflight({ ...INPUT, cameraConfigured: false, coreKnown: false });
    expect(confirmBlockers(mixed)).toEqual({ tone: "blocked", lines: ["1 required check failed: Camera", "1 required check not read yet or not verified: Processing core / trust"] });
    expect(confirmBlockers(derivePreflight({ ...INPUT, cameraExpected: "Other" }))).toEqual({ tone: "warn", lines: ["1 required check with a warning: Camera"] });
  });
});

describe("checklist rows in the UI language", () => {
  const states: CheckStatus[] = ["passed", "warning", "failed", "unknown", "unsupported", "not-required"];
  it("uses the kit tones through the backend statuses", () => {
    expect(states.map(checkTone)).toEqual(["confirmed", "warn", "blocked", "unknown", "unsupported", "neutral"]);
    expect(states.map(checkFactTone)).toEqual(["confirmed", "warn", "blocked", "unknown", undefined, undefined]);
  });

  const row = (over: Partial<PreflightCheck>): PreflightCheck =>
    ({ id: "x", label: "X", requirement: "optional", status: "passed", expected: "", detected: "—", detail: "Fine.", recovery: [], blocks: false, ...over });
  it("writes the state first, then what was detected and expected", () => {
    expect(checkText(row({ status: "unknown", detail: "PL core identity not read yet." }))).toBe("Unknown · PL core identity not read yet.");
    expect(checkText(row({ detected: "COM3", detail: "Connected and responding." }))).toBe("Pass · COM3 · Connected and responding.");
    expect(checkText(row({ status: "failed", detected: "build 1", expected: "build 2", detail: "Reload." }))).toBe("Fail · build 1 · expected build 2 · Reload.");
  });

  it("a card shows its worst state", () => {
    expect(worstState([])).toBeNull();
    expect(worstState([row({}), row({ status: "unknown" }), row({ status: "warning" })])).toEqual({ status: "unknown", text: "Unknown" });
    expect(worstState([row({ status: "failed" }), row({ status: "failed" })])).toEqual({ status: "failed", text: "Fail · 2 checks" });
    expect(worstState([row({ status: "unsupported" })])).toEqual({ status: "unsupported", text: "Unsupported" });
  });
});

describe("required checks summary", () => {
  const input = INPUT;
  it("counts ready, unknown and blocking required checks", () => {
    expect(summarizeRequired(derivePreflight(input))).toMatchObject({ title: "Required checks: 2 of 2 ready", allReady: true });
    const unread = summarizeRequired(derivePreflight({ ...input, coreKnown: false }));
    expect(unread).toMatchObject({ title: "Required checks: 1 of 2 ready · 1 unknown", allReady: false });
    expect(unread.attention.map((c) => c.id)).toEqual(["core"]);
    const failed = summarizeRequired(derivePreflight({ ...input, cameraConfigured: false }));
    expect(failed.title).toBe("Required checks: 1 of 2 ready · 1 blocking");
  });

  it("a required warning is not ready: it blocks (#548)", () => {
    expect(summarizeRequired(derivePreflight({ ...input, cameraExpected: "Other" })).title).toBe("Required checks: 1 of 2 ready · 1 blocking");
  });
});

describe("confirmation state", () => {
  it("follows the signature and the blocking checks", () => {
    expect(confirmationState("sig", "", true)).toBe("none");
    expect(confirmationState("sig", "sig", true)).toBe("confirmed");
    expect(confirmationState("sig", "old", true)).toBe("changed");
    expect(confirmationState("sig", "sig", false)).toBe("blocked");
    expect(confirmationState("", "", true)).toBe("none");
    expect(CONFIRMATION_TEXT.changed.tone).toBe("warn");
  });
});

describe("coarse Z stage fact", () => {
  const stage = (over: Partial<StageStatus>) => ({ valid: true, connected: true, configured: true, zero_set: true, emergency_stop: false, model: "ZC300", serial: "SN42", ...over }) as StageStatus;
  it("states only what the backend reports", () => {
    expect(zStageFact(null)).toEqual({ value: "Unknown · not read yet", tone: "unknown", status: "unknown" });
    expect(zStageFact(stage({ valid: false }))).toMatchObject({ tone: "unknown", status: "unknown" });
    expect(zStageFact(stage({ connected: false }))).toEqual({ value: "Not connected", status: "not-required" });
    expect(zStageFact(stage({}))).toEqual({ value: "ZC300 SN42 · zero set", tone: "confirmed", status: "passed" });
    expect(zStageFact(stage({ zero_set: false }))).toMatchObject({ tone: "warn", status: "warning" });
    expect(zStageFact(stage({ configured: false }))).toMatchObject({ tone: "blocked", status: "failed" });
    expect(zStageFact(stage({ emergency_stop: true }))).toMatchObject({ tone: "blocked", status: "failed" });
    // The Focus card's chip takes the stage into account.
    expect(worstState([{ status: "passed" }, zStageFact(stage({ zero_set: false }))])?.status).toBe("warning");
  });
});
