// Prepare's Calibration card model (#716 A4b): the words and the availability rules, pure.
import { describe, expect, it } from "vitest";
import { CALIBRATION_APPLY_OUTCOMES as O, READINESS_GATE_STATUSES as G } from "../bridgeContract";
import { applyReply, calList, calRecord, SETUP } from "./calibrationFixtures";
import {
  CALIBRATION_GATE,
  ageText,
  applyAvailability,
  applyErrorNotice,
  applyReplyNotice,
  calibrationGate,
  evidenceFails,
  factorChange,
  factorText,
  failedGates,
  fixAction,
  identityDiff,
  notInEffectText,
  statusText,
  statusTone,
} from "./calibrationModel";
import type { CalibrationApplyReply } from "../calibrationRecords";

const gate = (status: number, remediation = "") => ({ id: CALIBRATION_GATE, status, reason: "r", remediation });
const reply = (id: string, outcome: number, over: Record<string, unknown> = {}) => applyReply(id, outcome, over) as unknown as CalibrationApplyReply;

describe("numbers are the backend's, never invented", () => {
  it("a missing factor is Not recorded, never 0", () => {
    expect(factorText(null)).toBe("Not recorded");
    expect(factorText(undefined)).toBe("Not recorded");
    expect(factorText(Number.NaN)).toBe("Not recorded");
    expect(factorText(0.523456789)).toBe("0.523457 µm/px");
    expect(factorChange(0.5, 0.52)).toBe("0.5 → 0.52 µm/px");
    expect(factorChange(null, 0.52)).toBe("Not recorded → 0.52 µm/px");
  });

  it("an age is unknown when the time does not parse or is in this clock's future", () => {
    const now = Date.parse("2026-10-21T10:00:00Z");
    expect(ageText("2026-10-21T09:30:00Z", now)).toBe("30 min");
    expect(ageText("2026-10-21T06:36:00Z", now)).toBe("3.4 h");
    expect(ageText("2026-10-18T10:00:00Z", now)).toBe("3.0 d");
    expect(ageText("2026-10-21T11:00:00Z", now)).toBe("unknown");
    expect(ageText("", now)).toBe("unknown");
    expect(ageText("not a time", now)).toBe("unknown");
  });

  it("names what differs between a record's setup and the current one", () => {
    expect(identityDiff(SETUP, { ...SETUP, roi: "0,8,512,96" })).toBe("ROI full → 0,8,512,96");
    expect(identityDiff(SETUP, { ...SETUP })).toBe("");
  });

  it("an insufficient record lists its failed gates in plain words", () => {
    expect(failedGates(calRecord("cal-x", "insufficient"))).toEqual(["Valid objects: 120, at least 200", "Diameter CV: 14.2 %, at most 10 %"]);
    expect(failedGates(calRecord("cal-ok", "proposed"))).toEqual([]);
  });
});

describe("status words follow the backend's in-effect rule", () => {
  it("an applied record says whether it is in effect, from in_effect_record_id alone", () => {
    const a = calRecord("cal-a", "applied");
    expect(statusText(a, calList([a], { in_effect_record_id: "cal-a" }))).toBe("Applied · in effect");
    expect(statusTone(a, calList([a], { in_effect_record_id: "cal-a" }))).toBe("confirmed");
    expect(statusText(a, calList([a]))).toBe("Applied · not in effect");
    expect(statusTone(a, calList([a]))).toBe("warn");
    expect(statusText(calRecord("x", "insufficient"), calList([]))).toBe("Insufficient");
  });

  it("an applied record out of effect after a factor change says to re-apply it", () => {
    const a = calRecord("cal-a", "applied");
    expect(notInEffectText(a, calList([a], { current_factor: 0.5 }))).toContain("Its factor 0.52 µm/px is not the current factor 0.5 µm/px");
    expect(notInEffectText(a, calList([a], { current_factor: 0.5 }))).toContain("Re-apply it");
  });
});

describe("Apply availability", () => {
  it("a proposed record of this setup can be applied", () => {
    const p = calRecord("cal-p", "proposed");
    expect(applyAvailability(p, calList([p]))).toEqual({ action: "Apply", reason: "" });
  });

  it("an insufficient record is never applicable", () => {
    const i = calRecord("cal-i", "insufficient");
    expect(applyAvailability(i, calList([i])).action).toBeNull();
    // Even if a list said it was applicable.
    expect(applyAvailability({ ...i, applicable: true }, calList([i])).action).toBeNull();
  });

  it("a proposal of another setup, or with the identity unknown or no factor, is shown but not pressable", () => {
    const p = calRecord("cal-p", "proposed", { identity: { ...SETUP, roi: "0,0,512,96" } });
    expect(applyAvailability(p, calList([p])).reason).toContain("derived under another setup (ROI 0,0,512,96 → full)");
    const q = calRecord("cal-q", "proposed");
    expect(applyAvailability(q, calList([q], { current_identity: { ...SETUP, camera: null, known: false } })).reason).toContain("identity is unknown");
    expect(applyAvailability(q, calList([q], { current_factor: null })).reason).toContain("not reported");
  });

  it("the applied record out of effect gets Re-apply; in effect, or superseded, nothing", () => {
    const a = calRecord("cal-a", "applied");
    expect(applyAvailability(a, calList([a]))).toEqual({ action: "Re-apply", reason: "" });
    expect(applyAvailability(a, calList([a], { in_effect_record_id: "cal-a" })).action).toBeNull();
    expect(applyAvailability(calRecord("cal-s", "superseded"), calList([])).action).toBeNull();
    expect(applyAvailability(calRecord("cal-v", "invalidated"), calList([])).action).toBeNull();
  });
});

describe("the gate's Fix as a button", () => {
  const applied = calRecord("cal-a", "applied");
  const proposed = calRecord("cal-p", "proposed", { factor: 0.53 });

  it("after a restart a required method's Fix re-applies the applied record (PR #771 ruling)", () => {
    const list = calList([proposed, applied]);
    expect(fixAction(gate(G.Fail, "re-apply record cal-a (calibration_apply), or run the calibration method"), list)).toEqual({ record: applied, action: "Re-apply" });
  });

  it("with nothing applied, a proposal of this setup", () => {
    expect(fixAction(gate(G.Warn), calList([proposed]))).toEqual({ record: proposed, action: "Apply" });
  });

  it("nothing while the gate passes or is not required, or with nothing to apply", () => {
    expect(fixAction(gate(G.Pass), calList([proposed]))).toBeNull();
    expect(fixAction(gate(G.NotRequired), calList([proposed]))).toBeNull();
    expect(fixAction(gate(G.Fail), calList([calRecord("cal-i", "insufficient")]))).toBeNull();
    expect(fixAction(null, calList([proposed]))).toBeNull();
    expect(fixAction(gate(G.Fail), null)).toBeNull();
  });

  it("finds the gate in a readiness report, null when not reported", () => {
    expect(calibrationGate({ valid: true, ready: false, generation: "1", gates: [gate(G.Warn)], method_calibration: null })!.status).toBe(G.Warn);
    expect(calibrationGate({ valid: true, ready: false, generation: "1", gates: [], method_calibration: null })).toBeNull();
    expect(calibrationGate(null)).toBeNull();
  });
});

describe("apply outcomes in plain words", () => {
  it("applied: previous → new from the reply", () => {
    const n = applyReplyNotice(reply("cal-p", O.Applied, { previous_factor: 0.5, new_factor: 0.53, superseded_record_id: "cal-a" }), 0.5);
    expect(n.tone).toBe("confirmed");
    expect(n.title).toBe("Applied · cal-p");
    expect(n.text).toBe("Pixel-to-micron 0.5 → 0.53 µm/px. Record cal-a is superseded. No method was changed.");
  });

  it.each([
    [O.StaleFactor, { previous_factor: 0.51 }, "Not applied · the factor changed since this page read it", "This page read 0.5 µm/px; the backend's factor is now 0.51 µm/px"],
    [O.RunActive, {}, "Not applied · a run is active", "only while the experiment is idle"],
    [O.Insufficient, {}, "Not applied · the record is insufficient", "can never be applied"],
    [O.IdentityMismatch, { message: "record cal-p was derived under another setup (ROI full -> 0,8,512,96)" }, "Not applied · the record is from another setup", "derived under another setup"],
    [O.IdentityMismatch, { message: "the camera or ROI identity is unknown now; connect the camera and let the ROI apply first" }, "Not applied · the setup's identity is unknown", "Connect the camera"],
    [O.NotApplicable, { message: "record cal-p is superseded; only a proposed record is applied" }, "Not applied · the record cannot be applied now", "superseded"],
    [O.StoreFailed, {}, "Not applied · the calibration store could not be written", "Nothing was applied"],
  ])("refusal %i says so and that nothing was applied", (outcome, over, title, text) => {
    const n = applyReplyNotice(reply("cal-p", outcome, over), 0.5);
    expect(n.title).toBe(title);
    expect(n.text).toContain(text);
    expect(n.text).toContain("Nothing was applied");
  });

  it("a viewer's refusal from the server and a lost reply", () => {
    expect(applyErrorNotice(new Error("VIEWER_ONLY: another client controls")).title).toBe("Not applied · another tab or client holds control");
    const lost = applyErrorNotice(new Error("TRANSPORT_LOST"));
    expect(lost.title).toBe("Apply outcome not known");
    expect(lost.text).toContain("reads the records again");
  });
});

describe("stored evidence is re-checked, as the backend does (Codex round 1)", () => {
  it("an applied record whose stored evidence fails is never re-applicable, and says why", () => {
    const bad = calRecord("cal-a", "applied", { factor: 0.5, gates: [{ id: "max_cv_percent", pass: false, value: 14, limit: 10, reason: null }] });
    const list = calList([bad], { current_factor: 0.5 });
    expect(evidenceFails(bad)).toBe(true);
    expect(applyAvailability(bad, list).action).toBeNull();
    expect(notInEffectText(bad, list)).toContain("stored evidence fails the minimum-evidence gates");
    expect(fixAction(gate(G.Fail), list)).toBeNull();
  });

  it("too few objects, a CV over the limit or no proposal fail the re-check even with every gate passed", () => {
    expect(evidenceFails(calRecord("x", "proposed", { results: { ...calRecord("x", "proposed").results, valid_objects: 150 } }))).toBe(true);
    expect(evidenceFails(calRecord("x", "proposed", { results: { ...calRecord("x", "proposed").results, cv_diameter_percent: 11 } }))).toBe(true);
    expect(evidenceFails(calRecord("x", "proposed", { results: { ...calRecord("x", "proposed").results, proposed_pixel_to_micron: null } }))).toBe(true);
    expect(evidenceFails(calRecord("x", "proposed"))).toBe(false);
  });

  it("matching setup and factor but not in effect: the card does not invent a cause", () => {
    const a = calRecord("cal-a", "applied", { factor: 0.5 });
    expect(notInEffectText(a, calList([a], { current_factor: 0.5 }))).toBe("The backend does not report it in effect, although its setup and factor match; the calibration gate says why.");
  });
});
