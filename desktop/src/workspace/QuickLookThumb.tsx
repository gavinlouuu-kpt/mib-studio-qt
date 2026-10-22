// A run tile's mini scatter on the O6 experiment board (#716 A7c; board O6): the run's display-only
// overlay points from its quick look (backend subsample, at most 2000 at a fixed stride), drawn at a
// shared domain so the tiles compare by eye. Display only, like the kit's AreaDeformChart: the only
// arithmetic is the mapping to pixels; no number is derived or shown.
import { conditionColor, type ConditionSlot } from "@mib/ui";
import type { ChartDomain } from "./liveModel";

export const THUMB_W = 96, THUMB_H = 56;

export function QuickLookThumb({ area, deformability, domain, condition, testId }: {
  area: readonly number[]; deformability: readonly number[]; domain: ChartDomain; condition?: ConditionSlot; testId: string;
}) {
  const [x0, x1] = domain.x, [y0, y1] = domain.y;
  const fill = condition ? conditionColor(condition) : "var(--mib-color-sub)";
  return (
    <svg className="ws-ql__thumb" width={THUMB_W} height={THUMB_H} viewBox={`0 0 ${THUMB_W} ${THUMB_H}`} role="img"
      aria-label="Area–deformability subsample (display only)" data-testid={testId}>
      <rect x={0.5} y={0.5} width={THUMB_W - 1} height={THUMB_H - 1} fill="none" stroke="var(--mib-color-line)" />
      {area.map((a, i) => (
        <circle key={i} r={1.2} fill={fill} opacity={0.7}
          cx={2 + ((a - x0) / (x1 - x0)) * (THUMB_W - 4)} cy={THUMB_H - 2 - ((deformability[i] - y0) / (y1 - y0)) * (THUMB_H - 4)} />
      ))}
    </svg>
  );
}
