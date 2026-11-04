// Pure derivations of the workspace Prepare view (O1, #713): the backend readiness gates read
// without a destination, and the hardware checklist, in the UI language's tones.
//
// Truth rules (desktop/ui/guides/ui-language.md): every tone comes from a backend status or a
// checklist state; Unknown is never shown as Pass; Unsupported never blocks; a missing value is
// shown as missing. Prepare reads readiness with an empty output path, so the backend reports
// `storage.output` Unavailable ("no output path chosen yet") and writes no probe file
// (ExperimentCoordinator, the `outputPath.empty()` branch); Start re-evaluates every gate with the
// chosen file.
//
// Pure module: no React or transport imports (the kit types are types only).

import type { StateTone } from "@mib/ui";
import { READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { StageStatus } from "../bridge";
import type { ExperimentReadiness, ReadinessGate } from "../eventAdapter";
import { checkGateStatus, type CheckStatus, type PreflightCheck, type PreflightReport } from "../preflight";

/** Prepare never names a destination: the backend then probes no file (#713). */
export const PREPARE_OUTPUT_PATH = "";
/** Readiness poll period while Prepare is shown. The checklist itself polls every 1.5 s. */
export const PREPARE_READINESS_POLL_MS = 2000;
/** The gate only Start can satisfy: the file is chosen there. */
export const DESTINATION_GATE = "storage.output";

const GATE_WORD: Readonly<Record<number, string>> = {
  [G.Pass]: "Pass",
  [G.Warn]: "Warn",
  [G.Fail]: "Fail",
  [G.Unavailable]: "Unavailable",
  [G.NotRequired]: "Not required",
};

/** Kit tone of a backend gate status. Unavailable reads as unknown (it was not evaluated) and
 *  still blocks Start; an unrecognized value is unknown, never a pass. */
export function gateTone(status: number): StateTone {
  switch (status) {
    case G.Pass: return "confirmed";
    case G.Warn: return "warn";
    case G.Fail: return "blocked";
    case G.NotRequired: return "neutral";
    default: return "unknown";
  }
}

/** "State · reason", e.g. "Unavailable · no output path chosen yet". */
export function gateText(g: Pick<ReadinessGate, "status" | "reason">): string {
  const word = GATE_WORD[g.status] ?? "Unknown";
  return g.reason ? `${word} · ${g.reason}` : word;
}

/** Fail and Unavailable block Start (ExperimentReadiness.h); unknown is never Pass. */
export const gateBlocksStart = (status: number) => status !== G.Pass && status !== G.Warn && status !== G.NotRequired;

/** The destination gate as Prepare expects it: Unavailable because no path was given. */
const pendingDestination = (g: ReadinessGate) => g.id === DESTINATION_GATE && g.status === G.Unavailable;

/** Plain labels for the backend readiness gates (ExperimentCoordinator, MethodProvenance). The
 *  raw id stays visible as secondary text so support can match it to the logs; an id missing
 *  here is shown as itself, never hidden. */
export const GATE_LABELS: Readonly<Record<string, string>> = {
  "instrument.mode": "Camera mode (Align / Run)",
  "instrument.strobe": "LED strobe timing",
  "camera.mode": "Camera mode",
  "camera.session": "Camera session",
  "camera.source": "Camera source",
  "camera.deliveryMode": "Frame delivery",
  "camera.geometry": "Frame size",
  "science.pl": "PL science",
  "ring.placement": "Frame ring memory",
  "run.frozen": "Stopped Run (buffered frames)",
  "processing.profileCompile": "Method on the PL",
  "processing.roi": "Region of interest",
  "processing.core": "Processing core",
  "processing.config": "Processing settings",
  "calibration.pixelToMicron": "Pixel → µm calibration",
  "calibration.record": "Calibration record",
  "processing.background": "Background image",
  "processing.backgroundCalibration": "Background calibration",
  "trigger.output": "Sort trigger output",
  "rf.generator": "RF generator",
  "clock.wall": "Wall clock",
  "storage.ssd": "SSD record store",
  "storage.output": "Destination file",
  "storage.roundtrip": "Destination write test",
  "storage.persistent": "Destination kept at power-off",
  "storage.buffer": "Recording memory budget",
  "storage.hdf5": "Recording file",
  "lifecycle.recording": "Raw recording",
  "lifecycle.experiment": "Running experiment",
  "lifecycle.fault": "Previous run fault",
  "telemetry.transportLoss": "Frame loss reporting",
  "method.revision": "Method revision",
};

/** The plain label of a gate, or its id when there is none. */
export const gateLabel = (id: string): string => GATE_LABELS[id] ?? id;

/** Remediations the workspace words itself (#763), because the backend's names a control the
 *  operator cannot press from here or leads to a step that cannot succeed yet:
 *  - camera.session: the backend names the Qt app's "Start Live View". The desktop's classic view
 *    has Start Camera, and the workspace never starts the camera (navigation never acts) (F11).
 *  - processing.background: Background calibration with no background set finds no empty frame on
 *    a feed with cells in every frame, so Set Background comes first (F8). */
export const GATE_REMEDIATIONS: Readonly<Record<string, string>> = {
  "camera.session": "in the classic view, press Start Camera and wait until the camera reports Running. The workspace does not start the camera",
  "processing.background": "Set Background first: in the classic view, with the camera running, press Set Background. Without a background, "
    + "Background calibration finds no empty frame when every frame has cells",
};

/** What the operator does about a gate: the workspace's own words when it has them, else the
 *  backend's remediation; "" for a gate that passes or is not required. */
export function gateRemediation(g: Pick<ReadinessGate, "id" | "status" | "remediation">): string {
  if (g.status === G.Pass || g.status === G.NotRequired) return g.remediation;
  return GATE_REMEDIATIONS[g.id] ?? g.remediation;
}

type IssueKind = "failed" | "unknown" | "warning";
/** `blocking`: the issue blocks Confirm (a check) or Start (a gate); it sets the Not ready tone. */
interface Issue { kind: IssueKind; name: string; blocking: boolean }

export interface Verdict {
  /** The same words wherever it is shown. */
  word: "Not ready" | "Readiness paused" | "Ready to confirm" | "Confirmed" | "Ready with warnings" | "Ready";
  tone: StateTone;
  /** "Not ready · 3 unknown, 1 warning", "Ready to confirm · Start blocked: Processing core". */
  title: string;
  /** What the counts are, in plain names. */
  detail: string;
  /** With "Ready to confirm": the gates Start will refuse on, "<label> — <remediation>". */
  startBlocked: string[];
}

const plural = (n: number, one: string, many = `${one}s`) => `${n} ${n === 1 ? one : many}`;

function countText(issues: Issue[]): string {
  const n = (k: IssueKind) => issues.filter((i) => i.kind === k).length;
  return [n("failed") ? `${n("failed")} failed` : "", n("unknown") ? `${n("unknown")} unknown` : "", n("warning") ? plural(n("warning"), "warning") : ""]
    .filter(Boolean).join(", ");
}

function namesText(issues: Issue[]): string {
  const group = (k: IssueKind, title: string) => {
    const names = issues.filter((i) => i.kind === k).map((i) => i.name);
    return names.length ? `${title}: ${names.join(", ")}.` : "";
  };
  return [group("failed", "Failed"), group("unknown", "Unknown"), group("warning", "Warnings")].filter(Boolean).join(" ");
}

/** "A, B and 2 more": the first names, so a chip stays one line. */
function firstNames(names: string[], shown = 2): string {
  const head = names.slice(0, shown).join(", ");
  return names.length > shown ? `${head} and ${names.length - shown} more` : head;
}

/** One verdict for Prepare from the hardware checklist and the backend gates read without a
 *  path. It never contradicts the Confirm button, which the checklist alone drives (as in the
 *  classic Preflight: some gates can only pass later, e.g. the ROI in Align and the destination
 *  at Start).
 *  - Not ready: a required check blocks (Confirm is unavailable).
 *  - Readiness paused · a run is active: the gates are not read during a run and Confirm is
 *    locked (`paused`).
 *  - Ready to confirm · Start blocked: the checklist passes, but a backend gate other than the
 *    pending destination blocks Start (Fail, or Unavailable: Start refuses both). Warn tone, and
 *    `startBlocked` says what Start will refuse on. With the gates not read, "Ready to confirm ·
 *    gates not read yet" in the unknown tone.
 *  - Confirmed · Start blocked / Confirmed · gates not read yet: as "Ready to confirm", once the
 *    operator confirmed readiness for this setup (`confirmed`, #763 F9): the words never ask for a
 *    confirmation that was given.
 *  - Ready with warnings: nothing blocks, but a check or gate warns (or an optional check failed).
 *  - Ready.
 *  The destination is checked at Start and never counted either way. */
export function prepareVerdict(report: PreflightReport, r: ExperimentReadiness | null, error = "", paused = false, confirmed = false): Verdict {
  const issues: Issue[] = [];
  for (const c of report.checks) {
    if (c.blocks) issues.push({ kind: c.status === "failed" ? "failed" : c.status === "unknown" ? "unknown" : "warning", name: c.label, blocking: true });
    else if (c.status === "warning" || c.status === "failed") issues.push({ kind: c.status === "failed" ? "failed" : "warning", name: c.label, blocking: false });
  }
  const checklistBlocks = issues.some((i) => i.blocking);
  const gatesRead = !!r && r.valid;
  const refused: ReadinessGate[] = [];
  if (!gatesRead) {
    issues.push({ kind: "unknown", name: error ? `Backend gates (${error})` : !r ? "Backend gates (not read yet)" : "Backend gates (not reported)", blocking: true });
  } else {
    for (const g of r.gates) {
      if (pendingDestination(g)) continue;
      if (g.status === G.Warn) issues.push({ kind: "warning", name: gateLabel(g.id), blocking: false });
      else if (gateBlocksStart(g.status)) {
        issues.push({ kind: g.status === G.Fail ? "failed" : "unknown", name: gateLabel(g.id), blocking: true });
        refused.push(g);
      }
    }
  }
  const destination = gatesRead && r.gates.some(pendingDestination);
  const tail = destination ? " The destination is checked at Start, with the chosen file." : "";
  const detail = namesText(issues) + tail;
  if (checklistBlocks) {
    // The tone takes the gates that refuse Start into account too: a failed gate reads blocked.
    const blocking = issues.filter((i) => i.blocking);
    const tone: StateTone = blocking.some((i) => i.kind === "failed") ? "blocked" : blocking.some((i) => i.kind === "unknown") ? "unknown" : "warn";
    return { word: "Not ready", tone, title: `Not ready · ${countText(issues)}`, detail, startBlocked: [] };
  }
  if (paused) return { word: "Readiness paused", tone: "info", title: "Readiness paused · a run is active", detail: "Start re-evaluates every gate.", startBlocked: [] };
  const word = confirmed ? "Confirmed" : "Ready to confirm";
  if (!gatesRead) return { word, tone: "unknown", title: `${word} · gates not read yet`, detail, startBlocked: [] };
  if (refused.length) {
    const startBlocked = refused.map((g) => {
      const fix = (gateRemediation(g) || g.reason).replace(/\.$/, "");
      return fix ? `${gateLabel(g.id)} — ${fix}` : gateLabel(g.id);
    });
    return { word, tone: "warn", title: `${word} · Start blocked: ${firstNames(refused.map((g) => gateLabel(g.id)))}`, detail, startBlocked };
  }
  if (issues.length) return { word: "Ready with warnings", tone: "warn", title: `Ready with warnings · ${countText(issues)}`, detail, startBlocked: [] };
  return { word: "Ready", tone: "confirmed", title: destination ? "Ready · destination checked at Start" : "Ready", detail: tail.trim(), startBlocked: [] };
}

/** The Notice next to an available Confirm when Start would still be refused. */
export function startRefusalText(startBlocked: readonly string[]): { title: string; text: string } | null {
  if (!startBlocked.length) return null;
  return { title: `Start blocked · ${plural(startBlocked.length, "gate")}`, text: `Start will be refused until: ${startBlocked.join("; ")}.` };
}

/** Why the Preflight confirmation is unavailable because of the checklist, in plain words
 *  ("3 required checks not read yet: PL core (build + weights), Sensor link, LED strobe"). */
export function confirmBlockers(report: PreflightReport): { tone: "blocked" | "unknown" | "warn"; lines: string[] } | null {
  const blocking = report.checks.filter((c) => c.blocks);
  if (!blocking.length) return null;
  const line = (status: CheckStatus, what: string) => {
    const names = blocking.filter((c) => c.status === status).map((c) => c.label);
    return names.length ? `${plural(names.length, "required check")} ${what}: ${names.join(", ")}` : "";
  };
  const lines = [line("failed", "failed"), line("unknown", "not read yet or not verified"), line("warning", "with a warning")].filter(Boolean);
  const tone = blocking.some((c) => c.status === "failed") ? "blocked" : blocking.some((c) => c.status === "unknown") ? "unknown" : "warn";
  return { tone, lines };
}

/** The fixed footnote of the "Ready to operate?" card. */
export const READINESS_FOOTNOTE = "Unknown is never Pass. Unsupported never blocks. Start re-evaluates every gate with the chosen file.";

const STATE_WORD: Readonly<Record<CheckStatus, string>> = {
  passed: "Pass",
  warning: "Warn",
  failed: "Fail",
  unknown: "Unknown",
  unsupported: "Unsupported",
  "not-required": "Not required",
};

/** Kit tone of a checklist state, through the backend status it corresponds to. */
export function checkTone(status: CheckStatus): StateTone {
  return status === "unsupported" ? "unsupported" : gateTone(checkGateStatus(status));
}

/** FactList tone of a checklist state (FactList colours backend states only). */
export function checkFactTone(status: CheckStatus): "confirmed" | "warn" | "blocked" | "unknown" | undefined {
  const tone = checkTone(status);
  return tone === "confirmed" || tone === "warn" || tone === "blocked" || tone === "unknown" ? tone : undefined;
}

/** FactList tone of a backend gate status. */
export function gateFactTone(status: number): "confirmed" | "warn" | "blocked" | "unknown" | undefined {
  const tone = gateTone(status);
  return tone === "confirmed" || tone === "warn" || tone === "blocked" || tone === "unknown" ? tone : undefined;
}

const WORST_FIRST: readonly CheckStatus[] = ["failed", "unknown", "warning", "passed", "unsupported", "not-required"];

/** A card's state: the worst state among its checks and facts ("Fail · 2 checks"); null for none. */
export function worstState(checks: readonly Pick<PreflightCheck, "status">[]): { status: CheckStatus; text: string } | null {
  const status = WORST_FIRST.find((s) => checks.some((c) => c.status === s));
  if (!status) return null;
  const n = checks.filter((c) => c.status === status).length;
  return { status, text: `${STATE_WORD[status]}${n > 1 ? ` · ${n} checks` : ""}` };
}

/** "State · detected · detail", e.g. "Unknown · PL core identity not read yet." */
export function checkText(c: PreflightCheck): string {
  const parts = [STATE_WORD[c.status]];
  if (c.detected && c.detected !== "—") parts.push(c.detected);
  if (c.expected) parts.push(`expected ${c.expected}`);
  parts.push(c.detail);
  return parts.join(" · ");
}

export interface RequiredSummary {
  title: string;
  /** Required checks that block or warn, in checklist order (at most two are shown as chips). */
  attention: PreflightCheck[];
  allReady: boolean;
}

/** "Required checks: 5 of 6 ready · 1 unknown". Ready = does not block (a warning that its
 *  backend gate does not fail is ready); unknown is never ready. */
export function summarizeRequired(report: PreflightReport): RequiredSummary {
  const required = report.checks.filter((c) => c.requirement === "required");
  const ready = required.filter((c) => !c.blocks).length;
  const unknown = required.filter((c) => c.status === "unknown").length;
  const failed = required.filter((c) => c.blocks && c.status !== "unknown").length;
  const extra = [unknown ? `${unknown} unknown` : "", failed ? `${failed} blocking` : ""].filter(Boolean).join(" · ");
  return {
    title: `Required checks: ${ready} of ${required.length} ready${extra ? ` · ${extra}` : ""}`,
    attention: report.checks.filter((c) => c.blocks || c.status === "warning"),
    allReady: ready === required.length,
  };
}

export type ConfirmationState = "none" | "confirmed" | "changed" | "blocked";

/** Where the operator's Preflight confirmation stands against the current signature. The
 *  confirmation is kept per browser tab (sessionStorage; it survives a reload of the tab), so it is
 *  advisory: Start does not require it (#763 F23) and re-evaluates every gate itself. */
export function confirmationState(signature: string, confirmedFor: string, criticalPassed: boolean): ConfirmationState {
  if (signature !== "" && signature === confirmedFor) return criticalPassed ? "confirmed" : "blocked";
  if (confirmedFor !== "") return "changed";
  return "none";
}

export const CONFIRMATION_TEXT: Readonly<Record<ConfirmationState, { tone: "confirmed" | "warn" | "blocked" | "unknown" | undefined; text: string }>> = {
  none: { tone: undefined, text: "Not confirmed in this browser tab" },
  confirmed: { tone: "confirmed", text: "Confirmed for this setup" },
  changed: { tone: "warn", text: "Setup changed since the confirmation · confirm again" },
  blocked: { tone: "blocked", text: "Confirmed, but a required check blocks · it does not count" },
};

/** The Preflight confirmation beside Start (#763 F23), while it is not given for this setup; null
 *  once it is. Advisory: it is kept per browser tab, and Start does not require it. */
export function startConfirmation(state: ConfirmationState): { tone: "unknown" | "warn" | "blocked"; chip: string; note: string } | null {
  if (state === "confirmed") return null;
  const chip = state === "changed" ? "Setup changed since the confirmation"
    : state === "blocked" ? "Confirmation does not count · a required check blocks"
    : CONFIRMATION_TEXT.none.text;
  return {
    tone: state === "changed" ? "warn" : state === "blocked" ? "blocked" : "unknown",
    chip,
    note: "Prepare's Confirm readiness is advisory: it is kept per browser tab, and Start does not require it. Start re-evaluates every gate with the chosen file.",
  };
}

export interface ZStageFact {
  value: string;
  tone?: "confirmed" | "warn" | "blocked" | "unknown";
  /** The same state in the checklist's vocabulary, for the card chip. */
  status: CheckStatus;
}

/** The coarse Z stage as a fact (#464): only what the backend reports. Optional hardware: an
 *  absent stage is not required, a refused one is shown as such but blocks nothing here. */
export function zStageFact(s: StageStatus | null): ZStageFact {
  if (!s) return { value: "Unknown · not read yet", tone: "unknown", status: "unknown" };
  if (!s.valid) return { value: "Unknown · not reported", tone: "unknown", status: "unknown" };
  if (!s.connected) return { value: "Not connected", status: "not-required" };
  const id = [s.model, s.serial].filter(Boolean).join(" ") || "Stage";
  if (s.emergency_stop) return { value: `${id} · emergency stop`, tone: "blocked", status: "failed" };
  if (!s.configured) return { value: `${id} · does not match the stage profile: motion refused`, tone: "blocked", status: "failed" };
  if (!s.zero_set) return { value: `${id} · zero not set: moves are refused`, tone: "warn", status: "warning" };
  return { value: `${id} · zero set`, tone: "confirmed", status: "passed" };
}
