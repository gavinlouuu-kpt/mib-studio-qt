import { describe, expect, it } from "vitest";
import type { StageStatus } from "../bridge";
import { READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { ExperimentReadiness, ReadinessGate } from "../eventAdapter";
import { derivePreflight, type CheckStatus, type PreflightCheck, type PreflightInput } from "../preflight";
import {
  CONFIRMATION_TEXT,
  PREPARE_OUTPUT_PATH,
  checkFactTone,
  checkText,
  checkTone,
  confirmationState,
  gateBlocksStart,
  gateText,
  gateTone,
  summarizeReadiness,
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

describe("readiness summary without a path", () => {
  it("is unknown before the first read, after a failed read, and when the backend is not initialized", () => {
    expect(summarizeReadiness(null)).toMatchObject({ tone: "unknown", title: "Unknown · readiness not read yet" });
    expect(summarizeReadiness(null, "bridge gone")).toMatchObject({ tone: "unknown", detail: "bridge gone" });
    expect(summarizeReadiness(snapshot([], { valid: false }))).toMatchObject({ tone: "unknown" });
  });

  it("does not count the destination Start chooses as a blocker, and never calls that ready", () => {
    const s = summarizeReadiness(snapshot([gate("camera.source", G.Pass), NO_PATH]));
    expect(s).toMatchObject({ tone: "info", title: "No blockers · destination checked at Start", blockers: [] });
    expect(s.tone).not.toBe("confirmed");
  });

  it("names every other blocking gate, Unavailable included", () => {
    const s = summarizeReadiness(snapshot([gate("camera.source", G.Unavailable, "no camera source configured"), gate("processing.core", G.Fail, "core pin"), NO_PATH]));
    expect(s.tone).toBe("blocked");
    expect(s.title).toBe("Start blocked · 2 gates");
    expect(s.detail).toContain("camera.source, processing.core.");
    expect(s.detail).toContain("checked at Start");
  });

  it("a destination gate that failed for another reason still blocks", () => {
    expect(summarizeReadiness(snapshot([gate("storage.output", G.Fail, "not writable")])).tone).toBe("blocked");
  });

  it("reports warnings without blocking", () => {
    const s = summarizeReadiness(snapshot([gate("telemetry.transportLoss", G.Warn, "cannot report"), NO_PATH]));
    expect(s).toMatchObject({ tone: "warn", title: "No blockers · 1 warning" });
  });

  it("is confirmed only when the backend itself reports ready", () => {
    expect(summarizeReadiness(snapshot([gate("camera.source", G.Pass)], { ready: true })).tone).toBe("confirmed");
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
  const input: PreflightInput = {
    backendReady: true, cameraConfigured: true, cameraRunning: false, cameraIdentity: "Cam", cameraExpected: "",
    coreValid: true, corePinSatisfied: true, coreVersion: "2.1.0", requiredCoreVersion: "2.1.0",
    autofocus: { valid: true, connected: false, identity: "" }, samplePump: { valid: true, connected: false, identity: "" },
    sheathPump: { valid: true, connected: false, identity: "" }, trigger: { valid: true, cameraAttached: false },
    storageKnown: false, storageWritable: false, storageFreeOk: false, storagePath: "",
  };
  it("counts ready, unknown and blocking required checks", () => {
    expect(summarizeRequired(derivePreflight(input))).toMatchObject({ title: "Required checks: 2 of 2 ready", allReady: true });
    const unread = summarizeRequired(derivePreflight({ ...input, coreKnown: false }));
    expect(unread).toMatchObject({ title: "Required checks: 1 of 2 ready · 1 unknown", allReady: false });
    expect(unread.attention.map((c) => c.id)).toEqual(["core"]);
    const failed = summarizeRequired(derivePreflight({ ...input, cameraConfigured: false }));
    expect(failed.title).toBe("Required checks: 1 of 2 ready · 1 blocking");
  });

  it("a required warning is ready unless its backend gate fails", () => {
    const warned = { ...input, cameraExpected: "Other" };
    expect(summarizeRequired(derivePreflight(warned)).title).toBe("Required checks: 2 of 2 ready");
    expect(summarizeRequired(derivePreflight(warned, undefined, [gate("camera.source", G.Fail)])).title).toBe("Required checks: 1 of 2 ready · 1 blocking");
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
