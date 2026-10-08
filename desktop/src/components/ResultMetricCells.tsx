import { metricNumber } from "../metricFormat";

export function ResultMetricCells({ row }: { row: { area?: number | null; deformability?: number | null; ring_ratio?: number | null; youngs_modulus?: number | null } }) {
  return <>
    <td>{metricNumber(row.area, 1)}</td>
    <td>{metricNumber(row.deformability, 3)}</td>
    <td>{metricNumber(row.ring_ratio, 3)}</td>
    <td>{metricNumber(row.youngs_modulus != null && row.youngs_modulus > 0 ? row.youngs_modulus : null, 2)}</td>
  </>;
}
