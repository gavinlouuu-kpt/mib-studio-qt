// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import type { MonitoringRow, MonitoringSnapshot } from "../../../bridge";
import type { Viewport } from "../../../review/charts/chartMath";
import { MonitoringCharts } from "../../../components/MonitoringCharts";
import { monitoringRowKey } from "../monitoringChartModel";
import { DEFAULT_MONITORING_SETTINGS, type MonitoringSettings } from "../monitoringSettings";
import { histogramViewport } from "./drawMonitoringHistogram";
import { SELECTION_SIZE } from "./drawMonitoringScatter";
import { recordingContext } from "./recordingContext";
import type { ScatterOverlay } from "./MonitoringScatter";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const REFERENCE = JSON.stringify({
  curves: [{ modulus: 1, points: [[25, 0.1], [400, 0.2]] }, { modulus: 2, points: [[25, 0.15], [400, 0.3]] }],
  curve_source: "Bundled reference, channel width 30 um",
});

// jsdom has no canvas and no layout: every getContext() is a recording context kept per canvas, and every
// element measures 700 x 340 CSS px, so the scatter's plot is left 64, top 30, 466 x 266 px.
type Recorder = ReturnType<typeof recordingContext>;
const draws = new Map<HTMLCanvasElement, Recorder[]>();
const lastOf = <T,>(a: readonly T[]): T => a[a.length - 1];
const lastDraw = (c: HTMLCanvasElement) => lastOf(draws.get(c)!);
const drawCount = (c: HTMLCanvasElement) => draws.get(c)?.length ?? 0;
const VP = { left: 64, top: 30, width: 466, height: 266 };

beforeEach(() => {
  draws.clear();
  vi.mocked(invoke).mockReset();
  vi.mocked(invoke).mockResolvedValue(REFERENCE);
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
// Points at area 100 / 500 (deformability 0.2 / 0.6): pixel x = 64 + area/1000 * 466, y = 30 + (1 - d) * 266.
const ROWS = [row({ frame_index: 10, area: 100, deformability: 0.2 }), row({ frame_index: 11, area: 500, deformability: 0.6, target_group: true })];
const pxOf = (area: number, d: number): [number, number] => [VP.left + (area / 1000) * VP.width, VP.top + (1 - d) * VP.height];

async function mount(props: Record<string, unknown> = {}, snap = snapshot(ROWS)) {
  await act(async () => root.render(<MonitoringCharts snapshot={snap} {...props} />));
  return {
    scatter: host.querySelector<HTMLCanvasElement>('[data-tour="monitoring-scatter"] canvas')!,
    histogram: host.querySelector<HTMLCanvasElement>('[data-tour="monitoring-histogram"] canvas')!,
  };
}
const fire = (el: Element, type: string, init: MouseEventInit & { deltaY?: number } = {}) =>
  act(async () => {
    el.dispatchEvent(type === "wheel" ? new WheelEvent(type, { bubbles: true, cancelable: true, ...init }) : new MouseEvent(type, { bubbles: true, cancelable: true, ...init }));
  });
const at = (p: [number, number]) => ({ clientX: p[0], clientY: p[1] });
const click = async (el: Element, p: [number, number]) => {
  await fire(el, "pointerdown", { ...at(p), button: 0, buttons: 1 });
  await fire(el, "pointerup", { ...at(p), button: 0, buttons: 0 });
};

/** An overlay that reports the viewport of every draw: the live axis transform. */
function viewportProbe() {
  const seen: Viewport[] = [];
  const overlay: ScatterOverlay = { draw: (_g, vp) => seen.push(vp) };
  return { seen, overlay, last: () => lastOf(seen) };
}

describe("the Monitoring charts", () => {
  it("shows the three charts with their tour anchors, the honest ring-ratio wording and the notes", async () => {
    await mount();
    expect(host.querySelector('[data-tour="monitoring-scatter"]')).not.toBeNull();
    expect(host.querySelector('[data-tour="monitoring-histogram"]')).not.toBeNull();
    expect(host.textContent).toContain("Deformability vs area (µm²)");
    expect(host.textContent).toContain("Ring-ratio distribution (dimensionless)");
    expect(host.textContent).toContain("This is ring ratio, not a physical ring-width measurement.");
    expect(host.textContent).toContain("Young’s modulus distribution (kPa)");
    expect(host.textContent).toContain("0 valid rows lack analysis-time calibration and are omitted");
  });

  it("counts the valid rows without calibration and plots only the calibrated ones", async () => {
    const { scatter } = await mount({}, snapshot([row({ pixel_to_micron: 0 }), row({ frame_index: 2, area: 100, pixel_to_micron: 0.5 }), row({ frame_index: 3, area: 100, pixel_to_micron: 2 })]));
    expect(host.textContent).toContain("1 valid rows lack analysis-time calibration");
    expect(scatter.getAttribute("aria-label")).toBe("Deformability versus calibrated area, 2 samples");
  });

  it("draws the isoelastic reference curves always, with a legend and the curve-source text, and no checkbox", async () => {
    const { scatter } = await mount();
    expect(invoke).toHaveBeenCalledWith("fetch_monitoring_chart_reference");
    expect(host.querySelector('input[type="checkbox"]')).toBeNull();
    expect(host.textContent).toContain("channel width 30 um");
    expect(host.textContent).toContain("Reference conditions are not inferred from current hardware settings");
    const texts = lastDraw(scatter).texts();
    expect(texts.filter((t) => t.endsWith("kPa"))).toEqual(["2.00 kPa", "1.00 kPa"]);
  });

  it("says when the reference is unavailable and still draws the scatter", async () => {
    vi.mocked(invoke).mockRejectedValue("no reference file");
    const { scatter } = await mount();
    expect(host.textContent).toContain("Isoelastic reference unavailable: no reference file");
    expect(lastDraw(scatter).texts()).toContain("Valid Frames");
  });

  it("keeps the Young's-modulus histogram (MON-051)", async () => {
    await mount({}, snapshot([row({ youngs_modulus: 2 }), row({ frame_index: 2, youngs_modulus: 6 })]));
    const svg = host.querySelector('svg[aria-label^="Young"]')!;
    expect(svg.getAttribute("aria-label")).toBe("Young's modulus histogram, 2 valid-object samples, ten bins");
    expect(svg.querySelectorAll("rect")).toHaveLength(10);
  });

  it("works without the studio state, on the Qt default axes", async () => {
    const { scatter } = await mount({}, snapshot([]));
    const texts = lastDraw(scatter).texts();
    expect(texts).toContain("1000");
    expect(texts).toContain("No calibrated valid cells yet");
    expect(host.textContent).toContain("No calibrated finite valid-object samples available.");
    expect(host.textContent).toContain("No finite ring-ratio samples available.");
  });
});

describe("fixed axes (MON-038)", () => {
  it("holds the default axes between polls instead of re-fitting the data", async () => {
    const probe = viewportProbe();
    await mount({ overlay: probe.overlay });
    expect([probe.last().x0, probe.last().x1, probe.last().y0, probe.last().y1]).toEqual([0, 1000, 0, 1]);
    await mount({ overlay: probe.overlay }, snapshot([row({ frame_index: 30, area: 40000, deformability: 3 })]));
    expect([probe.last().x0, probe.last().x1, probe.last().y0, probe.last().y1]).toEqual([0, 1000, 0, 1]);
  });

  it("takes the axes from the settings and drops the zoom when they are applied again", async () => {
    const probe = viewportProbe();
    const { scatter } = await mount({ overlay: probe.overlay });
    await fire(scatter, "wheel", { ...at(pxOf(500, 0.5)), deltaY: -120 });
    expect(probe.last().x1 - probe.last().x0).toBeLessThan(1000);
    const applied: MonitoringSettings = { ...DEFAULT_MONITORING_SETTINGS, scatterXMax: 400, scatterYMax: 0.5, scatterEpoch: 1 };
    await mount({ overlay: probe.overlay, settings: applied });
    expect([probe.last().x0, probe.last().x1, probe.last().y0, probe.last().y1]).toEqual([0, 400, 0, 0.5]);
  });
});

describe("zoom and pan (MON-039, MON-040, MON-041)", () => {
  it("zooms both axes about the pointer by about 10 % a notch", async () => {
    const probe = viewportProbe();
    const { scatter } = await mount({ overlay: probe.overlay });
    await fire(scatter, "wheel", { ...at(pxOf(500, 0.5)), deltaY: -120 });
    expect(probe.last().x1 - probe.last().x0).toBeCloseTo(1000 / 1.1, 3);
    expect(probe.last().y1 - probe.last().y0).toBeCloseTo(1 / 1.1, 6);
    // Zoom is about the pointer: the data value under it stays put.
    expect((probe.last().x0 + probe.last().x1) / 2).toBeCloseTo(500, 3);
    // A notch out is 1 - 1/10 (Qt: 1 + delta / 1200), so in and out do not cancel exactly.
    await fire(scatter, "wheel", { ...at(pxOf(500, 0.5)), deltaY: 120 });
    expect(probe.last().x1 - probe.last().x0).toBeCloseTo(1000 / 1.1 / 0.9, 3);
  });

  it("zooms X only with Ctrl, Y only with Shift, and only the axis under the pointer over an axis", async () => {
    const probe = viewportProbe();
    const { scatter } = await mount({ overlay: probe.overlay });
    const span = () => [probe.last().x1 - probe.last().x0, probe.last().y1 - probe.last().y0];
    await fire(scatter, "wheel", { ...at(pxOf(500, 0.5)), deltaY: -120, ctrlKey: true });
    expect(span()[0]).toBeLessThan(1000);
    expect(span()[1]).toBe(1);
    await fire(scatter, "dblclick", at([300, 100])); // reset
    await fire(scatter, "wheel", { ...at(pxOf(500, 0.5)), deltaY: -120, shiftKey: true });
    expect(span()[0]).toBe(1000);
    expect(span()[1]).toBeLessThan(1);
    await fire(scatter, "dblclick", at([300, 100]));
    await fire(scatter, "wheel", { clientX: 30, clientY: 150, deltaY: -120 }); // over the Y axis labels
    expect(span()[0]).toBe(1000);
    expect(span()[1]).toBeLessThan(1);
    await fire(scatter, "dblclick", at([300, 100]));
    await fire(scatter, "wheel", { clientX: 300, clientY: 320, deltaY: -120 }); // over the X axis labels
    expect(span()[0]).toBeLessThan(1000);
    expect(span()[1]).toBe(1);
  });

  it("pans by dragging past the threshold, with the left or the middle button", async () => {
    const probe = viewportProbe();
    const { scatter } = await mount({ overlay: probe.overlay });
    const start: [number, number] = [300, 150];
    for (const [button, mask] of [[0, 1], [1, 4]] as const) {
      await fire(scatter, "dblclick", at(start)); // back to the defaults
      await fire(scatter, "pointerdown", { ...at(start), button, buttons: mask });
      await fire(scatter, "pointermove", { clientX: 304, clientY: 150, buttons: mask }); // under the threshold
      expect(probe.last().x0).toBe(0);
      await fire(scatter, "pointermove", { clientX: 340, clientY: 150, buttons: mask });
      await fire(scatter, "pointerup", { clientX: 340, clientY: 150, button, buttons: 0 });
      // 40 px to the right moves the content with the pointer: the range shifts left by 40 px of data.
      expect(probe.last().x0).toBeCloseTo((-40 * 1000) / VP.width, 3);
      expect(probe.last().x1 - probe.last().x0).toBeCloseTo(1000, 6);
    }
  });

  it("a press that never travels is a click, not a pan", async () => {
    const probe = viewportProbe();
    const { scatter } = await mount({ overlay: probe.overlay });
    await fire(scatter, "pointerdown", { ...at([300, 150]), button: 0, buttons: 1 });
    await fire(scatter, "pointermove", { clientX: 303, clientY: 152, buttons: 1 });
    await fire(scatter, "pointerup", { clientX: 303, clientY: 152, button: 0, buttons: 0 });
    expect(probe.last().x0).toBe(0);
  });

  it("double-click on empty space resets the zoom, but never on a point", async () => {
    const probe = viewportProbe();
    const onSelectPoint = vi.fn();
    const { scatter } = await mount({ overlay: probe.overlay, onSelectPoint });
    const zoomIn = () => fire(scatter, "wheel", { ...at(pxOf(500, 0.6)), deltaY: -240 });
    await zoomIn();
    expect(probe.last().x1 - probe.last().x0).toBeLessThan(1000);
    // Two clicks and the double-click on the point at (500, 0.6): the zoom stays.
    const point = pxOf(500, 0.6);
    await click(scatter, point);
    await click(scatter, point);
    await fire(scatter, "dblclick", at(point));
    expect(probe.last().x1 - probe.last().x0).toBeLessThan(1000);
    // On empty space it resets.
    const empty: [number, number] = [VP.left + 20, VP.top + VP.height - 20];
    await click(scatter, empty);
    await click(scatter, empty);
    await fire(scatter, "dblclick", at(empty));
    expect(probe.last().x1 - probe.last().x0).toBe(1000);
  });

  it("has a Reset zoom button that is enabled only while zoomed", async () => {
    const probe = viewportProbe();
    const { scatter } = await mount({ overlay: probe.overlay });
    const button = host.querySelector<HTMLButtonElement>('[data-tour="monitoring-scatter-reset-zoom"]')!;
    expect(button.disabled).toBe(true);
    await fire(scatter, "wheel", { ...at(pxOf(500, 0.5)), deltaY: -120 });
    expect(button.disabled).toBe(false);
    await act(async () => button.click());
    expect(probe.last().x1 - probe.last().x0).toBe(1000);
    expect(button.disabled).toBe(true);
  });

  it("zooms and pans the histogram on its own fixed axes, and resets it on a double-click", async () => {
    const { histogram } = await mount({}, snapshot([row({ ring_ratio: 20 })]));
    const ticks = () => lastDraw(histogram).texts().filter((t) => /^\d+\.\d\d$/.test(t));
    expect(ticks()[0]).toBe("15.00");
    expect(lastOf(ticks())).toBe("25.00");
    await fire(histogram, "wheel", { clientX: 300, clientY: 100, deltaY: -240 });
    expect(Number(ticks()[0])).toBeGreaterThan(15);
    expect(Number(lastOf(ticks()))).toBeLessThan(25);
    await fire(histogram, "dblclick", at([300, 100]));
    expect(ticks()[0]).toBe("15.00");
  });
});

describe("pointer feedback (MON-043)", () => {
  it("shows Area and deformability anywhere in the plot", async () => {
    const { scatter } = await mount();
    await fire(scatter, "pointermove", { ...at([VP.left + VP.width / 2, VP.top + VP.height / 2]), buttons: 0 });
    expect(host.querySelector('[role="tooltip"]')!.textContent).toBe("Area 500.0 µm² · deformability 0.500");
    expect(scatter.style.cursor).toBe("default");
  });

  it("adds the E readout when an E table is given", async () => {
    const emodulusAt = vi.fn(() => 3.254);
    const { scatter } = await mount({ emodulusAt });
    await fire(scatter, "pointermove", { ...at([VP.left + VP.width / 2, VP.top + VP.height / 2]), buttons: 0 });
    expect(emodulusAt).toHaveBeenCalled();
    expect(host.querySelector('[role="tooltip"]')!.textContent).toBe("Area 500.0 µm² · deformability 0.500 · E 3.25 kPa");
  });

  it("names the frame and shows a hand over a point", async () => {
    const { scatter } = await mount();
    await fire(scatter, "pointermove", { ...at(pxOf(500, 0.6)), buttons: 0 });
    expect(host.querySelector('[role="tooltip"]')!.textContent).toBe("Frame 11 · 500.0 µm² · deformability 0.6000 · click to view");
    expect(scatter.style.cursor).toBe("pointer");
  });

  it("shows nothing outside the plot or after the pointer leaves", async () => {
    const { scatter } = await mount();
    await fire(scatter, "pointermove", { clientX: 10, clientY: 10, buttons: 0 });
    expect(host.querySelector('[role="tooltip"]')).toBeNull();
    await fire(scatter, "pointermove", { ...at(pxOf(500, 0.6)), buttons: 0 });
    expect(host.querySelector('[role="tooltip"]')).not.toBeNull();
    await fire(scatter, "pointerout", { relatedTarget: null });
    expect(host.querySelector('[role="tooltip"]')).toBeNull();
  });

  it("does not redraw the canvas for hover, only for data or view changes", async () => {
    const { scatter } = await mount();
    const before = drawCount(scatter);
    await fire(scatter, "pointermove", { ...at([200, 100]), buttons: 0 });
    await fire(scatter, "pointermove", { ...at([210, 110]), buttons: 0 });
    expect(drawCount(scatter)).toBe(before);
    await fire(scatter, "wheel", { ...at([200, 100]), deltaY: -120 });
    expect(drawCount(scatter)).toBe(before + 1);
  });
});

describe("selection seam for click-to-view (MON-044 later, MON-045)", () => {
  it("calls onSelectPoint with the row of the clicked point, and not for empty space", async () => {
    const onSelectPoint = vi.fn();
    const { scatter } = await mount({ onSelectPoint });
    await click(scatter, pxOf(100, 0.2));
    expect(onSelectPoint).toHaveBeenCalledTimes(1);
    expect(onSelectPoint.mock.calls[0][0]).toMatchObject({ frame_index: 10, object_id: 0 });
    await click(scatter, [VP.left + 5, VP.top + 5]);
    expect(onSelectPoint).toHaveBeenCalledTimes(1);
  });

  it("does not select from a drag", async () => {
    const onSelectPoint = vi.fn();
    const { scatter } = await mount({ onSelectPoint });
    const p = pxOf(100, 0.2);
    await fire(scatter, "pointerdown", { ...at(p), button: 0, buttons: 1 });
    await fire(scatter, "pointermove", { clientX: p[0] + 30, clientY: p[1], buttons: 1 });
    await fire(scatter, "pointerup", { clientX: p[0] + 30, clientY: p[1], button: 0, buttons: 0 });
    expect(onSelectPoint).not.toHaveBeenCalled();
  });

  it("draws the selected cell's orange 13 px marker for selectedKey, and none without it", async () => {
    const key = monitoringRowKey(ROWS[1]);
    const { scatter } = await mount({ selectedKey: key });
    const marker = lastDraw(scatter).of("arc").filter((c) => c.args[2] === SELECTION_SIZE / 2);
    expect(marker).toHaveLength(1);
    expect(marker[0].args[0] as number).toBeCloseTo(pxOf(500, 0.6)[0], 6);
    await mount({ selectedKey: "1:2:999:0" });
    expect(lastDraw(scatter).of("arc").filter((c) => c.args[2] === SELECTION_SIZE / 2)).toHaveLength(0);
    await mount({ selectedKey: null });
    expect(lastDraw(scatter).of("arc").filter((c) => c.args[2] === SELECTION_SIZE / 2)).toHaveLength(0);
  });
});

describe("overlay seam for the gate (later)", () => {
  it("hands the overlay the live axis transform, repainting when its version changes", async () => {
    const probe = viewportProbe();
    const { scatter } = await mount({ overlay: { ...probe.overlay, version: 1 } });
    const n = probe.seen.length;
    await mount({ overlay: { ...probe.overlay, version: 2 } });
    expect(probe.seen.length).toBe(n + 1);
    await fire(scatter, "wheel", { ...at(pxOf(500, 0.5)), deltaY: -120 });
    expect(probe.last().x1 - probe.last().x0).toBeCloseTo(1000 / 1.1, 3);
    expect(probe.last()).toMatchObject(VP);
  });

  it("lets the overlay take a press on a handle: its drag moves the overlay, not the axes, and no cell is selected", async () => {
    const events: string[] = [];
    const probe = viewportProbe();
    const onSelectPoint = vi.fn();
    const handle: [number, number] = pxOf(100, 0.2);
    const overlay: ScatterOverlay = {
      ...probe.overlay,
      interceptor: {
        press: (px, py) => {
          const hit = Math.abs(px - handle[0]) < 8 && Math.abs(py - handle[1]) < 8;
          if (hit) events.push("press");
          return hit;
        },
        move: (px) => events.push(`move ${px}`),
        end: (_x, _y, _vp, completed) => events.push(`end ${completed}`),
      },
    };
    const { scatter } = await mount({ overlay, onSelectPoint });
    await fire(scatter, "pointerdown", { ...at(handle), button: 0, buttons: 1 });
    await fire(scatter, "pointermove", { clientX: handle[0] + 3, clientY: handle[1], buttons: 1 }); // under the threshold
    await fire(scatter, "pointermove", { clientX: handle[0] + 30, clientY: handle[1], buttons: 1 });
    await fire(scatter, "pointerup", { clientX: handle[0] + 30, clientY: handle[1], button: 0, buttons: 0 });
    expect(events).toEqual(["press", `move ${handle[0] + 30}`, "end true"]);
    expect(probe.last().x0).toBe(0);
    expect(onSelectPoint).not.toHaveBeenCalled();
    // A press on the handle that never travels still selects the cell under it, like Qt.
    events.length = 0;
    await click(scatter, handle);
    expect(events).toEqual(["press"]);
    expect(onSelectPoint).toHaveBeenCalledTimes(1);
  });

  it("a press away from the handles pans as usual, and the overlay can claim the hover", async () => {
    const probe = viewportProbe();
    const overlay: ScatterOverlay = { ...probe.overlay, interceptor: { press: () => false, move: vi.fn(), end: vi.fn() }, hover: () => true };
    const { scatter } = await mount({ overlay });
    await fire(scatter, "pointermove", { ...at(pxOf(500, 0.6)), buttons: 0 });
    expect(host.querySelector('[role="tooltip"]')).toBeNull();
    await fire(scatter, "pointerdown", { ...at([300, 150]), button: 0, buttons: 1 });
    await fire(scatter, "pointermove", { clientX: 340, clientY: 150, buttons: 1 });
    await fire(scatter, "pointerup", { clientX: 340, clientY: 150, button: 0, buttons: 0 });
    expect(probe.last().x0).toBeLessThan(0);
  });
});

describe("the ring histogram's settings (MON-049..051, MON-089..091)", () => {
  const rect = (c: HTMLCanvasElement) => lastDraw(c).of("fillRect").filter((x) => x.fillStyle === "#209fdf");
  it("bins by the bin width over the X range and holds Y at its maximum", async () => {
    const rows = [row({ ring_ratio: 15.1 }), row({ frame_index: 2, ring_ratio: 15.2 }), row({ frame_index: 3, ring_ratio: 15.7 })];
    const { histogram } = await mount({}, snapshot(rows));
    expect(rect(histogram)).toHaveLength(2); // 0.5-wide bins: two in the first, one in the second
    await mount({ settings: { ...DEFAULT_MONITORING_SETTINGS, histogramBinWidth: 1 } }, snapshot(rows));
    expect(rect(histogram)).toHaveLength(1); // 1.0-wide: all three in the first
    const bars = rect(histogram);
    expect(bars[0].args[3] as number).toBeCloseTo((3 / 100) * histogramViewport(700, 340, { x: { min: 15, max: 25 }, y: { min: 0, max: 100 } }).height, 6);
  });

  it("follows the X range of the settings, such as the config's ring_ratio_min/max", async () => {
    const { histogram } = await mount({ settings: { ...DEFAULT_MONITORING_SETTINGS, histogramXMin: 10, histogramXMax: 30, histogramEpoch: 1 } }, snapshot([row({ ring_ratio: 12 })]));
    const texts = lastDraw(histogram).texts();
    expect(texts).toContain("10.00");
    expect(texts).toContain("30.00");
  });
});
