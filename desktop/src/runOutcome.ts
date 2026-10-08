// How a finished run ended, for the operator (#549). The backend reconciles every admitted frame
// and files the run as Complete, IntentionallyPartial (declared policy), IncompleteLoss (frames
// lost without being declared) or Failed. "Finalized" with finalization_ok only says the file was
// written; before this the UI never showed the completion or the loss counts, so an undeclared
// loss (for example a frame lost to a sensor-link error) went unseen.
//
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.

import { RUN_COMPLETION_STATES } from "./bridgeContract";
import type { RunAccounting } from "./bridge";
import type { ExperimentStatus } from "./eventAdapter";

export type OutcomeSeverity = "ok" | "partial" | "loss" | "failed" | "unknown" | "legacy";

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
  /** A declared outcome the operator should still look at: malformed frames above the warning
   *  fraction. Always false for loss, failure and unknown, which are alerts by severity. */
  attention: boolean;
}

/** Malformed frames above this share of the admitted frames are worth a warning even though they
 *  are a declared loss (mirrors kMalformedWarnFraction in RecordingAccounting.h). */
export const MALFORMED_WARN_FRACTION = 0.001;

// Backend keys (RecordingAccounting::finalize) and what they mean to an operator.
const LOSS_LABELS: Record<string, string> = {
  storeOverwritten: "overwritten in the frame store before they were saved",
  storeNotCommitted: "never committed to the frame store",
  storeMalformed: "malformed (an ingress error from the sensor link, or an unusable frame)",
  processingFailed: "failed in processing",
  sequenceGaps: "missing from the frame sequence",
  // Declared by policy (IntentionallyPartial), not losses.
  cancelledByPolicy: "cancelled by the delivery policy",
  pendingAtStop: "still pending when the run stopped",
  persistencePendingAtStop: "still waiting to be written when the run stopped",
  persistenceCancelledByPolicy: "left unwritten by policy",
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

/** The outcome for a completion value, its reason text and the number of frames the run admitted. */
export function describeCompletion(completion: number, reason: string, admitted: number): RunOutcome {
  const base = { admitted, reason, losses: [] as OutcomeCount[], lossFraction: null as number | null, attention: false };
  switch (completion) {
    case RUN_COMPLETION_STATES.Complete:
      return { ...base, severity: "ok", headline: `Run complete: all ${admitted} admitted frames reconciled.` };
    case RUN_COMPLETION_STATES.IntentionallyPartial: {
      // Frames that were detected, counted and booked (a malformed frame from a sensor-link ingress
      // error, a cancellation by policy) are declared, so this is informational. Malformed frames
      // above the warning fraction still get the operator's attention.
      const losses = parseLossCounts(reason);
      const malformed = losses.find((l) => l.key === "storeMalformed")?.count ?? 0;
      const total = losses.reduce((a, l) => a + l.count, 0);
      const lossFraction = admitted > 0 ? total / admitted : null;
      const attention = admitted > 0 && malformed / admitted > MALFORMED_WARN_FRACTION;
      const what = losses.map((l) => `${noun(l.count, "frame was", "frames were")} ${l.label}`).join("; ");
      const of = losses.length && admitted > 0 ? ` (${(100 * (lossFraction ?? 0)).toFixed(3)} % of ${admitted} admitted)` : "";
      const warn = attention ? ` Malformed frames are above ${(MALFORMED_WARN_FRACTION * 100).toFixed(1)} % of the run: check the sensor link.` : "";
      return {
        severity: "partial", admitted, reason, losses, lossFraction, attention,
        headline: `Run finished with a declared partial result${losses.length ? `: ${what}${of}` : " (by policy)"}.${warn}`,
      };
    }
    case RUN_COMPLETION_STATES.IncompleteLoss: {
      const losses = parseLossCounts(reason);
      const total = losses.reduce((a, l) => a + l.count, 0);
      const lossFraction = admitted > 0 ? total / admitted : null;
      const what = losses.length
        ? losses.map((l) => `${noun(l.count, "frame was", "frames were")} ${l.label}`).join("; ")
        : reason;
      const of = admitted > 0 ? ` of ${admitted} admitted (${(100 * (lossFraction ?? 0)).toFixed(3)} %)` : "";
      return { severity: "loss", headline: `Run finished with undeclared loss${of}: ${what}.`, admitted, reason, losses, lossFraction, attention: false };
    }
    case RUN_COMPLETION_STATES.Failed:
      return { ...base, severity: "failed", headline: `Run failed: ${reason || "no reason reported"}.` };
    default:
      return { ...base, severity: "unknown", headline: `Run outcome unknown${reason ? `: ${reason}` : ""}.` };
  }
}

/** The outcome of the last finished run, or null while there is none to report (no terminal status,
 *  a cancelled run, or a status the backend has not filled in). `accounting` is the run's
 *  `fetch_run_accounting("last_run")`: with it the loss fractions use the frames the run admitted;
 *  without it the status only knows the rows it saved, which understates the denominator when
 *  frames are empty. */
export function describeRunOutcome(s: ExperimentStatus | null, accounting?: RunAccounting | null): RunOutcome | null {
  if (!s || !s.valid || !s.terminal || s.cancelled) return null;
  const mine = !!accounting && accounting.available && accounting.recorded !== false && accounting.completion !== undefined &&
    (accounting.start_generation === undefined || String(accounting.start_generation) === s.start_generation);
  const admitted = mine ? Number(accounting!.admitted) || 0 : Number(s.persistence_admitted) || 0;
  return describeCompletion(s.completion, s.completion_reason, admitted);
}

/** The outcome saved in the file loaded for review, or null when none is open. A raw recording or a
 *  legacy file without accounting is a quiet "not recorded" note, not a warning. */
export function describeReviewOutcome(a: RunAccounting | null): RunOutcome | null {
  if (!a || !a.available) return null;
  if (a.recorded === false || a.completion === undefined) {
    return {
      severity: "legacy", admitted: 0, losses: [], lossFraction: null, attention: false, reason: "",
      headline: "No run accounting is saved in this file (a raw recording or an older file).",
    };
  }
  const o = describeCompletion(a.completion, a.completion_reason ?? "", Number(a.admitted) || 0);
  if (a.reconciled === false && o.severity !== "failed" && o.severity !== "unknown") {
    return { ...o, severity: "failed", headline: `This file's run accounting does not reconcile. ${o.headline}` };
  }
  return o;
}

/** Identity of a finished run, so a notice or log line is made once per run. */
export function runKey(s: ExperimentStatus | null): string {
  return s && s.terminal ? `${s.start_generation}:${s.end_time_ns}` : "";
}
