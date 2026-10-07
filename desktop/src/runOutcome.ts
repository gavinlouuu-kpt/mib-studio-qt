// How a finished run ended, for the operator (#549). The backend reconciles every admitted frame
// and files the run as Complete, IntentionallyPartial (declared policy), IncompleteLoss (frames
// lost without being declared) or Failed. "Finalized" with finalization_ok only says the file was
// written; before this the UI never showed the completion or the loss counts, so an undeclared
// loss (for example a frame lost to a sensor-link error) went unseen.
//
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.

import { RUN_COMPLETION_STATES } from "./bridgeContract";
import type { ExperimentStatus } from "./eventAdapter";

export type OutcomeSeverity = "ok" | "partial" | "loss" | "failed" | "unknown";

export interface OutcomeCount {
  key: string;
  label: string;
  count: number;
}

export interface RunOutcome {
  severity: OutcomeSeverity;
  /** One line: what happened. */
  headline: string;
  /** The non-zero loss counts, in the order the backend lists them. */
  losses: OutcomeCount[];
  /** Frames the run admitted (the denominator), "0" when unknown. */
  admitted: number;
  /** Lost frames over admitted, 0..1; null without a denominator. */
  lossFraction: number | null;
  /** The backend's own reason text, kept for the log and the tooltip. */
  reason: string;
}

// Backend keys (RecordingAccounting::finalize) and what they mean to an operator.
const LOSS_LABELS: Record<string, string> = {
  storeOverwritten: "overwritten in the frame store before they were saved",
  storeNotCommitted: "never committed to the frame store",
  storeMalformed: "malformed (an ingress error from the sensor link, or an unusable frame)",
  processingFailed: "failed in processing",
  sequenceGaps: "missing from the frame sequence",
};

/** Non-zero `key=value` counts from "undeclared loss: a=0 b=1 ...". */
export function parseLossCounts(reason: string): OutcomeCount[] {
  const out: OutcomeCount[] = [];
  for (const m of reason.matchAll(/(\w+)=(\d+)/g)) {
    const count = Number(m[2]);
    if (count > 0 && LOSS_LABELS[m[1]]) out.push({ key: m[1], label: LOSS_LABELS[m[1]], count });
  }
  return out;
}

function noun(n: number, one: string, many: string): string {
  return `${n} ${n === 1 ? one : many}`;
}

/** The outcome of the last finished run, or null while there is none to report (no terminal status,
 *  a cancelled run, or a status the backend has not filled in). */
export function describeRunOutcome(s: ExperimentStatus | null): RunOutcome | null {
  if (!s || !s.valid || !s.terminal || s.cancelled) return null;
  const admitted = Number(s.persistence_admitted) || 0;
  const reason = s.completion_reason;
  const base = { admitted, reason, losses: [] as OutcomeCount[], lossFraction: null as number | null };
  switch (s.completion) {
    case RUN_COMPLETION_STATES.Complete:
      return { ...base, severity: "ok", headline: `Run complete: all ${admitted} admitted frames reconciled.` };
    case RUN_COMPLETION_STATES.IntentionallyPartial:
      return { ...base, severity: "partial", headline: "Run finished with a declared partial result (by policy)." };
    case RUN_COMPLETION_STATES.IncompleteLoss: {
      const losses = parseLossCounts(reason);
      const total = losses.reduce((a, l) => a + l.count, 0);
      const lossFraction = admitted > 0 ? total / admitted : null;
      const what = losses.length
        ? losses.map((l) => `${noun(l.count, "frame was", "frames were")} ${l.label}`).join("; ")
        : reason;
      const of = admitted > 0 ? ` of ${admitted} admitted (${(100 * (lossFraction ?? 0)).toFixed(3)} %)` : "";
      return { severity: "loss", headline: `Run finished with undeclared loss${of}: ${what}.`, admitted, reason, losses, lossFraction };
    }
    case RUN_COMPLETION_STATES.Failed:
      return { ...base, severity: "failed", headline: `Run failed: ${reason || "no reason reported"}.` };
    default:
      return { ...base, severity: "unknown", headline: `Run outcome unknown${reason ? `: ${reason}` : ""}.` };
  }
}

/** Identity of a finished run, so a notice or log line is made once per run. */
export function runKey(s: ExperimentStatus | null): string {
  return s && s.terminal ? `${s.start_generation}:${s.end_time_ns}` : "";
}
