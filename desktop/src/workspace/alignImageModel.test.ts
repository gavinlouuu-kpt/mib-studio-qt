// O2 Align image tools (A3): the view transform, the probe, the histogram and the line profile,
// on bounded inputs; and the inspector that keeps inspection off the frame delivery path.
import { describe, expect, it } from "vitest";
import {
  clampOffset,
  fitTransform,
  FrameInspector,
  histogram,
  HISTOGRAM_MAX_SAMPLES,
  histogramPath,
  histogramSummary,
  INSPECT_INTERVAL_MS,
  lineProfile,
  MAX_SCALE,
  MIN_SCALE,
  oneToOneTransform,
  panBy,
  probeAt,
  PROFILE_MAX_POINTS,
  profilePoints,
  screenToImage,
  transformFor,
  zoomAt,
  type InspectFrame,
  type Inspection,
} from "./alignImageModel";

/** A frame whose pixel (x, y) is f(x, y), with optional row padding. */
function frame(width: number, height: number, f: (x: number, y: number) => number, pad = 0, index = "7"): InspectFrame {
  const stride = width + pad;
  const data = new Uint8Array(stride * height).fill(250); // padding bytes must never be read
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) data[y * stride + x] = f(x, y) & 255;
  return { data, width, height, stride_bytes: stride, frame_index: index };
}

const IMG = { width: 512, height: 96 };
const VIEW = { width: 640, height: 300 };

describe("view transform: fit, 1:1, zoom, pan", () => {
  it("fit shows the whole image, centred", () => {
    const t = fitTransform(IMG, VIEW);
    expect(t).toEqual({ scale: 1.25, offsetX: 0, offsetY: 90 });
    expect(fitTransform({ width: 816, height: 624 }, VIEW).scale).toBeCloseTo(300 / 624);
    expect(fitTransform({ width: 0, height: 0 }, VIEW)).toEqual({ scale: 1, offsetX: 0, offsetY: 0 });
  });

  it("1:1 is one image pixel per screen pixel, centred", () => {
    expect(oneToOneTransform(IMG, VIEW)).toEqual({ scale: 1, offsetX: 64, offsetY: 102 });
    expect(oneToOneTransform({ width: 816, height: 624 }, VIEW)).toEqual({ scale: 1, offsetX: -88, offsetY: -162 });
  });

  it("zoom keeps the pixel under the anchor where it was", () => {
    const t0 = fitTransform(IMG, VIEW), anchor = { x: 321, y: 151 };
    const before = screenToImage(t0, anchor, IMG)!;
    const t1 = zoomAt(t0, 2, anchor, IMG, VIEW);
    expect(t1.scale).toBe(2.5);
    expect(screenToImage(t1, anchor, IMG)).toEqual(before);
  });

  it("zoom is bounded", () => {
    let t = fitTransform(IMG, VIEW);
    for (let i = 0; i < 100; i++) t = zoomAt(t, 1.25, { x: 10, y: 10 }, IMG, VIEW);
    expect(t.scale).toBe(MAX_SCALE);
    for (let i = 0; i < 200; i++) t = zoomAt(t, 0.8, { x: 10, y: 10 }, IMG, VIEW);
    expect(t.scale).toBe(MIN_SCALE);
  });

  it("pan never loses the image: a larger image covers the view, a smaller one stays inside", () => {
    const big = { scale: 4, offsetX: 0, offsetY: 0 }; // 2048 × 384 on 640 × 300
    expect(panBy(big, 500, 500, IMG, VIEW)).toEqual({ scale: 4, offsetX: 0, offsetY: 0 });
    expect(panBy(big, -5000, -5000, IMG, VIEW)).toEqual({ scale: 4, offsetX: 640 - 2048, offsetY: 300 - 384 });
    const small = { scale: 0.5, offsetX: 10, offsetY: 10 }; // 256 × 48
    expect(panBy(small, -100, -100, IMG, VIEW)).toEqual({ scale: 0.5, offsetX: 0, offsetY: 0 });
    expect(panBy(small, 1000, 1000, IMG, VIEW)).toEqual({ scale: 0.5, offsetX: 640 - 256, offsetY: 300 - 48 });
    expect(clampOffset({ scale: 1.25, offsetX: 0, offsetY: 90 }, IMG, VIEW)).toEqual({ scale: 1.25, offsetX: 0, offsetY: 90 });
  });

  it("fit and 1:1 follow the sizes; zoom keeps the operator's transform, re-clamped", () => {
    const mine = { scale: 3, offsetX: -10, offsetY: 5 }; // 1536 × 288 on 640 × 300
    expect(transformFor("fit", mine, IMG, VIEW)).toEqual(fitTransform(IMG, VIEW));
    expect(transformFor("1:1", mine, IMG, VIEW)).toEqual(oneToOneTransform(IMG, VIEW));
    expect(transformFor("zoom", mine, IMG, VIEW)).toEqual(mine);
    expect(transformFor("zoom", { scale: 3, offsetX: 99, offsetY: 0 }, IMG, VIEW).offsetX).toBe(0);
  });
});

describe("pixel probe", () => {
  it("maps the cursor to the image pixel and reads its raw value through the row stride", () => {
    const f = frame(8, 4, (x, y) => 10 * y + x, 3);
    const t = { scale: 10, offsetX: 5, offsetY: 5 };
    const at = screenToImage(t, { x: 5 + 10 * 6 + 9.9, y: 5 + 10 * 2 }, f)!;
    expect(at).toEqual({ x: 6, y: 2 });
    expect(probeAt(f, at.x, at.y)).toEqual({ x: 6, y: 2, value: 26 });
  });

  it("is null outside the image, never a padding byte", () => {
    const f = frame(8, 4, () => 1, 3);
    const t = { scale: 1, offsetX: 0, offsetY: 0 };
    expect(screenToImage(t, { x: 8, y: 0 }, f)).toBeNull();
    expect(screenToImage(t, { x: -0.1, y: 0 }, f)).toBeNull();
    expect(probeAt(f, 8, 0)).toBeNull();
    expect(probeAt(f, 0, 4)).toBeNull();
    expect(probeAt(f, 1.5, 0)).toBeNull();
  });
});

describe("histogram", () => {
  it("counts every pixel of a small frame", () => {
    const f = frame(4, 2, (x) => (x === 3 ? 255 : x === 0 ? 0 : 100), 2);
    const h = histogram(f);
    expect(h.step).toBe(1);
    expect(h.samples).toBe(8);
    expect([h.bins[0], h.bins[100], h.bins[255], h.bins[250]]).toEqual([2, 4, 2, 0]); // padding not counted
    expect([h.min, h.max]).toEqual([0, 255]);
    expect(h.mean).toBeCloseTo((0 * 2 + 100 * 4 + 255 * 2) / 8);
    expect(h.saturated).toBe(0.25);
    expect(h.black).toBe(0.25);
  });

  it("reads at most HISTOGRAM_MAX_SAMPLES pixels of a large frame, on a regular grid", () => {
    const f = frame(1000, 1000, (x, y) => x + y);
    const h = histogram(f);
    expect(h.step).toBe(4);
    expect(h.samples).toBe(250 * 250);
    expect(h.samples).toBeLessThanOrEqual(HISTOGRAM_MAX_SAMPLES);
    expect(h.bins.reduce((a, b) => a + b, 0)).toBe(h.samples);
    expect(histogram(f, 100).samples).toBeLessThanOrEqual(100);
  });

  it("says what it read, and draws one bar per occupied value", () => {
    const h = histogram(frame(4, 2, (x) => (x === 3 ? 255 : 100)));
    expect(histogramSummary(h)).toBe("min 100 · max 255 · mean 138.8 · saturated 25.0 % · 8 px (every pixel)");
    expect(histogramSummary(histogram(frame(1000, 1000, () => 1)))).toContain("62,500 px (every 4th pixel)");
    expect(histogramPath(h, 256, 64).match(/M/g)).toHaveLength(2);
    expect(histogramPath({ ...h, bins: new Uint32Array(256) }, 256, 64)).toBe("");
  });
});

describe("line profile", () => {
  const f = frame(6, 3, (x, y) => 10 * y + x, 2);

  it("reads one row or one column through the stride", () => {
    expect(lineProfile(f, "row", 1)!.values).toEqual([10, 11, 12, 13, 14, 15]);
    const col = lineProfile(f, "column", 4)!;
    expect(col.values).toEqual([4, 14, 24]);
    expect(col.positions).toEqual([0, 1, 2]);
    expect([col.min, col.max]).toEqual([4, 24]);
  });

  it("is null for a row or column that is not in the frame", () => {
    expect(lineProfile(f, "row", 3)).toBeNull();
    expect(lineProfile(f, "column", -1)).toBeNull();
    expect(lineProfile(f, "row", 0.5)).toBeNull();
  });

  it("returns at most PROFILE_MAX_POINTS values of a long line", () => {
    const wide = frame(5000, 1, (x) => x);
    const p = lineProfile(wide, "row", 0)!;
    expect(p.values.length).toBeLessThanOrEqual(PROFILE_MAX_POINTS);
    expect(p.positions[1]).toBe(5); // every 5th pixel
    expect(profilePoints(p, 5000, 256, 64).split(" ")).toHaveLength(p.values.length);
  });
});

const last = <T>(list: T[]) => list[list.length - 1];

describe("FrameInspector keeps inspection off the delivery path", () => {
  /** A hand-driven clock and timer queue. */
  function harness(intervalMs = INSPECT_INTERVAL_MS) {
    let now = 0;
    const timers: { at: number; fn: () => void; id: number }[] = [];
    let ids = 0;
    const published: Inspection[] = [];
    const inspector = new FrameInspector((i) => published.push(i), {
      set: (fn, ms) => { const id = ++ids; timers.push({ at: now + ms, fn, id }); return id; },
      clear: (h) => { const i = timers.findIndex((t) => t.id === h); if (i >= 0) timers.splice(i, 1); },
    }, () => now, intervalMs);
    const advance = (ms: number) => {
      const end = now + ms;
      for (;;) {
        timers.sort((a, b) => a.at - b.at);
        const next = timers[0];
        if (!next || next.at > end) break;
        timers.shift();
        now = next.at;
        next.fn();
      }
      now = end;
    };
    return { inspector, published, advance, pendingTimers: () => timers.length };
  }

  it("deliver only keeps the frame: nothing is computed until the timer runs", () => {
    const h = harness();
    for (let i = 0; i < 100; i++) h.inspector.deliver(frame(64, 32, () => i, 0, String(i)));
    expect(h.inspector.computations).toBe(0);
    expect(h.pendingTimers()).toBe(1); // one scheduled computation, not one per frame
    expect(h.inspector.latest()!.frame_index).toBe("99");
    h.advance(0);
    expect(h.inspector.computations).toBe(1);
    expect(h.published[0].frameIndex).toBe("99"); // the newest frame; the others were skipped
  });

  it("computes at most once per interval at a high frame rate", () => {
    const h = harness();
    for (let t = 0; t < 1000; t += 2) { h.inspector.deliver(frame(64, 32, () => 1, 0, String(t))); h.advance(2); }
    expect(h.inspector.computations).toBeLessThanOrEqual(1000 / INSPECT_INTERVAL_MS + 1);
    expect(h.inspector.computations).toBeGreaterThanOrEqual(3);
  });

  it("a frozen display keeps its frame; the profile line can move on it", () => {
    const h = harness();
    h.inspector.deliver(frame(8, 4, (_x, y) => y, 0, "1"));
    h.advance(0);
    h.inspector.setFrozen(true);
    expect(h.inspector.deliver(frame(16, 2, () => 9, 0, "2"))).toBe(false); // not taken: neither its pixels nor its size
    expect(h.inspector.latest()!.width).toBe(8);
    h.advance(1000);
    expect(h.inspector.latest()!.frame_index).toBe("1");
    expect(last(h.published).profile).toMatchObject({ axis: "row", index: 2 }); // the middle row by default
    h.inspector.setProfile("row", 3);
    h.advance(0);
    expect(last(h.published).profile).toMatchObject({ axis: "row", index: 3, values: [3, 3, 3, 3, 3, 3, 3, 3] });
    expect(last(h.published).frameIndex).toBe("1");
    h.inspector.dispose();
    expect(h.pendingTimers()).toBe(0);
  });
});
