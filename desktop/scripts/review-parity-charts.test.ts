// YOFO Review parity sign-off (tools/review_parity/run.sh): the chart numbers
// the React views compute in TS — scatter home extent (ReviewScatter /
// chartExport) and the ring-ratio histogram with its y range (Histogram) —
// from the scatter the review bridge returned (crates/mib-bridge/examples/
// review_parity.rs). Opt-in: runs only with MIB_REVIEW_PARITY_DIR set. Lives
// outside src/ because it uses Node APIs and `tsc` (npm run build) has no Node types.
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { paddedExtent, ringRatioHistogram } from "../src/review/charts/chartMath";

const dir = process.env.MIB_REVIEW_PARITY_DIR ?? "";
const yofo = join(dir, "yofo");

describe.skipIf(!dir || !existsSync(join(yofo, "scatter.json")))("parity dump", () => {
  it("writes yofo/charts.json", () => {
    const sc = JSON.parse(readFileSync(join(yofo, "scatter.json"), "utf8"));
    const info = JSON.parse(readFileSync(join(yofo, "summary.json"), "utf8"));
    const extent = paddedExtent(sc.area_um2, sc.deformability);
    const hist = ringRatioHistogram(sc.ring_ratio ?? [], info.ring_ratio_min, info.ring_ratio_max);
    const labels = hist.counts.map((_, k) => {
      const start = hist.min + k * hist.binWidth;
      const end = k === hist.counts.length - 1 ? hist.max : start + hist.binWidth;
      return `${start.toFixed(1)}-${end.toFixed(1)}`;
    });
    const charts = {
      scatter: { count: sc.area_um2.length, x: extent.x, y: extent.y },
      // Histogram.tsx: yMax = max(1, ceil(1.1 × max count)), no nice-number step.
      histogram: { counts: hist.counts, labels, y: { min: 0, max: Math.max(1, Math.ceil(hist.maxCount * 1.1)) } },
    };
    writeFileSync(join(yofo, "charts.json"), JSON.stringify(charts, null, 2));
    expect(charts.scatter.count).toBe(sc.area_um2.length);
  });
});
