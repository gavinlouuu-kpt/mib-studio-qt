import { describe, expect, it } from "vitest";
import hits from "../../../../tests/fixtures/review_scatter_hits.json";
import { REVIEW_DENSITY } from "../../bridgeContract";
import {
  levelColor,
  niceTicks,
  paddedExtent,
  panRanges,
  rampColor,
  recordContours,
  ringRatioHistogram,
  toData,
  toPx,
  zoomAxes,
  zoomRange,
  type Viewport,
} from "./chartMath";
import { doubleClickResets, DRAG_THRESHOLD_PX, GestureTracker } from "./scatterGestures";
import { nearestPoint } from "./scatterHitTest";

describe("scatter hit test (shared fixture with the Qt tab)", () => {
  const xs = hits.points.map((p) => p.x);
  const ys = hits.points.map((p) => p.y);
  const frames = hits.points.map((p) => p.frame);
  for (const c of hits.cases) {
    for (const click of c.clicks) {
      it(`${c.name}: ${click.why}`, () => {
        const i = nearestPoint(xs, ys, frames, c.viewport, click.px, click.py, hits.tolerance_px);
        expect(i < 0 ? null : frames[i]).toBe(click.expect);
      });
    }
  }
});

const VP: Viewport = { x0: 0, x1: 100, y0: 0, y1: 1, left: 50, top: 10, width: 200, height: 100 };

describe("chart maths", () => {
  it("pads the extent by 10 % and falls back when empty", () => {
    expect(paddedExtent([10, 110], [0.1, 0.3])).toEqual({ x: { min: 0, max: 120 }, y: { min: expect.closeTo(0.08, 9), max: expect.closeTo(0.32, 9) } });
    expect(paddedExtent([], [])).toEqual({ x: { min: 0, max: 1000 }, y: { min: 0, max: 1 } });
    expect(paddedExtent([NaN], [1])).toEqual({ x: { min: 0, max: 1000 }, y: { min: 0, max: 1 } });
    const single = paddedExtent([50], [0.5]);
    expect(single.x.max).toBeGreaterThan(single.x.min);
    expect(single.y.max).toBeGreaterThan(single.y.min);
  });

  it("zooms about the anchor, about 10 % per notch", () => {
    const r = zoomRange({ min: 0, max: 100 }, 50, -120);
    expect(r.min).toBeCloseTo(50 - 50 / 1.1, 9);
    expect(r.max).toBeCloseTo(50 + 50 / 1.1, 9);
    const out = zoomRange({ min: 0, max: 100 }, 0, 120);
    expect(out.min).toBe(0);
    expect(out.max).toBeCloseTo(100 / 0.9, 9);
    expect(zoomRange({ min: 0, max: 100 }, 50, 1200)).toEqual({ min: 0, max: 100 });
  });

  it("chooses wheel axes like ZoomableChartView", () => {
    expect(zoomAxes(100, 50, VP, true, false)).toEqual({ x: true, y: false });
    expect(zoomAxes(100, 50, VP, false, true)).toEqual({ x: false, y: true });
    expect(zoomAxes(20, 50, VP, false, false)).toEqual({ x: false, y: true });
    expect(zoomAxes(100, 130, VP, false, false)).toEqual({ x: true, y: false });
    expect(zoomAxes(100, 50, VP, false, false)).toEqual({ x: true, y: true });
  });

  it("pans so the content follows the pointer", () => {
    const p = panRanges(VP, 20, 10); // right 20 px = 10 units; down 10 px = 0.1
    expect(p.x).toEqual({ min: -10, max: 90 });
    expect(p.y.min).toBeCloseTo(0.1, 9);
    expect(p.y.max).toBeCloseTo(1.1, 9);
  });

  it("maps data to pixels and back", () => {
    expect(toPx(VP, 0, 1)).toEqual([50, 10]);
    expect(toPx(VP, 100, 0)).toEqual([250, 110]);
    const [x, y] = toData(VP, ...toPx(VP, 37, 0.42));
    expect(x).toBeCloseTo(37, 9);
    expect(y).toBeCloseTo(0.42, 9);
  });

  it("produces 1/2/5 ticks inside the range", () => {
    expect(niceTicks({ min: 0, max: 100 }, 5)).toEqual([0, 20, 40, 60, 80, 100]);
    expect(niceTicks({ min: 0.013, max: 0.047 }, 4)).toEqual([0.02, 0.03, 0.04]);
    expect(niceTicks({ min: 1, max: 1 })).toEqual([]);
  });

  it("bins ring ratios like the Qt tab", () => {
    const h = ringRatioHistogram([0, -1, 10, 15, 15.4, 15.5, 24.9, 25, 30, NaN], 15, 25);
    expect(h.counts.length).toBe(20);
    expect(h.counts[0]).toBe(3); // 10 (clamped), 15, 15.4
    expect(h.counts[1]).toBe(1);
    expect(h.counts[19]).toBe(3); // 24.9, 25, 30 (clamped, last bin closed)
    expect(h.counts.reduce((a, b) => a + b, 0)).toBe(7);
    expect(h.maxCount).toBe(3);
    expect(ringRatioHistogram([], 15, 25).maxCount).toBe(0);
  });

  it("colours levels on the contract ramp", () => {
    const stops = REVIEW_DENSITY.ramp_rgb as readonly (readonly number[])[];
    const first = stops[0], last = stops[stops.length - 1];
    expect(levelColor(0, 8)).toBe(`rgb(${first[0]}, ${first[1]}, ${first[2]})`);
    expect(levelColor(7, 8)).toBe(`rgb(${last[0]}, ${last[1]}, ${last[2]})`);
    expect(rampColor(-1)).toBe(rampColor(0));
    expect(rampColor(2)).toBe(rampColor(1));
  });

  it("parses KDE core records", () => {
    const r = recordContours(JSON.stringify({ contours: [[[1, 0.1], [2, 0.2], [3, 0.1]], [[5, 0.5]]], core_fraction: 0.8, provisional: false, source: "full_run" }));
    expect(r).toEqual({ provisional: false, source: "full_run", coreFraction: 0.8, loops: [[[1, 0.1], [2, 0.2], [3, 0.1]]] });
    expect(recordContours("")).toBeNull();
    expect(recordContours("{bad")).toBeNull();
    expect(recordContours("{}")).toBeNull();
    expect(recordContours(JSON.stringify({ contours: [] }))?.provisional).toBe(true);
  });
});

describe("chart gestures", () => {
  it("a press without travel is a click", () => {
    const g = new GestureTracker();
    g.down(10, 10, 0);
    expect(g.move(12, 13, 1).kind).toBe("none");
    expect(g.up(12, 13, 0)).toEqual({ kind: "click", x: 12, y: 13, button: 0 });
    expect(g.active).toBe(false);
  });

  it("travel past the threshold pans from the press point", () => {
    const g = new GestureTracker();
    g.down(0, 0, 1);
    expect(g.move(DRAG_THRESHOLD_PX, 0, 4)).toEqual({ kind: "pan", dx: DRAG_THRESHOLD_PX, dy: 0 });
    expect(g.move(DRAG_THRESHOLD_PX + 5, 3, 4)).toEqual({ kind: "pan", dx: 5, dy: 3 });
    expect(g.up(DRAG_THRESHOLD_PX + 5, 3, 1).kind).toBe("none");
  });

  it("right button never starts a gesture; idle moves hover", () => {
    const g = new GestureTracker();
    g.down(0, 0, 2);
    expect(g.active).toBe(false);
    expect(g.move(4, 5, 2)).toEqual({ kind: "hover", x: 4, y: 5 });
  });

  it("a move with the button released elsewhere ends the gesture", () => {
    const g = new GestureTracker();
    g.down(0, 0, 0);
    expect(g.move(50, 0, 0)).toEqual({ kind: "hover", x: 50, y: 0 });
    expect(g.up(50, 0, 0).kind).toBe("none");
  });

  it("double-click resets only off points", () => {
    expect(doubleClickResets(false, false)).toBe(true);
    expect(doubleClickResets(true, false)).toBe(false);
    expect(doubleClickResets(false, true)).toBe(false);
  });
});
