import { describe, expect, it } from "vitest";
import { clipBox, DARK_MEAN_DN, holdBest, measureImage, sameBox } from "./imageQuality";

const W = 64, H = 48;
function frame(fn: (x: number, y: number) => number, stride = W): Uint8Array {
  const d = new Uint8Array(stride * H);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) d[y * stride + x] = fn(x, y);
  return d;
}
const BOX = { x: 8, y: 8, width: 40, height: 30 };
// Deterministic noise.
const noise = (x: number, y: number) => ((x * 73856093) ^ (y * 19349663)) & 0x1f;

describe("image focus and brightness (#501 Align)", () => {
  it("scores a flat frame zero and measures its brightness", () => {
    const m = measureImage(frame(() => 143), W, H, W, BOX)!;
    expect(m.focus).toBe(0);
    expect(m.mean).toBe(143);
    expect(m.p99).toBe(143);
    expect(m.saturated).toBe(0);
    expect(m.pixels).toBe(1200);
  });

  it("scores a sharp edge above a blurred one (the number rises toward focus)", () => {
    const sharp = frame((x) => (x < 28 ? 60 : 200));
    const soft = frame((x) => Math.max(60, Math.min(200, 60 + (x - 20) * 17.5))); // same step over 8 px
    const a = measureImage(sharp, W, H, W, BOX)!, b = measureImage(soft, W, H, W, BOX)!;
    expect(a.focus).toBeGreaterThan(b.focus * 3);
    expect(b.focus).toBeGreaterThan(0);
  });

  it("scores texture by its edge energy and ignores a smooth gradient's brightness", () => {
    const grad = measureImage(frame((x) => x * 3), W, H, W, BOX)!;
    expect(grad.focus).toBeCloseTo(0, 6); // a linear ramp has zero Laplacian
    const textured = measureImage(frame((x, y) => 100 + noise(x, y)), W, H, W, BOX)!;
    expect(textured.focus).toBeGreaterThan(50);
  });

  it("reports the 99th percentile and the saturated fraction", () => {
    const m = measureImage(frame((x, y) => (x === 10 && y < 18 ? 255 : 100)), W, H, W, BOX)!;
    // 18 saturated pixels in column 10, inside the box (rows 8..17)
    expect(m.saturated).toBeCloseTo(10 / 1200, 6);
    expect(m.p99).toBe(100);
    const bright = measureImage(frame((x) => (x < 12 ? 255 : 80)), W, H, W, BOX)!;
    expect(bright.saturated).toBeCloseTo(4 / 40, 6);
    expect(bright.p99).toBe(255);
  });

  it("honours the stride and stays inside the box", () => {
    const padded = frame((x, y) => (x >= 8 && x < 48 && y >= 8 && y < 38 ? 90 : 250), 80);
    const m = measureImage(padded, W, H, 80, BOX)!;
    expect(m.mean).toBe(90);
    expect(m.focus).toBe(0);
  });

  it("clips a box that leaves the frame and refuses one with no interior", () => {
    expect(clipBox({ x: 50, y: 40, width: 40, height: 30 }, W, H)).toEqual({ x: 50, y: 40, width: 14, height: 8 });
    expect(clipBox({ x: 100, y: 0, width: 10, height: 10 }, W, H)).toBeNull();
    expect(measureImage(frame(() => 1), W, H, W, { x: 0, y: 0, width: 2, height: 2 })).toBeNull();
  });

  it("holds the best focus and restarts when the box changes", () => {
    expect(holdBest(null, 10)).toBe(10);
    expect(holdBest(10, 7)).toBe(10);
    expect(holdBest(10, 12)).toBe(12);
    expect(sameBox({ x: 1, y: 2, width: 3, height: 4 }, { x: 1, y: 2, width: 3, height: 4 })).toBe(true);
    expect(sameBox({ x: 1, y: 2, width: 3, height: 4 }, { x: 8, y: 2, width: 3, height: 4 })).toBe(false);
    expect(sameBox(null, null)).toBe(false);
  });

  it("keeps the dark threshold between a dark frame (~18 DN) and a lit one (~143 DN)", () => {
    expect(DARK_MEAN_DN).toBeGreaterThan(18);
    expect(DARK_MEAN_DN).toBeLessThan(143);
  });
});
