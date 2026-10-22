// The summary-first Prepare's pure derivations (O1b, Gavin's decisions 2026-10-22): what counts as
// an issue, the headline, the Next · Align line, the Start-refusal line and the All checks summary.
import { describe, expect, it } from "vitest";
import { READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { ExperimentReadiness, ReadinessGate } from "../eventAdapter";
import { derivePreflight, type PreflightInput } from "../preflight";
import { zStageFact } from "./prepareModel";
import {
  allChecksSummary,
  calibrationRowState,
  cameraName,
  countedIssues,
  detectedLine,
  gateRole,
  gateRow,
  issueChip,
  nextAlignText,
  prepareHeadline,
  startRefusalLine,
} from "./prepareSummaryModel";

const g = (id: string, status: number, reason = ""): ReadinessGate => ({ id, status, reason, remediation: "" });
const R = (gates: ReadinessGate[]): ExperimentReadiness => ({ valid: true, ready: false, generation: "1", gates, method_calibration: null });
const OFF = { valid: true, connected: false, identity: "" };
const INPUT: PreflightInput = {
  backendReady: true, cameraConfigured: true, cameraRunning: false, cameraIdentity: "Mock camera", cameraExpected: "",
  coreKnown: true, coreValid: true, corePinSatisfied: true, coreVersion: "0.3.2", requiredCoreVersion: "0.3.2",
  autofocus: OFF, samplePump: OFF, sheathPump: OFF, trigger: { valid: true, cameraAttached: false },
  storageKnown: false, storageWritable: false, storageFreeOk: false, storagePath: "",
};
const report = derivePreflight(INPUT);
const NO_STAGE = zStageFact({ valid: true, connected: false } as never);
const SIM = g("camera.source", G.Warn, "simulated camera source selected");
const FALLBACK = g("camera.source", G.Fail, "requested egrabber camera is unavailable; no eGrabber driver on this computer");
const NO_PATH = g("storage.output", G.Unavailable, "no output path chosen yet");
const MOCK_IDLE = [g("camera.session", G.Fail, "camera is idle"), SIM, g("camera.deliveryMode", G.Unavailable), g("camera.geometry", G.Unavailable),
  g("telemetry.transportLoss", G.Unavailable), g("processing.roi", G.Warn), g("processing.background", G.Warn), g("processing.core", G.Pass),
  g("trigger.output", G.NotRequired), NO_PATH];
const input = (gates: ReadinessGate[] | null, over: object = {}) => ({ report, readiness: gates ? R(gates) : null, error: "", paused: false, zStage: NO_STAGE, ...over });

describe("what counts as an issue", () => {
  it("Next · Align gates, the chosen mock camera and the destination are never counted", () => {
    for (const x of MOCK_IDLE) expect(["next", "simulated", "destination", "settled"]).toContain(gateRole(x));
    expect(gateRole(NO_PATH)).toBe("destination");
    expect(gateRole(g("storage.output", G.Fail, "x"))).toBe("destination");
    expect(countedIssues(input(MOCK_IDLE))).toEqual([]);
  });

  it("a fallback camera source, a failing, unavailable or warning gate count", () => {
    expect(gateRole(FALLBACK)).toBe("failed");
    expect(gateRole(g("camera.source", G.Warn, "something else"))).toBe("warning"); // only the backend's exact words are the chosen mock
    expect(gateRole(g("processing.core", G.Unavailable))).toBe("unknown");
    expect(gateRole(g("future.gate", G.Warn))).toBe("warning");
  });

  it("gates not read count as one unknown, but not while a run is active", () => {
    expect(countedIssues(input(null))).toEqual([{ kind: "unknown", name: "Backend gates", row: "all" }]);
    expect(countedIssues(input(null, { paused: true }))).toEqual([]);
  });

  it("rows: a gate of no device, or one this build does not know, goes to Method", () => {
    expect(gateRow("camera.session")).toBe("camera");
    expect(gateRow("instrument.strobe")).toBe("camera");
    expect(gateRow("calibration.record")).toBe("calibration");
    expect(gateRow("storage.output")).toBe("destination");
    expect(gateRow("lifecycle.recording")).toBe("destination");
    expect(gateRow("clock.wall")).toBe("method");
    expect(gateRow("future.gate")).toBe("method");
  });
});

describe("the headline", () => {
  it("Hardware ready · simulated camera, Hardware ready, Ready with warnings · N, Needs attention · N", () => {
    expect(prepareHeadline(input(MOCK_IDLE)).text).toBe("Hardware ready · simulated camera");
    expect(prepareHeadline(input([g("camera.source", G.Pass), NO_PATH])).text).toBe("Hardware ready");
    expect(prepareHeadline(input([...MOCK_IDLE, g("clock.wall", G.Warn)])).text).toBe("Ready with warnings · 1");
    const attention = prepareHeadline(input([...MOCK_IDLE.filter((x) => x.id !== "camera.source"), FALLBACK, g("calibration.record", G.Warn)]));
    expect(attention).toEqual({ text: "Needs attention · 2", tone: "blocked", counts: "1 failed, 1 warning" });
    expect(prepareHeadline(input(null)).text).toBe("Needs attention · 1"); // never ready before the gates are read
    expect(prepareHeadline(input(null, { paused: true })).text).toBe("Readiness paused · a run is active");
  });
});

describe("the Next · Align line", () => {
  it("says what Align still has to do and how many gates Start waits for", () => {
    expect(nextAlignText(R(MOCK_IDLE))).toBe("start the camera, set ROI and background · Start waits for these 6 gates");
    expect(nextAlignText(R([g("camera.session", G.Pass), g("camera.geometry", G.Unavailable), g("processing.roi", G.Warn)])))
      .toBe("wait for the first frame, set ROI · Start waits for these 2 gates");
    expect(nextAlignText(R([g("processing.background", G.Warn)]))).toBe("set background · it does not refuse Start");
    expect(nextAlignText(R([g("camera.session", G.Pass), NO_PATH]))).toBe("nothing left: the camera runs, ROI and background are set");
    expect(nextAlignText(null)).toBe("not read yet");
  });
});

describe("the Start-refusal line beside Confirm", () => {
  it("names counted gates, and Align for the deferred ones; nothing when Start would not refuse", () => {
    expect(startRefusalLine(R(MOCK_IDLE))).toBe("Start is refused until the camera runs in Align.");
    expect(startRefusalLine(R([...MOCK_IDLE, FALLBACK]))).toBe("Start is refused until Camera source passes and the camera runs in Align.");
    expect(startRefusalLine(R([g("processing.roi", G.Fail)]))).toBe("Start is refused until Align is done.");
    expect(startRefusalLine(R([g("processing.roi", G.Warn), NO_PATH]))).toBe(""); // a warning and the destination never refuse here
  });
});

describe("words of the rows and the detected line", () => {
  it("the camera's name says chosen or fallback", () => {
    expect(cameraName(report, R(MOCK_IDLE))).toBe("Mock camera (chosen)");
    expect(cameraName(report, R([FALLBACK]))).toBe("Mock camera (fallback: eGrabber camera unavailable)");
    expect(cameraName(report, null)).toBe("Mock camera");
  });

  it("the detected and selected line", () => {
    expect(detectedLine(report, R(MOCK_IDLE), NO_STAGE, "calibration cal-0042 in effect"))
      .toBe("Mock camera (chosen) · core 0.3.2 trusted · calibration cal-0042 in effect · optional: focus, pumps and trigger not connected");
  });

  it("a row chip names its first issue", () => {
    expect(issueChip([{ kind: "failed", name: "Camera source", row: "camera" }])).toEqual({ tone: "blocked", text: "Fail · Camera source" });
    expect(issueChip([{ kind: "warning", name: "A", row: "method" }, { kind: "warning", name: "B", row: "method" }])).toEqual({ tone: "warn", text: "Warn · A and 1 more" });
    expect(issueChip([])).toBeNull();
  });

  it("the All checks summary", () => {
    expect(allChecksSummary(R(MOCK_IDLE), "", false)).toBe("10 backend gates · 1 pass · 6 next in Align · 1 simulated camera · 1 not required · destination checked at Start");
    expect(allChecksSummary(null, "bridge gone", false)).toBe("Unknown · bridge gone");
    expect(allChecksSummary(null, "", true)).toBe("Not read while a run is active");
  });
});

describe("the Calibration row opens by itself", () => {
  const CAL = (status: number) => g("calibration.record", status, "x");
  const method = (purpose: "measurement" | "calibration") => ({ declared: true, purpose, calibration_policy: "none", calibration_max_age_hours: 24,
    calibration_reference: null, error: null }) as never;
  it("locked open for a counted gate issue; open but closable for a calibration method or a proposed record", () => {
    expect(calibrationRowState(CAL(G.Warn), null, [])).toEqual({ issue: true, auto: false });
    expect(calibrationRowState(CAL(G.Fail), null, [])).toEqual({ issue: true, auto: false });
    expect(calibrationRowState(CAL(G.Unavailable), null, [])).toEqual({ issue: true, auto: false });
    expect(calibrationRowState(CAL(G.Pass), null, [])).toEqual({ issue: false, auto: false });
    expect(calibrationRowState(CAL(G.NotRequired), method("calibration"), [])).toEqual({ issue: false, auto: true });
    expect(calibrationRowState(CAL(G.Pass), method("measurement"), [{ status: "proposed" }])).toEqual({ issue: false, auto: true });
    expect(calibrationRowState(null, null, null)).toEqual({ issue: false, auto: false });
  });
});
