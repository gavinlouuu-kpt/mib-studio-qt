// The "Density (KDE)" toggle beside the Monitoring scatter (epic #866 PR E2: MON-058, MON-059). Qt's
// `kdeToggleCheck` in the tab's top row: a checkbox whose tooltip carries the point count, bandwidth, compute
// time, refresh spacing, core cells and loops and the reference label (densityModel.ts densityTooltip).
//
// It sits inside a ViewerGate: switching the density on sends `monitoring_density_set_settings`, a control
// command, and the service is one per instrument. A viewer therefore cannot switch it on (the gate disables the
// box and gives the reason); the tab then draws the plain chart for it (useMonitoringDensity.ts).
import { VIEWER_ONLY_REASON, ViewerGate, useViewerOnly } from "../../../components/ViewerGate";
import { LEVEL_COLOURS, type DensityToggleModel } from "./densityModel";

export interface DensityToggleProps {
  on: boolean;
  toggle: DensityToggleModel;
}

/** Eight swatches, sparsest to densest: the ramp the points are coloured with. */
function DensityKey() {
  return (
    <span className="mon-density-key" data-tour="monitoring-density-key">
      <span>sparse</span>
      {LEVEL_COLOURS.map((colour, level) => (
        <i key={level} style={{ background: colour }} />
      ))}
      <span>dense</span>
    </span>
  );
}

export function DensityToggle({ on, toggle }: DensityToggleProps) {
  const viewerOnly = useViewerOnly();
  return (
    <div className="mon-density">
      <ViewerGate>
        <label className="mon-density-toggle" title={viewerOnly ? VIEWER_ONLY_REASON : toggle.tooltip}>
          <input type="checkbox" checked={on} onChange={(e) => toggle.onChange(e.target.checked)} data-tour="monitoring-density-toggle" />
          Density (KDE)
        </label>
      </ViewerGate>
      {on && <DensityKey />}
      {toggle.notice && (
        <p role="status" className="mon-density-notice" data-tour="monitoring-density-notice">
          {toggle.notice}
        </p>
      )}
    </div>
  );
}
