// Start Experiment readiness, in the operator's words (epic #866: MON-003, 004, 008, 010, 028).
//
// The backend evaluates every gate (ExperimentCoordinator::evaluateReadiness); this module only
// phrases them the way the Qt app does (MainWindow::explainReadiness / gateStatusLabel) and sorts
// them into what blocks Start and what is only worth knowing. Pure: no React or Tauri imports.
import { EXPERIMENT_START_OUTCOMES, READINESS_GATE_STATUSES } from "../../bridgeContract";

export interface StartGate { id: string; status: number; reason: string; remediation?: string }

/** The gate that is open until the operator picks a file. Qt leaves it out of the preflight. */
export const DESTINATION_GATE = "storage.output";
/** Gates that have their own Start UI, so they stay out of the advisories (the ring-discard confirmation). */
export const HANDLED_ELSEWHERE: ReadonlySet<string> = new Set(["run.frozen"]);
/** With no destination chosen the throughput gate always warns ("choose the destination at Start"):
 *  not worth a standing line before the operator can act on it. A Fail still shows. */
const DESTINATION_DEPENDENT_WARN = "storage.throughput";

/** Qt gateStatusLabel: the word in front of each line of the readiness dialog. */
export function gateStatusLabel(status: number): string {
  switch (status) {
    case READINESS_GATE_STATUSES.Pass: return "OK";
    case READINESS_GATE_STATUSES.Warn: return "Warning";
    case READINESS_GATE_STATUSES.Fail: return "Blocked";
    case READINESS_GATE_STATUSES.Unavailable: return "Unknown";
    case READINESS_GATE_STATUSES.NotRequired: return "Not required";
    default: return "?";
  }
}

/** Fail and Unavailable block Start; a status this UI does not know is never a pass. */
export function blocksStart(g: StartGate): boolean {
  return g.status === READINESS_GATE_STATUSES.Fail || g.status === READINESS_GATE_STATUSES.Unavailable ||
    !Object.values(READINESS_GATE_STATUSES).includes(g.status as never);
}

/** The gates that block Start, except the ids in `ignore` (the preflight ignores storage.output). */
export function blockingGates(gates: readonly StartGate[], ignore: readonly string[] = []): StartGate[] {
  return gates.filter((g) => blocksStart(g) && !ignore.includes(g.id));
}

/** Non-blocking advisories: Warn gates that have no UI of their own. `destinationChosen` false (the standing
 *  readiness, before a file is picked) also drops the throughput warning that only says "pick a destination". */
export function warningGates(gates: readonly StartGate[], destinationChosen = true): StartGate[] {
  return gates.filter((g) => g.status === READINESS_GATE_STATUSES.Warn && !HANDLED_ELSEWHERE.has(g.id) &&
    (destinationChosen || g.id !== DESTINATION_DEPENDENT_WARN));
}

/** Qt's line for one gate: "Blocked — id: reason" and, on the next line, "→ remediation". */
export function gateLine(g: StartGate): string {
  const head = `${gateStatusLabel(g.status)} — ${g.id}: ${g.reason}`;
  return g.remediation ? `${head}\n    → ${g.remediation}` : head;
}

export interface Explanation { title: string; text: string }

/** The refusal dialog (Qt explainReadiness): every blocking gate with its remediation, then the Warn
 *  advisories, which Qt never showed. `blockers` empty gives null: nothing to explain. */
export function explainReadiness(blockers: readonly StartGate[], warnings: readonly StartGate[] = []): Explanation | null {
  if (blockers.length === 0) return null;
  const parts = [
    "The experiment cannot start until every readiness check passes.",
    blockers.map(gateLine).join("\n\n"),
  ];
  if (warnings.length) parts.push(`Also worth knowing (these do not block Start):\n\n${warnings.map(gateLine).join("\n\n")}`);
  return { title: "Start Experiment", text: parts.join("\n\n") };
}

/** One line for the inline alert and the log: "path: id: reason — remediation; ..." (the format the inline
 *  alert had before the dialog; no path before a file is chosen). */
export function blockersLine(path: string | null, blockers: readonly StartGate[]): string {
  const reason = blockers.map((g) => `${g.id}: ${g.reason}${g.remediation ? ` — ${g.remediation}` : ""}`).join("; ");
  return `${path ? `${path}: ` : ""}${reason || "Backend readiness unavailable; experiment was not started."}`;
}

/** The Warn gates a started run carries, as the notice under the toolbar (one line per gate). */
export function warningNotice(gates: readonly StartGate[]): string {
  return warningGates(gates).map((g) => `${g.reason}${g.remediation ? ` (${g.remediation})` : ""}`).join("\n");
}

export interface ReadinessSummary {
  blocked: StartGate[];
  warnings: StartGate[];
  /** "Ready to start", "Blocked (2)", "Ready, 1 warning" ... */
  line: string;
  tone: "ok" | "warn" | "blocked";
}

/** The standing readiness (no destination yet): what blocks Start and what only warns. */
export function summarizeReadiness(gates: readonly StartGate[]): ReadinessSummary {
  const blocked = blockingGates(gates, [DESTINATION_GATE]);
  const warnings = warningGates(gates, false);
  if (blocked.length) return { blocked, warnings, tone: "blocked", line: `Blocked (${blocked.length})${warnings.length ? `, ${warnings.length} warning${warnings.length === 1 ? "" : "s"}` : ""}` };
  if (warnings.length) return { blocked, warnings, tone: "warn", line: `Ready, ${warnings.length} warning${warnings.length === 1 ? "" : "s"}` };
  return { blocked, warnings, tone: "ok", line: "Ready to start" };
}

// ---- Start outcomes (MON-010, closes SHELL-058) -------------------------------------------------------

export interface StartDialog { kind: "info" | "warning" | "error"; title: string; text: string }
export interface OutcomeWording {
  /** The modal, when the outcome has one (Qt shows one for every refusal). */
  dialog: StartDialog | null;
  /** One line for the inline alert and the log. */
  summary: string;
  /** Re-evaluate the gates and explain them: Qt re-explains the readiness the refused Start carried. */
  explainGates: boolean;
}

/** The wording of a refused Start by its typed outcome (bridge ABI 40 `start_outcome`; Qt MainWindow.cpp:1566-1592).
 *  With no outcome (an older backend, a start that never reached the coordinator) only the message is known. */
export function startOutcomeWording(outcome: number | undefined, message: string, path: string): OutcomeWording {
  switch (outcome) {
    case EXPERIMENT_START_OUTCOMES.StaleReadiness:
      return {
        explainGates: false,
        summary: "Experiment start refused: readiness changed",
        dialog: {
          kind: "warning", title: "Start Experiment",
          text: `The configuration changed while the experiment was being prepared, so the readiness check is no longer valid.\n\n${message}\n\nPlease start again.`,
        },
      };
    case EXPERIMENT_START_OUTCOMES.NotReady:
      return { explainGates: true, summary: `Experiment not ready: ${message}`, dialog: null };
    case EXPERIMENT_START_OUTCOMES.StorageFailed:
    case EXPERIMENT_START_OUTCOMES.ProvenanceFailed:
      return {
        explainGates: false,
        summary: `Experiment start failed: ${message}`,
        dialog: { kind: "error", title: "Error", text: `Failed to prepare the experiment file:\n${path}\n\n${message}` },
      };
    case EXPERIMENT_START_OUTCOMES.AlreadyActive:
    case EXPERIMENT_START_OUTCOMES.Busy:
    case EXPERIMENT_START_OUTCOMES.InvalidRequest:
      return { explainGates: false, summary: message, dialog: { kind: "info", title: "Experiment", text: message } };
    default:
      return { explainGates: false, summary: message, dialog: null };
  }
}

// ---- Latest Frame at Start (#858, SHELL-054) -----------------------------------------------------------

/** The gate that carries the capture's delivery mode. */
export const DELIVERY_MODE_GATE = "camera.deliveryMode";

/** True when the readiness says the capture runs Latest Frame. The backend warns on this gate only for a running
 *  capture whose Latest Frame mode is confirmed (ExperimentCoordinator::evaluateReadiness); Every Frame passes, and an
 *  unconfirmed mode is Unavailable, which blocks Start on its own. */
export function runsLatestFrame(gates: readonly StartGate[]): boolean {
  return gates.some((g) => g.id === DELIVERY_MODE_GATE && g.status === READINESS_GATE_STATUSES.Warn);
}

/** Qt's question before the file dialog (MainWindow::onStartExperiment, #332), word for word. */
export const LATEST_FRAME_PROMPT = {
  title: "Start Experiment",
  text: "Latest Frame prioritizes low latency and may intentionally discard frames. The recorded sequence may be incomplete.",
  switchLabel: "Switch to Every Frame",
  continueLabel: "Continue with Latest Frame",
  cancelLabel: "Cancel",
} as const;

export type LatestFrameChoice = "switch" | "continue" | "cancel";

/** The buttons of the question, in Qt's order: Switch (the default) first when it is offered. */
export function latestFrameButtons(offerSwitch: boolean): { yes: string; no?: string; cancel: string } {
  return offerSwitch
    ? { yes: LATEST_FRAME_PROMPT.switchLabel, no: LATEST_FRAME_PROMPT.continueLabel, cancel: LATEST_FRAME_PROMPT.cancelLabel }
    : { yes: LATEST_FRAME_PROMPT.continueLabel, cancel: LATEST_FRAME_PROMPT.cancelLabel };
}

/** The operator's answer for the button clicked (`choose`'s yes / no / cancel) under `latestFrameButtons(offerSwitch)`. */
export function latestFrameChoice(answer: "yes" | "no" | "cancel", offerSwitch: boolean): LatestFrameChoice {
  if (answer === "cancel") return "cancel";
  if (!offerSwitch) return answer === "yes" ? "continue" : "cancel";
  return answer === "yes" ? "switch" : "continue";
}

/** The start notice after "Switch to Every Frame": the running capture keeps Latest Frame until it restarts, so this
 *  recording is still Latest Frame (Qt says so only in a code comment; the operator is told here). */
export const SWITCHED_NOTICE =
  "Delivery mode set to Every Frame; it applies at the next camera start. This recording runs with Latest Frame.";

// ---- Multi-image series (MON-008) ----------------------------------------------------------------------

export const INLINE_SERIES_NOTICE =
  "Multi-image series capture requires inline realtime processing.\n" +
  "This experiment will run in inline mode so series images remain reviewable.\n" +
  "Your previous realtime mode will be restored when the experiment stops.";

/** True when the processing configuration (fetch_processing_config_json text) asks for a multi-image series,
 *  which the backend runs in inline realtime mode (ExperimentCoordinator::start switches it and restores it
 *  at the end). Qt also checks that the mode is AsyncBatch; the bridge does not carry the mode, so a series
 *  that already runs inline still gets the note. */
export function seriesForcesInline(configJson: string): boolean {
  try {
    const multi = (JSON.parse(configJson) as { multi_image?: { enabled?: unknown; count?: unknown } }).multi_image;
    return multi?.enabled === true && typeof multi.count === "number" && multi.count > 1;
  } catch {
    return false;
  }
}

/** What a readiness read decides, without the reason texts (some carry live figures, such as the memory a
 *  storage.buffer gate measured): equal keys block and warn on the same gates. "" for no read. */
export function readinessDecisionKey(r: { valid: boolean; ready: boolean; gates: readonly StartGate[] } | null): string {
  return r ? `${r.valid}|${r.ready}|${r.gates.map((g) => `${g.id}:${g.status}`).join(",")}` : "";
}

/** The backend's refusal of an unacknowledged Latest Frame start (ExperimentCoordinator::start). */
export const LATEST_FRAME_REFUSAL = "Latest Frame delivery discards frames";

/** True when two readiness reads say the same thing, so a poll that changed nothing sets no state. */
export function sameReadiness(
  a: { valid: boolean; ready: boolean; gates: readonly StartGate[] } | null,
  b: { valid: boolean; ready: boolean; gates: readonly StartGate[] },
): boolean {
  return !!a && a.valid === b.valid && a.ready === b.ready && a.gates.length === b.gates.length &&
    a.gates.every((g, i) => g.id === b.gates[i].id && g.status === b.gates[i].status && g.reason === b.gates[i].reason && (g.remediation ?? "") === (b.gates[i].remediation ?? ""));
}
