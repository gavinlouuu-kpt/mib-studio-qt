// @vitest-environment jsdom
// The density (KDE) layer in the Monitoring charts (epic #866 PR E2: MON-058, 059, 060, 061): the toggle beside
// the scatter, the colouring from a layer, and that nothing is redrawn without a reason.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import type { MonitoringRow, MonitoringSnapshot } from "../../../bridge";
import { MonitoringCharts } from "../../../components/MonitoringCharts";
import { recordingContext } from "../charts/recordingContext";
import { monitoringRowKey } from "../monitoringChartModel";
import { callsDigest } from "./densityFixtures";
import { LEVEL_COLOURS, type MonitoringDensityLayer } from "./densityModel";
import { CORE_CONTOUR_COLOUR, REFERENCE_CONTOUR_COLOUR } from "./drawDensity";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

// jsdom has no canvas and no layout: a recording context per canvas, every element 700 x 340.
type Recorder = ReturnType<typeof recordingContext>;
const draws = new Map<HTMLCanvasElement, Recorder[]>();
const lastDraw = (c: HTMLCanvasElement) => draws.get(c)![draws.get(c)!.length - 1];
const drawCount = (c: HTMLCanvasElement) => draws.get(c)?.length ?? 0;

beforeEach(() => {
  draws.clear();
  vi.mocked(invoke).mockReset();
  vi.mocked(invoke).mockResolvedValue(JSON.stringify({ curves: [], curve_source: "test" }));
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockImplementation(function (this: HTMLCanvasElement) {
    const rec = recordingContext();
    draws.set(this, [...(draws.get(this) ?? []), rec]);
    return rec.g as never;
  } as never);
  vi.spyOn(HTMLElement.prototype, "clientWidth", "get").mockReturnValue(700);
  vi.spyOn(HTMLElement.prototype, "clientHeight", "get").mockReturnValue(340);
});
afterEach(() => vi.restoreAllMocks());

let host: HTMLDivElement, root: Root;
beforeEach(() => {
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
});

const row = (over: Partial<MonitoringRow> = {}): MonitoringRow => ({
  frame_index: 1, timestamp_ns: 1, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: 0, centroid_x: 0, centroid_y: 0,
  area: 100, deformability: 0.2, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 1, start_generation: "1", capture_epoch: "2", ...over,
});
const snapshot = (rows: MonitoringRow[]): MonitoringSnapshot => ({
  valid: true, monitoring_active: true, valid_held: rows.length, invalid_held: 0, valid_appended: rows.length, invalid_appended: 0, capacity: 1000,
  latest_timestamp_ns: rows.length, rows,
});
const ROWS = [
  row({ frame_index: 10, area: 100, deformability: 0.2 }),
  row({ frame_index: 11, area: 500, deformability: 0.6, target_group: true }),
  row({ frame_index: 12, area: 800, deformability: 0.3 }), // newer than the estimate below
];
const NO: readonly [number, number][][] = [];
const layer = (over: Partial<MonitoringDensityLayer> = {}): MonitoringDensityLayer => ({
  on: true,
  byRow: new Map([[monitoringRowKey(ROWS[0]), 1], [monitoringRowKey(ROWS[1]), 0.3]]),
  contours: NO, reference: NO,
  toggle: { onChange: vi.fn(), tooltip: "Density (KDE) over 2 points", notice: "" },
  ...over,
});

// The same snapshot object for the same rows, as the studio's poll keeps one while nothing is new.
const snapshots = new WeakMap<MonitoringRow[], MonitoringSnapshot>();
async function mount(density: MonitoringDensityLayer | null | undefined, rows = ROWS) {
  if (!snapshots.has(rows)) snapshots.set(rows, snapshot(rows));
  await act(async () => root.render(<MonitoringCharts snapshot={snapshots.get(rows)!} density={density} />));
  return host.querySelector<HTMLCanvasElement>('[data-tour="monitoring-scatter"] canvas')!;
}
const digest = (c: HTMLCanvasElement) => callsDigest(lastDraw(c).calls);
const checkbox = () => host.querySelector<HTMLInputElement>('[data-tour="monitoring-density-toggle"]');

describe("the Density (KDE) toggle (MON-058)", () => {
  it("is a checkbox beside the scatter with the tour anchor and the tooltip, switching the layer on and off", async () => {
    const d = layer({ on: false });
    await mount(d);
    const box = checkbox()!;
    expect(box).not.toBeNull();
    expect(box.checked).toBe(false);
    expect(box.closest("label")!.textContent).toContain("Density (KDE)");
    expect(box.closest("label")!.title).toBe("Density (KDE) over 2 points");
    await act(async () => box.click());
    expect(d.toggle.onChange).toHaveBeenCalledWith(true);
    await mount(layer({ on: true, toggle: d.toggle }));
    expect(checkbox()!.checked).toBe(true);
    await act(async () => checkbox()!.click());
    expect(d.toggle.onChange).toHaveBeenLastCalledWith(false);
  });

  it("is not there without a layer (a standalone chart, or an older shell)", async () => {
    await mount(undefined);
    expect(checkbox()).toBeNull();
    await mount(null);
    expect(checkbox()).toBeNull();
  });

  it("shows the level key only while on, and says a refused command once", async () => {
    await mount(layer({ on: false }));
    expect(host.querySelector('[data-tour="monitoring-density-key"]')).toBeNull();
    await mount(layer({ on: true, toggle: { onChange: vi.fn(), tooltip: "t", notice: "Density (KDE) settings were not accepted by the backend (VIEWER_ONLY)." } }));
    expect(host.querySelector('[data-tour="monitoring-density-key"]')!.querySelectorAll("i")).toHaveLength(8);
    expect(host.querySelector('[data-tour="monitoring-density-notice"]')!.textContent).toContain("not accepted by the backend");
  });
});

describe("the plain chart when the layer is off (MON-058)", () => {
  it("draws the same canvas calls whether the layer is absent or present and off", async () => {
    const absent = digest(await mount(undefined));
    await act(async () => root.unmount());
    host.remove();
    host = document.createElement("div");
    document.body.append(host);
    root = createRoot(host);
    draws.clear();
    const off = await mount(layer({ on: false, contours: [[[0, 0], [100, 0.5]]], reference: [[[0, 0], [100, 0.5]]] }));
    expect(digest(off)).toBe(absent);
    // And no density colour, contour or square marker is in it.
    const rec = lastDraw(off);
    expect(rec.of("fill").some((f) => LEVEL_COLOURS.includes(String(f.fillStyle)))).toBe(false);
    expect(rec.of("rect").filter((c) => c.args[2] === 6 && c.args[3] === 6)).toHaveLength(0); // no square marker (the clip's rect is the plot's)
    expect(rec.of("stroke").some((s) => s.strokeStyle === CORE_CONTOUR_COLOUR || s.strokeStyle === REFERENCE_CONTOUR_COLOUR)).toBe(false);
  });
});

describe("the coloured chart", () => {
  it("colours each point by the estimate's density for its row, a newer row in the sparsest level, the target group as a square", async () => {
    const rec = lastDraw(await mount(layer()));
    const fills = rec.of("fill").map((f) => f.fillStyle);
    expect(fills).toContain(LEVEL_COLOURS[7]); // frame 10: density 1
    expect(fills).toContain(LEVEL_COLOURS[2]); // frame 11: density 0.3, a target-group cell
    expect(fills).toContain(LEVEL_COLOURS[0]); // frame 12: no density yet
    expect(rec.of("rect").filter((c) => c.args[2] === 6)).toHaveLength(1); // one target-group square (the legend's is 7 px)
  });

  it("draws the core contour and the reference from the layer", async () => {
    const rec = lastDraw(await mount(layer({ contours: [[[100, 0.1], [400, 0.5], [100, 0.1]]], reference: [[[200, 0.2], [600, 0.6], [200, 0.2]]] })));
    const strokes = rec.of("stroke").map((s) => s.strokeStyle);
    expect(strokes).toContain(CORE_CONTOUR_COLOUR);
    expect(strokes).toContain(REFERENCE_CONTOUR_COLOUR);
  });

  it("before the first estimate every point is in the sparsest level", async () => {
    const rec = lastDraw(await mount(layer({ byRow: null })));
    const fills = rec.of("fill").map((f) => f.fillStyle);
    expect(LEVEL_COLOURS.filter((c) => fills.includes(c))).toEqual([LEVEL_COLOURS[0], LEVEL_COLOURS[6]]); // level 0 and the legend's dots
  });
});

describe("redrawing only for a reason (#857: no work per poll beyond what is drawn)", () => {
  it("does not redraw the scatter when the layer's data, contours and reference are the same objects", async () => {
    const d = layer({ contours: [[[100, 0.1], [400, 0.5], [100, 0.1]]] });
    const canvas = await mount(d);
    const drawn = drawCount(canvas);
    // A new layer object with a new tooltip and toggle (a new poll, the same estimate), the same arrays.
    await mount({ ...d, toggle: { ...d.toggle, tooltip: "refreshed" } });
    expect(drawCount(canvas)).toBe(drawn);
  });

  it("redraws once for a new estimate, a new reference, or a changed snapshot", async () => {
    const d = layer();
    const canvas = await mount(d);
    const drawn = drawCount(canvas);
    await mount({ ...d, byRow: new Map([[monitoringRowKey(ROWS[0]), 0.5]]) });
    expect(drawCount(canvas)).toBe(drawn + 1);
    await mount({ ...d, byRow: new Map([[monitoringRowKey(ROWS[0]), 0.5]]), reference: [[[0, 0], [10, 0.1]]] });
    expect(drawCount(canvas)).toBeGreaterThan(drawn + 1);
  });

  it("makes no per-point element", async () => {
    const many = Array.from({ length: 1000 }, (_, i) => row({ frame_index: i, area: 10 + i, deformability: (i % 100) / 100 }));
    await mount(layer({ byRow: new Map(many.map((r, i) => [monitoringRowKey(r), (i % 8) / 8])) }), many);
    // The tree holds the same few nodes as for three points: the points are canvas paths.
    expect(host.querySelectorAll("*").length).toBeLessThan(200);
  });
});
