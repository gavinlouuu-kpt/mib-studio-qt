// The Monitoring ring histogram (MON-049, MON-050): the valid cells' ring ratios (> 0) in the rolling
// window, binned like Qt (bins = (X max - X min) / bin width, out-of-range values in the end bins), on
// fixed default axes from the Monitoring Settings (X follows the config's ring_ratio_min/max, Y is
// 0..the fixed maximum), with the same wheel zoom, drag pan and double-click reset as the scatter.
import { useEffect, useMemo } from "react";
import { ringHistogram } from "../monitoringChartModel";
import type { MonitoringSettings } from "../monitoringSettings";
import { drawMonitoringHistogram, histogramViewport } from "./drawMonitoringHistogram";
import { paintCanvas, useChartView } from "./useChartView";

export interface MonitoringHistogramProps {
  /** Ring ratios of the valid cells, > 0. */
  values: readonly number[];
  settings: Pick<MonitoringSettings, "histogramXMin" | "histogramXMax" | "histogramYMax" | "histogramBinWidth" | "histogramEpoch">;
}

const MIN_SIZE = { w: 260, h: 200 };

export function MonitoringHistogram({ values, settings }: MonitoringHistogramProps) {
  const { histogramXMin, histogramXMax, histogramYMax, histogramBinWidth, histogramEpoch } = settings;
  const defaults = useMemo(
    () => ({ x: { min: histogramXMin, max: histogramXMax }, y: { min: 0, max: Math.max(1, histogramYMax) } }),
    [histogramXMin, histogramXMax, histogramYMax],
  );
  const hist = useMemo(
    () => ringHistogram(values, histogramXMin, histogramXMax, histogramBinWidth),
    [values, histogramXMin, histogramXMax, histogramBinWidth],
  );
  const view = useChartView({ defaults, epoch: histogramEpoch, viewport: histogramViewport, minSize: MIN_SIZE });
  const { canvasRef, size, ranges } = view;
  useEffect(() => {
    paintCanvas(canvasRef.current, size, (g) => drawMonitoringHistogram(g, size.w, size.h, { hist, ranges }));
  }, [size, ranges, hist]);
  return (
    <div className="mon-chart" data-tour="monitoring-histogram">
      <div ref={view.wrapRef} className="mon-chart-wrap mon-histogram-wrap">
        <canvas
          ref={canvasRef}
          className="mon-chart-canvas"
          role="img"
          aria-label={`Ring-ratio histogram, ${hist.total} samples, ${hist.counts.length} bins of ${hist.binWidth}`}
          style={{ cursor: view.panning ? "grabbing" : "default" }}
          {...view.canvasProps}
        />
      </div>
      <div className="mon-chart-actions">
        <button className="btn" onClick={view.resetZoom} disabled={!view.zoomed} data-tour="monitoring-histogram-reset-zoom">
          Reset zoom
        </button>
      </div>
    </div>
  );
}
