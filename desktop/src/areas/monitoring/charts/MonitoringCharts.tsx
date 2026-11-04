// The Monitoring sub-tab's charts (epic #866): the deformability-vs-area scatter with the isoelastic
// reference curves, the ring histogram and the Tauri-only Young's-modulus histogram, all over the rolling
// window of the latest monitoring snapshot. Qt reference: ExperimentMonitoringTab (setupCharts,
// updateScatterplot, updateHistogram). The pieces are MonitoringScatter / MonitoringHistogram; the model
// is monitoringChartModel.ts and the fixed axes are monitoringSettings.ts.
import { useEffect, useMemo, useState, type ReactNode } from "react";
import type { MonitoringSnapshot } from "../../../bridge";
import { metricNumber } from "../../../metricFormat";
import { invoke } from "../../../transport";
import { densityLevels, type MonitoringDensityLayer, type ScatterDensityModel } from "../density/densityModel";
import { DensityToggle } from "../density/DensityToggle";
import { extentHistogram, MONITORING_WINDOW, monitoringChartData, type ScatterData } from "../monitoringChartModel";
import { DEFAULT_MONITORING_SETTINGS, type MonitoringSettings } from "../monitoringSettings";
import type { ReferenceCurve } from "./drawMonitoringScatter";
import { MonitoringHistogram } from "./MonitoringHistogram";
import { MonitoringScatter, type MonitoringScatterProps } from "./MonitoringScatter";

export interface MonitoringChartsViewProps extends Pick<MonitoringScatterProps, "selectedKey" | "onSelectPoint" | "overlay" | "emodulusAt"> {
  snapshot: MonitoringSnapshot | null;
  settings?: MonitoringSettings;
  /** PR E1: the selected cell's pane, docked over the histogram (at the top of its card), from the plotted points
   *  (Previous / Next walk them); null or absent: the histogram alone. */
  histogramPane?: (scatter: ScatterData) => ReactNode;
  /** PR E1: under the scatter, from the plotted points (the gate's live count). */
  scatterFooter?: (scatter: ScatterData) => ReactNode;
  /** The density (KDE) toggle and layer (PR E2); absent = the plain chart, no toggle. */
  density?: MonitoringDensityLayer | null;
}

interface Reference {
  curves: ReferenceCurve[];
  source: string;
}

/** `fetch_monitoring_chart_reference`: {curves: [{modulus, points: [[area µm², deformability]]}], curve_source}. */
function parseReference(text: string): Reference {
  const doc = JSON.parse(text) as { curves?: ReferenceCurve[]; curve_source?: string };
  if (!Array.isArray(doc.curves)) throw new Error("no curves in the reference document");
  // Qt adds the curves highest modulus first, so the legend lists them in that order.
  const curves = doc.curves.filter((c) => Number.isFinite(c.modulus) && Array.isArray(c.points)).sort((a, b) => b.modulus - a.modulus);
  return { curves, source: String(doc.curve_source ?? "") };
}

export function MonitoringChartsView(props: MonitoringChartsViewProps) {
  const { snapshot, selectedKey, onSelectPoint, overlay, emodulusAt, histogramPane, scatterFooter, density } = props;
  const settings = props.settings ?? DEFAULT_MONITORING_SETTINGS;
  const data = useMemo(() => monitoringChartData(snapshot?.valid ? snapshot.rows : []), [snapshot]);
  // The density layer: a level per plotted point, only while the toggle is on (off: no model, the plain chart).
  const densityOn = !!density?.on, densityByRow = density?.byRow ?? null;
  const levels = useMemo(() => (densityOn ? densityLevels(data.scatter.rows, densityByRow) : null), [densityOn, data.scatter, densityByRow]);
  const coreLoops = density?.contours, referenceLoops = density?.reference;
  const scatterDensity = useMemo<ScatterDensityModel | undefined>(
    () => (levels && coreLoops && referenceLoops ? { levels, contours: coreLoops, reference: referenceLoops } : undefined),
    [levels, coreLoops, referenceLoops],
  );
  const modulus = useMemo(() => extentHistogram(data.moduli), [data.moduli]);
  const [reference, setReference] = useState<Reference | null>(null);
  const [referenceError, setReferenceError] = useState("");
  useEffect(() => {
    let live = true;
    invoke<string>("fetch_monitoring_chart_reference")
      .then((text) => {
        if (live) setReference(parseReference(text));
      })
      .catch((e) => {
        if (live) setReferenceError(String(e));
      });
    return () => {
      live = false;
    };
  }, []);
  const curves = reference?.curves ?? NO_CURVES;
  const maxModulusBin = Math.max(1, ...modulus.counts);
  const compact = (value: number) => metricNumber(value, 4, true);
  return (
    <>
      <div className="config-group mon-chart-group mon-scatter-group">
        <h5>Deformability vs area (µm²)</h5>
        {density && <DensityToggle on={density.on} toggle={density.toggle} />}
        <MonitoringScatter
          data={data.scatter}
          curves={curves}
          settings={settings}
          selectedKey={selectedKey}
          onSelectPoint={onSelectPoint}
          overlay={overlay}
          emodulusAt={emodulusAt}
          density={scatterDensity}
        />
        {scatterFooter?.(data.scatter)}
        {!snapshot?.valid || !data.scatter.xs.length ? <p>No calibrated finite valid-object samples available.</p> : null}
        <p>
          Valid objects only; green: valid, blue: target group; newest {MONITORING_WINDOW} valid rows, each point with its exact
          analysis-time calibration. {data.uncalibrated} valid rows lack analysis-time calibration and are omitted. Historical rows are
          never recalibrated with current settings.
        </p>
        {reference && (
          <p>
            Isoelastic reference curves: {reference.source || "bundled reference"}. Reference conditions are not inferred from current
            hardware settings.
          </p>
        )}
        {referenceError && <p>Isoelastic reference unavailable: {referenceError}</p>}
      </div>
      <div className="config-group mon-chart-group mon-histogram-group">
        {histogramPane?.(data.scatter)}
        <h5>Ring-ratio distribution (dimensionless)</h5>
        <MonitoringHistogram values={data.ratios} settings={settings} />
        {!snapshot?.valid || !data.ratios.length ? <p>No finite ring-ratio samples available.</p> : null}
        <p>
          {data.ratios.length} valid-object positive finite samples. This is ring ratio, not a physical ring-width measurement.
        </p>
      </div>
      <div className="config-group mon-chart-group mon-modulus-group">
        <h5>Young’s modulus distribution (kPa)</h5>
        {!snapshot?.valid || !data.moduli.length ? (
          <p>No positive finite modulus measurements available for valid objects.</p>
        ) : (
          <svg viewBox="0 0 390 210" role="img" aria-label={`Young's modulus histogram, ${modulus.total} valid-object samples, ten bins`}>
            <path d="M45 25V165H350" fill="none" stroke="currentColor" />
            {modulus.counts.map((count, index) => (
              <rect key={index} x={46 + index * 30} y={165 - (count / maxModulusBin) * 135} width="28" height={(count / maxModulusBin) * 135} fill="#4ade80">
                <title>
                  Bin {index + 1}: {count} samples
                </title>
              </rect>
            ))}
            <text x="40" y="36" textAnchor="end" fontSize="11" fill="currentColor">{maxModulusBin}</text>
            <text x="45" y="185" fontSize="11" fill="currentColor">{compact(modulus.min)}</text>
            <text x="345" y="185" textAnchor="end" fontSize="11" fill="currentColor">{compact(modulus.max)}</text>
            <text x="195" y="205" textAnchor="middle" fontSize="11" fill="currentColor">Young’s modulus (kPa)</text>
          </svg>
        )}
        <p>Stored per-object LUT result; no recalculation with current calibration. Zero/unavailable and non-finite values excluded.</p>
      </div>
    </>
  );
}

const NO_CURVES: readonly ReferenceCurve[] = [];
