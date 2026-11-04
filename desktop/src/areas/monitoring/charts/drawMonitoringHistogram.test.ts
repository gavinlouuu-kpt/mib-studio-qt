import { describe, expect, it } from "vitest";
import { ringHistogram } from "../monitoringChartModel";
import { drawMonitoringHistogram, histogramViewport } from "./drawMonitoringHistogram";
import { recordingContext } from "./recordingContext";

const ranges = { x: { min: 15, max: 25 }, y: { min: 0, max: 100 } };

describe("drawMonitoringHistogram", () => {
  it("draws one bar per non-empty bin at its place on the fixed X range", () => {
    const hist = ringHistogram([15.1, 15.2, 20.1], 15, 25, 0.5);
    const rec = recordingContext();
    drawMonitoringHistogram(rec.g, 400, 260, { hist, ranges });
    const bars = rec.of("fillRect").filter((c) => c.fillStyle === "#209fdf");
    expect(bars).toHaveLength(2);
    const vp = histogramViewport(400, 260, ranges);
    const binPx = (0.5 / 10) * vp.width;
    expect(bars[0].args[0] as number).toBeCloseTo(vp.left + 0.5, 6);
    expect(bars[0].args[2] as number).toBeCloseTo(binPx - 1, 6);
    expect(bars[1].args[0] as number).toBeCloseTo(vp.left + 10 * binPx + 0.5, 6);
    // Y is fixed at 0..100: two counts are 2 % of the plot height, one count 1 %.
    expect(bars[0].args[3] as number).toBeCloseTo(vp.height * 0.02, 6);
    expect(bars[1].args[3] as number).toBeCloseTo(vp.height * 0.01, 6);
  });

  it("labels the axes honestly and ticks the visible range with two decimals", () => {
    const rec = recordingContext();
    drawMonitoringHistogram(rec.g, 400, 260, { hist: ringHistogram([], 15, 25, 0.5), ranges });
    const texts = rec.texts();
    expect(texts).toEqual(expect.arrayContaining(["Ring ratio", "Frequency", "15.00", "17.00", "25.00", "No ring ratios yet"]));
  });

  it("follows a zoomed range: bars outside it are not drawn", () => {
    const hist = ringHistogram([15.1, 24.9], 15, 25, 0.5);
    const rec = recordingContext();
    drawMonitoringHistogram(rec.g, 400, 260, { hist, ranges: { x: { min: 20, max: 25 }, y: { min: 0, max: 10 } } });
    expect(rec.of("fillRect").filter((c) => c.fillStyle === "#209fdf")).toHaveLength(1);
    expect(rec.texts()).toContain("20.00");
  });

  it("clips bars to the plot so a count above the fixed maximum does not leave it", () => {
    const hist = ringHistogram(Array.from({ length: 30 }, () => 15.1), 15, 25, 0.5);
    const rec = recordingContext();
    drawMonitoringHistogram(rec.g, 400, 260, { hist, ranges: { x: ranges.x, y: { min: 0, max: 10 } } });
    const order = rec.calls.map((c) => c.fn);
    const clip = order.indexOf("clip");
    const bar = rec.calls.findIndex((c) => c.fn === "fillRect" && c.fillStyle === "#209fdf");
    expect(clip).toBeGreaterThan(-1);
    expect(bar).toBeGreaterThan(clip);
    expect(order.indexOf("restore")).toBeGreaterThan(bar);
  });
});
