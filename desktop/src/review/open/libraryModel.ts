// R1 Open's experiment library (#717 B1b part 2, #269): a folder's run files
// grouped by what their headers say about themselves (`review_probe_runs`,
// review ABI 2). Pure: nothing here reads a file. A member a file does not
// record stays null, and no group invents a name, a date or a status.

import type { ReviewRunIdentity, ReviewRunProbe } from "../reviewBridge";
import { REVIEW_PROBE } from "../reviewContract";
import { completionChip, recordedTime, type Chip, type FolderRun } from "./openModel";

/** What a group holds, in display order. */
export type GroupKind =
  | "experiment" // a v3 snapshot that declares an experiment_id
  | "no-experiment" // a run snapshot without an experiment_id (v1, undeclared v3, unreadable)
  | "legacy" // no run snapshot at all
  | "unreadable" // the probe could not read the file (per-file message)
  | "unprobed"; // not read (yet): probing, or the probe was refused

export interface LibraryRun {
  run: FolderRun;
  /** The file's probe; null while not read. */
  probe: ReviewRunProbe | null;
}

export interface RunGroup {
  key: string;
  kind: GroupKind;
  /** The declared experiment (kind "experiment" only). */
  experimentId: string | null;
  runs: LibraryRun[];
}

/** The probe's per-call bound (review contract `review_probe.max_paths`). */
export const PROBE_CHUNK = REVIEW_PROBE.max_paths;

/** A folder's paths in probe calls of at most PROBE_CHUNK paths each. */
export function probeChunks(paths: readonly string[], size: number = PROBE_CHUNK): string[][] {
  const chunks: string[][] = [];
  for (let i = 0; i < paths.length; i += size) chunks.push(paths.slice(i, i + size));
  return chunks;
}

const byName = (a: LibraryRun, b: LibraryRun) =>
  a.run.name.localeCompare(b.run.name, undefined, { numeric: true, sensitivity: "base" });

/** Runs in an experiment: by run_index, runs that declare none after, each
 * tie by file name. */
function byRunIndex(a: LibraryRun, b: LibraryRun) {
  const ia = a.probe?.identity.run_index ?? null;
  const ib = b.probe?.identity.run_index ?? null;
  if (ia !== null && ib !== null && ia !== ib) return ia - ib;
  if (ia === null && ib !== null) return 1;
  if (ia !== null && ib === null) return -1;
  return byName(a, b);
}

/** The newest recorded start of a group's runs (decimal ns), or null. */
function newestStart(group: RunGroup): bigint | null {
  let newest: bigint | null = null;
  for (const r of group.runs) {
    const ns = r.probe?.start_time_ns;
    if (!ns || !/^\d+$/.test(ns) || /^0+$/.test(ns)) continue;
    const v = BigInt(ns);
    if (newest === null || v > newest) newest = v;
  }
  return newest;
}

function kindOf(probe: ReviewRunProbe | null): GroupKind {
  if (!probe) return "unprobed";
  if (!probe.ok) return "unreadable";
  const id = probe.identity;
  if (id.experiment_id) return "experiment";
  if (id.has_snapshot || id.snapshot_unreadable) return "no-experiment";
  return "legacy";
}

const KIND_ORDER: GroupKind[] = ["experiment", "no-experiment", "legacy", "unreadable", "unprobed"];

/**
 * A folder's runs grouped for the library: one group per declared
 * experiment_id (runs by run_index), newest experiment first by its runs'
 * recorded starts (experiments without one after, by id); then the runs
 * without an experiment, the legacy files, the files that could not be read
 * and those not read yet, each by file name.
 */
export function groupRuns(runs: readonly FolderRun[], probes: ReadonlyMap<string, ReviewRunProbe>): RunGroup[] {
  const experiments = new Map<string, RunGroup>();
  const others = new Map<GroupKind, RunGroup>();
  for (const run of runs) {
    const probe = probes.get(run.path) ?? null;
    const kind = kindOf(probe);
    if (kind === "experiment") {
      const id = probe!.identity.experiment_id!;
      let group = experiments.get(id);
      if (!group) {
        group = { key: `experiment:${id}`, kind, experimentId: id, runs: [] };
        experiments.set(id, group);
      }
      group.runs.push({ run, probe });
    } else {
      let group = others.get(kind);
      if (!group) {
        group = { key: kind, kind, experimentId: null, runs: [] };
        others.set(kind, group);
      }
      group.runs.push({ run, probe });
    }
  }
  const declared = Array.from(experiments.values());
  for (const g of declared) g.runs.sort(byRunIndex);
  declared.sort((a, b) => {
    const sa = newestStart(a);
    const sb = newestStart(b);
    if (sa !== null && sb !== null && sa !== sb) return sa > sb ? -1 : 1;
    if (sa === null && sb !== null) return 1;
    if (sa !== null && sb === null) return -1;
    return a.experimentId!.localeCompare(b.experimentId!);
  });
  const rest = KIND_ORDER.filter((k) => k !== "experiment")
    .map((k) => others.get(k))
    .filter((g): g is RunGroup => !!g);
  for (const g of rest) g.runs.sort(byName);
  return [...declared, ...rest];
}

// ---- cells: what one run's header says, null when it says nothing ---------

/** "Run 3" from the declared run_index; null when none is declared. */
export function runLabel(identity: ReviewRunIdentity | undefined): string | null {
  const i = identity?.run_index;
  return i === null || i === undefined ? null : `Run ${i}`;
}

/** The sample: the sample sheet reference and the annotation's sample name. */
export function sampleLabel(identity: ReviewRunIdentity | undefined): string | null {
  const parts = [identity?.sample_ref, identity?.annotation?.sample].filter((v): v is string => !!v);
  const unique = parts.filter((v, i) => parts.indexOf(v) === i);
  return unique.length ? unique.join(" · ") : null;
}

/** The planned condition from the annotation. */
export function conditionLabel(identity: ReviewRunIdentity | undefined): string | null {
  return identity?.annotation?.condition ?? null;
}

/** Valid and invalid counts as recorded; null when the file records neither. */
export function recordsLabel(probe: ReviewRunProbe): string | null {
  if (probe.recording_file) return probe.total_valid === null ? null : `${probe.total_valid} frames`;
  if (probe.total_valid === null && probe.total_invalid === null) return null;
  return `${probe.total_valid ?? "–"} valid · ${probe.total_invalid ?? "–"} invalid`;
}

/** The run snapshot as the file carries it. */
export function snapshotLabel(identity: ReviewRunIdentity | undefined): { label: string; tone?: "warn" | "unknown" } {
  if (!identity) return { label: "Not read", tone: "unknown" };
  if (identity.snapshot_unreadable) return { label: "Snapshot unreadable", tone: "warn" };
  if (identity.has_snapshot) return { label: identity.snapshot_version === null ? "Snapshot" : `Snapshot v${identity.snapshot_version}` };
  return { label: "No snapshot · legacy file", tone: "unknown" };
}

/** The run accounting chip of a probed run. */
export function probeChip(probe: ReviewRunProbe): Chip {
  return completionChip(probe.accounting_completion !== null, probe.accounting_completion);
}

/** A recorded start as a date (YYYY-MM-DD), or null. */
export function recordedDate(ns: string | null | undefined): string | null {
  return recordedTime(ns)?.slice(0, 10) ?? null;
}

// ---- a group's summary line: distinct recorded values, never a guess -----

function distinct(values: (string | null | undefined)[]): string[] {
  const out: string[] = [];
  for (const v of values) if (v && !out.includes(v)) out.push(v);
  return out;
}

export interface GroupSummary {
  /** "3 runs · 3 samples" ("samples not recorded" when no run declares one). */
  runs: string;
  /** Earliest to newest recorded start date, or null. */
  dates: string | null;
  conditions: string | null;
  cores: string | null;
  calibrations: string | null;
  /** One chip for the group's run accounting. */
  status: Chip;
}

const SEVERITY: Record<string, number> = { blocked: 5, warn: 4, unknown: 3, info: 2, neutral: 1, confirmed: 0, unsupported: 1 };

export function groupSummary(group: RunGroup): GroupSummary {
  const probed = group.runs.map((r) => r.probe).filter((p): p is ReviewRunProbe => !!p && p.ok);
  const n = group.runs.length;
  const samples = distinct(probed.map((p) => p.identity.sample_ref));
  const runs = `${n} run${n === 1 ? "" : "s"} · ${samples.length ? `${samples.length} sample${samples.length === 1 ? "" : "s"}` : "samples not recorded"}`;
  const dates = distinct(
    probed
      .map((p) => p.start_time_ns)
      .filter((ns): ns is string => !!ns && /^\d+$/.test(ns) && !/^0+$/.test(ns))
      .sort((a, b) => (BigInt(a) < BigInt(b) ? -1 : BigInt(a) > BigInt(b) ? 1 : 0))
      .map((ns) => recordedDate(ns)),
  );
  const chips = probed.map(probeChip);
  let status: Chip = { tone: "unknown", label: "Not read" };
  if (chips.length) {
    const worst = chips.reduce((w, c) => (SEVERITY[c.tone] > SEVERITY[w.tone] ? c : w), chips[0]);
    const same = chips.filter((c) => c.label === worst.label).length;
    status = same === chips.length ? worst : { tone: worst.tone, label: `${same} of ${chips.length} runs · ${worst.label}` };
  }
  return {
    runs,
    dates: dates.length === 0 ? null : dates.length === 1 ? dates[0] : `${dates[0]} – ${dates[dates.length - 1]}`,
    conditions: distinct(probed.map((p) => p.identity.annotation?.condition)).join(" · ") || null,
    cores: distinct(probed.map((p) => p.core_version)).join(" · ") || null,
    calibrations: distinct(probed.map((p) => p.identity.calibration_record_id)).join(" · ") || null,
    status,
  };
}
