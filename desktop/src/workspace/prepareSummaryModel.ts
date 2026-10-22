// Pure derivations of the summary-first Prepare (O1b, approved by Gavin 2026-10-22): one headline
// with a "detected and selected" line, a neutral "Next · Align" line, and the disclosure rows of
// "What was checked". The header chip and the Plan keep `prepareVerdict`'s words; the headline is
// derived alongside them and never replaces them.
//
// What counts as an issue in Prepare (Gavin's decisions, 2026-10-22):
// - The gates a running camera or Align resolves (NEXT_ALIGN_GATES) are not counted. They are shown
//   in neutral words on the "Next · Align" line and in their rows; the backend still refuses Start
//   until they pass (nothing changes in the backend).
// - The mock camera chosen on purpose (camera.source Warn "simulated camera source selected") is not
//   counted and reads "simulated camera". A camera.source Fail (the requested camera is unavailable and
//   the backend fell back) still counts.
// - storage.output is never counted: Prepare reads the gates without a destination, so it is always
//   Unavailable here ("no output path chosen yet"); Start checks the chosen file.
// - A checklist check counts as `prepareVerdict` counts it: a blocking check, or a warning or failure.
// - A row with a counted issue opens by itself and cannot be collapsed while the issue lasts.
//
// Pure module: no React or transport imports (the kit types are types only).

import type { StateTone } from "@mib/ui";
import { READINESS_GATE_STATUSES as G } from "../bridgeContract";
import type { ExperimentReadiness, ReadinessGate } from "../eventAdapter";
import type { CalibrationRecord, MethodCalibration } from "../calibrationRecords";
import type { PreflightCheck, PreflightReport } from "../preflight";
import { DESTINATION_GATE, gateBlocksStart, gateLabel, type ZStageFact } from "./prepareModel";

/** Gates a running camera or Align resolves: shown on the "Next · Align" line, never counted. */
export const NEXT_ALIGN_GATES: readonly string[] = [
  "camera.session",
  "camera.deliveryMode",
  "camera.geometry",
  "telemetry.transportLoss",
  "processing.roi",
  "processing.background",
];
const CAMERA_RUN_GATES = ["camera.session", "camera.deliveryMode", "camera.geometry", "telemetry.transportLoss"];

/** The backend's camera.source reason for a mock camera chosen on purpose (ExperimentCoordinator). */
export const SIMULATED_SOURCE_REASON = "simulated camera source selected";

export const isNextAlignGate = (id: string): boolean => NEXT_ALIGN_GATES.includes(id);
const settled = (status: number) => status === G.Pass || status === G.NotRequired;

/** The mock camera chosen on purpose: its camera.source warning is not counted. */
export const isSimulatedSource = (g: Pick<ReadinessGate, "id" | "status" | "reason">): boolean =>
  g.id === "camera.source" && g.status === G.Warn && g.reason === SIMULATED_SOURCE_REASON;

export type IssueKind = "failed" | "unknown" | "warning";

/** How Prepare reads a gate: a counted issue, a deferred (Next · Align) or chosen (simulated) one
 *  shown in neutral words, the destination checked at Start, or settled. */
export type GateRole = IssueKind | "next" | "simulated" | "destination" | "settled";

export function gateRole(g: Pick<ReadinessGate, "id" | "status" | "reason">): GateRole {
  if (g.id === DESTINATION_GATE) return "destination";
  if (settled(g.status)) return "settled";
  if (isNextAlignGate(g.id)) return "next";
  if (isSimulatedSource(g)) return "simulated";
  if (g.status === G.Warn) return "warning";
  return g.status === G.Fail ? "failed" : "unknown";
}

export const isCounted = (role: GateRole): role is IssueKind => role === "failed" || role === "unknown" || role === "warning";

/** A checklist check as `prepareVerdict` counts it; null when it does not count. */
export function checkIssue(c: Pick<PreflightCheck, "blocks" | "status">): IssueKind | null {
  if (c.blocks) return c.status === "failed" ? "failed" : c.status === "unknown" ? "unknown" : "warning";
  if (c.status === "failed") return "failed";
  if (c.status === "warning") return "warning";
  return null;
}

/** The optional Z stage counts only when it reports a fault or a warning (as an optional check). */
export function zStageIssue(z: ZStageFact): IssueKind | null {
  return z.status === "failed" ? "failed" : z.status === "warning" ? "warning" : null;
}

/** "focus" and "fluidics" split the optional row where the stage also holds their forms (the
 *  classic Hardware Preflight). */
export type RowKey = "camera" | "calibration" | "method" | "destination" | "optional" | "focus" | "fluidics" | "all";

/** The row a backend gate belongs to (prepare-structure-notes.md). A gate that belongs to no device,
 *  or an id this build does not know, goes to the Method row, which is always shown. */
export function gateRow(id: string): Exclude<RowKey, "optional" | "all"> {
  if (id.startsWith("calibration.")) return "calibration";
  if (id.startsWith("storage.") || id === "lifecycle.recording") return "destination";
  if (id.startsWith("camera.") || id.startsWith("instrument.") || ["science.pl", "ring.placement", "run.frozen", "processing.core",
    "processing.profileCompile", "telemetry.transportLoss", "trigger.output", "rf.generator"].includes(id)) return "camera";
  return "method";
}

/** Checklist ids per row. Trigger is shown in both the Camera row and the optional row (as drawn). */
export const CAMERA_CHECKS = ["camera", "core", "plCore", "sensorLink", "led", "capture", "trigger"];
export const OPTIONAL_CHECKS = ["autofocus", "samplePump", "sheathPump", "trigger"];
export const DESTINATION_CHECKS = ["storage"];

/** `check`: the checklist id ("zStage" for the Z stage), or the gate id, behind the issue. */
export interface CountedIssue { kind: IssueKind; name: string; row: RowKey; check?: string }

export interface PrepareSummaryInput {
  report: PreflightReport;
  readiness: ExperimentReadiness | null;
  error: string;
  paused: boolean;
  zStage: ZStageFact;
}

const plural = (n: number, one: string, many = `${one}s`) => `${n} ${n === 1 ? one : many}`;

/** "1 failed, 2 unknown, 1 warning". */
export function issueCountText(issues: readonly Pick<CountedIssue, "kind">[]): string {
  const n = (k: IssueKind) => issues.filter((i) => i.kind === k).length;
  return [n("failed") ? `${n("failed")} failed` : "", n("unknown") ? `${n("unknown")} unknown` : "", n("warning") ? plural(n("warning"), "warning") : ""]
    .filter(Boolean).join(", ");
}

/** Every counted issue, with the row that shows it. */
export function countedIssues(p: PrepareSummaryInput): CountedIssue[] {
  const out: CountedIssue[] = [];
  for (const c of p.report.checks) {
    const kind = checkIssue(c);
    if (!kind) continue;
    // Trigger is drawn in both rows; it is counted once, in the Camera row.
    const row: RowKey = CAMERA_CHECKS.includes(c.id) ? "camera" : DESTINATION_CHECKS.includes(c.id) ? "destination" : "optional";
    out.push({ kind, name: c.label, row, check: c.id });
  }
  const z = zStageIssue(p.zStage);
  if (z) out.push({ kind: z, name: "Coarse Z stage", row: "optional", check: "zStage" });
  if (gatesRead(p)) {
    for (const g of p.readiness!.gates) {
      const role = gateRole(g);
      if (isCounted(role)) out.push({ kind: role, name: gateLabel(g.id), row: gateRow(g.id), check: g.id });
    }
  } else if (!p.paused) {
    // Gates not read are unknown, never a pass; the All checks row says why.
    out.push({ kind: "unknown", name: "Backend gates", row: "all" });
  }
  return out;
}

const gatesRead = (p: Pick<PrepareSummaryInput, "readiness">) => !!p.readiness && p.readiness.valid;

export interface Headline { text: string; tone: StateTone; counts: string }

/** The headline of the verdict card: "Hardware ready", "Hardware ready · simulated camera",
 *  "Ready with warnings · N" or "Needs attention · N"; "Readiness paused · a run is active" while a
 *  run is active (the gates are not read then). */
export function prepareHeadline(p: PrepareSummaryInput, issues = countedIssues(p)): Headline {
  if (p.paused) return { text: "Readiness paused · a run is active", tone: "info", counts: "" };
  const counts = issueCountText(issues);
  if (issues.some((i) => i.kind !== "warning")) return { text: `Needs attention · ${issues.length}`, tone: issues.some((i) => i.kind === "failed") ? "blocked" : "unknown", counts };
  if (issues.length) return { text: `Ready with warnings · ${issues.length}`, tone: "warn", counts };
  const simulated = gatesRead(p) && p.readiness!.gates.some(isSimulatedSource);
  return { text: simulated ? "Hardware ready · simulated camera" : "Hardware ready", tone: "confirmed", counts: "" };
}

/** The neutral "Next · Align" line: what Align still has to do, and how many gates Start waits for. */
export function nextAlignText(r: ExperimentReadiness | null): string {
  if (!r || !r.valid) return "not read yet";
  const pending = r.gates.filter((g) => isNextAlignGate(g.id) && !settled(g.status));
  if (!pending.length) return "nothing left: the camera runs, ROI and background are set";
  const has = (id: string) => pending.some((g) => g.id === id);
  const parts: string[] = [];
  if (has("camera.session")) parts.push("start the camera");
  else if (CAMERA_RUN_GATES.some((id) => pending.some((g) => g.id === id && g.status !== G.Warn))) parts.push("wait for the first frame");
  else if (has("camera.deliveryMode")) parts.push("check the frame delivery");
  if (has("processing.roi") && has("processing.background")) parts.push("set ROI and background");
  else if (has("processing.roi")) parts.push("set ROI");
  else if (has("processing.background")) parts.push("set background");
  const refusing = pending.filter((g) => gateBlocksStart(g.status)).length;
  const waits = pending.length === 1 ? "Start waits for this gate" : `Start waits for these ${pending.length} gates`;
  // A warning does not refuse Start; say so when none of them would.
  return `${parts.join(", ")} · ${refusing ? waits : `${pending.length === 1 ? "it does" : "they do"} not refuse Start`}`;
}

/** "A, B and 2 more". */
export function firstNames(names: readonly string[], shown = 2): string {
  if (names.length <= shown) return names.length > 1 ? `${names.slice(0, -1).join(", ")} and ${names[names.length - 1]}` : names.join("");
  return `${names.slice(0, shown).join(", ")} and ${names.length - shown} more`;
}

/** The one-line note beside Confirm when gates would still refuse Start; "" when none would.
 *  "Start is refused until Camera source passes and the camera runs in Align." */
export function startRefusalLine(r: ExperimentReadiness | null): string {
  if (!r || !r.valid) return "";
  const refused = r.gates.filter((g) => g.id !== DESTINATION_GATE && gateBlocksStart(g.status));
  if (!refused.length) return "";
  const counted = refused.filter((g) => !isNextAlignGate(g.id)).map((g) => gateLabel(g.id));
  const next = refused.filter((g) => isNextAlignGate(g.id));
  const parts: string[] = [];
  if (counted.length) parts.push(`${firstNames(counted)} ${counted.length === 1 ? "passes" : "pass"}`);
  if (next.some((g) => CAMERA_RUN_GATES.includes(g.id))) parts.push("the camera runs in Align");
  else if (next.length) parts.push("Align is done");
  // "A and B pass, and the camera runs in Align": a comma keeps the two "and"s apart.
  return `Start is refused until ${parts.join(counted.length > 1 ? ", and " : " and ")}.`;
}

const CAMERA_NAMES: Readonly<Record<string, string>> = { egrabber: "eGrabber", mindvision: "MindVision", mock: "mock" };

/** The requested camera of a fallback ("eGrabber"), from the backend's camera.source Fail reason. */
export function fallbackCamera(g: ReadinessGate | undefined): string | null {
  if (!g || g.id !== "camera.source" || g.status !== G.Fail) return null;
  const m = /^requested (\S+) camera is unavailable/.exec(g.reason);
  return m ? CAMERA_NAMES[m[1]] ?? m[1] : "";
}

/** "Mock camera (chosen)", "Mock camera (fallback: eGrabber camera unavailable)", "Camera 01". */
export function cameraName(report: PreflightReport, r: ExperimentReadiness | null): string {
  const camera = report.checks.find((c) => c.id === "camera");
  const name = camera && camera.detected && camera.detected !== "—" ? camera.detected : "No camera";
  const source = r?.valid ? r.gates.find((g) => g.id === "camera.source") : undefined;
  const fallback = fallbackCamera(source);
  if (fallback !== null) return fallback ? `${name} (fallback: ${fallback} camera unavailable)` : `${name} (fallback)`;
  if (source && isSimulatedSource(source)) return `${name} (chosen)`;
  return name;
}

/** "core 0.3.2 trusted", "PL core 1a2b3c4d", or the core check's state. */
export function coreText(report: PreflightReport): string {
  const core = report.checks.find((c) => c.id === "core");
  const pl = report.checks.find((c) => c.id === "plCore");
  if (pl) return pl.status === "passed" ? `PL core ${pl.detected}` : `PL core ${pl.status === "unknown" ? "unknown" : pl.status}`;
  if (!core) return "";
  if (core.status === "passed") return `core ${core.detected} trusted`;
  return core.status === "unknown" ? "core not read yet" : `core ${core.status}`;
}

/** The optional hardware that is not connected: "focus, pumps and trigger". */
export function notConnectedText(report: PreflightReport, z: ZStageFact): string {
  const off = (id: string) => report.checks.some((c) => c.id === id && c.requirement !== "required" && (c.status === "not-required" || c.status === "unsupported"));
  const focusOff = (off("autofocus") || !report.checks.some((c) => c.id === "autofocus")) && (z.status === "not-required" || z.status === "unsupported");
  const sample = off("samplePump"), sheath = off("sheathPump");
  const names = [
    focusOff ? "focus" : "",
    sample && sheath ? "pumps" : sample ? "sample pump" : sheath ? "sheath pump" : "",
    off("trigger") ? "trigger" : "",
  ].filter(Boolean);
  return names.length ? firstNames(names, names.length) : "";
}

/** The "detected and selected" line under the headline. */
export function detectedLine(report: PreflightReport, r: ExperimentReadiness | null, z: ZStageFact, calibration: string): string {
  const off = notConnectedText(report, z);
  return [cameraName(report, r), coreText(report), calibration, off ? `optional: ${off} not connected` : ""].filter(Boolean).join(" · ");
}

/** A row's chip from its counted issues: "Fail · Camera source", "Warn · Region of interest and 1 more". */
export function issueChip(issues: readonly CountedIssue[]): { tone: StateTone; text: string } | null {
  if (!issues.length) return null;
  const worst: IssueKind = issues.some((i) => i.kind === "failed") ? "failed" : issues.some((i) => i.kind === "unknown") ? "unknown" : "warning";
  const word = worst === "failed" ? "Fail" : worst === "unknown" ? "Unknown" : "Warn";
  const tone: StateTone = worst === "failed" ? "blocked" : worst === "unknown" ? "unknown" : "warn";
  return { tone, text: `${word} · ${firstNames(issues.map((i) => i.name), 1)}` };
}

/** The All checks summary: "20 backend gates · 11 pass · 6 next in Align · 1 simulated camera · 1 not
 *  required · destination checked at Start". */
export function allChecksSummary(r: ExperimentReadiness | null, error: string, paused: boolean): string {
  if (paused) return "Not read while a run is active";
  if (!r) return error ? `Unknown · ${error}` : "Unknown · not read yet";
  if (!r.valid) return "Unknown · not reported";
  const roles = r.gates.map((g) => ({ g, role: gateRole(g) }));
  const n = (f: (x: { g: ReadinessGate; role: GateRole }) => boolean) => roles.filter(f).length;
  const parts = [
    plural(r.gates.length, "backend gate"),
    n((x) => x.role === "failed") ? `${n((x) => x.role === "failed")} failed` : "",
    n((x) => x.role === "unknown") ? `${n((x) => x.role === "unknown")} unknown` : "",
    n((x) => x.role === "warning") ? plural(n((x) => x.role === "warning"), "warning") : "",
    n((x) => x.g.status === G.Pass) ? `${n((x) => x.g.status === G.Pass)} pass` : "",
    n((x) => x.role === "next") ? `${n((x) => x.role === "next")} next in Align` : "",
    n((x) => x.role === "simulated") ? "1 simulated camera" : "",
    n((x) => x.g.status === G.NotRequired) ? `${n((x) => x.g.status === G.NotRequired)} not required` : "",
    n((x) => x.role === "destination") ? "destination checked at Start" : "",
  ];
  return parts.filter(Boolean).join(" · ");
}

/** "How Prepare reads readiness", inside All checks (no side card on the first page). */
export const HOW_PREPARE_READS: readonly string[] = [
  "The backend evaluates every gate; this page only shows them, read without a destination: no file is written while you look.",
  "Unknown is never Pass. Unsupported never blocks. Start re-evaluates every gate with the chosen file.",
  "Gates that a running camera or Align resolves are listed under Next · Align and are not counted here; Start still waits for them.",
  "A mock camera chosen on purpose reads Ready · simulated camera. A row with a counted issue opens by itself, with its Fix.",
  "Confirm follows the hardware checklist, is kept for this browser tab, and Start does not require it.",
];

/** The note under a deferred gate, and under the chosen mock camera's source gate. */
export const NEXT_ALIGN_NOTE = "Next · Align: not counted in Prepare; Start waits for it";
export const NEXT_ALIGN_WARN_NOTE = "Next · Align: not counted in Prepare";
export const SIMULATED_NOTE = "Chosen on purpose: expected for development/tests; not a hardware run. Not counted.";
export const DESTINATION_NOTE = "Expected in Prepare: the file is chosen at Start. Never counted as an issue.";

/** How a disclosure row opens: `issue` = it has a counted issue (open, cannot be collapsed);
 *  `auto` = it opens by itself, but the operator may close it. */
export interface RowOpenState { issue: boolean; auto: boolean }

/** The Calibration row (prepare-structure-notes.md): locked open while the calibration.record gate
 *  is a counted issue; open by itself (closable) when the applied method is a calibration method or
 *  a proposed record awaits Apply. */
export function calibrationRowState(gate: ReadinessGate | null, method: MethodCalibration | null,
  records: readonly Pick<CalibrationRecord, "status">[] | null | undefined): RowOpenState {
  return {
    issue: !!gate && isCounted(gateRole(gate)),
    auto: method?.purpose === "calibration" || !!records?.some((r) => r.status === "proposed"),
  };
}
