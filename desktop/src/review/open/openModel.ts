// R1 Open (#717 B1, #269): the pure part of the experiment library view.
// What a folder holds, what the open run says about itself, and the words
// and tones for both. Nothing here reads a file or computes a statistic; a
// value the file does not record stays absent (null), never a guess.

import type { Fact, StateTone } from "@mib/ui";
import { RUN_COMPLETION_STATES } from "../../bridgeContract";
import { metricNumber } from "../../metricFormat";
import { baseName, dirName, joinPath } from "../exports/exportHelpers";
import type { ReviewInfo, ReviewRunAnnotation, ReviewRunIdentity } from "../reviewBridge";

/** A run file in a folder listing. */
export interface FolderRun {
  /** File name as listed. */
  name: string;
  /** Folder + name. */
  path: string;
}

const RUN_FILE = /\.(h5|hdf5)$/i;

/** The run files of a `review_list_dir` listing: `.h5` / `.hdf5`, without
 * hidden files (a regenerate's `.<name>.partial-…` work file starts with a
 * dot), in natural name order (run_2 before run_10). */
export function runFilesIn(folder: string, names: readonly string[]): FolderRun[] {
  return names
    .filter((n) => RUN_FILE.test(n) && !n.startsWith("."))
    .sort((a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: "base" }))
    .map((name) => ({ name, path: joinPath(folder, name) }));
}

/** A recorded wall-clock time (decimal ns string) as local date and time;
 * null when the file records none ("0", empty or not a number). */
export function recordedTime(ns: string | null | undefined): string | null {
  if (!ns || !/^\d+$/.test(ns) || /^0+$/.test(ns)) return null;
  const ms = Number(BigInt(ns) / 1_000_000n);
  const d = new Date(ms);
  if (Number.isNaN(d.getTime())) return null;
  const pad = (v: number) => String(v).padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

export interface Chip {
  tone: StateTone;
  label: string;
}

/** The run accounting (#367) as one chip: what the file recorded about how
 * the run ended. A file without accounting is Unknown, never Complete. */
export function completionChip(hasAccounting: boolean, completion: number | null | undefined): Chip {
  if (!hasAccounting || completion === null || completion === undefined) return { tone: "unknown", label: "Accounting · not recorded" };
  switch (completion) {
    case RUN_COMPLETION_STATES.Complete:
      return { tone: "confirmed", label: "Complete" };
    case RUN_COMPLETION_STATES.IntentionallyPartial:
      return { tone: "info", label: "Partial · declared" };
    case RUN_COMPLETION_STATES.IncompleteLoss:
      return { tone: "warn", label: "Partial · loss" };
    case RUN_COMPLETION_STATES.Failed:
      return { tone: "blocked", label: "Failed" };
    default:
      return { tone: "unknown", label: "Unknown · accounting unreadable" };
  }
}

/** The planned annotation's members other than sample and condition (which
 * have their own facts), as one line; null when it declares none of them. */
function annotationExtras(a: ReviewRunAnnotation): string | null {
  const parts = [
    a.experimental_unit ? `unit ${a.experimental_unit}` : null,
    a.concentration ? `${a.concentration.value} ${a.concentration.unit}` : null,
    a.channel ? `channel ${a.channel}` : null,
    a.medium ? `medium ${a.medium}` : null,
    a.temperature_c !== null ? `${a.temperature_c} °C` : null,
    a.notes,
    ...a.free.map((kv) => `${kv.key} ${kv.value}`),
  ].filter((v): v is string => !!v);
  return parts.length ? parts.join(" · ") : null;
}

/** The open run's identity (review ABI 2, `fetch_review_info.identity`): what
 * its run snapshot and planned annotation declare. A legacy file says so
 * once (Run snapshot) and every other member is null. */
export function identityFacts(identity: ReviewRunIdentity | undefined): Fact[] {
  const id = identity;
  const snapshot: Fact = !id
    ? { label: "Run snapshot", value: null, tone: "unknown" }
    : id.snapshot_unreadable
      ? { label: "Run snapshot", value: "Present but unreadable", tone: "warn" }
      : id.has_snapshot
        ? { label: "Run snapshot", value: id.snapshot_version === null ? "Recorded" : `Version ${id.snapshot_version}` }
        : { label: "Run snapshot", value: "Not recorded · legacy file", tone: "unknown" };
  const operator = id?.operator ? [id.operator.name, id.operator.id].filter(Boolean).join(" · ") || null : null;
  const sample = [id?.sample_ref, id?.annotation?.sample].filter((v): v is string => !!v);
  return [
    snapshot,
    { label: "Experiment", value: id?.experiment_id ?? null },
    { label: "Run", value: id?.run_index === null || id?.run_index === undefined ? null : `Run ${id.run_index}` },
    { label: "Sample", value: sample.filter((v, i) => sample.indexOf(v) === i).join(" · ") || null },
    { label: "Condition", value: id?.annotation?.condition ?? null },
    {
      label: "Annotation",
      value: id?.annotation_unreadable ? "Present but unreadable" : id?.annotation ? annotationExtras(id.annotation) : null,
      tone: id?.annotation_unreadable ? "warn" : undefined,
    },
    { label: "Calibration record", value: id?.calibration_record_id ?? null },
    { label: "Operator (selected at Start)", value: operator },
  ];
}

/** What the open run says about itself, for the Open run card. Each value
 * the file does not record is null (the FactList shows "Not reported"). */
export function openRunFacts(info: ReviewInfo): Fact[] {
  const recording = info.recording_file;
  const facts: Fact[] = [
    { label: "File", value: baseName(info.file_path) || null },
    { label: "Folder", value: dirName(info.file_path) || null },
    { label: "Kind", value: recording ? "Raw recording (images only)" : "Experiment run" },
    ...identityFacts(info.identity),
    {
      label: "Records",
      value: recording ? `${info.total_valid} frames` : `${info.total_valid} valid · ${info.total_invalid} invalid`,
    },
    { label: "Started", value: recordedTime(info.start_time_ns) },
    { label: "Ended", value: recordedTime(info.end_time_ns) },
    {
      label: "Pixel → µm",
      value: `${metricNumber(info.pixel_to_micron, 4)} µm/px · ${info.pixel_to_micron_from_file ? "recorded in the file" : "fallback (the file records none)"}`,
      tone: info.pixel_to_micron_from_file ? undefined : "warn",
    },
    {
      label: "Processing core",
      value: info.has_core_identity ? [info.core_version, info.core_source].filter(Boolean).join(" · ") || null : null,
    },
    {
      label: "Run accounting",
      value: info.has_accounting ? info.accounting_summary.replace(/^\s*·\s*/, "") || null : "Not recorded · legacy file",
      tone: info.has_accounting ? undefined : "unknown",
    },
  ];
  return facts;
}
