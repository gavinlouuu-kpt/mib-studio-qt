// Experiment start/stop and Monitoring (epic #866): the Monitoring sub-tab: buffer counts, charts,
// the processing-settings link and the latest result rows. The trigger tests in its toolbar are
// areas/hardware. Moved from App.tsx (#869 wave 0); Clear Buffer's question and the polling failure
// notices are #866 additions.
import { bridge } from "../../bridge";
import { MonitoringCharts } from "../../components/MonitoringCharts";
import { ResultMetricCells } from "../../components/ResultMetricCells";
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";
import { ask } from "../../transport/dialogs";
import { TriggerTestControls } from "../hardware/TriggerTestControls";
import { clearMonitoringBuffer } from "./clearBuffer";

export function MonitoringPanel() {
  const {
    append, ready, caps, trigStatus, monSnapshot, expActive, setExpTab, setConfigTab, setMonSnapshot, notifyMonitoringCleared,
    monitoringNotices,
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
      <div className="config-grid" style={{ flex: 1 }}>
        <MonitoringCharts snapshot={monSnapshot} />
        <div className="config-group">
          <h5>Processing settings</h5>
          <button disabled={expActive} onClick={() => {setExpTab("preview");setConfigTab("app");}}>Edit processing configuration</button>
          <p className="pending-note">
            Filter thresholds, target groups and multi-image settings use the checked processing configuration. Changes are locked during an experiment.
          </p>
        </div>
      </div>
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
            {(monSnapshot?.rows ?? []).slice(-15).map((r) => (
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
