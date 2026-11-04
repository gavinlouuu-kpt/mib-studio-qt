import { describe, expect, it } from "vitest";
import hits from "../../../../tests/fixtures/review_scatter_hits.json";
import { REVIEW_DENSITY } from "../reviewContract";
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
import {
  clampCoreFraction,
  contourFamilies,
  CORE_FRACTION_DEFAULT,
  CORE_FRACTION_KEY,
  framePaneTitle,
  loadCoreFraction,
  loadCurves,
  saveCoreFraction,
  unsavedRecordJson,
} from "./chartData";
import {
  CHARTS_RIGHT_SPLIT_DEFAULT,
  CHARTS_SPLIT_DEFAULT,
  clampShare,
  dragShare,
  loadSplit,
  saveSplit,
} from "./splitter";
import { reviewBridge } from "../reviewBridge";
import { vi } from "vitest";
import { scatterModel, type ChartExportInputs } from "./chartExport";

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
    expect(r).toEqual({ provisional: false, source: "full_run", coreFraction: 0.8, cellCount: 0, populationCount: 0, loops: [[[1, 0.1], [2, 0.2], [3, 0.1]]] });
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

describe("chart export inputs", () => {
  const rec = (fraction: number, provisional: boolean) => JSON.stringify({ contours: [[[1, 0.1], [2, 0.2], [1, 0.3]]], core_fraction: fraction, provisional, source: "x" });
  it("orders contour families like the view and hides superseded records", () => {
    const none = { kde_analysis_json: "", kde_live_json: "" };
    expect(contourFamilies(none, "", "").map((c) => c.name)).toEqual([]);
    expect(contourFamilies(none, "", rec(0.9, false)).map((c) => c.name)).toEqual(["Core 90% (full run, not saved)"]);
    const stored = { kde_analysis_json: rec(0.8, false), kde_live_json: rec(0.9, true) };
    expect(contourFamilies(stored, "", rec(0.9, false)).map((c) => c.name)).toEqual(["Core 80% (full run)", "Core 90% (live, provisional)"]);
    const fams = contourFamilies(stored, rec(0.7, false), "");
    expect(fams.map((c) => [c.name, c.dashed])).toEqual([
      ["Core 70% (full run, not saved)", false],
      ["Core 90% (live, provisional)", true],
    ]);
    expect(unsavedRecordJson(stored, "", rec(0.9, false))).toBe("");
  });

  it("maps the selected valid position to the highlighted point", () => {
    const inp: ChartExportInputs = {
      scatter: { valid: true, pixel_to_micron: 0.25, frame_index: ["0", "2", "4"], valid_position: ["0", "2", "4"], area_um2: [1, 2, 3], deformability: [0.1, 0.2, 0.3], target_group: [0, 0, 0], ring_ratio: [20, 21, 22] },
      density: null,
      curves: [],
      contours: [],
      ranges: { x: { min: 0, max: 4 }, y: { min: 0, max: 1 } },
      colourByDensity: true,
      ringMin: 15,
      ringMax: 25,
      selected: 4,
    };
    const m = scatterModel(inp);
    expect(m.highlight).toBe(2);
    expect(m.colourByDensity).toBe(false); // no density result
    expect(scatterModel({ ...inp, selected: 3 }).highlight).toBe(-1); // position without a point
  });
});

describe("REVIEW-036/037 KDE core share (Monitoring/KdeCoreFraction)", () => {
  const store = (initial?: string) => {
    const m = new Map<string, string>(initial === undefined ? [] : [[CORE_FRACTION_KEY, initial]]);
    return { getItem: (k: string) => m.get(k) ?? null, setItem: (k: string, v: string) => void m.set(k, v), m };
  };

  it("defaults to 0.9 when nothing is stored or the value is unreadable", () => {
    expect(CORE_FRACTION_DEFAULT).toBe(0.9);
    expect(loadCoreFraction(store())).toBe(0.9);
    expect(loadCoreFraction(store("nonsense"))).toBe(0.9);
    expect(loadCoreFraction(store(""))).toBe(0.9);
    expect(loadCoreFraction(store("NaN"))).toBe(0.9);
    expect(loadCoreFraction(null)).toBe(0.9);
  });

  it("clamps to 0.05 – 1", () => {
    expect(clampCoreFraction(0.7)).toBe(0.7);
    expect(clampCoreFraction(0.01)).toBe(0.05);
    expect(clampCoreFraction(0)).toBe(0.05);
    expect(clampCoreFraction(-3)).toBe(0.05);
    expect(clampCoreFraction(7)).toBe(1);
    expect(clampCoreFraction(Infinity)).toBe(0.9);
    expect(loadCoreFraction(store("9"))).toBe(1);
    expect(loadCoreFraction(store("0.01"))).toBe(0.05);
  });

  it("saves the clamped share and reads it back", () => {
    const s = store();
    expect(saveCoreFraction(0.55, s)).toBe(0.55);
    expect(s.m.get(CORE_FRACTION_KEY)).toBe("0.55");
    expect(loadCoreFraction(s)).toBe(0.55);
    expect(saveCoreFraction(2, s)).toBe(1);
    expect(s.m.get(CORE_FRACTION_KEY)).toBe("1");
    expect(saveCoreFraction(NaN, s)).toBe(0.9);
  });

  it("survives storage that throws", () => {
    const broken = { getItem: () => { throw new Error("blocked"); }, setItem: () => { throw new Error("blocked"); } };
    expect(loadCoreFraction(broken)).toBe(0.9);
    expect(saveCoreFraction(0.6, broken)).toBe(0.6);
  });
});

describe("REVIEW-039 Charts splits", () => {
  it("default to Qt's sizes", () => {
    expect(CHARTS_SPLIT_DEFAULT).toBeCloseTo(0.6, 12); // {600, 400}
    expect(CHARTS_RIGHT_SPLIT_DEFAULT).toBeCloseTo(550 / 850, 12); // {550, 300}
  });

  it("keep both panes above their minimum sizes", () => {
    expect(clampShare(0.5, 1000, 420, 320)).toBe(0.5);
    expect(clampShare(0.1, 1000, 420, 320)).toBeCloseTo(0.42, 12);
    expect(clampShare(0.99, 1000, 420, 320)).toBeCloseTo(0.68, 12);
    // Too small for both minimums: share them out as they are.
    expect(clampShare(0.5, 500, 420, 320)).toBeCloseTo(420 / 740, 12);
    // Not measured yet: only the sanity range.
    expect(clampShare(0.99, 0, 420, 320)).toBe(0.95);
    expect(clampShare(NaN, 1000, 0, 0)).toBe(0.5);
  });

  it("turn a pointer position into the first pane's share (the handle is 8 px)", () => {
    expect(dragShare(504, 1000, 420, 320)).toBeCloseTo(0.5, 12);
    expect(dragShare(0, 1000, 420, 320)).toBeCloseTo(0.42, 12);
  });

  it("are stored under the module's keys and only usable numbers are read back", () => {
    const m = new Map<string, string>();
    const s = { getItem: (k: string) => m.get(k) ?? null, setItem: (k: string, v: string) => void m.set(k, v) };
    expect(loadSplit("k", 0.6, s)).toBe(0.6);
    saveSplit("k", 0.123456, s);
    expect(m.get("k")).toBe("0.1235");
    expect(loadSplit("k", 0.6, s)).toBe(0.1235);
    for (const bad of ["", "x", "0", "1", "0.01", "-1"]) {
      m.set("k", bad);
      expect(loadSplit("k", 0.6, s), bad).toBe(0.6);
    }
    expect(loadSplit("k", 0.6, null)).toBe(0.6);
  });
});

describe("REVIEW-032 frame pane title (HdfReviewTab::refreshFramePane)", () => {
  it("reads Frame N · valid row k of n", () => {
    expect(framePaneTitle("42", 6, 250, true)).toBe("Frame 42 · valid row 7 of 250");
  });
  it("adds the failed-validation note for a cell with no scatter point", () => {
    expect(framePaneTitle("42", 6, 250, false)).toBe("Frame 42 · valid row 7 of 250 · failed validation, not on the scatter");
  });
  it("is the empty-state text with no cell selected, and waits for the frame index", () => {
    expect(framePaneTitle(undefined, -1, 250, true)).toBe("Click a point on the scatter to view the cell");
    expect(framePaneTitle(undefined, 0, 250, true)).toBe("Frame … · valid row 1 of 250");
  });
});

describe("isoelastic curves that cannot be fetched", () => {
  it("say so through the log instead of silently drawing none, and are fetched again next time", async () => {
    const fetch = vi.spyOn(reviewBridge, "isoelasticCurves");
    fetch.mockRejectedValueOnce(new Error("resource missing"));
    const log: string[] = [];
    expect(await loadCurves((l) => void log.push(l))).toEqual([]);
    expect(log).toEqual(["isoelastic curves unavailable: Error: resource missing"]);
    fetch.mockResolvedValueOnce({ source: "embedded", curves: [{ emodulus_kpa: 1, points: [[10, 0.01]] }] });
    expect(await loadCurves((l) => void log.push(l))).toHaveLength(1);
    expect(log).toHaveLength(1);
    fetch.mockRestore();
  });

  it("still return [] when no log is given", async () => {
    // The previous call cached its success; a fresh failure needs a fresh module state.
    vi.resetModules();
    const { reviewBridge: bridge } = await import("../reviewBridge");
    const { loadCurves: load } = await import("./chartData");
    vi.spyOn(bridge, "isoelasticCurves").mockRejectedValueOnce(new Error("x"));
    expect(await load()).toEqual([]);
  });
});
