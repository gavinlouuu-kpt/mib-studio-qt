// The E-modulus table in the Monitoring view (epic #866, PR E1: MON-043 rest, MON-064, 066, 068, 077).
//
// The backend owns the table (backend::EModulusLut, loaded once at bootstrap); `fetch_emodulus_lut` hands
// the 200 x 200 grid over once per identity (monitoringDensity.ts `decodeEmodulusLut`, the Monitoring
// hook useEmodulusTable.ts). This module turns that grid into the lookup the Qt tab does on its private
// copy of the table: `lookupEmodulus` is `EModulusLut::lookup` (src/backend/processing/EModulusLut.cpp),
// bilinear between the four grid points around a position, NaN outside the table's coverage or when any
// of the four corners has no value. Pure: no React, no bridge.
import type { EmodulusGrid, EmodulusLutInfo } from "../../monitoringDensity";

/** A table ready for lookups and contours: the grid in one Float64Array (NaN where the table has no value,
 *  the wire's null), and the derived upper bounds and steps the way EModulusLut computes them. */
export interface PreparedLut {
  areaMin: number;
  areaMax: number;
  areaBins: number;
  /** (areaMax - areaMin) / (areaBins - 1), recomputed from the bounds like EModulusLut::lookup does. */
  areaStep: number;
  deformMin: number;
  deformMax: number;
  deformBins: number;
  deformStep: number;
  /** values[deformIndex * areaBins + areaIndex], NaN = no value. */
  values: Float64Array;
}

/** The table the backend reported: what the lookups run on and who it is. */
export interface EmodulusTable {
  /** "<sha256>|<path>": the key to send as `knownIdentity` next time. */
  identity: string;
  info: EmodulusLutInfo;
  lut: PreparedLut;
}

/** What the Monitoring view knows about the table. `unknown`: not asked yet (or the backend was not ready);
 *  `none`: the backend has no table loaded (Qt's "no table" case: the E layer is skipped);
 *  `error`: the read failed. */
export type EmodulusTableState =
  | { status: "unknown" }
  | { status: "none" }
  | { status: "error"; message: string }
  | { status: "loaded"; table: EmodulusTable };

export const EMODULUS_TABLE_UNKNOWN: EmodulusTableState = { status: "unknown" };

/** The grid of a `fetch_emodulus_lut` reply as a PreparedLut (null becomes NaN). Throws on a grid that is
 *  not area_bins x deform_bins; the decoder has checked that already. */
export function prepareLut(grid: EmodulusGrid): PreparedLut {
  if (grid.values.length !== grid.area_bins * grid.deform_bins) throw new Error("E-modulus grid: values is not area_bins x deform_bins");
  const values = new Float64Array(grid.values.length);
  for (let i = 0; i < values.length; i++) {
    const v = grid.values[i];
    values[i] = v === null ? NaN : v;
  }
  const areaMax = grid.area_min + grid.area_step * (grid.area_bins - 1);
  const deformMax = grid.deform_min + grid.deform_step * (grid.deform_bins - 1);
  return {
    areaMin: grid.area_min,
    areaMax,
    areaBins: grid.area_bins,
    areaStep: (areaMax - grid.area_min) / (grid.area_bins - 1),
    deformMin: grid.deform_min,
    deformMax,
    deformBins: grid.deform_bins,
    deformStep: (deformMax - grid.deform_min) / (grid.deform_bins - 1),
    values,
  };
}

/** `EModulusLut::lookup(area_um, deformability)`: bilinear, NaN without a table, outside its coverage
 *  (inclusive bounds), for a non-finite position, or when any of the four corners has no value. */
export function lookupEmodulus(lut: PreparedLut | null | undefined, areaUm: number, deformability: number): number {
  if (!lut) return NaN;
  if (areaUm < lut.areaMin || areaUm > lut.areaMax || deformability < lut.deformMin || deformability > lut.deformMax) return NaN;
  const aIdx = (areaUm - lut.areaMin) / lut.areaStep;
  const dIdx = (deformability - lut.deformMin) / lut.deformStep;
  // A zero step or a non-finite position: the C++ casts a NaN index to size_t (undefined), this is NaN.
  if (!Number.isFinite(aIdx) || !Number.isFinite(dIdx)) return NaN;
  const ai0 = Math.min(Math.floor(aIdx), lut.areaBins - 1);
  const di0 = Math.min(Math.floor(dIdx), lut.deformBins - 1);
  const ai1 = Math.min(ai0 + 1, lut.areaBins - 1);
  const di1 = Math.min(di0 + 1, lut.deformBins - 1);
  const fa = aIdx - ai0;
  const fd = dIdx - di0;
  const v00 = lut.values[di0 * lut.areaBins + ai0];
  const v10 = lut.values[di0 * lut.areaBins + ai1];
  const v01 = lut.values[di1 * lut.areaBins + ai0];
  const v11 = lut.values[di1 * lut.areaBins + ai1];
  if (Number.isNaN(v00) || Number.isNaN(v10) || Number.isNaN(v01) || Number.isNaN(v11)) return NaN;
  const v0 = v00 * (1 - fa) + v10 * fa;
  const v1 = v01 * (1 - fa) + v11 * fa;
  return v0 * (1 - fd) + v1 * fd;
}

/** The loaded table's lookup, or null when there is none (unknown, none or error). */
export const tableLut = (state: EmodulusTableState): PreparedLut | null => (state.status === "loaded" ? state.table.lut : null);

/** The E readout of the scatter's empty-plot tooltip: the lookup, or null when there is no table or no value. */
export function emodulusReadout(state: EmodulusTableState, areaUm: number, deformability: number): number | null {
  const lut = tableLut(state);
  if (!lut) return null;
  const e = lookupEmodulus(lut, areaUm, deformability);
  return Number.isFinite(e) ? e : null;
}

/** The hint under the tune panel's Young's-modulus range (MON-077). Qt has the tooltip "When no table is
 *  loaded this range is skipped and the target group is the area and deformability box alone."; the
 *  Tauri panel states it as a line that follows the table. */
export function emodulusTableHint(state: EmodulusTableState): string {
  switch (state.status) {
    case "loaded": {
      const { info } = state.table;
      const name = info.lut_id || info.path.split(/[\\/]/).pop() || "";
      return name ? `E-modulus table loaded (${name}): this range filters the target group.` : "E-modulus table loaded: this range filters the target group.";
    }
    case "none": return "No E-modulus table is loaded: this range is skipped and the target group is the area and deformability box alone.";
    case "error": return `The E-modulus table could not be read (${state.message}): this range may be skipped.`;
    case "unknown": return "Checking for the E-modulus table…";
  }
}
