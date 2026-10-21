// Pure model of Prepare's Calibration card (#716 A4b; ADR 0015 item 7, exec plan P2): the
// `calibration.record` readiness gate, the record in effect, proposed and insufficient records, the
// plain list, and the words for an explicit apply and its refusals.
//
// Truth rules: every number is the backend's (calibration_records_list / calibration_record_get /
// calibration_apply); a null stays "Not recorded" or "unknown", never 0. "In effect" is the
// backend's own rule (`in_effect_record_id`: an applied record whose identity is the current one
// and whose factor is the current factor); the card never recomputes it. Whether Apply can be
// pressed is read from the list (status, applicable, identity, factor) so the button is not offered
// where the backend would surely refuse, but the backend decides: every refusal is shown as it came.
//
// Pure module: no React or transport imports (the kit types are types only).

import type { StateTone } from "@mib/ui";
import { CALIBRATION_APPLY_OUTCOMES as O, CALIBRATION_RECORDS, READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { CalibrationApplyReply, CalibrationIdentity, CalibrationRecord, CalibrationRecordsList } from "../calibrationRecords";
import type { ExperimentReadiness, ReadinessGate } from "../eventAdapter";

/** The readiness gate of the applied calibration record (CalibrationRecord.cpp). */
export const CALIBRATION_GATE = "calibration.record";
/** The plain list shows at most this many records, newest first (the store keeps every version). */
export const CALIBRATION_LIST_ROWS = 20;
/** Insufficient records shown with their failed gates (newest first); the list has the rest. */
export const INSUFFICIENT_SHOWN = 3;

/** The calibration gate in a readiness report read without a path; null when it was not reported. */
export function calibrationGate(r: ExperimentReadiness | null): ReadinessGate | null {
  if (!r || !r.valid) return null;
  return r.gates.find((g) => g.id === CALIBRATION_GATE) ?? null;
}

/** Whether the gate asks for something (Warn, Fail, Unavailable or a status it does not know). */
export const gateNeedsAction = (g: ReadinessGate | null): boolean => !!g && g.status !== G.Pass && g.status !== G.NotRequired;

const finite = (n: unknown): n is number => typeof n === "number" && Number.isFinite(n);

/** "0.52 µm/px" (six significant figures), "Not recorded" for a factor the backend did not give. */
export function factorText(n: number | null | undefined): string {
  return finite(n) ? `${Number(n.toPrecision(6))} µm/px` : "Not recorded";
}

/** "0.5 → 0.52 µm/px"; either side "Not recorded" when it is null. */
export function factorChange(previous: number | null | undefined, next: number | null | undefined): string {
  const p = finite(previous) ? String(Number(previous.toPrecision(6))) : "Not recorded";
  return `${p} → ${factorText(next)}`;
}

/** How long ago an ISO UTC time was: "12 min", "3.4 h", "2.0 d"; "unknown" for a time that does not
 *  parse, or one later than this computer's clock (the age cannot be known). */
export function ageText(utc: string | null | undefined, nowMs: number): string {
  const t = utc ? Date.parse(utc) : NaN;
  if (!Number.isFinite(t) || t > nowMs) return "unknown";
  const minutes = (nowMs - t) / 60000;
  if (minutes < 60) return `${Math.floor(minutes)} min`;
  const hours = minutes / 60;
  return hours < 48 ? `${hours.toFixed(1)} h` : `${(hours / 24).toFixed(1)} d`;
}

/** "camera mock:Camera 01 · ROI 0,0,512,96 · chip not tracked · objective not tracked". */
export function identityText(id: Partial<CalibrationIdentity> | null | undefined): string {
  if (!id) return "Not recorded";
  return `camera ${id.camera ?? "unknown"} · ROI ${id.roi ?? "unknown"} · chip ${id.chip ?? "unknown"} · objective ${id.objective ?? "unknown"}`;
}

const IDENTITY_KEYS = ["camera", "roi", "chip", "objective"] as const;
const IDENTITY_NAMES: Readonly<Record<(typeof IDENTITY_KEYS)[number], string>> = { camera: "camera", roi: "ROI", chip: "chip", objective: "objective" };

/** What differs between a record's identity and the current one: "ROI 0,0,512,96 → 0,8,512,96". */
export function identityDiff(record: CalibrationIdentity, current: Partial<CalibrationIdentity>): string {
  return IDENTITY_KEYS.filter((k) => record[k] !== current[k])
    .map((k) => `${IDENTITY_NAMES[k]} ${record[k] ?? "unknown"} → ${current[k] ?? "unknown"}`).join(", ");
}

/** "Bead 7 µm · revision 3", from the record's method block; "Not recorded" with neither name nor id. */
export function methodText(method: Record<string, unknown>): string {
  const name = typeof method.display_name === "string" && method.display_name ? method.display_name
    : typeof method.method_id === "string" && method.method_id ? method.method_id : "";
  if (!name) return "Not recorded";
  const rev = typeof method.revision_number === "number" && method.revision_number > 0 ? ` · revision ${method.revision_number}` : "";
  return `${name}${rev}`;
}

/** The record's own file, as its source. */
export const sourceText = (r: CalibrationRecord): string => r.source?.file || "Not recorded";

/** A record's status in plain words: an applied record says whether it is in effect. */
export function statusText(r: CalibrationRecord, list: CalibrationRecordsList): string {
  switch (r.status) {
    case "applied": return list.in_effect_record_id === r.record_id ? "Applied · in effect" : "Applied · not in effect";
    case "proposed": return "Proposed";
    case "superseded": return "Superseded";
    case "invalidated": return "Invalidated";
    case "insufficient": return "Insufficient";
  }
}

/** Kit tone of a record's status. */
export function statusTone(r: CalibrationRecord, list: CalibrationRecordsList): StateTone {
  switch (r.status) {
    case "applied": return list.in_effect_record_id === r.record_id ? "confirmed" : "warn";
    case "proposed": return "info";
    case "superseded": return "neutral";
    case "invalidated":
    case "insufficient": return "blocked";
  }
}

/** The factor a record stands for: the applied version's new factor, else its proposal. */
export function recordFactor(r: CalibrationRecord): number | null {
  if (r.apply && finite(r.apply.new_factor)) return r.apply.new_factor;
  return finite(r.results?.proposed_pixel_to_micron) ? r.results.proposed_pixel_to_micron : null;
}

/** The diameter CV the record measured, or null. */
export const recordCv = (r: CalibrationRecord): number | null => {
  const v = r.results?.cv_diameter_percent;
  return finite(v) ? v : null;
};

const num = (v: number | null, digits = 1) => (v === null ? "not recorded" : Number(v.toFixed(digits)).toString());

/** A record's failed minimum-evidence gates in plain words ("Valid objects: 120, at least 200"). */
export function failedGates(r: CalibrationRecord): string[] {
  return (r.gates ?? []).filter((g) => !g.pass).map((g) => {
    switch (g.id) {
      case "min_valid_objects": return `Valid objects: ${num(g.value, 0)}, at least ${g.limit}`;
      case "max_cv_percent": return `Diameter CV: ${num(g.value)} %, at most ${g.limit} %`;
      case "run_finalized": return `Run finalized: no${g.reason ? ` · ${g.reason}` : ""}`;
      case "file_readable": return `Run file readable: no${g.reason ? ` · ${g.reason}` : ""}`;
      default: return `${g.id}: failed${g.reason ? ` · ${g.reason}` : ""}`;
    }
  });
}

export type ApplyAction = "Apply" | "Re-apply";

export interface ApplyAvailability {
  /** The button the record gets; null for a record that cannot be applied at all. */
  action: ApplyAction | null;
  /** Why the button cannot be pressed now, "" when it can (control and run locks come on top). */
  reason: string;
}

/** Whether a record can be applied (or re-applied) now, from the list alone. Insufficient records
 *  never get a button. A proposed record gets "Apply"; the applied record that is no longer in
 *  effect (its factor changed after it was applied, e.g. after a restart) gets "Re-apply". The
 *  identity and the current factor must be known, and the identity the record's. */
export function applyAvailability(r: CalibrationRecord, list: CalibrationRecordsList): ApplyAvailability {
  if (r.status === "insufficient") return { action: null, reason: "an insufficient record is never applicable" };
  let action: ApplyAction | null = null;
  if (r.status === "proposed" && r.applicable) action = "Apply";
  else if (r.status === "applied" && list.applied_record_id === r.record_id && list.in_effect_record_id !== r.record_id) action = "Re-apply";
  if (!action) return { action: null, reason: "" };
  const current = list.current_identity;
  if (!current || !current.known) return { action, reason: "the camera or ROI identity is unknown now: connect the camera and let the ROI apply first" };
  const diff = identityDiff(r.identity, current);
  if (diff) return { action, reason: `derived under another setup (${diff}): run the calibration method on this setup` };
  if (!finite(list.current_factor) || list.current_factor <= 0) return { action, reason: "the current pixel-to-micron factor is not reported, so there is nothing to compare against" };
  return { action, reason: "" };
}

/** The record the gate's Fix stands for, offered as a button beside it: the applied record to
 *  re-apply (the ruling of PR #771: after a restart a `required` method needs one explicit
 *  re-apply), else a proposed record of this setup. Null when the gate asks for nothing or the
 *  list offers no such record. */
export function fixAction(g: ReadinessGate | null, list: CalibrationRecordsList | null): { record: CalibrationRecord; action: ApplyAction } | null {
  if (!gateNeedsAction(g) || !list?.valid || !list.records) return null;
  const applied = list.records.find((r) => r.record_id === list.applied_record_id);
  if (applied) {
    const a = applyAvailability(applied, list);
    if (a.action === "Re-apply" && a.reason === "") return { record: applied, action: "Re-apply" };
  }
  for (const r of list.records) {
    const a = applyAvailability(r, list);
    if (a.action === "Apply" && a.reason === "") return { record: r, action: "Apply" };
  }
  return null;
}

/** The applied record when it is not in effect: what took it out, in plain words. */
export function notInEffectText(applied: CalibrationRecord, list: CalibrationRecordsList): string {
  const current = list.current_identity;
  if (!current?.known) return "The camera or ROI identity is unknown now, so the applied record cannot be confirmed.";
  const diff = identityDiff(applied.identity, current);
  if (diff) return `It was derived under another setup (${diff}).`;
  const factor = applied.apply?.new_factor;
  return `Its factor ${factorText(factor)} is not the current factor ${factorText(list.current_factor)}: the factor changed after it was applied (a restart restores the method's factor, never the record's). Re-apply it to put it back in effect.`;
}

/** The kit Notice of an apply's outcome. */
export interface CalibrationNotice {
  tone: "confirmed" | "warn" | "blocked" | "unknown" | "info";
  title: string;
  text: string;
}

const NOTHING = "Nothing was applied; the page reads the records again.";

/** An apply reply in plain words: previous → new from the reply when applied, else the refusal. */
export function applyReplyNotice(reply: CalibrationApplyReply, expectedFactor: number): CalibrationNotice {
  const id = reply.record_id;
  const said = reply.message ? ` The backend: ${reply.message}.` : "";
  switch (reply.outcome) {
    case O.Applied: return {
      tone: "confirmed", title: `Applied · ${id}`,
      text: `Pixel-to-micron ${factorChange(reply.previous_factor, reply.new_factor)}.${reply.superseded_record_id ? ` Record ${reply.superseded_record_id} is superseded.` : ""} No method was changed.`,
    };
    case O.StaleFactor: return {
      tone: "warn", title: "Not applied · the factor changed since this page read it",
      text: `This page read ${factorText(expectedFactor)}; the backend's factor is now ${factorText(reply.previous_factor)}. ${NOTHING} Check the records, then apply again.`,
    };
    case O.RunActive: return { tone: "warn", title: "Not applied · a run is active", text: `Calibration is applied only while the experiment is idle and no raw recording runs. ${NOTHING}` };
    case O.Insufficient: return { tone: "blocked", title: "Not applied · the record is insufficient", text: `Its evidence fails a minimum-evidence gate, so it can never be applied. ${NOTHING}` };
    case O.IdentityMismatch: return /identity is unknown/.test(reply.message)
      ? { tone: "unknown", title: "Not applied · the setup's identity is unknown", text: `Connect the camera and let the ROI apply, then try again. ${NOTHING}${said}` }
      : { tone: "blocked", title: "Not applied · the record is from another setup", text: `It was derived under another camera, ROI, chip or objective: run the calibration method on this setup. ${NOTHING}${said}` };
    case O.NotApplicable: return { tone: "warn", title: "Not applied · the record cannot be applied now", text: `${NOTHING}${said}` };
    case O.NotFound: return { tone: "warn", title: "Not applied · the backend has no such record", text: `${NOTHING}${said}` };
    case O.StoreFailed: return { tone: "blocked", title: "Not applied · the calibration store could not be written", text: `${NOTHING}${said}` };
    case O.InvalidRequest: return { tone: "blocked", title: "Not applied · the request was refused", text: `${NOTHING}${said}` };
    default: return { tone: "unknown", title: "Not applied · an answer this page does not know", text: `${NOTHING}${said}` };
  }
}

/** An apply that got no decoded reply: a viewer's refusal from the server, or no answer at all. */
export function applyErrorNotice(error: unknown): CalibrationNotice {
  const text = String(error);
  if (/VIEWER_ONLY/.test(text))
    return { tone: "blocked", title: "Not applied · another tab or client holds control", text: `Only the controlling client applies calibration. ${NOTHING}` };
  if (/INVALID_RECORD_ID|INVALID_EXPECTED_FACTOR/.test(text))
    return { tone: "blocked", title: "Not applied · nothing was sent", text: `The record id or the factor this page read cannot be sent (${text}).` };
  return { tone: "unknown", title: "Apply outcome not known", text: `No reply was read (${text}). The page reads the records again: the record in effect and the factor show what happened.` };
}

/** What the backend derives when a run of a calibration method closes (the A4 rulings). */
export const DERIVATION_TEXT = "When a run of a calibration method closes, the backend derives a record from that run's own file: the median equivalent "
  + "diameter in px with its MAD and CV, the proposed pixel-to-micron (the reference's nominal diameter ÷ the median), the deformation offset "
  + "(report only), event-rate stability, focus only when the file records a focus metric, and E against its nominal only where both exist. "
  + `It needs at least ${CALIBRATION_RECORDS.min_valid_objects} valid objects and a diameter CV within the method's limit (default `
  + `${CALIBRATION_RECORDS.default_max_cv_percent} %); otherwise the record is insufficient and can never be applied. Nothing applies on its own.`;

/** How a calibration is run from the workspace: the existing Plan/Start path, no separate one. */
export const RUN_CALIBRATION_TEXT = "Apply the calibration method (a method whose purpose is calibration, with its bead reference) in the classic "
  + "view, then record the beads from Experiment · Plan with Start run, like any run.";

/** The gap, said plainly: the bridge reports no method purpose yet. */
export const PURPOSE_GAP_TEXT = "The workspace cannot tell yet whether the applied method is a calibration method: the bridge does not report a "
  + "method's purpose. The calibration gate says it in the backend's words (Not required · a calibration method records the reference).";
