// Chart snapshots for Export All and Export Charts (plan
// 2026-10-01-standalone-review-app, PR 4). The Qt tab renders both charts at
// 1200 × 1200 over the whole run (data extent, no selection ring; the
// view's toggles and contours as shown) and writes `scatter_plot.tiff` /
// `ring_width_histogram.tiff`; here the same drawing
// code (drawScatter / drawHistogram) renders offscreen, the PNG bytes go to
// the backend one raw IPC call each (`review_stage_chart`) and the export
// job decodes and writes them as TIFFs.
import { invoke } from "@tauri-apps/api/core";
import { reviewBridge, type ReviewDensity, type ReviewInfo, type ReviewScatter } from "../reviewBridge";
import { paddedExtent, ringRatioHistogram, type Range } from "./chartMath";
import { contourFamilies, loadChartPrefs, loadCurves } from "./chartData";
import { drawHistogram } from "./Histogram";
import { drawScatter, type ContourFamily, type IsoCurve, type ScatterModel } from "./drawScatter";

export const CHART_EXPORT_SIZE = 1200;
export const SCATTER_FILE = "scatter_plot.tiff";
export const HISTOGRAM_FILE = "ring_width_histogram.tiff";

/** What the Charts view currently shows (null: not mounted yet, use the
 * defaults an unopened view would show). Zoom and selection are not part
 * of an export (Qt: exports always show the whole run). */
export interface ChartViewState {
  colourByDensity: boolean;
  showCurves: boolean;
  /** Unsaved record computed this session ("" = none). */
  computedJson: string;
}

export interface ChartExportInputs {
  scatter: ReviewScatter;
  density: ReviewDensity | null;
  curves: IsoCurve[];
  contours: ContourFamily[];
  ranges: { x: Range; y: Range };
  colourByDensity: boolean;
  ringMin: number;
  ringMax: number;
  /** Highlighted valid-set position (-1 none; exports pass none). */
  selected: number;
}

/** Inputs for the open file: the view's state when the Charts tab is
 * mounted, otherwise the persisted toggles and the data extent. Density
 * levels are used when a ready result for this file exists. */
export async function gatherChartInputs(info: ReviewInfo, view: ChartViewState | null): Promise<ChartExportInputs> {
  const prefs = loadChartPrefs();
  const [scatter, rawDensity, curves] = await Promise.all([reviewBridge.scatter(), reviewBridge.density(), loadCurves()]);
  const density = rawDensity.valid && rawDensity.ready && rawDensity.levels.length === scatter.area_um2.length ? rawDensity : null;
  const showCurves = view ? view.showCurves : prefs.curves;
  return {
    scatter,
    density,
    curves: showCurves ? curves : [],
    contours: contourFamilies(info, view?.computedJson ?? "", density?.computed_record_json ?? ""),
    ranges: paddedExtent(scatter.area_um2, scatter.deformability),
    colourByDensity: view ? view.colourByDensity : prefs.density,
    ringMin: info.ring_ratio_min,
    ringMax: info.ring_ratio_max,
    selected: -1,
  };
}

export function scatterModel(inp: ChartExportInputs): ScatterModel {
  const n = inp.scatter.area_um2.length;
  let highlight = -1;
  for (let i = 0; i < n && inp.selected >= 0; i++) {
    if (Number(inp.scatter.valid_position[i]) === inp.selected) {
      highlight = i;
      break;
    }
  }
  return {
    xs: Float64Array.from(inp.scatter.area_um2),
    ys: Float64Array.from(inp.scatter.deformability),
    levels: inp.density?.levels ?? null,
    levelCount: inp.density?.level_count ?? 8,
    colourByDensity: inp.colourByDensity && !!inp.density,
    curves: inp.curves,
    contours: inp.contours,
    highlight,
    ranges: inp.ranges,
  };
}

async function canvasPng(draw: (g: CanvasRenderingContext2D) => void): Promise<Uint8Array> {
  const c = document.createElement("canvas");
  c.width = CHART_EXPORT_SIZE;
  c.height = CHART_EXPORT_SIZE;
  const g = c.getContext("2d");
  if (!g) throw new Error("no 2D canvas context");
  draw(g);
  const blob = await new Promise<Blob | null>((resolve) => c.toBlob(resolve, "image/png"));
  if (!blob) throw new Error("chart encoding failed");
  return new Uint8Array(await blob.arrayBuffer());
}

/** Both charts as PNG bytes, named as the Qt tab names its TIFFs. */
export async function renderChartPngs(inp: ChartExportInputs): Promise<{ name: string; bytes: Uint8Array }[]> {
  const s = CHART_EXPORT_SIZE;
  const scatter = await canvasPng((g) => drawScatter(g, s, s, scatterModel(inp)));
  const hist = await canvasPng((g) => drawHistogram(g, s, s, ringRatioHistogram(inp.scatter.ring_ratio ?? [], inp.ringMin, inp.ringMax)));
  return [
    { name: SCATTER_FILE, bytes: scatter },
    { name: HISTOGRAM_FILE, bytes: hist },
  ];
}

/** Replace the staged snapshots with `charts` (raw bytes, no JSON). */
export async function stageCharts(charts: { name: string; bytes: Uint8Array }[]): Promise<void> {
  await invoke("review_clear_charts");
  for (const c of charts) await invoke("review_stage_chart", c.bytes, { headers: { "x-chart-name": c.name } });
}
