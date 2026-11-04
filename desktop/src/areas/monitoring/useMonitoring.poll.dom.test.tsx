// @vitest-environment jsdom
import { act, useEffect, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { MonitoringRow, MonitoringSnapshot } from "../../bridge";
import { MONITORING_FETCH_ROWS } from "./monitoringChartModel";
import { DEFAULT_MONITORING_SETTINGS, type MonitoringSettings } from "./monitoringSettings";

const calls = vi.hoisted(() => ({ snapshot: vi.fn(), trigger: vi.fn(), active: vi.fn() }));
vi.mock("../../bridge", async (original) => ({
  ...(await original<typeof import("../../bridge")>()),
  bridge: {
    monitoringSetActive: (...a: unknown[]) => calls.active(...a),
    fetchMonitoringSnapshot: (...a: unknown[]) => calls.snapshot(...a),
    fetchTriggerStatus: (...a: unknown[]) => calls.trigger(...a),
  },
}));
import { useMonitoringConfigRange, useMonitoringPolling } from "./useMonitoring";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => {
  vi.useFakeTimers();
  calls.snapshot.mockReset();
  calls.trigger.mockReset().mockResolvedValue({ valid: true });
  calls.active.mockReset().mockResolvedValue({ ok: true });
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
});

const row = (frame: number): MonitoringRow => ({
  frame_index: frame, timestamp_ns: frame, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: 0, centroid_x: 0, centroid_y: 0,
  area: 1, deformability: 0.1, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0,
});
const snap = (frames: number[]): MonitoringSnapshot => ({
  valid: true, monitoring_active: true, valid_held: frames.length, invalid_held: 0, valid_appended: frames.length, invalid_appended: 0, capacity: 1000,
  latest_timestamp_ns: frames[frames.length - 1] ?? 0, rows: frames.map(row),
});

describe("the Monitoring poll (MON-036)", () => {
  let latest: MonitoringSnapshot | null = null;
  const commits = vi.fn();
  function Poller({ visible }: { visible: boolean }) {
    const [monSnapshot, setMonSnapshot] = useState<MonitoringSnapshot | null>(null);
    latest = monSnapshot;
    useEffect(() => commits()); // after every commit: a bailed-out update does not run it
    useMonitoringPolling({
      ready: true, tab: visible ? "experiment" : "connect", expTab: "monitoring", setMonSnapshot, setTrigStatus: () => undefined,
      // PR D's deps (failure notices and poll health): not under test here, see useMonitoring.dom.test.tsx.
      append: () => undefined, caps: { trigger: true }, setMonitoringIssue: () => undefined,
      setSnapshotHealth: () => undefined, setTriggerHealth: () => undefined,
    } as never);
    return null;
  }
  const tick = () => act(async () => { await vi.advanceTimersByTimeAsync(500); });

  it("fetches the whole window, 1000 valid and 1000 invalid rows, every 500 ms while visible", async () => {
    calls.snapshot.mockResolvedValue(snap([1]));
    await act(async () => root.render(<Poller visible />));
    expect(calls.active).toHaveBeenCalledWith(true);
    expect(calls.snapshot).not.toHaveBeenCalled();
    await tick();
    expect(calls.snapshot).toHaveBeenCalledWith(MONITORING_FETCH_ROWS);
    expect(MONITORING_FETCH_ROWS).toBe(1015);
    await tick();
    expect(calls.snapshot).toHaveBeenCalledTimes(2);
  });

  it("does not poll while the Monitoring view is hidden, and tells the backend", async () => {
    await act(async () => root.render(<Poller visible={false} />));
    expect(calls.active).toHaveBeenCalledWith(false);
    await tick();
    await tick();
    expect(calls.snapshot).not.toHaveBeenCalled();
  });

  it("keeps the same snapshot object for a poll that finds nothing new, so nothing is committed or redrawn", async () => {
    calls.snapshot.mockImplementation(async () => snap([1, 2]));
    await act(async () => root.render(<Poller visible />));
    await tick();
    const first = latest;
    expect(first?.rows).toHaveLength(2);
    const before = commits.mock.calls.length;
    await tick();
    await tick();
    expect(calls.snapshot).toHaveBeenCalledTimes(3);
    expect(latest).toBe(first);
    expect(commits.mock.calls.length).toBe(before);
    calls.snapshot.mockImplementation(async () => snap([1, 2, 3]));
    await tick();
    expect(latest).not.toBe(first);
    expect(latest?.rows).toHaveLength(3);
  });
});

describe("the histogram's X range follows the config (MON-050)", () => {
  const config = (min: number, max: number) => JSON.stringify({ image_processing: { ring_ratio_min: min, ring_ratio_max: max } });
  let settings: MonitoringSettings = DEFAULT_MONITORING_SETTINGS;
  let setSettings: React.Dispatch<React.SetStateAction<MonitoringSettings>>;
  function Harness({ visible, baseline }: { visible: boolean; baseline: string }) {
    const [monSettings, setMonSettings] = useState<MonitoringSettings>(DEFAULT_MONITORING_SETTINGS);
    settings = monSettings;
    setSettings = setMonSettings;
    useMonitoringConfigRange({ monitoringVisible: visible, liveDraft: { baseline } as never, setMonSettings });
    return null;
  }
  const range = () => [settings.histogramXMin, settings.histogramXMax];

  it("takes ring_ratio_min/max when the Monitoring tab is shown, not before", async () => {
    await act(async () => root.render(<Harness visible={false} baseline={config(12, 30)} />));
    expect(range()).toEqual([15, 25]);
    await act(async () => root.render(<Harness visible baseline={config(12, 30)} />));
    expect(range()).toEqual([12, 30]);
    expect(settings.histogramEpoch).toBe(1);
  });

  it("follows an applied or reverted config, and overwrites a range set in the dialog each time the tab is shown", async () => {
    await act(async () => root.render(<Harness visible baseline={config(12, 30)} />));
    await act(async () => setSettings((s) => ({ ...s, histogramXMin: 1, histogramXMax: 2 })));
    expect(range()).toEqual([1, 2]);
    await act(async () => root.render(<Harness visible baseline={config(14, 28)} />));
    expect(range()).toEqual([14, 28]);
    await act(async () => setSettings((s) => ({ ...s, histogramXMin: 1, histogramXMax: 2 })));
    await act(async () => root.render(<Harness visible={false} baseline={config(14, 28)} />));
    await act(async () => root.render(<Harness visible baseline={config(14, 28)} />));
    expect(range()).toEqual([14, 28]);
  });

  it("keeps a dialog-set range while the tab stays shown and the config does not change", async () => {
    await act(async () => root.render(<Harness visible baseline={config(12, 30)} />));
    await act(async () => setSettings((s) => ({ ...s, histogramXMin: 1, histogramXMax: 2 })));
    await act(async () => root.render(<Harness visible baseline={config(12, 30)} />));
    expect(range()).toEqual([1, 2]);
  });

  it("ignores a config without a usable ring-ratio range, or with min >= max, like Qt", async () => {
    await act(async () => root.render(<Harness visible baseline="" />));
    expect(range()).toEqual([15, 25]);
    await act(async () => root.render(<Harness visible baseline={config(30, 12)} />));
    expect(range()).toEqual([15, 25]);
    await act(async () => root.render(<Harness visible baseline={"{}"} />));
    expect(range()).toEqual([15, 25]);
  });
});
