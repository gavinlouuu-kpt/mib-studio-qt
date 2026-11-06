// Shell (epic #862): the collapsible telemetry sidebar. Moved from App.tsx without change (#869 wave 0).
// Epic #866 (monitoring) made small edits here: the readouts (Algo Time, Camera, Flush Status, Runtime) come from
// areas/monitoring/statsReadouts, the background thumbnail is areas/monitoring/BackgroundThumbnail.
import { EXPERIMENT_STATES } from "../../bridgeContract";
import { experimentCounterRows } from "../../experimentCounters";
import { latencyReadout, linkReadout, sensorReadout } from "../../linkHealth";
import { metricNumber } from "../../metricFormat";
import { useStudio } from "../../studio/StudioContext";
import { BackgroundThumbnail } from "../monitoring/BackgroundThumbnail";
import { algoTimeReadout, cameraRateReadouts, cameraStatusReadout, flushReadout, runtimeText } from "../monitoring/statsReadouts";
import { EXPERIMENT_STATE_NAMES } from "../monitoring/useMonitoring";

function SideRow(props: { k: string; v: string; cls?: string; title?: string }) {
  return (
    <div className="side-row" title={props.title}>
      <span className="k">{props.k}</span>
      <span className={`v ${props.cls ?? ""}`}>{props.v}</span>
    </div>
  );
}

export function TelemetrySidebar() {
  const {
    sidebarCollapsed, caps, backgroundSet, displayFps, hostProcessing, stats, algoFps, validFps, invalidFps, running,
    dataRate, pz7035, instrument, afStatus, expState, expActive, runOutcome, expStatus, elapsedWallSeconds,
    toggleSidebar, ready, backgroundRevision,
  } = useStudio();
  const algoTime = algoTimeReadout(stats?.algo_avg_us, stats?.algo_avg_age_ms);
  const camStatus = cameraStatusReadout(running);
  // The capture rate and data rate are the backend's own (processing_stats.camera, ABI next (#886, set at merge), the same reply as the algorithm time).
  const camera = stats?.telemetry?.camera ?? null;
  const camRates = cameraRateReadouts({
    running, displayFps, dataRateMBs: dataRate, captureFps: camera?.frame_rate, captureMBs: camera?.data_rate_mbps, telemetryError: stats?.telemetry_error,
  });
  const flush = flushReadout(!!expStatus?.flushing);
  return (
    <>
      <aside className={`sidebar ${sidebarCollapsed ? "collapsed" : ""}`} aria-label="Telemetry sidebar">
        {caps.host_background && <div className="side-section">
          <BackgroundThumbnail backgroundSet={backgroundSet} revision={backgroundRevision} ready={ready} />
        </div>}
        <div className="side-section">
          <h4>Display</h4>
          <SideRow k="FPS:" v={metricNumber(displayFps, 1)} />
        </div>
        <div className="side-section">
          <h4>Processing</h4>
          {!hostProcessing && <SideRow k="Runs on:" v="PL (every frame)" />}
          {hostProcessing && algoTime && <SideRow k="Algo Time:" v={algoTime.text} cls={algoTime.cls} title={algoTime.title} />}
          {hostProcessing && <SideRow k="Algo FPS:" v={stats?.valid ? metricNumber(algoFps) : "—"} cls={stats?.valid ? "" : "dim"} />}
          {hostProcessing && <SideRow k="Valid FPS:" v={stats?.valid ? metricNumber(validFps) : "—"} cls={stats?.valid ? "" : "dim"} />}
          {hostProcessing && <SideRow k="Invalid FPS:" v={stats?.valid ? metricNumber(invalidFps) : "—"} cls={stats?.valid ? "" : "dim"} />}
          <SideRow k="px→µm:" v={stats?.valid ? String(stats.pixel_to_micron) : "—"} cls={stats?.valid ? "" : "dim"} />
        </div>
        <div className="side-section">
          <h4>Camera</h4>
          <SideRow k="Status:" v={camStatus.text} cls={camStatus.cls} />
          <SideRow k="FPS:" v={camRates.fps.text} title={camRates.fps.title} />
          <SideRow k="Data Rate:" v={camRates.dataRate.text} title={camRates.dataRate.title} />
        </div>
        {pz7035 && <div className="side-section" title={instrument && !instrument.available && instrument.error ? instrument.error : "PZ7035 PL core and health (#501)"}>
          <h4>PL core</h4>
          {/* No PL device (a replay, or the bridge not loaded) reads "no PL device", not "—" (#550 m12). */}
          <SideRow k="Build:" v={instrument?.core ? instrument.core.build_id.slice(0, 8) || "—" : instrument && !instrument.available ? "no PL device" : "—"}
            cls={instrument?.core?.build_match === "match" ? "ok" : "dim"} />
          <SideRow k="Weights:" v={instrument?.core ? instrument.core.profile_id.slice(0, 8) || "—" : instrument && !instrument.available ? "no PL device" : "—"}
            cls={instrument?.core?.profile_match === "match" ? "ok" : "dim"} />
          <SideRow k="LED:" v={instrument?.led ? (instrument.led.guard_fault ? "GUARD TRIPPED" : instrument.led.on ? `${instrument.led.preset} ${instrument.led.delay_us}/${instrument.led.width_us} µs` : "off") : "—"}
            cls={instrument?.led && !instrument.led.guard_fault ? "" : "dim"} />
          <SideRow k="Sensor:" v={sensorReadout(instrument).text} cls={sensorReadout(instrument).cls} />
          <SideRow k="Link:" v={linkReadout(instrument).text} cls={linkReadout(instrument).cls} />
          <SideRow k="Latency:" v={latencyReadout(instrument).text} cls={latencyReadout(instrument).cls} />
        </div>}
        {caps.autofocus && <div className="side-section">
          <h4>Autofocus</h4>
          <SideRow
            k="Ring width:"
            v={afStatus?.valid && afStatus.last_ring_ratio_update_us > 0 ? metricNumber(afStatus.median_ring_ratio, 3) : "—"}
            cls={afStatus?.valid && afStatus.last_ring_ratio_update_us > 0 ? "" : "dim"}
          />
          <SideRow
            k="Controller:"
            v={afStatus?.connected ? `connected (${afStatus.enabled ? "auto" : "manual"})` : "disconnected"}
            cls={afStatus?.connected ? "ok" : "dim"}
          />
        </div>}
        <div className="side-section">
          <h4>Experiment</h4>
          <SideRow
            k="Status:"
            v={EXPERIMENT_STATE_NAMES[expState] ?? "Inactive"}
            cls={expActive ? "ok" : expState === EXPERIMENT_STATES.Failed ? "" : "dim"}
          />
          {runOutcome && !expActive && (
            <SideRow
              k="Last run:"
              v={{ ok: "Complete", partial: "Partial (declared)", loss: "Undeclared loss", failed: "Failed", unknown: "Unknown", legacy: "No accounting" }[runOutcome.severity]}
              cls={runOutcome.severity === "ok" ? "ok" : ""}
            />
          )}
          <SideRow k="Valid Buffered:" v={expStatus?.valid ? expStatus.valid_buffered : "Unavailable"} />
          <SideRow k="Invalid Buffered:" v={expStatus?.valid ? expStatus.invalid_buffered : "Unavailable"} />
          <SideRow k="Flush Status:" v={flush.text} cls={flush.cls} />
          {experimentCounterRows(expStatus).map(([label, value]) => <SideRow key={label} k={label} v={value} />)}
          <SideRow k="Runtime:" v={runtimeText(expActive, elapsedWallSeconds)} />
        </div>
        {caps.autofocus && <div className="side-section" title="Live backend state of the nanopositioner autofocus. Connect and configure it in the Connect tab, under Autofocus / nanopositioner.">
          <h4>Nanopositioner Autofocus</h4>
          <SideRow
            k="Endpoint:"
            v={afStatus?.connected ? (afStatus.endpoint_id || `${afStatus.backend_name || "Controller"} COM${afStatus.com_port}`) : "Disconnected"}
            cls={afStatus?.connected ? "" : "dim"}
          />
          <SideRow
            k="Voltage:"
            v={afStatus?.connected ? `${metricNumber(afStatus.current_voltage, 1)} V` : "—"}
            cls={afStatus?.connected ? "" : "dim"}
          />
          <SideRow
            k="Metric age:"
            v={
              afStatus?.valid && afStatus.last_ring_ratio_update_us > 0
                ? `${metricNumber(afStatus.ring_ratio_age_us / 1000, 0)} ms`
                : "—"
            }
            cls={afStatus?.valid && afStatus.last_ring_ratio_update_us > 0 ? "" : "dim"}
          />
        </div>}
      </aside>
      <button
        className="sidebar-toggle"
        onClick={toggleSidebar}
        title={sidebarCollapsed ? "Show telemetry sidebar" : "Hide telemetry sidebar"}
        aria-label={sidebarCollapsed ? "Show telemetry sidebar" : "Hide telemetry sidebar"}
      >
        {sidebarCollapsed ? "▶" : "◀"}
      </button>
    </>
  );
}
