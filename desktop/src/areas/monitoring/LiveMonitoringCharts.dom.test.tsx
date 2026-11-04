// @vitest-environment jsdom
// The Monitoring charts with the cell and the gate (PR E1) as the panel mounts them, on the real state hook: a click selects a
// cell and the pane docked over the histogram shows it (pinned, pulled by identity), Previous / Next walk the plotted points,
// the window follows the selection, ✕ clears it, Clear Buffer clears it, and the gate's live count sits under the scatter. The
// pin lifecycle runs where the studio controller runs it (always mounted), so the panel can come and go under a selection.
import { act, useEffect } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import type { MonitoringRow, MonitoringSnapshot } from "../../bridge";
import { MONITORING_CELL_PIN_OUTCOMES as O } from "../../bridgeContract";
import type { MonitoringCell, MonitoringCellPinReply } from "../../monitoringCells";
import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";
import { recordingContext } from "./charts/recordingContext";
import { syntheticTable } from "./gate/syntheticLut";
import { LiveMonitoringCharts } from "./LiveMonitoringCharts";
import { useCellView } from "./clickView/useCellView";
import { monitoringRowKey } from "./monitoringChartModel";
import type { TuneValues } from "./tuneDraft";
import { useMonitoringState } from "./useMonitoring";
import { useSelectionReset } from "./useMonitoringView";

const bridge = vi.hoisted(() => ({ monitoringCellPin: vi.fn(), monitoringCellUnpin: vi.fn(), fetchMonitoringCellImage: vi.fn() }));
vi.mock("../../bridge", () => ({ bridge }));
vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
// A viewer is a browser session whose controller is another client (ViewerGate reads this).
const role = vi.hoisted(() => ({ viewerOnly: false }));
vi.mock("../../transport/useControlRole", () => ({
  useControlRole: () => ({ viewerOnly: role.viewerOnly, takeControl: async () => undefined }),
  isViewerOnly: () => role.viewerOnly,
}));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const draws = new Map<HTMLCanvasElement, ReturnType<typeof recordingContext>[]>();
const VP = { left: 64, top: 30, width: 466, height: 266 };
const pxOf = (area: number, d: number): [number, number] => [VP.left + (area / 1000) * VP.width, VP.top + (1 - d) * VP.height];
const imagePuts: Uint8ClampedArray[] = [];

beforeEach(() => {
  draws.clear();
  imagePuts.length = 0;
  vi.mocked(invoke).mockReset();
  vi.mocked(invoke).mockResolvedValue(JSON.stringify({ curves: [], curve_source: "test" }));
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockImplementation(function (this: HTMLCanvasElement) {
    const rec = recordingContext();
    draws.set(this, [...(draws.get(this) ?? []), rec]);
    const g = rec.g as unknown as Record<string, unknown>;
    g.createImageData = (w: number, h: number) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) });
    g.putImageData = (img: { data: Uint8ClampedArray }) => { imagePuts.push(Uint8ClampedArray.from(img.data)); };
    return rec.g as never;
  } as never);
  vi.spyOn(HTMLElement.prototype, "clientWidth", "get").mockReturnValue(700);
  vi.spyOn(HTMLElement.prototype, "clientHeight", "get").mockReturnValue(340);
});
afterEach(() => vi.restoreAllMocks());

const row = (frame: number, area: number, d: number, over: Partial<MonitoringRow> = {}): MonitoringRow => ({
  frame_index: frame, timestamp_ns: frame, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: 0, centroid_x: 0, centroid_y: 0,
  area, deformability: d, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 1, start_generation: "1", capture_epoch: "2", ...over,
});
const snapshot = (rows: MonitoringRow[]): MonitoringSnapshot => ({
  valid: true, monitoring_active: true, valid_held: rows.length, invalid_held: 0, valid_appended: rows.length, invalid_appended: 0, capacity: 1000,
  latest_timestamp_ns: rows.length, rows,
});
// Four cells on the plot: frame 10 at (100, 0.2), 11 at (300, 0.4), 12 at (500, 0.6), 13 at (700, 0.8).
const ROWS = [row(10, 100, 0.2), row(11, 300, 0.4), row(12, 500, 0.6, { target_group: true }), row(13, 700, 0.8)];

const W = 4, H = 3;
const cellOf = (r: MonitoringRow, over: Partial<MonitoringCell> = {}): MonitoringCell => ({
  start_generation: "1", capture_epoch: "2", frame_index: String(r.frame_index), object_id: r.object_id, timestamp_ns: "0", valid: true, target_group: r.target_group,
  object_count: 1, track_id: 0, centroid_x: 1, centroid_y: 1, bbox: { x: 0, y: 0, w: 2, h: 2 }, area: r.area, deformability: r.deformability, area_ratio: 1,
  ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 1, reasons: [],
  image: { width: W, height: H, crop_x: 0, crop_y: 0, source_width: 100, source_height: 50, has_mask: true, bytes: "12" }, no_image_reason: null, ...over,
});
const running = (over: Partial<TuneValues> = {}): TuneValues => ({
  areaEnabled: true, areaMin: 60, areaMax: 290, deformabilityEnabled: false, deformabilityMin: 0, deformabilityMax: 1,
  ringRatioEnabled: true, ringRatioMin: 15, ringRatioMax: 25, areaRatioEnabled: false, areaRatioMax: 1.5, borderEnabled: true,
  singleInnerContourEnabled: true, targetGroupEnabled: false, targetAreaMin: 200, targetAreaMax: 600, targetDeformabilityMin: 0.3,
  targetDeformabilityMax: 0.7, targetEmodulusEnabled: false, targetEmodulusMin: 0.1, targetEmodulusMax: 9, multiImageEnabled: false, multiImageCount: 1, ...over,
});

let host: HTMLDivElement, root: Root;
let studioNow: ReturnType<typeof useMonitoringState> & { ready: boolean; append: ReturnType<typeof vi.fn>; cellView: ReturnType<typeof useCellView> };
const append = vi.fn();

/** `show` false is the Monitoring panel unmounted (another sub-tab or stage): the state and the pin hook stay, like the controller's. */
function Harness({ snap, values, table, show = true }: { snap: MonitoringSnapshot | null; values: TuneValues | null; table?: ReturnType<typeof useMonitoringState>["emodulusTable"]; show?: boolean }) {
  const m = useMonitoringState();
  useSelectionReset(m);
  // What useStudioController does with it (a harness line: the ViewerGate source scan must not mistake it for the controller's).
  const pinState = useCellView({ selection: m.monSelection, ready: true, canPin: !role.viewerOnly, log: append });
  useEffect(() => { if (values) m.dispatchTune({ type: "external", values }); }, []); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { if (table) m.setEmodulusTable(table); }, [table]); // eslint-disable-line react-hooks/exhaustive-deps
  studioNow = { ...m, ready: true, append, cellView: pinState };
  return <StudioContext.Provider value={studioNow as unknown as Studio}>{show ? <LiveMonitoringCharts snapshot={snap} /> : null}</StudioContext.Provider>;
}
const render = (props: Parameters<typeof Harness>[0]) => act(async () => root.render(<Harness {...props} />));
const settle = () => act(async () => { for (let i = 0; i < 12; i++) await new Promise((r) => setTimeout(r, 0)); });
const fire = (el: Element, type: string, init: MouseEventInit = {}) => act(async () => { el.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, ...init })); });
const scatter = () => host.querySelector<HTMLCanvasElement>('[data-tour="monitoring-scatter"] canvas')!;
const clickPoint = async (area: number, d: number) => {
  const [x, y] = pxOf(area, d);
  await fire(scatter(), "pointerdown", { clientX: x, clientY: y, button: 0, buttons: 1 });
  await fire(scatter(), "pointerup", { clientX: x, clientY: y, button: 0, buttons: 0 });
  await settle();
};
const q = (sel: string) => host.querySelector<HTMLElement>(sel);
const pane = () => q('[data-tour="monitoring-cell-pane"]');
const title = () => q('[data-testid="cell-pane-title"]')?.textContent ?? null;
const click = (sel: string) => act(async () => q(sel)!.click());
const sent = () => [...bridge.monitoringCellPin.mock.calls.map((c) => `pin ${c[0].frame_index}`)];

let calls: string[];
beforeEach(() => {
  calls = [];
  role.viewerOnly = false;
  append.mockReset();
  for (const f of Object.values(bridge)) f.mockReset();
  bridge.monitoringCellPin.mockImplementation(async (k): Promise<MonitoringCellPinReply> => {
    calls.push(`pin ${k.frame_index}`);
    return { ok: true, outcome: O.Pinned, message: "pinned", cell: cellOf(ROWS.find((r) => r.frame_index === Number(k.frame_index)) ?? ROWS[0]) };
  });
  bridge.monitoringCellUnpin.mockImplementation(async (k) => { calls.push(`unpin ${k.frame_index}`); return { ok: true, message: "unpinned" }; });
  bridge.fetchMonitoringCellImage.mockImplementation(async (k, layer) => {
    calls.push(`${layer} ${k.frame_index}`);
    return { valid: true, width: W, height: H, data: new Uint8Array(W * H).fill(layer === "mask" ? 255 : 80) };
  });
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("click to view (MON-044, 046, 048)", () => {
  it("shows no pane until the first click, then the clicked cell in a pane docked in the histogram's card", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await settle();
    expect(pane()).toBeNull();
    expect(bridge.monitoringCellPin).not.toHaveBeenCalled();
    await clickPoint(300, 0.4);
    expect(pane()).not.toBeNull();
    expect(pane()!.closest(".mon-histogram-group")).not.toBeNull();
    expect(title()).toBe("Frame 11 · 300.0 µm² · deformability 0.4000");
    expect(calls).toEqual(["pin 11", "image 11", "mask 11"]);
    expect(pane()!.querySelector("canvas.mon-cell-canvas")).not.toBeNull();
    expect(studioNow.monSelection && monitoringRowKey(studioNow.monSelection)).toBe("1:2:11:0");
  });
  it("draws the mask over the cell only while the valid strip's Show Overlay is on", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    const plain = imagePuts[imagePuts.length - 1];
    expect(Array.from(plain.slice(0, 4))).toEqual([80, 80, 80, 255]); // the grey crop
    await act(async () => studioNow.setValidOverlay(true));
    const overlaid = imagePuts[imagePuts.length - 1];
    expect(Array.from(overlaid.slice(0, 4))).not.toEqual([80, 80, 80, 255]); // tinted and outlined
  });
  it("does not pull again for a poll that hands over the same snapshot content, or for a re-click on the same cell", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    await render({ snap: snapshot([...ROWS]), values: running() });
    await clickPoint(300, 0.4);
    await settle();
    expect(calls).toEqual(["pin 11", "image 11", "mask 11"]);
  });
  it("steps Previous / Next through the plotted points in buffer order, wrapping, releasing the pin it leaves", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(700, 0.8);
    expect(title()).toContain("Frame 13");
    await click('[data-tour="monitoring-cell-next"]');
    await settle();
    expect(title()).toContain("Frame 10"); // wrapped
    await click('[data-tour="monitoring-cell-prev"]');
    await settle();
    expect(title()).toContain("Frame 13");
    expect(calls.filter((c) => c.startsWith("pin") || c.startsWith("unpin"))).toEqual(["pin 13", "unpin 13", "pin 10", "unpin 10", "pin 13"]);
  });
  it("says 'no longer on the scatter' when the cell left the snapshot, and starts Previous / Next at an end", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    await render({ snap: snapshot([ROWS[0], ROWS[2], ROWS[3]]), values: running() });
    expect(title()).toBe("Frame 11 · 300.0 µm² · deformability 0.4000 · no longer on the scatter (left the monitoring buffer)");
    expect(pane()).not.toBeNull(); // the pane keeps its own copy
    await click('[data-tour="monitoring-cell-next"]');
    await settle();
    expect(title()).toContain("Frame 10"); // the oldest
    expect(title()).not.toContain("no longer");
  });
  it("closes with ✕: the pane goes, the selection is cleared and the pin released", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    await click('[data-tour="monitoring-cell-pane-close"]');
    await settle();
    expect(pane()).toBeNull();
    expect(studioNow.monSelection).toBeNull();
    expect(calls).toContain("unpin 11");
  });
  it("is cleared by Clear Buffer", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    await act(async () => studioNow.notifyMonitoringCleared());
    await settle();
    expect(pane()).toBeNull();
    expect(studioNow.monSelection).toBeNull();
    expect(studioNow.cellWindowOpen).toBe(false);
    expect(calls).toContain("unpin 11");
  });
  it("shows the backend's refusal as a plain message in the pane, with the measurements", async () => {
    bridge.monitoringCellPin.mockResolvedValue({ ok: false, outcome: O.CaptureMismatch, message: "capture 2 ended: the camera stopped", cell: null });
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    expect(pane()!.querySelector("[role='alert']")!.textContent).toBe("capture 2 ended: the camera stopped");
    expect(pane()!.textContent).toContain("deformability 0.4000");
    expect(pane()!.querySelector("canvas")).toBeNull();
  });
  it("shows the measurements and the reason when the pin kept no image", async () => {
    bridge.monitoringCellPin.mockImplementation(async () => ({
      ok: true, outcome: O.PinnedWithoutImage, message: "the frame left the ring before the pin", cell: cellOf(ROWS[1], { image: null, no_image_reason: "x" }),
    }));
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    expect(pane()!.textContent).toContain("No image: the frame left the ring before the pin");
    expect(pane()!.textContent).toContain("ring ratio 20.000");
  });
  it("has a tour anchor on every control", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    for (const t of ["monitoring-cell-pane", "monitoring-cell-pane-close", "monitoring-cell-prev", "monitoring-cell-next", "monitoring-cell-open-window"]) {
      expect(q(`[data-tour="${t}"]`), t).not.toBeNull();
    }
  });
});

describe("the pin outlives the Monitoring view (the controller runs the lifecycle)", () => {
  it("keeps the pin and the image when the panel unmounts and mounts again, and releases the pin once, on ✕", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    expect(calls).toEqual(["pin 11", "image 11", "mask 11"]);
    const shown = imagePuts[imagePuts.length - 1];
    // Another sub-tab or stage: the panel unmounts, the selection stays.
    await render({ snap: snapshot(ROWS), values: running(), show: false });
    await settle();
    expect(pane()).toBeNull();
    expect(studioNow.monSelection?.frame_index).toBe(11);
    expect(calls).toEqual(["pin 11", "image 11", "mask 11"]); // nothing unpinned, nothing pulled while hidden
    // Back, and the row has left the monitoring ring meanwhile: a pin now would fail, so the pane must not need one.
    bridge.monitoringCellPin.mockResolvedValue({ ok: false, outcome: O.NotFound, message: "the backend holds neither the row nor its measurements", cell: null });
    await render({ snap: snapshot([ROWS[0], ROWS[2], ROWS[3]]), values: running(), show: true });
    await settle();
    expect(title()).toContain("Frame 11");
    expect(title()).toContain("no longer on the scatter");
    expect(pane()!.querySelector("canvas.mon-cell-canvas")).not.toBeNull(); // the same image
    expect(Array.from(imagePuts[imagePuts.length - 1])).toEqual(Array.from(shown));
    expect(calls).toEqual(["pin 11", "image 11", "mask 11"]); // no second pin, no second pull, no unpin
    expect(bridge.monitoringCellPin).toHaveBeenCalledTimes(1);
    expect(bridge.monitoringCellUnpin).not.toHaveBeenCalled();
    // ✕: exactly one unpin.
    await click('[data-tour="monitoring-cell-pane-close"]');
    await settle();
    expect(pane()).toBeNull();
    expect(bridge.monitoringCellUnpin).toHaveBeenCalledTimes(1);
    expect(calls).toEqual(["pin 11", "image 11", "mask 11", "unpin 11"]);
  });
  it("releases the pin of a selection that is cleared while the panel is hidden (Clear Buffer), and on a real unmount of the studio", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    await render({ snap: snapshot(ROWS), values: running(), show: false });
    await act(async () => studioNow.notifyMonitoringCleared());
    await settle();
    expect(calls.filter((c) => c.startsWith("unpin"))).toEqual(["unpin 11"]);
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(500, 0.6);
    await act(async () => root.unmount());
    await settle();
    expect(calls.filter((c) => c.startsWith("unpin"))).toEqual(["unpin 11", "unpin 12"]);
    root = createRoot(host);
  });
});

describe("Open in window… (MON-047)", () => {
  it("opens a non-modal viewer that follows the selection while the charts keep updating", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    await click('[data-tour="monitoring-cell-open-window"]');
    const win = () => q('[data-tour="monitoring-cell-window"]');
    expect(win()).not.toBeNull();
    expect(win()!.getAttribute("aria-modal")).toBe("false");
    expect(q(".modal-backdrop")).toBeNull();
    expect(q('[data-testid="cell-window-title"]')!.textContent).toContain("Frame 11");
    await clickPoint(500, 0.6); // the charts are live: another click moves the selection and the window follows
    expect(q('[data-testid="cell-window-title"]')!.textContent).toContain("Frame 12");
    expect(title()).toContain("Frame 12");
    await render({ snap: snapshot([...ROWS, row(14, 800, 0.9)]), values: running() }); // a poll adds a cell: nothing closes
    expect(win()).not.toBeNull();
    expect(win()!.querySelector("canvas")).not.toBeNull();
  });
  it("steps the same selection from the window, and closing it hides it without clearing the selection", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    await click('[data-tour="monitoring-cell-open-window"]');
    await click('[data-tour="monitoring-cell-window-next"]');
    await settle();
    expect(title()).toContain("Frame 12");
    expect(q('[data-testid="cell-window-title"]')!.textContent).toContain("Frame 12");
    await click('[data-tour="monitoring-cell-window-close"]');
    expect(q('[data-tour="monitoring-cell-window"]')).toBeNull();
    expect(pane()).not.toBeNull();
    expect(studioNow.monSelection?.frame_index).toBe(12);
    await click('[data-tour="monitoring-cell-open-window"]'); // reused by the next open
    expect(q('[data-tour="monitoring-cell-window"]')).not.toBeNull();
  });
  it("is closed by the pane's ✕ together with the selection", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    await click('[data-tour="monitoring-cell-open-window"]');
    await click('[data-tour="monitoring-cell-pane-close"]');
    expect(q('[data-tour="monitoring-cell-window"]')).toBeNull();
    expect(studioNow.cellWindowOpen).toBe(false);
  });
});

describe("the live count under the scatter (MON-067)", () => {
  const label = () => q('[data-tour="monitoring-gate-label"]')?.textContent ?? null;
  it("counts the plotted valid cells in the draft's box, and is hidden while the gate is off", async () => {
    await render({ snap: snapshot(ROWS), values: running() });
    expect(label()).toBeNull();
    await act(async () => studioNow.setTuneField("targetGroupEnabled", true));
    // box area 200..600 x deformability 0.3..0.7: frames 11 (300, 0.4) and 12 (500, 0.6)
    expect(label()).toBe("Target group preview: 2 of 4 plotted valid cells (50.0%)");
    await act(async () => studioNow.setTuneField("targetAreaMax", 800));
    expect(label()).toBe("Target group preview: 2 of 4 plotted valid cells (50.0%)");
    await act(async () => studioNow.setTuneField("targetDeformabilityMax", 0.9));
    expect(label()).toBe("Target group preview: 3 of 4 plotted valid cells (75.0%)");
  });
  it("follows an unapplied edit of the E range with a table, from each row's stored modulus", async () => {
    const rows = ROWS.map((r, i) => ({ ...r, youngs_modulus: [1, 2, 3, 4][i] }));
    await render({ snap: snapshot(rows), values: running({ targetGroupEnabled: true, targetAreaMin: 0, targetAreaMax: 1000, targetDeformabilityMin: 0, targetDeformabilityMax: 1 }), table: { status: "loaded", table: syntheticTable() } });
    expect(label()).toBe("Target group preview: 4 of 4 plotted valid cells (100.0%)");
    await act(async () => studioNow.setTuneField("targetEmodulusEnabled", true));
    await act(async () => studioNow.setTuneField("targetEmodulusMin", 2));
    await act(async () => studioNow.setTuneField("targetEmodulusMax", 3));
    expect(label()).toBe("Target group preview: 2 of 4 plotted valid cells (50.0%)");
  });
  it("does not read the E values with no table: the count is the box, and a red notice says so", async () => {
    const rows = ROWS.map((r) => ({ ...r, youngs_modulus: 0 }));
    await render({ snap: snapshot(rows), values: running({ targetGroupEnabled: true, targetEmodulusEnabled: true, targetEmodulusMin: 5, targetEmodulusMax: 9 }), table: { status: "none" } });
    expect(label()).toBe("Target group preview: 2 of 4 plotted valid cells (50.0%)");
    expect(q('[data-tour="monitoring-gate-no-table"]')!.textContent).toBe("No E-modulus table loaded: the target group uses the box only");
  });
  it("says there are no plotted cells yet", async () => {
    await render({ snap: snapshot([]), values: running({ targetGroupEnabled: true }) });
    expect(label()).toBe("Target group preview: no plotted valid cells yet");
  });
});

describe("a client that does not control the instrument", () => {
  it("shows the cell it can still pull, without pinning", async () => {
    role.viewerOnly = true;
    await render({ snap: snapshot(ROWS), values: running() });
    await clickPoint(300, 0.4);
    expect(bridge.monitoringCellPin).not.toHaveBeenCalled();
    expect(calls).toEqual(["image 11", "mask 11"]);
    expect(sent()).toEqual([]);
  });
});
