// The Monitoring charts (epic #866). The implementation is areas/monitoring/charts/; this module keeps
// the `<MonitoringCharts snapshot={...}/>` call of MonitoringPanel working: the fixed axes come from
// the Monitoring Settings in the studio state when it is mounted inside the classic view, from the Qt
// defaults otherwise (tests, a standalone use), and `settings` overrides both.
import { useContext } from "react";
import { MonitoringChartsView, type MonitoringChartsViewProps } from "../areas/monitoring/charts/MonitoringCharts";
import { DEFAULT_MONITORING_SETTINGS } from "../areas/monitoring/monitoringSettings";
import { StudioContext } from "../studio/StudioContext";

export type { ScatterOverlay } from "../areas/monitoring/charts/MonitoringScatter";

export function MonitoringCharts(props: MonitoringChartsViewProps) {
  const studio = useContext(StudioContext);
  return (
    <MonitoringChartsView
      {...props}
      settings={props.settings ?? studio?.monSettings ?? DEFAULT_MONITORING_SETTINGS}
      density={props.density ?? studio?.kdeLayer}
    />
  );
}
