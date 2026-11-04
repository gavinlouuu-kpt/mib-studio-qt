// Experiment start/stop and Monitoring (epic #866): the Monitoring sub-tab: buffer counts, the charts
// beside the tune panel (TunePanel.tsx), the live valid / invalid cell strips (CellStrips.tsx) and the latest
// result rows. The trigger tests in its toolbar are areas/hardware. Moved from App.tsx (#869 wave 0); the tune
// panel replaced its processing-settings link; Clear Buffer's question and the polling failure notices are
// #866 additions.
import { bridge } from "../../bridge";
import { ResultMetricCells } from "../../components/ResultMetricCells";
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";
import { ask } from "../../transport/dialogs";
import { TriggerTestControls } from "../hardware/TriggerTestControls";
import { CellStrips } from "./CellStrips";
import { LiveMonitoringCharts } from "./LiveMonitoringCharts";
import { clearMonitoringBuffer } from "./clearBuffer";
import { TunePanel } from "./TunePanel";
import { latestRows, MONITORING_TABLE_ROWS } from "./monitoringChartModel";
import { isMonitoringVisible } from "./useMonitoring";

export function MonitoringPanel() {
  const {
    append, ready, caps, trigStatus, monSnapshot, tab, expTab, setMonSnapshot, notifyMonitoringCleared, monitoringNotices,
    validOverlay, setValidOverlay,
  } = useStudio();
  return (
    <>
      <div className="toolbar">
        <ViewerGate>
        <button
          onClick={() => void clearMonitoringBuffer({
            ask,
            clear: () => bridge.monitoringClear(),
            // The charts and the table empty at once instead of at the next poll; the listeners are where a later PR clears
            // the KDE state and the selection (MON-029).
            onCleared: () => { setMonSnapshot(null); notifyMonitoringCleared(); },
            append,
          })}
          disabled={!ready}
          data-tour="monitoring-clear-buffer"
        >
          Clear Buffer
        </button>
        </ViewerGate>
        <TriggerTestControls />
        <span className="mono right">
          {caps.trigger ? `triggers ${trigStatus?.trigger_count ?? 0} · ` : ""}buffered v{monSnapshot?.valid_held ?? 0}/i
          {monSnapshot?.invalid_held ?? 0} · evicted{" "}
          {Math.max(
            0,
            (monSnapshot?.valid_appended ?? 0) +
              (monSnapshot?.invalid_appended ?? 0) -
              (monSnapshot?.valid_held ?? 0) -
              (monSnapshot?.invalid_held ?? 0),
          )}
        </span>
      </div>
      {monitoringNotices.map((notice) => <p key={notice} role="alert" className="monitoring-notice" data-tour="monitoring-notice">{notice}</p>)}
      <div className="monitoring-split">
        <div className="config-grid monitoring-charts">
          <LiveMonitoringCharts snapshot={monSnapshot} />
        </div>
        <TunePanel />
      </div>
      <CellStrips ready={ready} visible={isMonitoringVisible(tab, expTab)} validOverlay={validOverlay} onValidOverlay={setValidOverlay} />
      <div className="table-panel" style={{ maxHeight: 180, overflow: "auto" }}>
        <table className="metrics-table">
          <thead>
            <tr>
              <th>Frame</th>
              <th>Object</th>
              <th>Track</th>
              <th>Valid</th>
              <th>Target</th>
              <th>Area (raw px²)</th>
              <th>Deformability</th>
              <th>Ring ratio</th>
              <th>E (kPa)</th>
            </tr>
          </thead>
          <tbody>
            {latestRows(monSnapshot?.rows ?? [], MONITORING_TABLE_ROWS).map((r) => (
              <tr key={`${r.frame_index}:${r.object_id}`}>
                <td>{r.frame_index}</td>
                <td>{r.object_id}</td>
                <td>{r.track_id}</td>
                <td>{r.valid ? "yes" : "no"}</td>
                <td>{r.target_group ? "yes" : "no"}</td>
                <ResultMetricCells row={r} />
              </tr>
            ))}
            {(monSnapshot?.rows?.length ?? 0) === 0 && (
              <tr>
                <td colSpan={9} style={{ color: "#777" }}>
                  No monitoring rows yet — rows appear while capture + realtime processing run with Monitoring
                  visible.
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </>
  );
}
