// Scatter density (KDE) colouring (epic #866 PR E2: MON-058, 059, 060, 061): the pure model. Qt reference:
// ExperimentMonitoringTab (kdeLevelForDensity, kdeLevelColor, refreshKdeTooltip, pollKdeResult) and
// include/backend/processing/MonitoringDensity.h (densityRampColor). No React, no DOM, no canvas.
//
// The backend (MonitoringDensityService) computes the estimate; this module turns what the shell
// fetched into what the chart needs: a level 0..7 per plotted point, the contour loops and the toggle's
// tooltip. A point the estimate does not name is newer than it and sits in the sparsest level, as in Qt.
import type { MonitoringRow } from "../../../bridge";
import { densityByRow, type MonitoringDensityReply } from "../../../monitoringDensity";
import { levelColor } from "../../../review/charts/chartMath";
import { monitoringRowKey } from "../monitoringChartModel";

/** Qt kKdeLevels: the number of density levels (0 = sparsest). */
export const KDE_LEVELS = 8;

/** A closed loop in µm² x deformability (the last vertex repeats the first). */
export type Loop = readonly (readonly [number, number])[];

/** Qt kdeLevelForDensity: the density (0..1, the densest sample is 1) truncated into one of 8 levels; a value
 *  that is not finite reads as the sparsest level. */
export function densityLevel(density: number | null | undefined): number {
  if (typeof density !== "number" || !Number.isFinite(density)) return 0;
  return Math.min(KDE_LEVELS - 1, Math.trunc(Math.min(1, Math.max(0, density)) * KDE_LEVELS));
}

/** The ramp colour of each level: Qt kdeLevelColor(level, 8) = densityRampColor(level / 7), blue, light to dark.
 *  The ramp is the contract's (REVIEW_DENSITY.ramp_rgb), which the Review scatter draws with as well. */
export const LEVEL_COLOURS: readonly string[] = Array.from({ length: KDE_LEVELS }, (_, level) => levelColor(level, KDE_LEVELS));

/** What the chart draws on top of the points while KDE is on. */
export interface ScatterDensityModel {
  /** Per plotted point, parallel to the scatter's arrays: its density level 0..7. */
  levels: Uint8Array;
  /** The core contour: solid, around the densest N % of the cells. */
  contours: readonly Loop[];
  /** The reference contour: dashed orange. */
  reference: readonly Loop[];
}

/** The level of every plotted point. `byRow` is `densityByRow(result)`; null (no estimate yet) and a row the
 *  estimate does not hold both give the sparsest level. */
export function densityLevels(rows: readonly MonitoringRow[], byRow: ReadonlyMap<string, number> | null): Uint8Array {
  const levels = new Uint8Array(rows.length);
  if (!byRow || byRow.size === 0) return levels;
  for (let i = 0; i < rows.length; i += 1) {
    const density = byRow.get(monitoringRowKey(rows[i]));
    if (density !== undefined) levels[i] = densityLevel(density);
  }
  return levels;
}

/** What the shell keeps of the last estimate the backend published (one object per generation). */
export interface DensityEstimate {
  generation: string;
  /** The density of each point of the estimate, by the row it belongs to (`densityByRow`). Empty when the
   *  service holds no estimate (the ring emptied). */
  byRow: ReadonlyMap<string, number>;
  contours: readonly Loop[];
  pointCount: number;
  bandwidthX: number | null;
  bandwidthY: number | null;
  computeMs: number;
  /** null: too few cells for a contour. */
  coreLevel: number | null;
  coreCount: number;
  /** The Qt tooltip's "refreshed every": max(interval, the service's next interval), ms. */
  refreshMs: number;
}

type NewDensityReply = Extract<MonitoringDensityReply, { valid: true; unchanged: false }>;

/** The estimate of a reply that carries a newer generation. A reply without a result (the service holds none: it
 *  is disabled or the ring emptied) gives an estimate with no densities and no contours, as Qt resets. */
export function estimateOfReply(reply: NewDensityReply): DensityEstimate {
  const result = reply.result;
  return {
    generation: reply.generation,
    byRow: result ? densityByRow(result) : new Map(),
    contours: result ? result.contours : [],
    pointCount: result ? result.points.count : 0,
    bandwidthX: result ? result.bandwidth.x : null,
    bandwidthY: result ? result.bandwidth.y : null,
    computeMs: result ? result.compute_ms : 0,
    coreLevel: result ? result.core_level : null,
    coreCount: result ? result.core_count : 0,
    refreshMs: reply.stats.refresh_ms,
  };
}

/** A loaded or pinned reference contour. */
export interface DensityReference {
  /** Qt's label: "pinned HH:mm:ss", or "<file>, full-run | live estimate <p>%". */
  label: string;
  contours: readonly Loop[];
}

/** Qt: "pinned %1" with the local time HH:mm:ss. */
export function pinnedLabel(now: Date): string {
  const two = (n: number) => String(n).padStart(2, "0");
  return `pinned ${two(now.getHours())}:${two(now.getMinutes())}:${two(now.getSeconds())}`;
}

/** The toggle's tooltip while KDE is off (the .ui file's toolTip). */
export const KDE_TOOLTIP_OFF = "Colour scatter points by local population density (Gaussian KDE, recomputed periodically)";

/** C's printf("%g") with three significant digits, as Qt's QString::arg(double, 0, 'g', 3) prints the seconds. */
function general3(value: number): string {
  return String(Number(value.toPrecision(3)));
}

export interface DensityTooltipInput {
  on: boolean;
  estimate: DensityEstimate | null;
  /** The setting (ms); the refresh shown is the larger of it and the service's own spacing. */
  intervalMs: number;
  /** The setting (%). */
  corePercent: number;
  reference: DensityReference | null;
}

/** Qt refreshKdeTooltip: the toggle's tooltip (MON-059). With KDE on and no estimate yet it reads as Qt's does
 *  before the first one: no points and "too few cells for a contour". */
export function densityTooltip({ on, estimate, intervalMs, corePercent, reference }: DensityTooltipInput): string {
  if (!on) return KDE_TOOLTIP_OFF;
  const refreshMs = Math.max(intervalMs, estimate?.refreshMs ?? 0);
  let text =
    `Density (KDE) over ${estimate?.pointCount ?? 0} points; bandwidth ${(estimate?.bandwidthX ?? 0).toFixed(1)} μm² × ${(estimate?.bandwidthY ?? 0).toFixed(4)}; ` +
    `computed in ${estimate?.computeMs ?? 0} ms; refreshed every ${general3(refreshMs / 1000)} s`;
  if (estimate && estimate.coreLevel !== null) {
    text += `\nCore ${Math.round(corePercent)}%: ${estimate.coreCount} cells, ${estimate.contours.length} contour loop(s)`;
  } else {
    text += "\nCore: too few cells for a contour";
  }
  if (reference && reference.contours.length > 0) text += `\nReference contour: ${reference.label}`;
  return text;
}

/** What the toggle beside the scatter needs. */
export interface DensityToggleModel {
  onChange: (on: boolean) => void;
  tooltip: string;
  /** A refused or failing density command or poll, said once ("" = none). */
  notice: string;
}

/** What the Monitoring charts take to draw and control the density layer (null/absent: no layer, the plain chart). */
export interface MonitoringDensityLayer {
  /** KDE is on (false for a viewer, who cannot switch the instrument's density on). When false the chart is the plain one. */
  on: boolean;
  byRow: ReadonlyMap<string, number> | null;
  contours: readonly Loop[];
  reference: readonly Loop[];
  toggle: DensityToggleModel;
}
