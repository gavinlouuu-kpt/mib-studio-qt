// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { SsdRunsPanel } from "./SsdRunsPanel";
import { SsdStrip } from "./SsdStrip";
import { bridge } from "../bridge";
import type { SsdRun, SsdStatus } from "../ssdView";

let host: HTMLDivElement, root: Root;
const flush = async () => { await act(async () => { await Promise.resolve(); await Promise.resolve(); await Promise.resolve(); }); };

const status = (over: Partial<SsdStatus> = {}): SsdStatus => ({
  configured: true, state: "READY", usable: true, active: false, reason: "", last_error: "OK", free_bytes: 100_000_000_000, recovered_runs: 0, recovered_ids: [], skipped_bad_entries: 0, ...over,
});
const run = (over: Partial<SsdRun> = {}): SsdRun => ({
  id: 1, open: false, deleted: false, incomplete: false, start_unix_ms: 1791530000000, wall_source: 1, client_tag: 7, filter: 2, sampler_n: 10, reason: 0, counts_unknown: false,
  size_bytes: 62_390_272, recoveries: 0, first_frame_id: 1000, last_frame_id: 50999, tick_hz: 100_000_000, first_ticks: 0, last_ticks: 999_980_000,
  seen: 50000, empty_filtered: 47150, invalid_not_sampled: 1800, passed: 1050, written: 1050, dropped: 0, failed: 0, ...over,
});

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.restoreAllMocks(); });

describe("SSD runs panel (#667 S1)", () => {
  it("lists the runs; a run recovered at mount shows the sentence and no numbers", async () => {
    vi.spyOn(bridge, "fetchSsdRuns").mockResolvedValue({
      ok: true, reason: "", runs: [run(), run({ id: 2, reason: 7, counts_unknown: true, seen: 999, written: 5, dropped: 1, deleted: false })],
    });
    await act(async () => root.render(<SsdRunsPanel status={status({ recovered_runs: 1, recovered_ids: [2] })} active />));
    await flush();
    const r1 = host.querySelector('[data-testid="ssd-run-1"]')!.textContent!;
    expect(r1).toContain("50,000");
    expect(r1).toContain("Clean");
    const r2 = host.querySelector('[data-testid="ssd-run-2"]')!.textContent!;
    expect(r2).toContain("Recovered after power loss: true totals unknown");
    expect(r2).not.toContain("999");
  });

  it("says why the table is not readable, and does not read while hidden", async () => {
    const spy = vi.spyOn(bridge, "fetchSsdRuns").mockResolvedValue({ ok: false, reason: "SSD run table unreadable: pzrec runs exited with 1", runs: [] });
    await act(async () => root.render(<SsdRunsPanel status={status({ state: "WEDGED", usable: false })} active={false} />));
    await flush();
    expect(spy).not.toHaveBeenCalled();
    await act(async () => root.render(<SsdRunsPanel status={status({ state: "WEDGED", usable: false })} active />));
    await flush();
    expect(host.querySelector('[data-testid="ssd-runs-empty"]')!.textContent).toBe("SSD run table unreadable: pzrec runs exited with 1");
  });

  it("an empty table says so, a failed read shows the error", async () => {
    vi.spyOn(bridge, "fetchSsdRuns").mockResolvedValueOnce({ ok: true, reason: "", runs: [] }).mockRejectedValue(new Error("link down"));
    await act(async () => root.render(<SsdRunsPanel status={status()} active />));
    await flush();
    expect(host.querySelector('[data-testid="ssd-runs-empty"]')!.textContent).toBe("No recorded runs yet.");
  });
});

describe("SSD strip", () => {
  it("renders the state text and keeps a recovery note under it", async () => {
    await act(async () => root.render(<SsdStrip status={status({ recovered_runs: 2, recovered_ids: [4, 5] })} />));
    expect(host.querySelector('[data-testid="ssd-strip-text"]')!.textContent).toContain("SSD ready");
    expect(host.querySelector('[data-testid="ssd-strip-detail"]')!.textContent).toBe("Recovered after power loss: runs 4, 5 closed, true totals unknown.");
    await act(async () => root.render(<SsdStrip status={status({ state: "WEDGED", usable: false, reason: "SSD not responding: x" })} />));
    expect(host.querySelector('[data-testid="ssd-strip"]')!.className).toContain("bad");
    await act(async () => root.render(<SsdStrip status={null} />));
    expect(host.querySelector('[data-testid="ssd-strip"]')).toBeNull();
  });
});
