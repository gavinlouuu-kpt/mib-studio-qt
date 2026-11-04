import { describe, expect, it, vi } from "vitest";
import type { MonitoringRow } from "../../../bridge";
import type { Viewport } from "../../../review/charts/chartMath";
import { monitoringChartData } from "../monitoringChartModel";
import {
  curveLabel, drawMonitoringScatter, scatterViewport, SELECTION_COLOUR, SELECTION_SIZE, TARGET_COLOUR, VALID_COLOUR,
  type MonitoringScatterModel,
} from "./drawMonitoringScatter";
import { recordingContext } from "./recordingContext";

const row = (over: Partial<MonitoringRow>): MonitoringRow => ({
  frame_index: 1, timestamp_ns: 1, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: 0, centroid_x: 0, centroid_y: 0,
  area: 100, deformability: 0.1, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 1, ...over,
});
const data = monitoringChartData([
  row({ frame_index: 1, area: 100, deformability: 0.2 }),
  row({ frame_index: 2, area: 300, deformability: 0.4, target_group: true }),
  row({ frame_index: 3, area: 5000, deformability: 0.5 }), // outside the default X range
]).scatter;
const ranges = { x: { min: 0, max: 1000 }, y: { min: 0, max: 1 } };
const curves = [
  { modulus: 2, points: [[25, 0.1], [400, 0.3]] as [number, number][] },
  { modulus: 1, points: [[25, 0.2], [400, 0.5]] as [number, number][] },
];
const model = (over: Partial<MonitoringScatterModel> = {}): MonitoringScatterModel => ({ data, curves, ranges, selected: -1, ...over });

describe("drawMonitoringScatter", () => {
  it("draws the in-range points green and the target group blue, over the curves", () => {
    const rec = recordingContext();
    drawMonitoringScatter(rec.g, 700, 360, model(), { selection: true });
    const fills = rec.of("fill");
    const pointFills = fills.filter((f) => f.fillStyle === VALID_COLOUR || f.fillStyle === TARGET_COLOUR);
    // Per colour: the points' path, then the legend dot.
    expect(pointFills.map((f) => f.fillStyle)).toEqual([VALID_COLOUR, TARGET_COLOUR, VALID_COLOUR, TARGET_COLOUR]);
    // 2 plotted points (the third is outside X 0..1000) + 2 legend dots.
    expect(rec.of("arc")).toHaveLength(4);
    // Two curves, each a stroked polyline.
    expect(rec.of("lineTo").length).toBeGreaterThanOrEqual(2);
  });

  it("lists the series and every curve in the legend, highest modulus first, as x.xx kPa", () => {
    const rec = recordingContext();
    drawMonitoringScatter(rec.g, 700, 360, model(), { selection: true });
    const texts = rec.texts();
    expect(texts).toContain("Valid Frames");
    expect(texts).toContain("Target Group");
    expect(texts.filter((t) => t.endsWith("kPa"))).toEqual(["2.00 kPa", "1.00 kPa"]);
    expect(curveLabel(0.5)).toBe("0.50 kPa");
  });

  it("labels the axes and puts the fixed default ticks on the plot", () => {
    const rec = recordingContext();
    drawMonitoringScatter(rec.g, 700, 360, model(), { selection: true });
    const texts = rec.texts();
    expect(texts).toContain("Area (μm²)");
    expect(texts).toContain("Deformability");
    expect(texts).toContain("0");
    expect(texts).toContain("1000");
  });

  it("draws the selected cell as an orange 13 px marker, and not when exporting", () => {
    const on = recordingContext();
    drawMonitoringScatter(on.g, 700, 360, model({ selected: 0 }), { selection: true });
    const marker = on.of("arc").filter((c) => c.args[2] === SELECTION_SIZE / 2);
    expect(marker).toHaveLength(1);
    expect(on.of("fill").some((f) => f.fillStyle === SELECTION_COLOUR)).toBe(true);
    const vp = scatterViewport(700, 360, ranges);
    expect(marker[0].args[0]).toBeCloseTo(vp.left + (100 / 1000) * vp.width, 6);

    const off = recordingContext();
    drawMonitoringScatter(off.g, 700, 360, model({ selected: 0 }), { selection: false });
    expect(off.of("arc").filter((c) => c.args[2] === SELECTION_SIZE / 2)).toHaveLength(0);
    expect(off.of("fill").some((f) => f.fillStyle === SELECTION_COLOUR)).toBe(false);
  });

  it("leaves a hook for the gate overlay: clipped to the plot, with the live axis transform", () => {
    const rec = recordingContext();
    const overlay = vi.fn((g: CanvasRenderingContext2D, _vp: Viewport) => g.fillText("OVERLAY", 0, 0));
    const zoomed = { x: { min: 200, max: 400 }, y: { min: 0.1, max: 0.5 } };
    drawMonitoringScatter(rec.g, 700, 360, model({ overlay, ranges: zoomed }), { selection: true });
    expect(overlay).toHaveBeenCalledOnce();
    const vp = overlay.mock.calls[0][1];
    expect([vp.x0, vp.x1, vp.y0, vp.y1]).toEqual([200, 400, 0.1, 0.5]);
    expect(vp).toMatchObject(scatterViewport(700, 360, zoomed));
    // Inside the plot's clip, after the points and before the legend: between clip and restore, and
    // before the first legend label.
    const order = rec.calls.map((c) => (c.fn === "fillText" ? `text:${String(c.args[0])}` : c.fn));
    const at = order.indexOf("text:OVERLAY");
    expect(order.lastIndexOf("clip", at)).toBeGreaterThan(-1);
    expect(order.indexOf("restore", at)).toBeGreaterThan(at);
    expect(order.indexOf("text:Valid Frames")).toBeGreaterThan(at);
  });

  it("says so when there is nothing to plot, but still draws the fixed axes", () => {
    const rec = recordingContext();
    drawMonitoringScatter(rec.g, 700, 360, model({ data: monitoringChartData([]).scatter, curves: [] }), { selection: true });
    expect(rec.texts()).toContain("No calibrated valid cells yet");
    expect(rec.texts()).toContain("1000");
  });

  it("titles an export", () => {
    const rec = recordingContext();
    drawMonitoringScatter(rec.g, 700, 360, model(), { selection: false, title: "Deformability vs Area (μm²)" });
    expect(rec.texts()).toContain("Deformability vs Area (μm²)");
  });
});
