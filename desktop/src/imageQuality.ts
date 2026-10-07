// Image-based focus and brightness for the PZ7035 Align view (#501). There is no nanopositioner on
// the instrument, so the operator focuses by hand while watching a number computed from the live
// frame: the variance of the Laplacian inside the Run window (the 512x96 box the U-Net will see).
// A sharper image has more edge energy, so the number rises toward the best focus; the best value
// seen is held so the operator can tell when they have passed it. Brightness (mean, 99th percentile,
// saturated fraction) says whether the LED is lit and not clipping.
//
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.

export interface Box {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface ImageMetrics {
  /** Variance of the 4-neighbour Laplacian over the box interior (grey levels squared). */
  focus: number;
  mean: number;
  /** 99th percentile grey level. */
  p99: number;
  /** Fraction of box pixels at 255. */
  saturated: number;
  /** Pixels measured (the box clipped to the frame). */
  pixels: number;
}

/** Box clipped to the frame, or null when nothing of it is left. */
export function clipBox(box: Box, width: number, height: number): Box | null {
  const x0 = Math.max(0, Math.floor(box.x)), y0 = Math.max(0, Math.floor(box.y));
  const x1 = Math.min(width, Math.floor(box.x + box.width)), y1 = Math.min(height, Math.floor(box.y + box.height));
  return x1 > x0 && y1 > y0 ? {x: x0, y: y0, width: x1 - x0, height: y1 - y0} : null;
}

/** Focus and brightness of `box` in a Mono8 frame. `stride` is bytes per row. Null when the clipped
 *  box is smaller than 3x3 (no Laplacian interior). */
export function measureImage(data: Uint8Array, width: number, height: number, stride: number, box: Box): ImageMetrics | null {
  const b = clipBox(box, width, height);
  if (!b || b.width < 3 || b.height < 3) return null;
  const hist = new Uint32Array(256);
  let sum = 0;
  for (let y = b.y; y < b.y + b.height; y++) {
    const row = y * stride;
    for (let x = b.x; x < b.x + b.width; x++) {
      const v = data[row + x];
      hist[v]++;
      sum += v;
    }
  }
  const pixels = b.width * b.height;
  // Variance of the Laplacian over the interior (every neighbour inside the box).
  let n = 0, lapSum = 0, lapSq = 0;
  for (let y = b.y + 1; y < b.y + b.height - 1; y++) {
    const row = y * stride;
    for (let x = b.x + 1; x < b.x + b.width - 1; x++) {
      const i = row + x;
      const lap = data[i - 1] + data[i + 1] + data[i - stride] + data[i + stride] - 4 * data[i];
      lapSum += lap;
      lapSq += lap * lap;
      n++;
    }
  }
  const mean = lapSum / n;
  const focus = lapSq / n - mean * mean;
  // 99th percentile from the histogram.
  const target = Math.ceil(pixels * 0.99);
  let acc = 0, p99 = 255;
  for (let v = 0; v < 256; v++) {
    acc += hist[v];
    if (acc >= target) { p99 = v; break; }
  }
  return {focus, mean: sum / pixels, p99, saturated: hist[255] / pixels, pixels};
}

/** Peak-hold of the focus number. Reset (null) when the window, mode or frame size changes. */
export function holdBest(best: number | null, current: number): number {
  return best === null || current > best ? current : best;
}

/** Same box? (to know when the peak hold must restart) */
export function sameBox(a: Box | null, b: Box | null): boolean {
  return !!a && !!b && a.x === b.x && a.y === b.y && a.width === b.width && a.height === b.height;
}

// Heuristics from the PL owner's measurements (docs/YOFO_HOST_INTERFACE.md): a lit Align frame is
// ~143 DN on the empty channel, a dark one ~18 DN, and the LED clips around 210 DN and above.
export const DARK_MEAN_DN = 40;
export const SATURATED_WARN_FRACTION = 0.01;
