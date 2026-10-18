import type { SVGAttributes } from "react";
import { cx } from "./classNames";
import { conditionColor, type ConditionSlot } from "./ConditionSwatch";

export interface AreaDeformPoint {
  /** Cell area in µm². */
  area: number;
  /** Deformability (1 − circularity). */
  deformability: number;
  /** Condition slot for colour; omit for the grey "all records" cloud. */
  condition?: ConditionSlot;
}

export interface Isoelastic {
  /** E label shown at the curve end, e.g. "1.5" (kPa). */
  label: string;
  /** Curve points from the E-LUT. The UI never computes E; pass what the backend/LUT gives. */
  points: Array<{ area: number; deformability: number }>;
}

export interface AreaDeformChartProps extends Omit<SVGAttributes<SVGSVGElement>, "points"> {
  /** Measured records to plot. */
  points: AreaDeformPoint[];
  /** Isoelastic E curves from the E-LUT, drawn dashed. */
  isoelastics?: Isoelastic[];
  /** Population polygon (area/deformability vertices), drawn teal dotted with handles. */
  polygon?: Array<{ area: number; deformability: number }>;
  /** Population label, format "Population A: N of M (x %)". */
  populationLabel?: string;
  /** The inspected record, drawn as a blue ring. */
  inspected?: { area: number; deformability: number; label: string };
  /** Legend entries (condition slot + label), drawn top-left inside the plot. */
  legend?: Array<{ condition: ConditionSlot; label: string }>;
  /** [min, max] area in µm². Default [0, 300]. */
  xDomain?: [number, number];
  /** [min, max] deformability. Default [0, 0.2]. */
  yDomain?: [number, number];
  /** Area ticks. Default 0, 75, 150, 225, 300. */
  xTicks?: number[];
  /** Deformability ticks. Default 0, 0.05, 0.10, 0.15, 0.20. */
  yTicks?: number[];
  /** Pixel width of the whole chart. Default 560. */
  width?: number;
  /** Pixel height of the whole chart. Default 320. */
  height?: number;
}

const MARGIN = { top: 28, right: 40, bottom: 44, left: 52 };

/**
 * The one chart component: area on X, deformability on Y, E curves from the
 * LUT, optional population polygon and inspected record. Display only — it
 * never edits populations or computes statistics.
 */
export function AreaDeformChart({
  points,
  isoelastics = [],
  polygon,
  populationLabel,
  inspected,
  legend,
  xDomain = [0, 300],
  yDomain = [0, 0.2],
  xTicks = [0, 75, 150, 225, 300],
  yTicks = [0, 0.05, 0.1, 0.15, 0.2],
  width = 560,
  height = 320,
  className,
  ...rest
}: AreaDeformChartProps) {
  const plotW = width - MARGIN.left - MARGIN.right;
  const plotH = height - MARGIN.top - MARGIN.bottom;
  const sx = (area: number) => MARGIN.left + ((area - xDomain[0]) / (xDomain[1] - xDomain[0])) * plotW;
  const sy = (d: number) => MARGIN.top + (1 - (d - yDomain[0]) / (yDomain[1] - yDomain[0])) * plotH;
  const clip = `mib-chart-clip-${width}-${height}`;
  return (
    <svg
      {...rest}
      role="img"
      aria-label="Area–deformability scatter"
      width={width}
      height={height}
      viewBox={`0 0 ${width} ${height}`}
      className={cx("mib-chart", className)}
    >
      <defs>
        <clipPath id={clip}>
          <rect x={MARGIN.left} y={MARGIN.top} width={plotW} height={plotH} />
        </clipPath>
      </defs>
      <rect className="mib-chart__frame" x={MARGIN.left} y={MARGIN.top} width={plotW} height={plotH} />
      {xTicks.map((t) => (
        <g key={`x${t}`}>
          <line className="mib-chart__grid" x1={sx(t)} x2={sx(t)} y1={MARGIN.top} y2={MARGIN.top + plotH} />
          <text className="mib-chart__tick" x={sx(t)} y={MARGIN.top + plotH + 16} textAnchor="middle">
            {t}
          </text>
        </g>
      ))}
      {yTicks.map((t) => (
        <g key={`y${t}`}>
          <line className="mib-chart__grid" x1={MARGIN.left} x2={MARGIN.left + plotW} y1={sy(t)} y2={sy(t)} />
          <text className="mib-chart__tick" x={MARGIN.left - 8} y={sy(t) + 3} textAnchor="end">
            {t.toFixed(2)}
          </text>
        </g>
      ))}
      <text className="mib-chart__axis-label" x={MARGIN.left + plotW / 2} y={height - 8} textAnchor="middle">
        Area (µm²)
      </text>
      <text className="mib-chart__axis-label" x={MARGIN.left} y={MARGIN.top - 10}>
        Deformability
      </text>
      <g clipPath={`url(#${clip})`}>
        {isoelastics.map((iso) => (
          <polyline
            key={iso.label}
            className="mib-chart__iso"
            points={iso.points.map((p) => `${sx(p.area)},${sy(p.deformability)}`).join(" ")}
          />
        ))}
        {points.map((p, i) => (
          <circle
            key={i}
            cx={sx(p.area)}
            cy={sy(p.deformability)}
            r={2}
            fill={p.condition ? conditionColor(p.condition) : "var(--mib-color-sub)"}
            opacity={p.condition ? 0.8 : 0.45}
          />
        ))}
        {polygon?.length ? (
          <polygon
            className="mib-chart__polygon"
            points={polygon.map((p) => `${sx(p.area)},${sy(p.deformability)}`).join(" ")}
          />
        ) : null}
        {polygon?.map((p, i) => (
          <circle key={`h${i}`} className="mib-chart__handle" cx={sx(p.area)} cy={sy(p.deformability)} r={4} />
        ))}
        {inspected ? (
          <g>
            <circle className="mib-chart__inspected" cx={sx(inspected.area)} cy={sy(inspected.deformability)} r={7} />
            <text className="mib-chart__inspected-label" x={sx(inspected.area) + 11} y={sy(inspected.deformability) + 4}>
              {inspected.label}
            </text>
          </g>
        ) : null}
      </g>
      {isoelastics.map((iso) => {
        const last = iso.points[iso.points.length - 1];
        if (!last) return null;
        const y = Math.max(MARGIN.top + 8, Math.min(sy(last.deformability), MARGIN.top + plotH));
        return (
          <text key={`l${iso.label}`} className="mib-chart__iso-label" x={MARGIN.left + plotW + 6} y={y + 3}>
            {iso.label}
          </text>
        );
      })}
      {legend?.map((entry, i) => (
        <g key={entry.label} transform={`translate(${MARGIN.left + 12 + i * 150}, ${MARGIN.top + 12})`}>
          <rect width={10} height={10} rx={2} fill={conditionColor(entry.condition)} />
          <text className="mib-chart__legend" x={16} y={9}>
            {entry.label}
          </text>
        </g>
      ))}
      {populationLabel ? (
        <text className="mib-chart__caption" x={MARGIN.left + plotW} y={MARGIN.top - 10} textAnchor="end">
          {populationLabel}
        </text>
      ) : null}
    </svg>
  );
}
