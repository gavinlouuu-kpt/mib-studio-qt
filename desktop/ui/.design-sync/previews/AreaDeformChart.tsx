import { AreaDeformChart, type AreaDeformPoint, type Isoelastic } from "@mib/ui";

// Deterministic illustrative cloud (no Math.random: previews must render identically).
function cloud(n: number, cx: number, cy: number, sx: number, sy: number, seed: number, condition?: 1 | 2 | 3): AreaDeformPoint[] {
  let s = seed;
  const rnd = () => ((s = (s * 16807) % 2147483647) / 2147483647);
  const gauss = () => (rnd() + rnd() + rnd() + rnd() - 2) / 0.8;
  return Array.from({ length: n }, () => ({ area: cx + gauss() * sx, deformability: Math.max(0.002, cy + gauss() * sy), condition }));
}

// Illustrative isoelastic shapes; real curves come from the E-LUT.
const iso = (label: string, amp: number): Isoelastic => ({
  label,
  points: Array.from({ length: 31 }, (_, k) => ({ area: (k / 30) * 300, deformability: 0.2 * amp * Math.pow(k / 30, 1.35) })),
});
const curves = [iso("1.0", 0.78), iso("1.5", 0.52), iso("2.0", 0.34)];

export const InspectPopulation = () => (
  <AreaDeformChart
    points={cloud(220, 150, 0.09, 40, 0.03, 11)}
    isoelastics={curves}
    polygon={[
      { area: 92, deformability: 0.11 },
      { area: 158, deformability: 0.147 },
      { area: 222, deformability: 0.102 },
      { area: 196, deformability: 0.04 },
      { area: 100, deformability: 0.04 },
    ]}
    populationLabel="Population A: 3 212 of 100 000 (3.2 %)"
    inspected={{ area: 186.8, deformability: 0.0612, label: "R-024210" }}
  />
);

export const RunVsReference = () => (
  <AreaDeformChart
    points={[...cloud(160, 150, 0.06, 38, 0.02, 3, 1), ...cloud(160, 140, 0.1, 40, 0.025, 5, 3)]}
    isoelastics={curves}
    legend={[
      { condition: 1, label: "01 vehicle (reference)" },
      { condition: 3, label: "03 cytoD 1 µM" },
    ]}
  />
);

export const Compact = () => (
  <AreaDeformChart width={360} height={220} points={cloud(120, 150, 0.08, 40, 0.03, 7)} isoelastics={curves} yTicks={[0, 0.1, 0.2]} />
);
