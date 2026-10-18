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

export interface ReadinessSummary {
  tone: StateTone;
  /** "State · detail" for a StateChip or a Notice title. */
  title: string;
  /** One line with the facts behind the title. */
  detail: string;
  /** Gates that block Start, other than the destination Start chooses. */
  blockers: ReadinessGate[];
  warnings: ReadinessGate[];
}

/** One summary of a readiness read without a path. */
export function summarizeReadiness(r: ExperimentReadiness | null, error = ""): ReadinessSummary {
  if (error && !r) return { tone: "unknown", title: "Unknown · readiness not read", detail: error, blockers: [], warnings: [] };
  if (!r) return { tone: "unknown", title: "Unknown · readiness not read yet", detail: "Waiting for the first backend evaluation.", blockers: [], warnings: [] };
  if (!r.valid) return { tone: "unknown", title: "Unknown · the backend did not report readiness", detail: "The backend is not initialized.", blockers: [], warnings: [] };
  const blockers = r.gates.filter((g) => gateBlocksStart(g.status) && !pendingDestination(g));
  const warnings = r.gates.filter((g) => g.status === G.Warn);
  const destination = r.gates.some(pendingDestination) ? " The destination is checked at Start, with the chosen file." : "";
  if (blockers.length) {
    return {
      tone: "blocked",
      title: `Start blocked · ${blockers.length} ${blockers.length === 1 ? "gate" : "gates"}`,
      detail: blockers.map((g) => g.id).join(", ") + "." + destination,
      blockers,
      warnings,
    };
  }
  if (r.ready) return { tone: "confirmed", title: "Ready · the backend reports ready", detail: "", blockers, warnings };
  if (warnings.length) {
    return {
      tone: "warn",
      title: `No blockers · ${warnings.length} ${warnings.length === 1 ? "warning" : "warnings"}`,
      detail: warnings.map((g) => g.id).join(", ") + "." + destination,
      blockers,
      warnings,
    };
  }
  // Not "confirmed": the backend did not report ready, because the destination is not chosen yet.
  return { tone: "info", title: "No blockers · destination checked at Start", detail: destination.trim(), blockers, warnings };
}

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

/** Where the operator's Preflight confirmation stands against the current signature. */
export function confirmationState(signature: string, confirmedFor: string, criticalPassed: boolean): ConfirmationState {
  if (signature !== "" && signature === confirmedFor) return criticalPassed ? "confirmed" : "blocked";
  if (confirmedFor !== "") return "changed";
  return "none";
}

export const CONFIRMATION_TEXT: Readonly<Record<ConfirmationState, { tone: "confirmed" | "warn" | "blocked" | "unknown" | undefined; text: string }>> = {
  none: { tone: undefined, text: "Not confirmed in this session" },
  confirmed: { tone: "confirmed", text: "Confirmed for this setup" },
  changed: { tone: "warn", text: "Setup changed since the confirmation · confirm again" },
  blocked: { tone: "blocked", text: "Confirmed, but a required check blocks · it does not count" },
};

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
