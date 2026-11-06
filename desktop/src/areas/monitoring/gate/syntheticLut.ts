// Test support: a synthetic E-modulus table whose bilinear lookup is a closed formula, shared by the gate tests.
// E(area, deformability) = 0.5 + area / 100 * (1 - 2 * deformability) on a 61 x 51 grid over area 0..600 µm²,
// deformability 0..0.5; the top-right corner block is "no value" (outside the table's coverage).
import type { EmodulusGrid } from "../../../monitoringDensity";
import { prepareLut, type EmodulusTable, type PreparedLut } from "../emodulusLut";

export const synthE = (area: number, deform: number): number => 0.5 + (area / 100) * (1 - 2 * deform);

export function syntheticGrid(): EmodulusGrid {
  const na = 61, nd = 51;
  const values: (number | null)[] = [];
  for (let di = 0; di < nd; di++) for (let ai = 0; ai < na; ai++) values.push(ai >= 55 && di >= 45 ? null : synthE(10 * ai, 0.01 * di));
  return { area_min: 0, area_step: 10, area_bins: na, deform_min: 0, deform_step: 0.01, deform_bins: nd, values };
}

export const syntheticLut = (): PreparedLut => prepareLut(syntheticGrid());

export function syntheticTable(identity = "synthetic|/tables/synthetic.txt"): EmodulusTable {
  return {
    identity,
    info: { path: "/tables/synthetic.txt", sha256: "synthetic", lut_id: "synthetic", revision: "1", source: "test", catalog_sha256: "", checksum_status: "ok" },
    lut: syntheticLut(),
  };
}
