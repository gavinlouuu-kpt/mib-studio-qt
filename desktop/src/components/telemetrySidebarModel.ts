// Pure derivations of the classic left column (#797; board "Classic · Hardware Preflight ·
// summary-first · redesigned left column"): one summary per subsystem from the App state the
// sidebar already reads. Every value is a backend fact or "—"; nothing here is simulated.
//
// Pure module: no React or transport imports (the kit types are types only).
import type { StateTone } from "@mib/ui";
import { EXPERIMENT_STATES, PUMP_RUN_STATES } from "../bridgeContract";
import type { AutofocusStatus, PumpStatus } from "../bridge";
import type { ExperimentStatus } from "../eventAdapter";
import { metricNumber } from "../metricFormat";
import type { StageStatus } from "../workflow";
import { pendingSaveCount } from "../experimentCounters";

/** A value column entry: the text, and how it is coloured. `tone` only for a backend state;
 *  `mute` for a zero or inactive value; plain ink otherwise. */
export interface SideValue { text: string; tone?: "confirmed" | "warn" | "blocked"; mute?: boolean }
export interface SideFact { label: string; value: SideValue }

/** An absent value is "—", never "Unavailable" (#797). */
export const DASH = "—";
export const dash = (mute = true): SideValue => ({ text: DASH, mute });

/** The stage chip's tone from the stage tabs' status (the same values the tabs use). */
export function stageTone(status: StageStatus): StateTone {
  switch (status) {
    case "ready":
    case "complete": return "confirmed";
    case "running": return "info";
    case "needs-attention": return "warn";
    default: return "neutral";
  }
}

/** "01 Hardware Preflight · Ready": the stage's index among the tabs, its title and status label. */
export const stageChipText = (index: number, title: string, statusLabel: string): string =>
  `${String(index + 1).padStart(2, "0")} ${title} · ${statusLabel}`;

export interface RunContext {
  /** The run's name: the output file's name without its extension. */
  name: string;
  /** "sample 3 of 7" from the sample queue; "" when no queue is in progress. */
  sample: string;
}

/** "Mock camera · idle · nothing runs"; "Mock camera · running · Exp-0142_03 · sample 3 of 7". */
export function contextLine(source: string, cameraRunning: boolean, run: RunContext | null): string {
  return [source || "No camera", cameraRunning ? "running" : "idle", run ? [run.name, run.sample].filter(Boolean).join(" · ") : "nothing runs"].join(" · ");
}

/** The run's name from the experiment status: the file name without its extension; "" for none. */
export function runName(status: ExperimentStatus | null): string {
  if (!status?.valid || !status.output_path) return "";
  const file = status.output_path.split(/[\\/]/).pop() ?? "";
  return file.replace(/\.[^.]+$/, "");
}

const number = (value: number | null | undefined, digits: number, unit: string): SideValue =>
  typeof value === "number" && Number.isFinite(value) ? { text: `${metricNumber(value, digits)}${unit}`, mute: value === 0 } : dash();

/** Camera: Status, Display rate, Data rate. */
export function cameraFacts(c: { running: boolean; status: string; displayFps: number | null; dataRateMBs: number | null }): SideFact[] {
  return [
    { label: "Status", value: c.running ? { text: "Running", tone: "confirmed" } : { text: c.status, mute: c.status === "unconfigured" || c.status === "idle" } },
    { label: "Display rate", value: number(c.displayFps, 1, " fps") },
    { label: "Data rate", value: number(c.dataRateMBs, 1, " MB/s") },
  ];
}

export interface ProcessingInput {
  hostProcessing: boolean;
  statsValid: boolean;
  algoFps: number | null; validFps: number | null; invalidFps: number | null;
  pixelToMicron: number | null;
  /** The host background (desktop): whether it is supported here, and set. */
  background: { supported: boolean; set: boolean };
}

/** Processing: "Algo / valid / invalid" as one row ("412 / 398 / 14 fps"), px → µm, Background;
 *  on the PL ("Runs on: PL (every frame)") there is no host algorithm rate. */
export function processingFacts(p: ProcessingInput): SideFact[] {
  const fps = (v: number | null) => (p.statsValid && typeof v === "number" && Number.isFinite(v) ? metricNumber(v, 0) : DASH);
  const rates = p.statsValid ? `${fps(p.algoFps)} / ${fps(p.validFps)} / ${fps(p.invalidFps)} fps` : DASH;
  const idle = !p.statsValid || [p.algoFps, p.validFps, p.invalidFps].every((v) => !v);
  return [
    ...(p.hostProcessing
      ? [{ label: "Algo / valid / invalid", value: { text: rates, mute: idle } }]
      : [{ label: "Runs on", value: { text: "PL (every frame)" } }]),
    { label: "px → µm", value: p.statsValid && typeof p.pixelToMicron === "number" ? { text: String(p.pixelToMicron) } : dash() },
    ...(p.background.supported
      ? [{ label: "Background", value: p.background.set ? { text: "Set", tone: "confirmed" as const } : { text: "Not set", mute: true } }]
      : []),
  ];
}

/** Focus: null while the controller is not connected (one dim line is shown instead); else
 *  Controller, Ring width, Voltage, Metric age (the "Autofocus" and "Nanopositioner Autofocus"
 *  sections of the old column, merged). */
export function focusFacts(af: AutofocusStatus | null): SideFact[] | null {
  if (!af?.valid || !af.connected) return null;
  const sampled = af.last_ring_ratio_update_us > 0;
  return [
    { label: "Controller", value: { text: `connected (${af.enabled ? "auto" : "manual"})`, tone: "confirmed" } },
    { label: "Ring width", value: sampled ? { text: metricNumber(af.median_ring_ratio, 3) } : dash() },
    { label: "Voltage", value: { text: `${metricNumber(af.current_voltage, 1)} V` } },
    { label: "Metric age", value: sampled ? { text: `${metricNumber(af.ring_ratio_age_us / 1000, 0)} ms` } : dash() },
  ];
}

/** The pump's live rate in µL/s from the unit the backend reports (100 = µL/min, 103 = mL/min);
 *  null for a unit this build does not know. */
export function microlitersPerSecond(status: PumpStatus): number | null {
  if (status.flow_rate_unit === 100) return status.current_flow_rate / 60;
  if (status.flow_rate_unit === 103) return (status.current_flow_rate * 1000) / 60;
  return null;
}

/** "0.040 µL/s · running" (the board's words); "—" for a pump that is not connected. */
export function pumpValue(status: PumpStatus | null): SideValue {
  if (!status?.valid || !status.connected) return dash();
  const rate = microlitersPerSecond(status);
  const running = status.run_status === PUMP_RUN_STATES.Forward || status.run_status === PUMP_RUN_STATES.Backward;
  const state = status.stalled ? "stalled" : running ? "running" : status.run_status === PUMP_RUN_STATES.Pause ? "paused" : "stopped";
  const text = `${rate === null ? `${status.current_flow_rate} (unit ${status.flow_rate_unit})` : `${metricNumber(rate, 3)} µL/s`} · ${state}`;
  return status.stalled ? { text, tone: "blocked" } : running ? { text, tone: "confirmed" } : { text, mute: true };
}

/** Fluidics: null while no pump is connected (one dim line is shown instead). */
export function fluidicsFacts(pumps: readonly [PumpStatus | null, PumpStatus | null]): SideFact[] | null {
  if (!pumps.some((p) => p?.valid && p.connected)) return null;
  return [
    { label: "Sample pump", value: pumpValue(pumps[0]) },
    { label: "Sheath pump", value: pumpValue(pumps[1]) },
  ];
}

export const EXPERIMENT_STATE_NAMES: Readonly<Record<number, string>> = {
  [EXPERIMENT_STATES.Idle]: "Inactive",
  [EXPERIMENT_STATES.Starting]: "Starting",
  [EXPERIMENT_STATES.Active]: "Active",
  [EXPERIMENT_STATES.Stopping]: "Stopping",
  [EXPERIMENT_STATES.Failed]: "Failed",
};

export const OUTCOME_WORDS: Readonly<Record<string, string>> = {
  ok: "Complete", partial: "Partial (declared)", loss: "Undeclared loss", failed: "Failed", unknown: "Unknown", legacy: "No accounting",
};

export interface ExperimentInput {
  status: ExperimentStatus | null;
  state: number;
  active: boolean;
  /** The last run's outcome severity (runOutcome.ts), null while none or a run is active. */
  outcome: string | null;
  elapsedWallSeconds: number | null;
}

/** Experiment: Status, Last run (when an outcome exists), Elapsed, then the grouped counters
 *  Buffered / Saved / Pending save / Dropped / Failed to save / Flush, dim while inactive. */
export function experimentFacts(e: ExperimentInput): SideFact[] {
  const s = e.status?.valid ? e.status : null;
  const pair = (valid: string | undefined, invalid: string | undefined): SideValue =>
    s ? { text: `${valid} valid · ${invalid} invalid`, mute: !e.active } : dash();
  const one = (v: string | undefined): SideValue => (s ? { text: String(v), mute: !e.active } : dash());
  const failed = e.state === EXPERIMENT_STATES.Failed;
  return [
    { label: "Status", value: { text: EXPERIMENT_STATE_NAMES[e.state] ?? "Inactive", tone: e.active ? "confirmed" : failed ? "blocked" : undefined, mute: !e.active && !failed } },
    ...(e.outcome && !e.active ? [{ label: "Last run", value: { text: OUTCOME_WORDS[e.outcome] ?? e.outcome, tone: e.outcome === "ok" ? "confirmed" as const : undefined } }] : []),
    { label: "Elapsed", value: e.elapsedWallSeconds === null || e.elapsedWallSeconds < 0 ? dash() : { text: `${e.elapsedWallSeconds}s`, mute: !e.active } },
    { label: "Buffered", value: pair(s?.valid_buffered, s?.invalid_buffered) },
    { label: "Saved", value: pair(s?.valid_saved, s?.invalid_saved) },
    { label: "Pending save", value: s ? one(pendingSaveCount(s)) : dash() },
    { label: "Dropped", value: pair(s?.dropped_valid, s?.dropped_invalid) },
    { label: "Failed to save", value: one(s?.persistence_failed) },
    { label: "Flush", value: s ? { text: s.flushing ? "Flushing" : "Idle", mute: !s.flushing } : dash() },
  ];
}
