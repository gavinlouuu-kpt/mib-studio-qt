// Pure export helpers (plan 2026-10-01-standalone-review-app, PR 4): default
// names, the series-range prompt's parser, the remembered export directory
// and the batch summary. Mirrors the Qt HdfReviewTab rules; unit-tested in
// exports.test.ts.

/** Last path component (either separator: the paths come from the OS). */
export function baseName(path: string): string {
  const parts = path.split(/[\\/]/);
  return parts[parts.length - 1] ?? "";
}

/** Directory part ("" when there is none). */
export function dirName(path: string): string {
  const i = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return i <= 0 ? (i === 0 ? path.slice(0, 1) : "") : path.slice(0, i);
}

/** File name without its last extension (`run.h5` → `run`). */
export function stem(path: string): string {
  const b = baseName(path);
  const dot = b.lastIndexOf(".");
  return dot > 0 ? b.slice(0, dot) : b;
}

export function joinPath(dir: string, name: string): string {
  if (!dir) return name;
  const sep = dir.includes("\\") && !dir.includes("/") ? "\\" : "/";
  return dir.endsWith("/") || dir.endsWith("\\") ? dir + name : dir + sep + name;
}

/** Numeric suffixes above this are ignored: they are never stepped (a suffix
 * such as 9007199254740992 cannot be incremented in a JS number). */
export const MAX_NAME_SUFFIX = 999_999_999;
/** Candidates tried past the highest suffix before the timestamp fallback. */
export const MAX_NAME_PROBES = 10_000;

/** `YYYYMMDD-HHMMSS` (local time) for the fallback name. */
function nameStamp(now: Date): string {
  const p = (n: number) => String(n).padStart(2, "0");
  return `${now.getFullYear()}${p(now.getMonth() + 1)}${p(now.getDate())}-${p(now.getHours())}${p(now.getMinutes())}${p(now.getSeconds())}`;
}

/** HdfExportService::nextAvailableName: `first` when free, else
 * `prefix + (max numeric suffix in use, at least 1) + 1 + suffix`.
 * Conservative for the proposal (#735 review): a name counts as taken when
 * any existing name matches it ignoring case (Windows and macOS volumes are
 * case-insensitive), suffixes above MAX_NAME_SUFFIX are ignored, and the
 * search is bounded; past it, `prefix + YYYYMMDD-HHMMSS + suffix` (then
 * `-2`, `-3`, …). The backend refuses an existing file in any case. */
export function nextAvailableName(
  names: readonly string[],
  first: string,
  prefix: string,
  suffix: string,
  now: Date = new Date(),
): string {
  const taken = new Set(names.map((n) => n.toLowerCase()));
  const free = (name: string) => !taken.has(name.toLowerCase());
  if (free(first)) return first;
  const lowPrefix = prefix.toLowerCase();
  const lowSuffix = suffix.toLowerCase();
  let max = 1;
  for (const n of taken) {
    if (n.length <= lowPrefix.length + lowSuffix.length || !n.startsWith(lowPrefix) || !n.endsWith(lowSuffix)) continue;
    const digits = n.slice(lowPrefix.length, n.length - lowSuffix.length);
    if (!/^\d{1,9}$/.test(digits)) continue; // at most MAX_NAME_SUFFIX
    max = Math.max(max, Number(digits));
  }
  for (let c = max + 1, probes = 0; c <= MAX_NAME_SUFFIX && probes < MAX_NAME_PROBES; c++, probes++) {
    const name = `${prefix}${c}${suffix}`;
    if (free(name)) return name;
  }
  const stamp = `${prefix}${nameStamp(now)}`;
  let name = `${stamp}${suffix}`;
  for (let k = 2; !free(name) && k < MAX_NAME_PROBES; k++) name = `${stamp}-${k}${suffix}`;
  return name;
}

/** `<basename>_metrics.csv`, then `_2`, `_3`, … (the Qt default). */
export function defaultMetricsName(sourcePath: string, existing: readonly string[]): string {
  const b = stem(sourcePath);
  return nextAvailableName(existing, `${b}_metrics.csv`, `${b}_metrics_`, ".csv");
}

/** Regenerate masks never replaces a file (#512 S8, D6): `<base>_remasked.h5`
 * while free, else `_2`, `_3`, … one past the highest suffix in `existing`. */
export function defaultRegenerateName(base: string, existing: readonly string[], now: Date = new Date()): string {
  return nextAvailableName(existing, `${base}_remasked.h5`, `${base}_remasked_`, ".h5", now);
}

// ---- series-range prompt (Export All) ---------------------------------------

export type SeriesChoice =
  | { kind: "all" }
  | { kind: "skip" }
  | { kind: "range"; start: number; end: number }; // 0-based, inclusive

/** Parse the prompt's custom range: 1-based `9-15` (also `9 – 15`, `9`),
 * against `count` images per record when known. */
export function parseSeriesRange(text: string, count?: number): { ok: true; choice: SeriesChoice } | { ok: false; error: string } {
  const m = /^\s*(\d+)\s*(?:[-–—]\s*(\d+))?\s*$/.exec(text);
  if (!m) return { ok: false, error: "Enter a range such as 9-15 (1-based)" };
  const a = Number(m[1]);
  const b = m[2] === undefined ? a : Number(m[2]);
  if (a < 1 || b < 1) return { ok: false, error: "Images are numbered from 1" };
  if (b < a) return { ok: false, error: "The end comes before the start" };
  if (count !== undefined && count > 0 && a > count) return { ok: false, error: `Each record has ${count} images` };
  const end = count !== undefined && count > 0 ? Math.min(b, count) : b;
  return { ok: true, choice: { kind: "range", start: a - 1, end: end - 1 } };
}

/** The bridge's SeriesRange for a prompt choice. */
export function seriesRangeFor(choice: SeriesChoice): { exportSeries: boolean; start: number; end: number | "" } {
  if (choice.kind === "skip") return { exportSeries: false, start: 0, end: "" };
  if (choice.kind === "all") return { exportSeries: true, start: 0, end: "" };
  return { exportSeries: true, start: choice.start, end: choice.end };
}

// ---- remembered export directory -------------------------------------------

export const LAST_EXPORT_DIR_KEY = "yofo.review.lastExportDir";

/** The Qt metricsExportDir(): last successful export dir, else the open
 * file's directory, else "" (the OS dialog's default). */
export function exportDir(openFile: string, storage: Pick<Storage, "getItem"> | null = safeStorage()): string {
  let last: string | null = null;
  try {
    last = storage?.getItem(LAST_EXPORT_DIR_KEY) ?? null;
  } catch {
    last = null;
  }
  return last || dirName(openFile);
}

export function rememberExportDir(dir: string, storage: Pick<Storage, "setItem"> | null = safeStorage()): void {
  if (!dir) return;
  try {
    storage?.setItem(LAST_EXPORT_DIR_KEY, dir);
  } catch {
    // Storage unavailable: remembered for nothing; the default still works.
  }
}

function safeStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

// ---- batch summary ------------------------------------------------------------

export interface BatchSummary {
  exported: number;
  total: number;
  failures: { file: string; reason: string }[];
}

/** Parse the batch job's terminal message
 * ("exported 2 of 3 file(s); failed: a.h5: why; b.h5: why"). */
export function parseBatchSummary(message: string): BatchSummary | null {
  const m = /^exported (\d+) of (\d+) file\(s\)(?:; failed: (.*))?$/s.exec(message.trim());
  if (!m) return null;
  const failures: { file: string; reason: string }[] = [];
  if (m[3]) {
    for (const part of m[3].split(/; (?=[^;:]+: )/)) {
      const i = part.indexOf(": ");
      failures.push(i < 0 ? { file: part, reason: "" } : { file: part.slice(0, i), reason: part.slice(i + 2) });
    }
  }
  return { exported: Number(m[1]), total: Number(m[2]), failures };
}

/** The dialog's one-line outcome for a batch. */
export function batchSummaryText(s: BatchSummary): string {
  if (s.failures.length === 0) return `Exported all ${s.total} file(s).`;
  return `Exported ${s.exported} of ${s.total} file(s); ${s.failures.length} failed.`;
}
