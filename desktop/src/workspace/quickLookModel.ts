// Pure model of the workspace's Experiment · Summary sub-view (#716 A7b): O5 Run summary and O6 Quick
// look over the backend's run quick look (bridge ABI 44 `run_quick_look`, #716 A7a, runQuickLook.ts).
//
// - The plan's closed runs: a queue's rows once the backend finalized their run (awaiting the
//   operator's confirmation, or closed) and the runs Repeat sample kept; a single sample once its
//   operation reported the run's end. Each is read by its file (the status's output_path, or the
//   single sample's recorded path).
// - Every number shown is the backend's: counters are decimal strings shown as sent (digit groups
//   only), medians, the target-group % and the event rate are formatted to a fixed number of
//   digits, and every delta is the backend's `run − reference`. **This module computes nothing from
//   them** (quickLookModel.test.ts pins it by reading this file and SummaryView.tsx).
// - Honesty (the A7 ruling 6): null reads "Not recorded", a recorded 0 stays 0, flags are
//   descriptive chips in grey, never a verdict colour, and nothing here recommends.
//
// Pure module: no React or transport imports.

import { RUN_QUICK_LOOK } from "../bridgeContract";
import type {
  RunDelta,
  RunDeltaValue,
  RunDeltaWithheld,
  RunIdentityComparison,
  RunQuickLookFlag,
  RunQuickLookReply,
  RunQuickLookRun,
  RunStatistics,
} from "../runQuickLook";
import type { MethodPurpose } from "../calibrationRecords";
import type { SampleRow } from "./planModel";
import { queueClosedRuns, recordedPath, type ChipTone, type QueueState, type RunSeen } from "./queueModel";
import type { PlanReference, PlanStarted } from "./usePlan";

/** What every unrecorded value reads (null, never 0). */
export const NOT_RECORDED = "Not recorded";
/** A deferred run (the A7 ruling 3). */
export const DEFERRED_TEXT = "computed when the instrument is idle";
/** The label the A7 ruling 11 (Q2 a) gives the target group's share. */
export const TARGET_GROUP_LABEL = "% in declared target group";
/** During a run: its medians are the quick look of its file, read once it is closed (Q5 a). */
export const NUMBERS_AT_CLOSE = "numbers at close";

/** One closed run of the plan. */
export interface PlanRun {
  /** The file the backend wrote and closed: the key of the run here, and the path read. */
  path: string;
  runIndex: number;
  /** The sheet row's ref (sample_ref), and the row as planned (null when no longer in the sheet). */
  ref: string;
  row: SampleRow | null;
  /** The queue's row is closed (confirmed by the operator), or still awaiting the confirmation. */
  confirmed: boolean;
  /** How the run ended, as the client observed it. */
  outcome: { cancelled: boolean; headline: string };
  /** How this page followed the run (#716 A7c, O5's checklist): a queue row by the experiment status
   *  (own generation and file), a single sample by its operation's end only (never the status). */
  followed: "status" | "operation";
  /** What the status showed of the run (a queue row); null for a single sample. */
  seen: RunSeen | null;
  /** The terminal status's start and end (backend wall clock, ns, decimal strings); null when not
   *  observed (a single sample, or a row ended before #716 A7c). */
  times: { startNs: string; endNs: string } | null;
  /** The applied method's purpose at the run's Start (ABI 43); null when that report could not say;
   *  undefined when not known. */
  methodPurpose?: MethodPurpose | null;
}

/** The plan's closed runs, in run order. */
export function planClosedRuns(p: { draft: { format: string; rows: readonly SampleRow[] }; queue: QueueState; started: PlanStarted | null }): PlanRun[] {
  const rowOf = (ref: string) => p.draft.rows.find((r) => r.ref === ref) ?? null;
  if (p.draft.format === "queue") {
    return queueClosedRuns(p.draft.rows.map((r) => r.ref), p.queue)
      .filter((c) => c.run.path !== "")
      .map((c) => {
        const e = c.run.end;
        const run: PlanRun = {
          path: c.run.path, runIndex: c.run.runIndex, ref: c.ref, row: rowOf(c.ref), confirmed: c.confirmed,
          outcome: { cancelled: e.cancelled, headline: e.headline }, followed: "status", seen: c.run.seen ?? {},
          times: e.startTimeNs !== undefined && e.endTimeNs !== undefined ? { startNs: e.startTimeNs, endNs: e.endTimeNs } : null,
        };
        if (c.run.methodPurpose !== undefined) run.methodPurpose = c.run.methodPurpose;
        return run;
      });
  }
  const st = p.started;
  if (!st || st.outcome !== "started" || !st.end?.ok || !st.path) return [];
  const row = p.draft.rows[0] ?? null;
  const single: PlanRun = { path: recordedPath(st.path), runIndex: st.runIndex, ref: row?.ref ?? "", row, confirmed: true,
    outcome: { cancelled: st.end.cancelled, headline: st.end.message }, followed: "operation", seen: null, times: null };
  if (st.methodPurpose !== undefined) single.methodPurpose = st.methodPurpose;
  return [single];
}

/** The paths one run_quick_look reads: the newest runs, at most the contract's bound. */
export function quickLookPaths(runs: readonly PlanRun[]): { paths: string[]; left: number } {
  const max = RUN_QUICK_LOOK.max_paths;
  const shown = runs.length > max ? runs.slice(runs.length - max) : runs;
  return { paths: shown.map((r) => r.path), left: runs.length - shown.length };
}

/** The reply's entry for a path. */
export function runFor(reply: RunQuickLookReply | null, path: string): RunQuickLookRun | null {
  return reply?.runs.find((r) => r.path === path) ?? null;
}

/** Whether a reply holds a run (or the reference) the backend will compute only when the instrument is idle. */
export const hasDeferred = (reply: RunQuickLookReply | null): boolean =>
  !!reply?.runs.some((r) => r.status === "deferred") || reply?.reference?.run?.status === "deferred";

/** The reference to send for the plan's choice; null when none is chosen. */
export function referenceArgs(r: PlanReference | null) {
  return r === null ? null : { path: r.path, run_id: r.run_id, start_generation: r.start_generation };
}

/** The reference a computed run would be: its file and the identity its quick look reported. */
export function referenceOf(run: PlanRun, q: RunQuickLookRun | null): PlanReference | null {
  if (q?.status !== "computed" || !q.statistics) return null;
  return { path: run.path, run_id: q.statistics.identity.run_id, start_generation: q.statistics.identity.start_generation, runIndex: run.runIndex, ref: run.ref };
}

// ---- text: formatting only --------------------------------------------------------------------------

/** A decimal string counter in digit groups ("1 234 567"); null stays null. No number is made of it. */
export function counterText(v: string | null | undefined): string | null {
  if (v === null || v === undefined) return null;
  return /^\d+$/.test(v) ? v.replace(/\B(?=(\d{3})+(?!\d))/g, " ") : v;
}

/** A backend number to a fixed number of digits; null stays null. */
export function numberText(v: number | null | undefined, digits: number): string | null {
  return typeof v === "number" && Number.isFinite(v) ? v.toFixed(digits) : null;
}

/** Digits per quantity (display only). */
export const DIGITS = { area: 2, deformability: 4, emodulus: 2, percent: 1, rate: 1 } as const;

/** Why the backend withheld a delta, in plain words. */
export const WITHHELD_TEXT: Readonly<Record<RunDeltaWithheld, string>> = {
  factor_not_recorded: "px→µm not recorded",
  no_emodulus: "no E on one side",
  lut_unidentified: "E-LUT not identified",
  lut_differs: "E-LUT differs",
  no_value: "no median on one side",
};

/** A backend delta as shown in a StatTile: signed to the quantity's digits, or "withheld · why". */
export function deltaText(d: RunDeltaValue | null | undefined, digits: number): string | undefined {
  if (!d) return undefined;
  if (d.withheld !== null) return `withheld · ${WITHHELD_TEXT[d.withheld]}`;
  const s = numberText(d.value, digits);
  if (s === null) return undefined;
  return s.startsWith("-") ? `−${s.slice(1)}` : /[1-9]/.test(s) ? `+${s}` : s;
}

/** The flags (A7 ruling 6) as descriptive chips: grey, never a verdict colour. */
export const FLAG_TEXT: Readonly<Record<RunQuickLookFlag, string>> = {
  low_n: `Low n · under ${RUN_QUICK_LOOK.low_n_valid_rows} valid rows`,
  loss: "Loss · accounting partial or incomplete",
  legacy: "Legacy file · no run snapshot",
  no_accounting: "No accounting recorded",
  e_out_of_domain: "E outside the LUT domain",
  factor_not_recorded: "px→µm not recorded",
  run_not_finalized: "Run not finalized",
};

export function flagChips(s: RunStatistics | null): { key: string; tone: ChipTone; text: string }[] {
  return (s?.flags ?? []).map((f) => ({ key: f, tone: f === "legacy" ? "unknown" : "neutral", text: f === "e_out_of_domain" && s?.emodulus.out_of_domain !== null
    ? `${FLAG_TEXT[f]} · ${counterText(s?.emodulus.out_of_domain ?? null)} rows` : FLAG_TEXT[f] }));
}

const DIFFERS_LABEL: Readonly<Record<keyof RunDelta["differs"], string>> = {
  profile: "Profile", method_revision: "Method revision", core: "Core", contract: "Contract",
};

/** What differs from the reference (flagged, never withheld): chips for `differs` and `unknown`. */
export function differsChips(d: RunDelta | null): { key: string; tone: ChipTone; text: string }[] {
  if (!d) return [];
  return (Object.keys(DIFFERS_LABEL) as (keyof RunDelta["differs"])[])
    .filter((k) => d.differs[k] !== "same")
    .map((k) => ({ key: k, tone: (d.differs[k] as RunIdentityComparison) === "differs" ? "neutral" as const : "unknown" as const,
      text: `${DIFFERS_LABEL[k]} ${d.differs[k] === "differs" ? "differs from the reference" : "· not known vs the reference"}` }));
}

/** One StatTile of a run: label, value (null = not recorded), the text for a missing value, the
 *  backend's n, and the backend's delta. */
export interface QuickTile {
  key: "valid" | "area" | "deformability" | "emodulus" | "target" | "rate";
  label: string;
  value: string | null;
  missing: string;
  detail: string;
  delta?: string;
}

/** The run's StatTiles: valid rows, the medians (with the backend's deltas), the target group and
 *  the event rate. */
export function quickTiles(s: RunStatistics, delta: RunDelta | null): QuickTile[] {
  const e = s.emodulus, tg = s.target_group, rate = s.event_rate;
  const nText = (n: string | null) => (n === null ? "n not recorded" : `n ${counterText(n)}`);
  return [
    { key: "valid", label: "Valid rows", value: counterText(s.counts.valid), missing: NOT_RECORDED,
      detail: `finite ${counterText(s.counts.finite)} · excluded ${counterText(s.counts.excluded)}` },
    { key: "area", label: "Median area (µm²)", value: numberText(s.area_um2.median, DIGITS.area),
      missing: s.area_um2.pixel_to_micron === null ? "px→µm not recorded" : NOT_RECORDED,
      detail: s.area_um2.pixel_to_micron === null ? "no recorded px→µm: no fallback" : `${nText(s.area_um2.n)} · ${s.area_um2.pixel_to_micron} µm/px recorded`,
      delta: deltaText(delta?.area_um2_median, DIGITS.area) },
    { key: "deformability", label: "Median deformability", value: numberText(s.deformability.median, DIGITS.deformability), missing: NOT_RECORDED,
      detail: nText(s.deformability.n), delta: deltaText(delta?.deformability_median, DIGITS.deformability) },
    { key: "emodulus", label: "Median E (kPa)", value: e.status === "computed" ? numberText(e.median_kpa, DIGITS.emodulus) : null,
      missing: e.status === "no_lut" ? "No E-LUT loaded" : NOT_RECORDED,
      detail: e.status === "computed" ? `${nText(e.n)} · finite E > 0${e.out_of_domain !== null ? ` · ${counterText(e.out_of_domain)} outside the LUT` : ""}`
        : e.status === "no_lut" ? "the run recorded no loaded E-LUT" : "the file records no E",
      delta: deltaText(delta?.emodulus_median_kpa, DIGITS.emodulus) },
    { key: "target", label: TARGET_GROUP_LABEL, value: tg.declared === true ? numberText(tg.percent, DIGITS.percent) : null,
      missing: tg.declared === false ? "None declared" : NOT_RECORDED,
      detail: tg.declared === true ? (tg.n !== null && tg.of !== null ? `${counterText(tg.n)} of ${counterText(tg.of)} valid rows` : "n not recorded")
        : tg.declared === false ? "the run declared no target group" : "the file records no target group" },
    { key: "rate", label: "Event rate (/s)", value: rate.status === "computed" ? numberText(rate.per_s, DIGITS.rate) : null,
      missing: rate.status === "too_short" ? "Too short" : NOT_RECORDED,
      detail: rate.status === "computed" ? `valid rows over ${numberText(rate.span_s, 3) ?? "?"} s on the file's clock`
        : rate.status === "too_short" ? "fewer than two valid rows, or no span" : "the file declares no clock" },
  ];
}

/** The quick look's state of one run, as a chip. */
export function statusChip(q: RunQuickLookRun | null, loading: boolean): { tone: ChipTone; text: string } {
  if (!q) return loading ? { tone: "unknown", text: "Reading…" } : { tone: "unknown", text: "Not read yet" };
  switch (q.status) {
    case "computed": return { tone: "neutral", text: q.computed_at === "close" ? "Computed at close" : q.from_cache ? "From the backend's cache" : "Computed on demand" };
    case "deferred": return { tone: "unknown", text: "Deferred · computed when idle" };
    case "not_available": return { tone: "unsupported", text: "Not available · raw recording" };
    case "error": return { tone: "unknown", text: "Not read" };
  }
}

const short = (v: string | null, n = 12) => (v === null ? null : v.length > n ? `${v.slice(0, n)}…` : v);

/** The run's provenance, from its identity (null = not recorded). */
export function provenanceFacts(s: RunStatistics, path: string): { label: string; value: string | null }[] {
  const id = s.identity, lut = s.emodulus.lut;
  const op = id.operator ? [id.operator.name, id.operator.id].filter((v) => v).join(" · ") || null : null;
  return [
    { label: "Experiment", value: id.experiment_id },
    { label: "Run id", value: id.run_id },
    { label: "Run index", value: id.run_index === null ? null : String(id.run_index) },
    { label: "Sample ref", value: id.sample_ref },
    { label: "Condition", value: id.annotation?.condition ?? null },
    { label: "Operator", value: op },
    { label: "Profile", value: id.profile_id || null },
    { label: "Method revision", value: id.method_revision_id },
    { label: "Config sha256", value: short(id.config_canonical_sha256) },
    { label: "Core", value: id.core_version },
    { label: "Contract", value: id.contract_version === null ? null : String(id.contract_version) },
    { label: "Snapshot", value: id.has_snapshot ? `v${id.snapshot_version ?? "?"}${id.snapshot_unreadable ? " · unreadable" : ""}` : null },
    { label: "Start generation", value: id.start_generation },
    { label: "Calibration record", value: id.calibration_record_id },
    { label: "px→µm", value: s.area_um2.pixel_to_micron === null ? null : `${s.area_um2.pixel_to_micron} µm/px (recorded)` },
    { label: "E-LUT", value: lut ? [lut.lut_id ?? "id not recorded", lut.revision ? `rev ${lut.revision}` : null, short(lut.sha256), lut.provenance, lut.loaded ? null : "not loaded"].filter((v) => v).join(" · ") : null },
    { label: "File", value: path },
  ];
}

/** The run's counts and its accounting, as the file records them (decimal strings shown as sent). */
export function countFacts(s: RunStatistics): { label: string; value: string | null }[] {
  const a = s.accounting;
  const acc = (key: string): string | null => {
    const v = a?.[key];
    return typeof v === "string" ? counterText(v) : null;
  };
  return [
    { label: "Rows in the valid set", value: counterText(s.counts.set_rows) },
    { label: "Valid", value: counterText(s.counts.valid) },
    { label: "Finite (plotted)", value: counterText(s.counts.finite) },
    { label: "Excluded (valid, not finite)", value: counterText(s.counts.excluded) },
    { label: "Completion", value: a ? String(a.completion) : null },
    { label: "Admitted", value: acc("admitted") },
    { label: "Processed", value: acc("processed") },
    { label: "Committed to the file", value: acc("persistence_committed") },
    { label: "Cancelled by policy", value: acc("cancelled_by_policy") },
    { label: "Store loss", value: acc("store_loss") },
    { label: "Processing failed", value: acc("processing_failed") },
  ];
}
