// Chart inputs shared by the Charts view and the chart exports (Export All,
// Export Charts): isoelastic curves, the per-viewer toggles and the KDE
// contour families, so an export draws what the view draws even when the
// Charts tab was never opened.
import { reviewBridge, type ReviewInfo } from "../reviewBridge";
import { recordContours } from "./chartMath";
import type { ContourFamily, IsoCurve } from "./drawScatter";

export const FULL_RUN_COLOUR = "#2a78d6";
export const LIVE_COLOUR = "#eb6834";

let curvesOnce: Promise<IsoCurve[]> | null = null;
/** Embedded isoelastic curves (fetched once per window). */
export function loadCurves(): Promise<IsoCurve[]> {
  curvesOnce ??= reviewBridge
    .isoelasticCurves()
    .then((c) => c.curves)
    .catch(() => {
      curvesOnce = null; // retry next time
      return [];
    });
  return curvesOnce;
}

// Per-viewer chart toggles (density colours, isoelastic curves).
export const CHART_PREFS_KEY = "yofo.review.charts";
export interface ChartPrefs {
  density: boolean;
  curves: boolean;
}
export function loadChartPrefs(): ChartPrefs {
  try {
    const o = JSON.parse(localStorage.getItem(CHART_PREFS_KEY) ?? "{}") as Record<string, unknown>;
    return { density: o.density !== false, curves: o.curves !== false };
  } catch {
    return { density: true, curves: true };
  }
}
export function saveChartPrefs(p: ChartPrefs): void {
  try {
    localStorage.setItem(CHART_PREFS_KEY, JSON.stringify(p));
  } catch {
    // Storage unavailable: the choice lasts for this session only.
  }
}

const pct = (f: number) => Math.round(f * 100);

/** The record drawn as "full run, not saved": one computed this session,
 * else the density job's (only for files with no stored record). */
export function unsavedRecordJson(info: Pick<ReviewInfo, "kde_analysis_json">, computedJson: string, densityRecordJson: string): string {
  if (computedJson) return computedJson;
  return recordContours(info.kde_analysis_json) ? "" : densityRecordJson;
}

/** Contour families in draw / legend order: stored full run (hidden while a
 * newer computed record is shown), unsaved full run, live provisional. */
export function contourFamilies(
  info: Pick<ReviewInfo, "kde_analysis_json" | "kde_live_json">,
  computedJson: string,
  densityRecordJson: string,
): ContourFamily[] {
  const stored = recordContours(info.kde_analysis_json);
  const live = recordContours(info.kde_live_json);
  const unsaved = recordContours(unsavedRecordJson(info, computedJson, densityRecordJson));
  const out: ContourFamily[] = [];
  if (stored && !computedJson) out.push({ name: `Core ${pct(stored.coreFraction)}% (full run)`, color: FULL_RUN_COLOUR, dashed: false, loops: stored.loops });
  if (unsaved) out.push({ name: `Core ${pct(unsaved.coreFraction)}% (full run, not saved)`, color: FULL_RUN_COLOUR, dashed: false, loops: unsaved.loops });
  if (live) out.push({ name: `Core ${pct(live.coreFraction)}% (live, provisional)`, color: LIVE_COLOUR, dashed: true, loops: live.loops });
  return out;
}
