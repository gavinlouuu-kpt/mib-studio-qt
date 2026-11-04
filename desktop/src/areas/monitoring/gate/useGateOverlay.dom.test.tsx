// @vitest-environment jsdom
// The gate on the real scatter (MON-064..066, 068): the overlay drawn from the tune draft, the hover feedback, a handle dragged
// into the draft through the panel's own edit path (clamped, rounded, min <= max), the rules that keep a handle shut, and the
// gestures that must stay what they were (a pan away from the handles, a click that still selects).
import { act, useCallback, useReducer } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { MonitoringRow } from "../../../bridge";
import type { Viewport } from "../../../review/charts/chartMath";
import { recordingContext } from "../charts/recordingContext";
import { MonitoringScatter } from "../charts/MonitoringScatter";
import { EMODULUS_TABLE_UNKNOWN, type EmodulusTableState } from "../emodulusLut";
import { monitoringChartData } from "../monitoringChartModel";
import { DEFAULT_MONITORING_SETTINGS } from "../monitoringSettings";
import { initialTuneState, tuneReducer, type TuneAction, type TuneFieldId, type TuneState, type TuneValues } from "../tuneDraft";
import { GATE_ACTIVE, GATE_WARN, NO_TABLE_NOTICE } from "./gateOverlay";
import { useGateOverlay } from "./useGateOverlay";
import { handleAnchor, isoContour, NO_CONTOURS, toDataPoint, toPixel } from "./targetGate";
import { syntheticTable } from "./syntheticLut";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

// jsdom has no canvas and no layout: every canvas records its calls, every element measures 700 x 340 CSS px, so the plot is
// left 64, top 30, 466 x 266 px over the default axes (area 0..1000, deformability 0..1).
const draws = new Map<HTMLCanvasElement, ReturnType<typeof recordingContext>[]>();
const lastDraw = (c: HTMLCanvasElement) => draws.get(c)![draws.get(c)!.length - 1];
const VP: Viewport = { x0: 0, x1: 1000, y0: 0, y1: 1, left: 64, top: 30, width: 466, height: 266 };
const pxOf = (area: number, d: number): [number, number] => {
  const [x, y] = toPixel(VP, area, d);
  return [x, y];
};
beforeEach(() => {
  draws.clear();
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockImplementation(function (this: HTMLCanvasElement) {
    const rec = recordingContext();
    draws.set(this, [...(draws.get(this) ?? []), rec]);
    return rec.g as never;
  } as never);
  vi.spyOn(HTMLElement.prototype, "clientWidth", "get").mockReturnValue(700);
  vi.spyOn(HTMLElement.prototype, "clientHeight", "get").mockReturnValue(340);
});
afterEach(() => vi.restoreAllMocks());

const running = (over: Partial<TuneValues> = {}): TuneValues => ({
  areaEnabled: true, areaMin: 60, areaMax: 290, deformabilityEnabled: false, deformabilityMin: 0, deformabilityMax: 1,
  ringRatioEnabled: true, ringRatioMin: 15, ringRatioMax: 25, areaRatioEnabled: false, areaRatioMax: 1.5, borderEnabled: true,
  singleInnerContourEnabled: true, targetGroupEnabled: true, targetAreaMin: 100, targetAreaMax: 400, targetDeformabilityMin: 0.2,
  targetDeformabilityMax: 0.6, targetEmodulusEnabled: false, targetEmodulusMin: 2, targetEmodulusMax: 4, multiImageEnabled: false, multiImageCount: 1, ...over,
});
const row = (frame: number, area: number, d: number): MonitoringRow => ({
  frame_index: frame, timestamp_ns: frame, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: 0, centroid_x: 0, centroid_y: 0,
  area, deformability: d, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 1, start_generation: "1", capture_epoch: "2",
});

let host: HTMLDivElement, root: Root;
let tuneNow: TuneState;
let dispatchNow: (a: TuneAction) => void;
let setField: ReturnType<typeof vi.fn>;
const onSelect = vi.fn();

function Harness({ values, table, viewerOnly = false, rows }: { values: TuneValues | null; table: EmodulusTableState; viewerOnly?: boolean; rows: MonitoringRow[] }) {
  const [tune, dispatch] = useReducer(tuneReducer, undefined, () => (values ? tuneReducer(initialTuneState(), { type: "external", values }) : initialTuneState()));
  tuneNow = tune;
  dispatchNow = dispatch;
  const set = useCallback((field: TuneFieldId, value: unknown) => { setField(field, value); dispatch({ type: "edit", field, value }); }, []);
  const gate = useGateOverlay({ tune, setTuneField: set, table, viewerOnly });
  const data = monitoringChartData(rows);
  return <MonitoringScatter data={data.scatter} curves={[]} settings={DEFAULT_MONITORING_SETTINGS} overlay={gate.overlay} emodulusAt={gate.emodulusAt} onSelectPoint={onSelect} />;
}
let mounts = 0;
/** A fresh harness (new tune state, new canvas) every time. */
async function mount(props: Partial<Parameters<typeof Harness>[0]> = {}) {
  await act(async () => root.render(<Harness key={++mounts} values={running()} table={{ status: "none" }} rows={[]} {...props} />));
  return host.querySelector<HTMLCanvasElement>("canvas")!;
}
const activeEdges = (c: HTMLCanvasElement) => lastDraw(c).of("stroke").filter((x) => x.strokeStyle === GATE_ACTIVE).length;
const fire = (el: Element, type: string, init: MouseEventInit = {}) => act(async () => { el.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, ...init })); });
const at = (p: readonly [number, number]) => ({ clientX: p[0], clientY: p[1] });
const press = (el: Element, p: readonly [number, number]) => fire(el, "pointerdown", { ...at(p), button: 0, buttons: 1 });
const move = (el: Element, p: readonly [number, number]) => fire(el, "pointermove", { ...at(p), buttons: 1 });
const release = (el: Element, p: readonly [number, number]) => fire(el, "pointerup", { ...at(p), button: 0, buttons: 0 });
const hover = (el: Element, p: readonly [number, number]) => fire(el, "pointermove", { ...at(p), buttons: 0 });
const tip = () => host.querySelector('[role="tooltip"]')?.textContent ?? null;
const draft = <K extends keyof TuneValues>(k: K) => tuneNow.draft![k];

beforeEach(() => {
  setField = vi.fn();
  onSelect.mockReset();
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("drawing from the draft (MON-064)", () => {
  it("draws the gate while the draft's target group is on, as 'Target group', and as '(unapplied)' once the draft differs", async () => {
    const canvas = await mount();
    expect(lastDraw(canvas).texts()).toContain("Target group");
    await act(async () => dispatchNow({ type: "edit", field: "targetAreaMax", value: 450 }));
    expect(lastDraw(canvas).texts()).toContain("Target group (unapplied)");
    expect(lastDraw(canvas).texts()).not.toContain("Target group");
    await act(async () => dispatchNow({ type: "edit", field: "targetAreaMax", value: 400 })); // back to the applied value
    expect(lastDraw(canvas).texts()).toContain("Target group");
  });
  it("draws nothing while the draft's target group is off or the running configuration is not known", async () => {
    const canvas = await mount({ values: running({ targetGroupEnabled: false }) });
    expect(lastDraw(canvas).texts().filter((t) => t.startsWith("Target group"))).toEqual([]);
    await act(async () => dispatchNow({ type: "edit", field: "targetGroupEnabled", value: true }));
    expect(lastDraw(canvas).texts()).toContain("Target group (unapplied)");
    const none = await mount({ values: null });
    expect(lastDraw(none).texts().filter((t) => t.startsWith("Target group"))).toEqual([]);
  });
  it("redraws for an edit, not for a poll that changes nothing", async () => {
    const canvas = await mount();
    const n = draws.get(canvas)!.length;
    await act(async () => dispatchNow({ type: "external", values: running() })); // the panel's poll finding the same configuration
    expect(draws.get(canvas)!.length).toBe(n);
    await act(async () => dispatchNow({ type: "edit", field: "targetDeformabilityMax", value: 0.7 }));
    expect(draws.get(canvas)!.length).toBe(n + 1);
  });
  it("draws the contours and their labels with the E gate on and a table, and the red pill without one (MON-068)", async () => {
    const withTable = await mount({ values: running({ targetEmodulusEnabled: true, targetEmodulusMin: 1, targetEmodulusMax: 3 }), table: { status: "loaded", table: syntheticTable() } });
    expect(lastDraw(withTable).texts()).toEqual(expect.arrayContaining(["E ≥ 1.00 kPa", "E ≤ 3.00 kPa"]));
    const without = await mount({ values: running({ targetEmodulusEnabled: true }), table: { status: "none" } });
    const texts = lastDraw(without).texts();
    expect(texts).toContain(NO_TABLE_NOTICE);
    expect(texts.filter((t) => t.startsWith("E "))).toEqual([]);
    expect(lastDraw(without).of("fillText").find((c) => c.args[0] === NO_TABLE_NOTICE)!.fillStyle).toBe(GATE_WARN);
    const unknown = await mount({ values: running({ targetEmodulusEnabled: true }), table: EMODULUS_TABLE_UNKNOWN });
    expect(lastDraw(unknown).texts()).not.toContain(NO_TABLE_NOTICE); // not asked yet: no claim either way
  });
});

describe("hover feedback (MON-065)", () => {
  const edgeMax = pxOf(400, 0.4); // the area-max edge, mid-height
  it("shows the resize cursor and Qt's tooltip over a box edge, and draws it active", async () => {
    const canvas = await mount();
    await hover(canvas, [edgeMax[0] + 3, edgeMax[1]]);
    expect(canvas.style.cursor).toBe("ew-resize");
    expect(tip()).toBe("Target group area maximum (µm²): 400 µm² — drag to change");
    expect(activeEdges(canvas)).toBe(1);
    await hover(canvas, pxOf(250, 0.4)); // inside the box, away from its edges: the plain readout
    expect(canvas.style.cursor).toBe("default");
    expect(tip()).toBe("Area 250.0 µm² · deformability 0.400");
    expect(activeEdges(canvas)).toBe(0);
  });
  it("uses a vertical resize cursor for a deformability edge", async () => {
    const canvas = await mount();
    await hover(canvas, pxOf(250, 0.6));
    expect(canvas.style.cursor).toBe("ns-resize");
    expect(tip()).toBe("Target group deformability maximum: 0.60 — drag to change");
  });
  it("lets a handle win over a cell under the pointer", async () => {
    const canvas = await mount({ rows: [row(5, 400, 0.4)] });
    await hover(canvas, pxOf(400, 0.4));
    expect(canvas.style.cursor).toBe("ew-resize");
    expect(tip()).toContain("drag to change");
    const off = await mount({ values: running({ targetGroupEnabled: false }), rows: [row(5, 400, 0.4)] });
    await hover(off, pxOf(400, 0.4));
    expect(off.style.cursor).toBe("pointer"); // no handle without the gate: the cell
    expect(tip()).toContain("click to view");
  });
  it("shows the E readout on empty plot when a table is loaded", async () => {
    const canvas = await mount({ values: running({ targetGroupEnabled: false }), table: { status: "loaded", table: syntheticTable() } });
    await hover(canvas, pxOf(200, 0.3));
    expect(tip()).toBe("Area 200.0 µm² · deformability 0.300 · E 1.30 kPa");
    await hover(canvas, pxOf(800, 0.3)); // outside the table's 0..600 µm²
    expect(tip()).toBe("Area 800.0 µm² · deformability 0.300");
  });
  it("shows no E without a table", async () => {
    const canvas = await mount({ values: running({ targetGroupEnabled: false }) });
    await hover(canvas, pxOf(200, 0.3));
    expect(tip()).toBe("Area 200.0 µm² · deformability 0.300");
  });
});

describe("dragging a handle into the draft (MON-065)", () => {
  it("moves an area edge by the travel of the pointer, relative to the press, and marks the draft unapplied", async () => {
    const canvas = await mount();
    const start: [number, number] = [pxOf(400, 0.4)[0] + 3, pxOf(400, 0.4)[1]]; // pressed 3 px beside the edge
    await press(canvas, start);
    await move(canvas, [start[0] + 3, start[1]]); // under the drag threshold: nothing yet
    expect(setField).not.toHaveBeenCalled();
    await move(canvas, [start[0] + 46.6, start[1]]); // 46.6 px = 100 µm² over 1000 µm² in 466 px
    expect(draft("targetAreaMax")).toBe(500);
    expect(tuneNow.changed).toEqual(["targetAreaMax"]);
    expect(tip()).toBeNull(); // no tooltip stays behind at the press point
    expect(lastDraw(canvas).texts()).toEqual(expect.arrayContaining(["Target group (unapplied)", "Target group area maximum (µm²): 500 µm²"]));
    await release(canvas, [start[0] + 46.6, start[1]]);
    expect(draft("targetAreaMax")).toBe(500);
    expect(onSelect).not.toHaveBeenCalled(); // a drag selects nothing
    expect(lastDraw(canvas).texts().filter((t) => t.includes("area maximum"))).toEqual([]); // the drag readout goes with the drag
  });
  it("moves a deformability edge vertically, rounds to two decimals and keeps min <= max", async () => {
    const canvas = await mount();
    const start = pxOf(250, 0.6);
    await press(canvas, start);
    await move(canvas, [start[0], start[1] - 26.6]); // up 26.6 px = +0.1
    expect(draft("targetDeformabilityMax")).toBe(0.7);
    await move(canvas, [start[0], start[1] + 400]); // far below: stops at the minimum (0.2)
    expect(draft("targetDeformabilityMax")).toBe(0.2);
    await release(canvas, [start[0], start[1] + 400]);
  });
  it("stops an area edge at the other edge", async () => {
    const canvas = await mount();
    const start = pxOf(400, 0.4);
    await press(canvas, start);
    await move(canvas, [VP.left + 2, start[1]]); // dragged past area min (100)
    expect(draft("targetAreaMax")).toBe(100);
    await release(canvas, [VP.left + 2, start[1]]);
  });
  it("stops an edge at the control's own range", async () => {
    const canvas = await mount();
    const min = pxOf(100, 0.4);
    await press(canvas, min);
    await move(canvas, [min[0] - 60, min[1]]); // left of 0: stops at the control's minimum
    expect(draft("targetAreaMin")).toBe(0);
    await release(canvas, [min[0] - 60, min[1]]);
  });
  it("leaves the pan alone away from the handles", async () => {
    const canvas = await mount({ rows: [row(5, 400, 0.4)] });
    const probe = pxOf(700, 0.1);
    await press(canvas, probe);
    await move(canvas, [probe[0] + 40, probe[1]]);
    await release(canvas, [probe[0] + 40, probe[1]]);
    expect(setField).not.toHaveBeenCalled();
    expect(onSelect).not.toHaveBeenCalled();
    // The view panned 40 px right: the area-max edge moved with it, so it is no longer where it was.
    await hover(canvas, pxOf(400, 0.4));
    expect(canvas.style.cursor).toBe("default");
    await hover(canvas, [pxOf(400, 0.4)[0] + 40, pxOf(400, 0.4)[1]]);
    expect(canvas.style.cursor).toBe("ew-resize");
  });
  it("selects the cell under a press on an edge that never travels, and edits nothing", async () => {
    const canvas = await mount({ rows: [row(5, 400, 0.4)] }); // the cell lies on the area-max edge
    const edge = pxOf(400, 0.4);
    await press(canvas, edge);
    await release(canvas, edge);
    expect(setField).not.toHaveBeenCalled();
    expect(draft("targetAreaMax")).toBe(400);
    expect(onSelect).toHaveBeenCalledTimes(1);
    expect(onSelect.mock.calls[0][0].frame_index).toBe(5);
  });
  it("does nothing while an apply is in flight, and the press pans instead", async () => {
    const canvas = await mount();
    await act(async () => dispatchNow({ type: "begin", requestId: 1 }));
    const start = pxOf(400, 0.4);
    await hover(canvas, start);
    expect(canvas.style.cursor).toBe("default"); // not editable: no resize cursor either
    await press(canvas, start);
    await move(canvas, [start[0] + 40, start[1]]);
    await release(canvas, [start[0] + 40, start[1]]);
    expect(setField).not.toHaveBeenCalled();
    expect(draft("targetAreaMax")).toBe(400);
  });
  it("stops writing when an apply begins in the middle of a drag", async () => {
    const canvas = await mount();
    const start = pxOf(400, 0.4);
    await press(canvas, start);
    await move(canvas, [start[0] + 23.3, start[1]]);
    expect(draft("targetAreaMax")).toBe(450);
    await act(async () => dispatchNow({ type: "begin", requestId: 1 }));
    await move(canvas, [start[0] + 46.6, start[1]]);
    expect(draft("targetAreaMax")).toBe(450);
    await release(canvas, [start[0] + 46.6, start[1]]);
  });
  it("is shut for a client that does not control the instrument, like the panel's inputs", async () => {
    const canvas = await mount({ viewerOnly: true });
    const start = pxOf(400, 0.4);
    await hover(canvas, start);
    expect(canvas.style.cursor).toBe("default");
    await press(canvas, start);
    await move(canvas, [start[0] + 40, start[1]]);
    await release(canvas, [start[0] + 40, start[1]]);
    expect(setField).not.toHaveBeenCalled();
  });
  it("has no handles when the gate is off in the draft", async () => {
    const canvas = await mount({ values: running({ targetGroupEnabled: false }) });
    const start = pxOf(400, 0.4);
    await press(canvas, start);
    await move(canvas, [start[0] + 40, start[1]]);
    await release(canvas, [start[0] + 40, start[1]]);
    expect(setField).not.toHaveBeenCalled();
  });
});

describe("dragging an iso-E contour (MON-066)", () => {
  const table = { status: "loaded", table: syntheticTable() } as const;
  const gate = () => running({ targetAreaMin: 0, targetAreaMax: 600, targetDeformabilityMin: 0, targetDeformabilityMax: 0.5, targetEmodulusEnabled: true, targetEmodulusMin: 2, targetEmodulusMax: 4 });
  const anchor = (kind: "emodMin" | "emodMax") => {
    const spec = { enabled: true, areaMin: 0, areaMax: 600, deformMin: 0, deformMax: 0.5, emodEnabled: true, emodMin: 2, emodMax: 4 };
    const lut = table.table.lut;
    return handleAnchor(kind, spec, { min: isoContour(lut, 2), max: isoContour(lut, 4) }, VP)!;
  };
  it("sets the E minimum from the change of E under the contour's anchor point", async () => {
    const canvas = await mount({ values: gate(), table });
    const start = anchor("emodMin");
    await hover(canvas, [start[0], start[1]]);
    expect(canvas.style.cursor).toBe("move");
    expect(tip()).toBe("Target group Young's modulus minimum (kPa): 2.00 kPa — drag to change");
    await press(canvas, [start[0], start[1]]);
    await move(canvas, [start[0] + 20, start[1]]);
    // 20 px = 42.9 µm² more area at this deformability raises E: the minimum follows, to two decimals.
    const [, d] = toDataPoint(VP, start[0], start[1]);
    const expected = 2 + (42.918 / 100) * (1 - 2 * d);
    expect(Number(draft("targetEmodulusMin"))).toBeGreaterThan(2);
    expect(Math.abs(Number(draft("targetEmodulusMin")) - expected)).toBeLessThan(0.02);
    expect(Math.abs(Number(draft("targetEmodulusMin")) * 100 - Math.round(Number(draft("targetEmodulusMin")) * 100))).toBeLessThan(1e-9);
    await move(canvas, [start[0] + 400, start[1]]);
    expect(Number(draft("targetEmodulusMin"))).toBeLessThanOrEqual(4); // never past the maximum
    await release(canvas, [start[0] + 400, start[1]]);
  });
  it("has no E handle without the E range, or without a table", async () => {
    const off = await mount({ values: running({ ...gate(), targetEmodulusEnabled: false }), table });
    const start = anchor("emodMin");
    await hover(off, [start[0], start[1]]);
    expect(off.style.cursor).toBe("default");
    const none = await mount({ values: gate(), table: { status: "none" } });
    await hover(none, [start[0], start[1]]);
    expect(none.style.cursor).toBe("default");
    void NO_CONTOURS;
  });
});
