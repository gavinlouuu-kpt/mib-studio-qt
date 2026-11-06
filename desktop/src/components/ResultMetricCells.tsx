import { metricNumber } from "../metricFormat";

/** A row's area in µm²: `area` (px²) × factor², as Qt's HdfMetricsModel column "Area (µm²)". */
export function areaUm2(area: number | null | undefined, pixelToMicron: number): number | null {
  return typeof area === "number" && pixelToMicron > 0 ? area * pixelToMicron * pixelToMicron : null;
}

/** `areaUm2Factor`: Review passes the open file's px→µm factor to add the "Area (µm²)" cell (2 dp) after the px² one. */
export function ResultMetricCells({ row, areaUm2Factor }: { row: { area?: number | null; deformability?: number | null; ring_ratio?: number | null; youngs_modulus?: number | null }; areaUm2Factor?: number }) {
  return <>
    <td>{metricNumber(row.area, 1)}</td>
    {areaUm2Factor !== undefined && <td>{metricNumber(areaUm2(row.area, areaUm2Factor), 2)}</td>}
    <td>{metricNumber(row.deformability, 3)}</td>
    <td>{metricNumber(row.ring_ratio, 3)}</td>
    <td>{metricNumber(row.youngs_modulus != null && row.youngs_modulus > 0 ? row.youngs_modulus : null, 2)}</td>
  </>;
}
