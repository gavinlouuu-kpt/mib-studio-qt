// Shell (epic #862): the UX-8 active-context bar, the status bar and the log drawer.
// Moved from App.tsx without change (#869 wave 0).
import { SEG_STATUS_LABEL } from "../../contextBar";
import { metricNumber } from "../../metricFormat";
import { useStudio } from "../../studio/StudioContext";
import { EXPERIMENT_STATE_NAMES } from "../monitoring/useMonitoring";

export function ContextBar() {
  const { contextBar, setTab } = useStudio();
  return (
    <div className="context-bar" role="region" aria-label="Active context">
      {contextBar.segments.map((s) => {
        const label = `${s.label}: ${s.value} (${SEG_STATUS_LABEL[s.status]})`;
        const clickable = !!s.tab;
        return (
          <button
            key={s.id}
            type="button"
            className={`context-seg ${s.status}${clickable ? " clickable" : ""}`}
            title={s.detail}
            aria-label={label}
            disabled={!clickable}
            onClick={clickable ? () => setTab(s.tab!) : undefined}
          >
            <span className={`seg-dot ${s.status}`} aria-hidden="true" />
            <span className="seg-text">
              <span className="seg-label">{s.label}</span>
              <span className="seg-value">{s.value}</span>
            </span>
          </button>
        );
      })}
    </div>
  );
}

export function StatusBar() {
  const {
    abi, ready, reviewing, recording, setShowLog, showLog, log, displayFps, hostProcessing, algoFps, validFps, invalidFps,
    running, camStatus, dataRate, expState, expActive, expStatus, caps, afStatus, afStaleMs,
  } = useStudio();
  // HW-034 (#868): Qt's status bar ends the camera part with the live median ring width ("Ring width=0.000"). Here it reads
  // an em dash until a first sample exists, and carries the staleness note the sidebar shows.
  const ringKnown = !!afStatus?.valid && afStatus.last_ring_ratio_update_us > 0;
  const ringStale = ringKnown && afStatus.ring_ratio_age_us / 1000 > afStaleMs;
  return (
    <>
      <footer className="statusbar">
        <span>
          Bridge ABI: {abi ?? "…"} · backend: {ready ? "ready" : "not initialized"}
          {reviewing ? " · reviewing" : ""}
          {recording ? " · recording" : ""}
        </span>
        <button className="log-toggle" onClick={() => setShowLog((s) => !s)}>
          Log {showLog ? "▾" : "▸"} ({log.length})
        </button>
        <span className="metrics">
          Display={metricNumber(displayFps, 1)} fps | {hostProcessing
            ? <>Algo={metricNumber(algoFps)}/s | Valid={metricNumber(validFps)}/s | Invalid={metricNumber(invalidFps)}/s</>
            : <>Science=PL</>} | Camera={running ? "running" : camStatus}, {metricNumber(dataRate, 1)} MB/s | {caps.autofocus && <>Ring width=<span title={ringStale ? `Stale: last update ${Math.round(afStatus.ring_ratio_age_us / 1000)} ms ago` : undefined}>{ringKnown ? metricNumber(afStatus.median_ring_ratio, 3) : "—"}</span> | </>}Experiment:{" "}
          {(EXPERIMENT_STATE_NAMES[expState] ?? "Inactive").toLowerCase()}
          {expActive ? ` (buffered ${String(BigInt(expStatus?.valid_buffered ?? "0") + BigInt(expStatus?.invalid_buffered ?? "0"))})` : ""}
        </span>
      </footer>
      {showLog && (
        <div className="log-drawer" role="log">
          {log.length === 0 ? <div>no log entries</div> : log.map((l, i) => <div key={i}>{l}</div>)}
        </div>
      )}
    </>
  );
}
