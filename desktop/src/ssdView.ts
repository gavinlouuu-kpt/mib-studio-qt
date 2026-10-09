// The SATA SSD record store in the UI (#667 S1, read only): pure view models over fetch_ssd_status and fetch_ssd_runs (ABI 35), unit tested like
// filesView.ts. The run table comes from pzrec (pz7035 tools/pzrec); nothing here estimates what the hardware reports. A run recovered at mount
// (reason 7, counts_unknown) shows no numbers, only the honest sentence.

export type SsdStateName =
  | "ABSENT" | "INITIALISING" | "RECOVERING" | "READY" | "RECORDING" | "STOPPING" | "WEDGED" | "RUN_TABLE_FULL" | "RAW_FULL" | "UNFORMATTED";

/** The short block of the instrument status (polled with it). */
export interface SsdBrief {
  configured: boolean;
  state: SsdStateName;
  usable: boolean;
  active: boolean;
  reason: string;
  last_error: string;
}

export interface SsdOpenRun {
  id: number;
  start_unix_ms: number;
  client_tag: number;
  wall_source: number;
  seen: number;
  empty_filtered: number;
  invalid_not_sampled: number;
  passed: number;
  written: number;
  dropped: number;
  failed: number;
  bytes_written: number;
  drain_kbps: number;
  first_frame_id: number;
  last_frame_id: number;
}

export interface SsdStatus extends SsdBrief {
  raw_bytes?: number;
  free_bytes?: number;
  min_run_bytes?: number;
  runs?: number;
  next_run_id?: number;
  table_entries?: number;
  recovered_runs?: number;
  recovered_ids?: number[];
  skipped_bad_entries?: number;
  open_run?: SsdOpenRun | null;
}

export interface SsdRun {
  id: number;
  open: boolean;
  deleted: boolean;
  incomplete: boolean;
  start_unix_ms: number;
  wall_source: number;
  client_tag: number;
  filter: number;
  sampler_n: number;
  reason: number;
  counts_unknown: boolean;
  size_bytes: number;
  recoveries: number;
  first_frame_id: number;
  last_frame_id: number;
  tick_hz: number;
  first_ticks: number;
  last_ticks: number;
  seen: number;
  empty_filtered: number;
  invalid_not_sampled: number;
  passed: number;
  written: number;
  dropped: number;
  failed: number;
}

export interface SsdRuns {
  ok: boolean;
  reason: string;
  runs: SsdRun[];
}

export type Tone = "ok" | "info" | "warn" | "bad" | "neutral";

/** A typical scene (the PC's: about 86 valid frames/s plus the sampled invalid ones) writes about this many 59,392-byte records per second. */
export const TYPICAL_RECORDS_PER_S = 100;
export const RECORD_BYTES = 59_392;

export function formatBytes(bytes: number): string {
  if (!(bytes >= 0) || !Number.isFinite(bytes)) return "—";
  if (bytes < 1e6) return `${Math.round(bytes / 1e3)} kB`;
  if (bytes < 1e9) return `${(bytes / 1e6).toFixed(bytes < 1e8 ? 1 : 0)} MB`;
  return `${(bytes / 1e9).toFixed(bytes < 1e10 ? 2 : 1)} GB`;
}

export function hoursAtTypicalRate(freeBytes: number): number {
  return freeBytes / (TYPICAL_RECORDS_PER_S * RECORD_BYTES) / 3600;
}

export function formatDuration(seconds: number): string {
  if (!(seconds >= 0) || !Number.isFinite(seconds)) return "—";
  if (seconds < 9.995) return `${seconds.toFixed(2)} s`;
  if (seconds < 99.95) return `${seconds.toFixed(1)} s`;
  const s = Math.round(seconds);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
  return h > 0 ? `${h} h ${String(m).padStart(2, "0")} min` : `${m} min ${String(r).padStart(2, "0")} s`;
}

/** The storage strip: what the SSD is doing and why, in the design's words (#667 section 4). */
export function stripView(s: SsdStatus): { tone: Tone; text: string; detail: string } {
  const recoveredNote = recoveryNotice(s);
  switch (s.state) {
    case "ABSENT":
      return { tone: "neutral", text: "No SSD: nothing can be saved", detail: "" };
    case "INITIALISING":
      return { tone: "info", text: "SSD starting", detail: "" };
    case "RECOVERING":
      return { tone: "info", text: (s.recovered_runs ?? 0) > 0 ? `SSD recovering (${s.recovered_runs} runs closed)` : "SSD recovering", detail: "" };
    case "READY": {
      const free = s.free_bytes ?? 0;
      const hours = hoursAtTypicalRate(free);
      const room = `${formatBytes(free)} free · about ${hours >= 10 ? Math.round(hours) : hours.toFixed(1)} h at a typical scene (dense scenes use more)`;
      return { tone: "ok", text: `SSD ready · ${room}`, detail: recoveredNote };
    }
    case "RECORDING": {
      const o = s.open_run;
      if (!o) return { tone: "info", text: "SSD recording", detail: "" };
      const passed = o.passed;
      const droppedPct = passed > 0 ? (100 * o.dropped) / passed : 0;
      const tone: Tone = o.dropped === 0 ? "ok" : droppedPct > 1 ? "bad" : "warn";
      return {
        tone,
        text: `SSD recording run ${o.id} · stored ${o.written.toLocaleString("en-US")} · empty skipped ${o.empty_filtered.toLocaleString("en-US")} · dropped ${o.dropped.toLocaleString("en-US")}`,
        detail: "",
      };
    }
    case "STOPPING":
      return { tone: "info", text: "SSD writing the last records", detail: "" };
    case "WEDGED":
      return { tone: "bad", text: s.reason || "SSD not responding. The drive is self-powered; power-cycle it.", detail: "" };
    case "RUN_TABLE_FULL":
      return { tone: "warn", text: s.reason || "SSD run table full: delete old runs", detail: recoveredNote };
    case "RAW_FULL":
      return { tone: "warn", text: s.reason || "SSD raw area full: export or delete old runs", detail: recoveredNote };
    case "UNFORMATTED":
      return { tone: "warn", text: s.reason || "SSD is not formatted for recording", detail: "" };
    default:
      return { tone: "bad", text: `SSD state ${String(s.state)}`, detail: "" };
  }
}

function recoveredRunsText(ids: number[], count: number): string {
  if (ids.length > 0 && ids.length <= 4) return `run${ids.length > 1 ? "s" : ""} ${ids.join(", ")}`;
  return `${count} run${count === 1 ? "" : "s"}`;
}

/** The completion of a finished run, from pzrec's end reason (0 clean, 1 drain fault, 2 stall, 3 length, 5 not provably stopped, 6 length limit, 7 recovered at mount, 8 aborted, 9 raw full). */
export function completionView(r: SsdRun): { tone: Tone; text: string } {
  if (r.open) return { tone: "info", text: "Recording" };
  if (r.reason === 7 || r.counts_unknown) return { tone: "warn", text: "Recovered after power loss: true totals unknown" };
  const pct = r.passed > 0 ? (100 * r.dropped) / r.passed : 0;
  const dropped = r.dropped > 0 ? `Partial: ${r.dropped.toLocaleString("en-US")} dropped (${pct < 0.1 ? "<0.1" : pct.toFixed(pct < 10 ? 1 : 0)} %)` : "";
  switch (r.reason) {
    case 0: return dropped ? { tone: pct > 1 ? "bad" : "warn", text: dropped } : { tone: "ok", text: "Clean" };
    case 6: return dropped ? { tone: "warn", text: `${dropped}, ended at the length limit` } : { tone: "ok", text: "Clean, ended at the length limit" };
    case 9: return dropped ? { tone: "warn", text: `${dropped}, ended: the raw area was full` } : { tone: "warn", text: "Ended: the raw area was full" };
    case 8: return { tone: "warn", text: dropped ? `Aborted · ${dropped}` : "Aborted" };
    case 1: return { tone: "bad", text: "Failed: the drain faulted" };
    case 2: return { tone: "bad", text: "Failed: the drain stalled" };
    case 3: return { tone: "bad", text: "Failed: length error" };
    case 5: return { tone: "bad", text: "Failed: the drain was not provably stopped" };
    default: return { tone: "bad", text: `Failed (end reason ${r.reason})` };
  }
}

export interface SsdRunRow {
  id: number;
  name: string;
  started: string;
  duration: string;
  seen: string;
  stored: string;
  empty: string;
  dropped: string;
  size: string;
  completion: string;
  tone: Tone;
  deleted: boolean;
  unknownTotals: boolean;
  /** The board's clock was not synced when the run started: the date may be wrong. */
  clockUnsynced: boolean;
}

const pad = (n: number) => String(n).padStart(2, "0");

export function startedText(unixMs: number): string {
  if (!(unixMs > 0)) return "—";
  const d = new Date(unixMs);
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

export function defaultRunName(id: number, unixMs: number): string {
  if (!(unixMs > 0)) return `run-${id}`;
  const d = new Date(unixMs);
  return `run-${id}-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
}

export function runDurationSeconds(r: Pick<SsdRun, "first_ticks" | "last_ticks" | "tick_hz">): number | null {
  if (!(r.tick_hz > 0) || r.last_ticks < r.first_ticks) return null;
  return (r.last_ticks - r.first_ticks) / r.tick_hz;
}

/** `live`: the open run's counters from the status (the table does not hold the run being recorded) and the wall clock for its elapsed time. */
export function runRow(r: SsdRun, live?: { open: SsdOpenRun | null | undefined; nowMs: number }): SsdRunRow {
  const open = r.open && live?.open && live.open.id === r.id ? live.open : null;
  const unknown = !open && (r.counts_unknown || r.reason === 7 || r.open);
  const seen = open ? open.seen : r.seen, written = open ? open.written : r.written, empty = open ? open.empty_filtered : r.empty_filtered;
  const droppedN = open ? open.dropped : r.dropped, passed = open ? open.passed : r.passed;
  const num = (n: number) => (unknown ? "—" : n.toLocaleString("en-US"));
  const completion = completionView(r);
  const pct = passed > 0 ? (100 * droppedN) / passed : 0;
  const duration = open ? (live!.nowMs > r.start_unix_ms ? (live!.nowMs - r.start_unix_ms) / 1000 : null) : unknown ? null : runDurationSeconds(r);
  return {
    id: r.id,
    name: defaultRunName(r.id, r.start_unix_ms),
    started: startedText(r.start_unix_ms),
    duration: duration === null ? "—" : formatDuration(duration),
    seen: num(seen),
    stored: num(written),
    empty: num(empty),
    dropped: unknown ? "—" : droppedN === 0 ? "0" : `${droppedN.toLocaleString("en-US")} (${pct < 0.1 ? "<0.1" : pct.toFixed(pct < 10 ? 1 : 0)} %)`,
    size: open ? formatBytes(open.bytes_written) : r.open ? "—" : formatBytes(r.size_bytes),
    completion: completion.text,
    tone: completion.tone,
    deleted: r.deleted,
    unknownTotals: !open && (r.counts_unknown || r.reason === 7),
    clockUnsynced: r.wall_source === 0,
  };
}

/** What the list says when it has no rows. */
export function emptyRunsText(reply: SsdRuns | null, status: SsdStatus | null): string {
  if (reply && !reply.ok) return reply.reason || "The SSD run table is not readable.";
  if (status && status.state === "ABSENT") return "No SSD: nothing can be saved";
  return "No recorded runs yet.";
}

/** The note after a mount-time recovery (R9), naming the runs it closed; empty when there was none. */
export function recoveryNotice(s: SsdStatus): string {
  const n = s.recovered_runs ?? 0;
  const skipped = s.skipped_bad_entries ?? 0;
  const parts: string[] = [];
  if (n > 0) parts.push(`Recovered after power loss: ${recoveredRunsText(s.recovered_ids ?? [], n)} closed, true totals unknown.`);
  if (skipped > 0) parts.push(`${skipped} run-table ${skipped === 1 ? "entry was" : "entries were"} unreadable and skipped.`);
  return parts.join(" ");
}
