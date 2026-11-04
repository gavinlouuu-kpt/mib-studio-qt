// @vitest-environment jsdom
// The density (KDE) behaviour of the Monitoring tab (epic #866 PR E2): the settings push, the poll, Clear Buffer,
// the reference contour and the stored preferences, on the real state block of useMonitoringState().
import { act, useEffect } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const calls = vi.hoisted(() => ({ set: vi.fn(), request: vi.fn(), fetch: vi.fn(), reference: vi.fn(), open: vi.fn(), message: vi.fn() }));
vi.mock("../../../bridge", async (original) => ({
  ...(await original<typeof import("../../../bridge")>()),
  bridge: {
    monitoringDensitySetSettings: (...a: unknown[]) => calls.set(...a),
    monitoringDensityRequestUpdate: (...a: unknown[]) => calls.request(...a),
    fetchMonitoringDensity: (...a: unknown[]) => calls.fetch(...a),
    fetchKdeReferenceFromFile: (...a: unknown[]) => calls.reference(...a),
  },
}));
vi.mock("../../../transport/dialogs", () => ({
  open: (...a: unknown[]) => calls.open(...a), message: (...a: unknown[]) => calls.message(...a), confirm: vi.fn(), save: vi.fn(), ask: vi.fn(),
}));

import { monitoringRowKey } from "../monitoringChartModel";
import { useMonitoringState } from "../useMonitoring";
import { densityReply, LOOP, referenceFailed, referenceLoaded, unchangedReply } from "./densityFixtures";
import { DENSITY_PREFS_KEY } from "./densityPrefs";
import { useMonitoringDensity, FROM_FILE_BUSY_REASON } from "./useMonitoringDensity";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

function memoryStorage() {
  const saved = new Map<string, string>();
  return {
    getItem: (key: string) => saved.get(key) ?? null, setItem: (key: string, value: string) => void saved.set(key, value),
    removeItem: (key: string) => void saved.delete(key), clear: () => saved.clear(),
  };
}
let local: ReturnType<typeof memoryStorage>;

interface Props { ready?: boolean; visible?: boolean; viewerOnly?: boolean; expActive?: boolean }
let api: ReturnType<typeof useMonitoringDensity> & { state: ReturnType<typeof useMonitoringState> };
const appended: string[] = [];
const commits = vi.fn();
function Harness({ ready = true, visible = true, viewerOnly = false, expActive = false }: Props) {
  const state = useMonitoringState();
  const density = useMonitoringDensity({
    ...state, ready, append: (line: string) => void appended.push(line), monitoringVisible: visible, viewerOnly, expActive,
  } as never);
  api = { ...density, state };
  useEffect(() => commits());
  return null;
}

let host: HTMLDivElement, root: Root;
const render = (props: Props = {}) => act(async () => root.render(<Harness {...props} />));
const tick = (ms = 500) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const toggle = (on: boolean) => act(async () => api.state.setKdeEnabled(on));
const lastSet = () => calls.set.mock.calls[calls.set.mock.calls.length - 1][0] as Record<string, unknown>;

beforeEach(() => {
  vi.useFakeTimers();
  local = memoryStorage();
  Object.defineProperty(window, "localStorage", { configurable: true, value: local });
  Object.defineProperty(window, "sessionStorage", { configurable: true, value: memoryStorage() });
  appended.length = 0;
  commits.mockReset();
  for (const c of Object.values(calls)) c.mockReset();
  calls.set.mockResolvedValue({ ok: true, message: "", settings: {} });
  calls.request.mockResolvedValue({ ok: true, message: "" });
  calls.fetch.mockResolvedValue(unchangedReply());
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
});

describe("the settings push (MON-058, 083..085)", () => {
  it("sends enabled false while the toggle is off, then enabled true with every setting when it goes on", async () => {
    await render();
    expect(calls.set).toHaveBeenCalledTimes(1);
    expect(lastSet()).toEqual({ enabled: false, intervalMs: 2000, bandwidthFactor: 1, coreFraction: 0.9, x0: 0, x1: 1000, y0: 0, y1: 1 });
    await toggle(true);
    expect(calls.set).toHaveBeenCalledTimes(2);
    expect(lastSet()).toEqual({ enabled: true, intervalMs: 2000, bandwidthFactor: 1, coreFraction: 0.9, x0: 0, x1: 1000, y0: 0, y1: 1 });
  });

  it("sends enabled false when Monitoring is hidden and true again when it is shown", async () => {
    await render({ visible: true });
    await toggle(true);
    expect(lastSet().enabled).toBe(true);
    await render({ visible: false });
    expect(lastSet().enabled).toBe(false);
    await render({ visible: true });
    expect(lastSet().enabled).toBe(true);
  });

  it("sends nothing before the backend is ready", async () => {
    await render({ ready: false });
    await toggle(true);
    expect(calls.set).not.toHaveBeenCalled();
    await render({ ready: true });
    expect(calls.set).toHaveBeenCalledTimes(1);
    expect(lastSet().enabled).toBe(true);
  });

  it("sends the new settings when the dialog changes a number or the scatter's fixed axes (the core fraction as a fraction)", async () => {
    await render();
    await act(async () => api.state.setMonSettings((s) => ({ ...s, kdeIntervalMs: 5000, kdeBandwidthFactor: 2.5, kdeCorePercent: 75, scatterXMax: 2000, scatterYMax: 0.8 })));
    expect(lastSet()).toEqual({ enabled: false, intervalMs: 5000, bandwidthFactor: 2.5, coreFraction: 0.75, x0: 0, x1: 2000, y0: 0, y1: 0.8 });
  });

  it("does not send again for a render that changed none of them", async () => {
    await render();
    await toggle(true);
    const sent = calls.set.mock.calls.length;
    await render();
    await act(async () => api.state.setMonSettings((s) => ({ ...s, histogramBinWidth: 0.25, scatterEpoch: s.scatterEpoch + 1 })));
    expect(calls.set).toHaveBeenCalledTimes(sent);
  });

  it("a viewer sends nothing (a control command, one set per instrument) and gets the plain chart", async () => {
    await render({ ready: false });
    await toggle(true); // the preference was on before the backend answered
    calls.set.mockClear();
    await render({ viewerOnly: true });
    await tick(2000);
    expect(calls.set).not.toHaveBeenCalled();
    expect(calls.fetch).not.toHaveBeenCalled();
    expect(api.kdeLayer.on).toBe(false);
    expect(api.state.kdeEnabled).toBe(true); // the preference is kept for when control comes back
    await render({ viewerOnly: false });
    expect(api.kdeLayer.on).toBe(true);
    expect(lastSet().enabled).toBe(true);
  });

  it("says a refused push once and clears it when a later one is accepted", async () => {
    calls.set.mockResolvedValue({ ok: false, message: "backend is not initialized", settings: null });
    await render();
    await toggle(true);
    await act(async () => api.state.setMonSettings((s) => ({ ...s, kdeIntervalMs: 4000 })));
    expect(api.kdeLayer.toggle.notice).toContain("backend is not initialized");
    expect(appended.filter((l) => l.includes("backend is not initialized"))).toHaveLength(1); // one log line, not one per push
    calls.set.mockResolvedValue({ ok: true, message: "", settings: {} });
    await act(async () => api.state.setMonSettings((s) => ({ ...s, kdeIntervalMs: 6000 })));
    expect(api.kdeLayer.toggle.notice).toBe("");
  });

  it("stays quiet about a refused push while the toggle is off (the start-up push of 'off' is routine)", async () => {
    calls.set.mockResolvedValue({ ok: false, message: "backend is not initialized", settings: null });
    await render();
    expect(api.kdeLayer.toggle.notice).toBe("");
    expect(appended).toEqual([]);
    calls.set.mockRejectedValue(new Error("older backend: unknown command"));
    await act(async () => api.state.setMonSettings((s) => ({ ...s, kdeIntervalMs: 4000 })));
    expect(api.kdeLayer.toggle.notice).toBe("");
  });

  it("says a rejected push (VIEWER_ONLY from the server) the same way", async () => {
    calls.set.mockRejectedValue(new Error("VIEWER_ONLY: another client controls this instrument"));
    await render();
    await toggle(true);
    expect(api.kdeLayer.toggle.notice).toContain("VIEWER_ONLY");
  });
});

describe("the poll", () => {
  it("asks every 500 ms while on and shown, never before the toggle is on, one request at a time", async () => {
    await render();
    await tick(1500);
    expect(calls.fetch).not.toHaveBeenCalled();
    await toggle(true);
    expect(calls.fetch).toHaveBeenCalledTimes(1);
    expect(calls.fetch).toHaveBeenLastCalledWith(null);
    await tick();
    await tick();
    expect(calls.fetch).toHaveBeenCalledTimes(3);
    // A reply that has not come back yet: the next ticks send nothing.
    let release!: (v: unknown) => void;
    calls.fetch.mockImplementation(() => new Promise((resolve) => { release = resolve; }));
    await tick();
    await tick();
    await tick();
    expect(calls.fetch).toHaveBeenCalledTimes(4);
    release(unchangedReply());
    await tick();
    expect(calls.fetch).toHaveBeenCalledTimes(5);
  });

  it("does not poll while Monitoring is hidden", async () => {
    await toggle(true);
    await render({ visible: false });
    calls.fetch.mockClear();
    await tick(2000);
    expect(calls.fetch).not.toHaveBeenCalled();
  });

  it("sets nothing for an unchanged reply: no new state, no render", async () => {
    await render();
    await toggle(true);
    await tick();
    const before = commits.mock.calls.length;
    await tick();
    await tick();
    expect(calls.fetch.mock.calls.length).toBeGreaterThan(3);
    expect(commits.mock.calls.length).toBe(before);
  });

  it("takes a newer generation as the estimate, asks since it, and reads a result-less reply as no estimate", async () => {
    await render();
    await toggle(true);
    const row = { frame_index: 10, object_id: 0, start_generation: "1", capture_epoch: "2" };
    calls.fetch.mockResolvedValue(densityReply([{ frame: 10, density: 0.9 }], { generation: "8" }));
    await tick();
    expect(api.state.kdeEstimate?.generation).toBe("8");
    expect(api.kdeLayer.byRow?.get(monitoringRowKey(row))).toBe(0.9);
    expect(api.kdeLayer.contours).toHaveLength(1);
    expect(api.kdeLayer.toggle.tooltip).toContain("Density (KDE) over 1 points; bandwidth 12.3 μm² × 0.0123; computed in 21 ms; refreshed every 3 s");
    calls.fetch.mockResolvedValue(unchangedReply("8"));
    await tick();
    expect(calls.fetch).toHaveBeenLastCalledWith("8");
    calls.fetch.mockResolvedValue({ ...densityReply([], { generation: "9" }), result: null });
    await tick();
    expect(api.state.kdeEstimate).toMatchObject({ generation: "9", pointCount: 0 });
    expect(api.kdeLayer.byRow?.size).toBe(0);
    expect(api.kdeLayer.contours).toHaveLength(0);
  });

  it("ignores {valid: false} (the backend is not initialized)", async () => {
    calls.fetch.mockResolvedValue({ valid: false });
    await render();
    await toggle(true);
    await tick();
    expect(api.state.kdeEstimate).toBeNull();
    expect(api.kdeLayer.toggle.notice).toBe("");
  });

  it("drops the estimate when the toggle goes off or Monitoring is hidden, and asks afresh when it is on again", async () => {
    await render();
    await toggle(true);
    calls.fetch.mockResolvedValue(densityReply([{ frame: 10, density: 0.9 }]));
    await tick();
    expect(api.state.kdeEstimate).not.toBeNull();
    await toggle(false);
    expect(api.state.kdeEstimate).toBeNull();
    expect(api.kdeLayer.byRow).toBeNull();
    calls.fetch.mockResolvedValue(unchangedReply());
    await toggle(true);
    expect(calls.fetch).toHaveBeenLastCalledWith(null);
    calls.fetch.mockResolvedValue(densityReply([{ frame: 10, density: 0.9 }]));
    await tick();
    await render({ visible: false });
    expect(api.state.kdeEstimate).toBeNull();
  });

  it("says polls that keep failing once, after three in a row, and stops saying so when one succeeds", async () => {
    calls.fetch.mockRejectedValue(new Error("link down"));
    await render();
    await toggle(true);
    await tick();
    expect(api.kdeLayer.toggle.notice).toBe("");
    await tick();
    expect(api.kdeLayer.toggle.notice).toContain("link down");
    expect(api.kdeLayer.toggle.notice).toContain("3 attempts");
    const logged = appended.length;
    await tick();
    await tick();
    expect(appended.length).toBe(logged); // not again on every tick
    calls.fetch.mockResolvedValue(unchangedReply());
    await tick();
    expect(api.kdeLayer.toggle.notice).toBe("");
  });
});

describe("Clear Buffer (Qt clearKdeState)", () => {
  it("drops the densities and the contours, wakes the service and keeps a reference", async () => {
    await render();
    await toggle(true);
    calls.fetch.mockResolvedValue(densityReply([{ frame: 10, density: 0.9 }]));
    await tick();
    await act(async () => api.kdeReferenceControls.onPin());
    calls.fetch.mockResolvedValue(unchangedReply());
    await act(async () => api.state.notifyMonitoringCleared());
    expect(api.state.kdeEstimate).toBeNull();
    expect(api.kdeLayer.byRow).toBeNull();
    expect(calls.request).toHaveBeenCalledTimes(1);
    expect(api.state.kdeReference).not.toBeNull();
  });

  it("does not draw a reply that was in flight when the buffer was cleared", async () => {
    await render();
    await toggle(true);
    let release!: (v: unknown) => void;
    calls.fetch.mockImplementation(() => new Promise((resolve) => { release = resolve; }));
    await tick();
    await act(async () => api.state.notifyMonitoringCleared());
    release(densityReply([{ frame: 10, density: 0.9 }]));
    await tick(0);
    expect(api.state.kdeEstimate).toBeNull();
  });

  it("does not wake the service when KDE is off", async () => {
    await render();
    await act(async () => api.state.notifyMonitoringCleared());
    expect(calls.request).not.toHaveBeenCalled();
  });
});

describe("the reference contour (MON-061, 086)", () => {
  const withEstimate = async (props: Props = {}) => {
    await render(props);
    await toggle(true);
    calls.fetch.mockResolvedValue(densityReply([{ frame: 10, density: 0.9 }]));
    await tick();
  };

  it("Pin current needs KDE on and a contour, keeps the live loops as 'pinned HH:mm:ss' and forgets the file path", async () => {
    await render();
    expect(api.kdeReferenceControls.canPin).toBe(false);
    await toggle(true);
    expect(api.kdeReferenceControls.canPin).toBe(false); // no contour yet
    calls.fetch.mockResolvedValue(densityReply([{ frame: 10, density: 0.9 }]));
    await tick();
    expect(api.kdeReferenceControls.canPin).toBe(true);
    await act(async () => api.state.setKdeReferencePath("/data/older.h5"));
    await act(async () => api.kdeReferenceControls.onPin());
    expect(api.state.kdeReference?.label).toMatch(/^pinned \d\d:\d\d:\d\d$/);
    expect(api.state.kdeReference?.contours).toEqual(api.state.kdeEstimate?.contours);
    expect(api.state.kdeReferencePath).toBe("");
    expect(api.kdeLayer.reference).toHaveLength(1);
    expect(api.kdeLayer.toggle.tooltip).toContain("\nReference contour: pinned ");
    expect(api.kdeReferenceControls.canClear).toBe(true);
  });

  it("Clear drops the reference and the path", async () => {
    await withEstimate();
    await act(async () => api.kdeReferenceControls.onPin());
    await act(async () => api.state.setKdeReferencePath("/data/run7.h5"));
    await act(async () => api.kdeReferenceControls.onClear());
    expect(api.state.kdeReference).toBeNull();
    expect(api.state.kdeReferencePath).toBe("");
    expect(api.kdeLayer.reference).toHaveLength(0);
    expect(api.kdeReferenceControls.canClear).toBe(false);
  });

  it("is kept through new estimates and drawn only while KDE is on", async () => {
    await withEstimate();
    await act(async () => api.kdeReferenceControls.onPin());
    calls.fetch.mockResolvedValue(densityReply([{ frame: 11, density: 0.5 }], { generation: "9" }));
    await tick();
    expect(api.kdeLayer.reference).toHaveLength(1);
    await toggle(false);
    expect(api.state.kdeReference).not.toBeNull();
    expect(api.kdeLayer.reference).toHaveLength(0);
  });

  it("From file… picks an HDF5 file, loads its contour with Qt's label and remembers the path", async () => {
    calls.open.mockResolvedValue("/data/run7.h5");
    calls.reference.mockResolvedValue(referenceLoaded());
    await render();
    await act(async () => api.kdeReferenceControls.onFromFile());
    expect(calls.open).toHaveBeenCalledWith(expect.objectContaining({ title: "Reference core contour from experiment", multiple: false, filters: [{ name: "Experiment files", extensions: ["h5", "hdf5"] }] }));
    expect(calls.reference).toHaveBeenCalledWith("/data/run7.h5");
    expect(api.state.kdeReference).toEqual({ label: "run7.h5, full-run 90%", contours: [LOOP] });
    expect(api.state.kdeReferencePath).toBe("/data/run7.h5");
    expect(api.kdeReferenceControls.label).toBe("run7.h5, full-run 90%");
    expect(calls.message).not.toHaveBeenCalled();
  });

  it("From file… starts the picker in the remembered file's folder, and does nothing when it is cancelled", async () => {
    calls.open.mockResolvedValue(null);
    await render();
    await act(async () => api.state.setKdeReferencePath("/data/runs/run7.h5"));
    calls.reference.mockResolvedValue(referenceFailed("file not found", "/data/runs/run7.h5")); // the restore may run first
    await act(async () => api.kdeReferenceControls.onFromFile());
    expect(calls.open).toHaveBeenCalledWith(expect.objectContaining({ defaultPath: "/data/runs" }));
  });

  it("From file… shows Qt's error dialog with the reason and changes nothing when the read fails, a busy refusal included", async () => {
    await withEstimate();
    await act(async () => api.kdeReferenceControls.onPin());
    const before = api.state.kdeReference;
    calls.open.mockResolvedValue("/data/gone.h5");
    calls.reference.mockResolvedValue(referenceFailed("file not found", "/data/gone.h5"));
    await act(async () => api.kdeReferenceControls.onFromFile());
    expect(calls.message).toHaveBeenCalledWith("No reference contour loaded from\n/data/gone.h5\n\nfile not found", { title: "Reference contour", kind: "error" });
    expect(api.state.kdeReference).toBe(before);
    expect(api.state.kdeReferencePath).toBe("");
    calls.reference.mockResolvedValue(referenceFailed("an experiment is running or starting, or the instrument is busy (HDF5 files are read only while it is idle); try again when it is idle", "/data/run7.h5"));
    calls.open.mockResolvedValue("/data/run7.h5");
    await act(async () => api.kdeReferenceControls.onFromFile());
    expect(calls.message).toHaveBeenLastCalledWith(expect.stringContaining("try again when it is idle"), expect.anything());
    calls.reference.mockRejectedValue(new Error("connection lost"));
    await act(async () => api.kdeReferenceControls.onFromFile());
    expect(calls.message).toHaveBeenLastCalledWith("No reference contour loaded from\n/data/run7.h5\n\nconnection lost", expect.anything());
  });

  it("From file… is blocked, with the reason, while an experiment runs", async () => {
    await render({ expActive: true });
    expect(api.kdeReferenceControls.fromFileBlocked).toBe(FROM_FILE_BUSY_REASON);
    await render({ expActive: false });
    expect(api.kdeReferenceControls.fromFileBlocked).toBeUndefined();
  });
});

describe("the remembered reference at start-up (MON-062)", () => {
  const remember = (path: string) => local.setItem(DENSITY_PREFS_KEY, JSON.stringify({ version: 1, enabled: false, bandwidthFactor: 1, intervalMs: 2000, coreFraction: 0.9, referencePath: path }));

  it("reloads the file once the backend is ready", async () => {
    remember("/data/run7.h5");
    calls.reference.mockResolvedValue(referenceLoaded());
    await render({ ready: false });
    expect(calls.reference).not.toHaveBeenCalled();
    await render({ ready: true });
    expect(calls.reference).toHaveBeenCalledTimes(1);
    expect(calls.reference).toHaveBeenCalledWith("/data/run7.h5");
    expect(api.state.kdeReference?.label).toBe("run7.h5, full-run 90%");
    expect(api.state.kdeReferencePath).toBe("/data/run7.h5");
    await render();
    expect(calls.reference).toHaveBeenCalledTimes(1);
  });

  it("drops a path whose file is gone, with one log line", async () => {
    remember("/data/gone.h5");
    calls.reference.mockResolvedValue(referenceFailed("file not found", "/data/gone.h5"));
    await render();
    expect(api.state.kdeReferencePath).toBe("");
    expect(api.state.kdeReference).toBeNull();
    expect(appended.filter((l) => l.includes("/data/gone.h5"))).toHaveLength(1);
    expect(appended[0]).toContain("file not found");
    await render();
    expect(calls.reference).toHaveBeenCalledTimes(1);
    expect(JSON.parse(local.getItem(DENSITY_PREFS_KEY)!).referencePath).toBe("");
  });

  it("keeps the path when the instrument refuses the read because a run is active, and loads it when the run ends", async () => {
    remember("/data/run7.h5");
    calls.reference.mockResolvedValue(referenceLoaded());
    await render({ expActive: true });
    expect(calls.reference).not.toHaveBeenCalled(); // not asked while a run is active
    expect(api.state.kdeReferencePath).toBe("/data/run7.h5");
    // The run state turns out busy anyway (a review job): refused, kept.
    calls.reference.mockResolvedValueOnce(referenceFailed("a review job is running; the file can be read when it finishes"));
    await render({ expActive: false });
    expect(calls.reference).toHaveBeenCalledTimes(1);
    expect(api.state.kdeReferencePath).toBe("/data/run7.h5");
    expect(api.state.kdeReference).toBeNull();
    expect(appended.filter((l) => l.includes("not restored yet"))).toHaveLength(1);
    await render({ expActive: true });
    await render({ expActive: false }); // the next idle: loaded
    expect(calls.reference).toHaveBeenCalledTimes(2);
    expect(api.state.kdeReference?.label).toBe("run7.h5, full-run 90%");
    expect(appended.filter((l) => l.includes("not restored yet"))).toHaveLength(1);
  });

  it("keeps the path when the call itself fails (a lost connection)", async () => {
    remember("/data/run7.h5");
    calls.reference.mockRejectedValue(new Error("connection lost"));
    await render();
    expect(api.state.kdeReferencePath).toBe("/data/run7.h5");
    expect(appended.some((l) => l.includes("connection lost"))).toBe(true);
  });

  it("does not replace a reference the operator pinned or loaded while the read was out", async () => {
    remember("/data/run7.h5");
    let release!: (v: unknown) => void;
    calls.reference.mockImplementation(() => new Promise((resolve) => { release = resolve; }));
    await render();
    await act(async () => api.kdeReferenceControls.onClear());
    release(referenceLoaded());
    await tick(0);
    expect(api.state.kdeReference).toBeNull();
    expect(api.state.kdeReferencePath).toBe("");
  });
});

describe("the preferences (MON-063)", () => {
  it("are stored as one versioned record when the toggle, a number or the reference path changes, and not before", async () => {
    await render();
    expect(local.getItem(DENSITY_PREFS_KEY)).toBeNull();
    await toggle(true);
    expect(JSON.parse(local.getItem(DENSITY_PREFS_KEY)!)).toEqual({ version: 1, enabled: true, bandwidthFactor: 1, intervalMs: 2000, coreFraction: 0.9, referencePath: "" });
    await act(async () => api.state.setMonSettings((s) => ({ ...s, kdeIntervalMs: 4500, kdeBandwidthFactor: 0.6, kdeCorePercent: 55 })));
    await act(async () => api.state.setKdeReferencePath("/data/run7.h5"));
    expect(JSON.parse(local.getItem(DENSITY_PREFS_KEY)!)).toEqual({ version: 1, enabled: true, bandwidthFactor: 0.6, intervalMs: 4500, coreFraction: 0.55, referencePath: "/data/run7.h5" });
  });

  it("are what the next start begins with", async () => {
    await render();
    await toggle(true);
    await act(async () => api.state.setMonSettings((s) => ({ ...s, kdeIntervalMs: 4500, kdeBandwidthFactor: 0.6, kdeCorePercent: 55 })));
    await act(async () => root.unmount());
    root = createRoot(host);
    calls.set.mockClear();
    await render();
    expect(api.state.kdeEnabled).toBe(true);
    expect(api.state.monSettings).toMatchObject({ kdeIntervalMs: 4500, kdeBandwidthFactor: 0.6, kdeCorePercent: 55 });
    expect(lastSet()).toMatchObject({ enabled: true, intervalMs: 4500, bandwidthFactor: 0.6, coreFraction: 0.55 });
  });

  it("an obsolete record starts from the defaults", async () => {
    local.setItem(DENSITY_PREFS_KEY, JSON.stringify({ version: 0, enabled: true, bandwidthFactor: 3, intervalMs: 9000, coreFraction: 0.5, referencePath: "" }));
    await render();
    expect(api.state.kdeEnabled).toBe(false);
    expect(api.state.monSettings).toMatchObject({ kdeIntervalMs: 2000, kdeBandwidthFactor: 1, kdeCorePercent: 90 });
  });
});
