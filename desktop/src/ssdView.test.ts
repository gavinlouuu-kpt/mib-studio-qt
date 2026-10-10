import { describe, expect, it } from "vitest";
import {
  completionView, defaultRunName, emptyRunsText, formatBytes, formatDuration, hoursAtTypicalRate, recoveryNotice, runDurationSeconds, runRow, startedText, stripView,
  type SsdRun, type SsdStatus,
} from "./ssdView";

const status = (over: Partial<SsdStatus> = {}): SsdStatus => ({
  configured: true, state: "READY", usable: true, active: false, reason: "", last_error: "OK",
  raw_bytes: 213_909_504, free_bytes: 150_500_000_000, runs: 1, next_run_id: 2, table_entries: 256, recovered_runs: 0, recovered_ids: [], skipped_bad_entries: 0, open_run: null,
  ...over,
});

const run = (over: Partial<SsdRun> = {}): SsdRun => ({
  id: 1, open: false, deleted: false, incomplete: false, start_unix_ms: 1791530000000, wall_source: 1, client_tag: 7, filter: 2, sampler_n: 10, reason: 0,
  counts_unknown: false, size_bytes: 62_390_272, recoveries: 0, first_frame_id: 1000, last_frame_id: 50999, tick_hz: 100_000_000, first_ticks: 0, last_ticks: 999_980_000,
  seen: 50000, empty_filtered: 47150, invalid_not_sampled: 1800, passed: 1050, written: 1050, dropped: 0, failed: 0, ...over,
});

describe("formatting", () => {
  it("sizes and durations", () => {
    expect(formatBytes(512)).toBe("1 kB");
    expect(formatBytes(62_390_272)).toBe("62.4 MB");
    expect(formatBytes(150_500_000_000)).toBe("150.5 GB");
    expect(formatBytes(-1)).toBe("—");
    expect(formatDuration(9.99)).toBe("9.99 s");
    expect(formatDuration(59.94)).toBe("59.9 s");
    expect(formatDuration(3725)).toBe("1 h 02 min");
    expect(formatDuration(125)).toBe("2 min 05 s");
    expect(formatDuration(NaN)).toBe("—");
  });
  it("hours at a typical scene", () => {
    expect(hoursAtTypicalRate(100 * 59_392 * 3600)).toBeCloseTo(1, 6);
  });
  it("the run name and time", () => {
    const d = new Date(1791530000000);
    const p = (n: number) => String(n).padStart(2, "0");
    expect(defaultRunName(3, 1791530000000)).toBe(`run-3-${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`);
    expect(defaultRunName(3, 0)).toBe("run-3");
    expect(startedText(0)).toBe("—");
    expect(startedText(1791530000000)).toMatch(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/);
  });
  it("durations from ticks", () => {
    expect(runDurationSeconds({ first_ticks: 0, last_ticks: 999_980_000, tick_hz: 100_000_000 })).toBeCloseTo(9.9998, 4);
    expect(runDurationSeconds({ first_ticks: 5, last_ticks: 1, tick_hz: 100 })).toBeNull();
    expect(runDurationSeconds({ first_ticks: 0, last_ticks: 10, tick_hz: 0 })).toBeNull();
  });
});

describe("the storage strip", () => {
  it("says so when there is no SSD", () => {
    expect(stripView(status({ configured: false, state: "ABSENT", usable: false, free_bytes: undefined }))).toEqual({ tone: "neutral", text: "No SSD: nothing can be saved", detail: "" });
  });
  it("ready shows free space and the typical-scene hours with the caveat", () => {
    const v = stripView(status());
    expect(v.tone).toBe("ok");
    expect(v.text).toContain("SSD ready");
    expect(v.text).toContain("150.5 GB free");
    expect(v.text).toContain("about 7");
    expect(v.text).toContain("dense scenes use more");
  });
  it("starting, recovering, wedged and full states", () => {
    expect(stripView(status({ state: "INITIALISING", usable: false }))).toMatchObject({ tone: "info", text: "SSD starting" });
    expect(stripView(status({ state: "RECOVERING", usable: false, recovered_runs: 2 }))).toMatchObject({ tone: "info", text: "SSD recovering (2 runs closed)" });
    expect(stripView(status({ state: "RECOVERING", usable: false, recovered_runs: 0 })).text).toBe("SSD recovering");
    expect(stripView(status({ state: "WEDGED", usable: false, reason: "SSD not responding: pzrec status exited with 1: disk gone" }))).toMatchObject({
      tone: "bad", text: "SSD not responding: pzrec status exited with 1: disk gone",
    });
    expect(stripView(status({ state: "WEDGED", usable: false, reason: "" })).text).toContain("power-cycle");
    expect(stripView(status({ state: "RAW_FULL", usable: false, reason: "" })).text).toBe("SSD raw area full: export or delete old runs");
    expect(stripView(status({ state: "RUN_TABLE_FULL", usable: false, reason: "" })).text).toBe("SSD run table full: delete old runs");
    expect(stripView(status({ state: "UNFORMATTED", usable: false, reason: "" })).tone).toBe("warn");
  });
  it("recording: drops are neutral at zero, amber above, red above 1 % of the passed frames", () => {
    const open = (dropped: number, passed = 1000) => status({
      state: "RECORDING", usable: false, active: true,
      open_run: { id: 2, start_unix_ms: 1, client_tag: 0, wall_source: 1, seen: 5000, empty_filtered: 3900, invalid_not_sampled: 100, passed, written: passed - dropped, dropped, failed: 0,
                  bytes_written: 0, drain_kbps: 0, first_frame_id: 0, last_frame_id: 0 },
    });
    expect(stripView(open(0)).tone).toBe("ok");
    expect(stripView(open(5)).tone).toBe("warn");
    expect(stripView(open(11)).tone).toBe("bad");
    expect(stripView(open(0)).text).toBe("SSD recording run 2 · stored 1,000 · empty skipped 3,900 · dropped 0");
    expect(stripView(status({ state: "RECORDING", active: true, open_run: null })).text).toBe("SSD recording");
  });
  it("a stopping run shows the elapsed time and the stop bound", () => {
    const stopping = status({ state: "STOPPING", active: true, open_run: null });
    expect(stripView(stopping).text).toBe("SSD writing the last records");
    expect(stripView(stopping).detail).toContain("up to about 8 s");
    expect(stripView(stopping, 0.4).text).toBe("SSD writing the last records");
    expect(stripView(stopping, 3.7).text).toBe("SSD writing the last records · 3 s");
  });
  it("a recovery is never hidden once the SSD is ready again", () => {
    const v = stripView(status({ recovered_runs: 2, recovered_ids: [4, 5] }));
    expect(v.detail).toBe("Recovered after power loss: runs 4, 5 closed, true totals unknown.");
    expect(recoveryNotice(status({ recovered_runs: 1, recovered_ids: [9], skipped_bad_entries: 2 }))).toBe(
      "Recovered after power loss: run 9 closed, true totals unknown. 2 run-table entries were unreadable and skipped.");
    expect(stripView(status({ recovered_runs: 0, skipped_bad_entries: 2 })).detail).toBe("2 run-table entries were unreadable and skipped.");
    expect(recoveryNotice(status())).toBe("");
    expect(recoveryNotice(status({ skipped_bad_entries: 1 }))).toBe("1 run-table entry was unreadable and skipped.");
  });
});

describe("the experiment list", () => {
  it("a clean run", () => {
    const r = runRow(run());
    expect(r).toMatchObject({ id: 1, seen: "50,000", stored: "1,050", empty: "47,150", dropped: "0", completion: "Clean", tone: "ok", unknownTotals: false, clockUnsynced: false });
    expect(r.duration).toBe("10.0 s");
    expect(r.size).toBe("62.4 MB");
  });
  it("a run recovered at mount shows the sentence and no numbers (reason 7, #668 wording)", () => {
    const r = runRow(run({ reason: 7, counts_unknown: true, seen: 123, written: 77, dropped: 4 }));
    expect(r.completion).toBe("Recovered after power loss: true totals unknown");
    expect([r.seen, r.stored, r.empty, r.dropped, r.duration]).toEqual(["—", "—", "—", "—", "—"]);
    expect(r.unknownTotals).toBe(true);
    // reason 7 alone is enough even if the flag were missing
    expect(runRow(run({ reason: 7, counts_unknown: false })).seen).toBe("—");
  });
  it("a run whose totals do not add up shows no numbers and says so", () => {
    const r = runRow(run({ totals_inconsistent: true }));
    expect(r.completion).toBe("Totals inconsistent: numbers not shown");
    expect([r.seen, r.stored, r.empty, r.dropped, r.duration]).toEqual(["—", "—", "—", "—", "—"]);
    expect(r.unknownTotals).toBe(true);
    expect(completionView(run({ reason: 7, counts_unknown: true, totals_inconsistent: true })).text).toBe("Recovered after power loss: true totals unknown");
  });
  it("drops make a run partial, with the percent of the frames that passed the filter", () => {
    expect(completionView(run({ dropped: 21 }))).toEqual({ tone: "bad", text: "Partial: 21 dropped (2.0 %)" });
    expect(completionView(run({ dropped: 5 }))).toEqual({ tone: "warn", text: "Partial: 5 dropped (0.5 %)" });
    expect(completionView(run({ dropped: 1, passed: 100000 }))).toEqual({ tone: "warn", text: "Partial: 1 dropped (<0.1 %)" });
    expect(runRow(run({ dropped: 21 })).dropped).toBe("21 (2.0 %)");
  });
  it("end reasons", () => {
    expect(completionView(run({ reason: 6 })).text).toBe("Clean, ended at the length limit");
    expect(completionView(run({ reason: 9 })).text).toBe("Ended: the raw area was full");
    expect(completionView(run({ reason: 8 })).text).toBe("Aborted");
    expect(completionView(run({ reason: 1 }))).toEqual({ tone: "bad", text: "Failed: the drain faulted" });
    expect(completionView(run({ reason: 5 })).tone).toBe("bad");
    expect(completionView(run({ reason: 42 })).text).toBe("Failed (end reason 42)");
    expect(completionView(run({ open: true })).text).toBe("Recording");
  });
  it("the run being recorded takes its numbers from the live status, not the table", () => {
    const open = { id: 3, start_unix_ms: 1_000_000, client_tag: 0, wall_source: 1, seen: 5000, empty_filtered: 3900, invalid_not_sampled: 100, passed: 1000, written: 996, dropped: 4,
                   failed: 0, bytes_written: 59_392_000, drain_kbps: 0, first_frame_id: 0, last_frame_id: 0 };
    const table = run({ id: 3, open: true, start_unix_ms: 1_000_000, seen: 0, written: 0, passed: 0, size_bytes: 0, reason: 0, last_ticks: 0 });
    const live = runRow(table, { open, nowMs: 1_065_000 });
    expect(live).toMatchObject({ seen: "5,000", stored: "996", empty: "3,900", dropped: "4 (0.4 %)", size: "59.4 MB", completion: "Recording", duration: "65.0 s" });
    // without the live block (or for another run) the table's zeros are not shown as numbers
    expect(runRow(table)).toMatchObject({ seen: "—", stored: "—", dropped: "—", size: "—", duration: "—", completion: "Recording" });
    expect(runRow(table, { open: { ...open, id: 9 }, nowMs: 2_000_000 }).seen).toBe("—");
  });
  it("a board-clock run is marked, a deleted run is flagged", () => {
    expect(runRow(run({ wall_source: 0 })).clockUnsynced).toBe(true);
    expect(runRow(run({ deleted: true })).deleted).toBe(true);
  });
  it("what an empty list says", () => {
    expect(emptyRunsText({ ok: false, reason: "SSD run table unreadable: x", runs: [] }, null)).toBe("SSD run table unreadable: x");
    expect(emptyRunsText({ ok: true, reason: "", runs: [] }, status())).toBe("No recorded runs yet.");
    expect(emptyRunsText({ ok: false, reason: "", runs: [] }, null)).toBe("The SSD run table is not readable.");
    expect(emptyRunsText(null, status({ state: "ABSENT" }))).toBe("No SSD: nothing can be saved");
  });
});
