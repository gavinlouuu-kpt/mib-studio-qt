// Experiment Preview (epic #865): the Preview sub-tab: frame-ring playback, the live / Run preview
// canvas and its status line, processing setup, Record, the preview buffer and the background.
// The configuration sub-tabs below it are areas/config, the LED controls areas/hardware.
// Moved from App.tsx without change (#869 wave 0).
import { bridge } from "../../bridge";
import { BackgroundCalibrationControls } from "../../components/BackgroundCalibrationControls";
import { ProcessedPreview } from "../../components/ProcessedPreview";
import { ProcessingSetupControls } from "../../components/ProcessingSetupControls";
import { RingPlaybackPanel } from "../../components/RingPlaybackPanel";
import { ViewerGate, viewerReason } from "../../components/ViewerGate";
import { metricNumber } from "../../metricFormat";
import { PreviewBufferControls } from "../../previewBuffer";
import { formatResultsRates } from "../../resultsRates";
import { useStudio } from "../../studio/StudioContext";
import { ConfigPanel } from "../config/ConfigPanel";
import { InstrumentLedPanel } from "../hardware/InstrumentLedPanel";

export function PreviewPanel() {
  const {
    runFrozen, ring, onResumeRun, append, experimentRequestBusy, lastMeta, runPreviewInfo, instrumentModes, runMode,
    cameraWin, modeError, previewCanvasRef, fitWindow, instrument, resultsNow, showRunMask, setShowRunMask, faultNote,
    expActive, onStopRun, viewerOnly, hostProcessing, ready, running, caps, backgroundSet, autoBackgroundEnabled,
    refreshConfig, setConfigTab, onToggleRecord, recording, setFitWindow, previewBuffer, tab, expTab, recordReason,
  } = useStudio();
  return (
    <>
      {runFrozen && ring && (
        <RingPlaybackPanel status={ring} fetchFrame={bridge.fetchRingFrame} onResume={onResumeRun} append={append} startPending={experimentRequestBusy} />
      )}
      <div className="canvas-wrap" hidden={runFrozen}>
        {!lastMeta && !runPreviewInfo && <span className="canvas-hint">{instrumentModes
          ? (runMode ? "Waiting for the PL cell capture…"
            : !cameraWin.placed ? "No run window placed yet"
            : modeError?.mode === "run" ? "The camera is not in Run mode" : "Switching to Run…")
          : "No frame yet — configure a camera and press Start Camera"}</span>}
        <canvas ref={previewCanvasRef} className={fitWindow ? "fit" : ""} />
      </div>
      {instrumentModes && runMode && runFrozen && (
        <p className="mono" role="status" data-testid="live-preview-paused">Live preview paused while Run is stopped.</p>
      )}
      {instrumentModes && runMode && !runFrozen && (
        <p className="mono" role="status">
          Run 512×96 at ({instrument?.mode?.run_x}, {instrument?.mode?.run_y}) · frame {runPreviewInfo?.frameId ?? "—"}
          {" · "}listed {runPreviewInfo?.listed ?? "—"} · cells {runPreviewInfo?.cells ?? "—"} · blemishes {runPreviewInfo?.blemishes ?? "—"}
          {" · "}latency max {instrument?.latency ? `${metricNumber(instrument.latency.max_us, 0)} µs` : "—"}
          {instrument?.results?.running && <><br />{formatResultsRates(resultsNow)}
            {(instrument.results.overruns ?? 0) > 0 && ` · ring overruns ${instrument.results.overruns}`}
            {(instrument.results.decode_errors ?? 0) > 0 && ` · decode errors ${instrument.results.decode_errors}`}
            {(instrument.results.sequence_gaps ?? 0) > 0 && ` · frame gaps ${instrument.results.sequence_gaps}`}</>}
          {" · "}<label><input type="checkbox" checked={showRunMask} onChange={(e) => setShowRunMask(e.target.checked)} /> U-Net mask</label>
          {faultNote && <>{" · "}<span className="warn" role="status" data-testid="fault-cleared">{faultNote}</span></>}
          {ring?.available && !runFrozen && !expActive && (
            <>{" · "}<button onClick={() => void onStopRun()} disabled={viewerOnly} title={viewerReason(viewerOnly, `Stop: hold the last ${ring.capacity_frames.toLocaleString("en-US")} frames for playback`)}>Stop</button></>
          )}
        </p>
      )}
      {instrumentModes && runMode && !runFrozen && ring && !ring.available && ring.reason && (
        <p className="pending-note" role="status" data-testid="ring-note">Frame ring: {ring.reason}</p>
      )}
      <InstrumentLedPanel />
      <div className="toolbar" style={{ marginTop: 6 }}>

        {hostProcessing && <span className="legend">
          <span className="chip"><span className="swatch" style={{ background: "#2b6cb0" }} /> Target</span>
          <span className="chip"><span className="swatch" style={{ background: "#1a7f37" }} /> Valid</span>
          <span className="chip"><span className="swatch" style={{ background: "#b42318" }} /> Invalid</span>
        </span>}
        <ViewerGate>
        <ProcessingSetupControls ready={ready} running={running} experimentActive={expActive}
          hostProcessing={hostProcessing} hostBackground={!!caps.host_background} backgroundSet={backgroundSet}
          autoBackgroundEnabled={autoBackgroundEnabled} append={append} refresh={refreshConfig}
          onConfigure={() => setConfigTab("app")} />

        {/* Raw Record is the host camera path; on a PL-science instrument it would save only the preview
            frames (G9), so it is hidden until #649 saves the buffered frames. */}
        {caps.frame_buffer && <button onClick={onToggleRecord} disabled={!running || !!recordReason} title={recordReason ?? (running ? "Record raw frames to an HDF5 file" : "Camera is not running")}>
          {recording ? "Stop Recording" : "Record"}
        </button>}
        </ViewerGate>
        <button onClick={() => setFitWindow((f) => !f)}>{fitWindow ? "Fit: Window" : "Fit: 1:1"}</button>
      </div>
      {caps.frame_buffer && <ViewerGate><PreviewBufferControls model={previewBuffer} /></ViewerGate>}
      {hostProcessing ? (
        <>
          <ProcessedPreview ready={ready} active={tab === "experiment" && expTab === "preview"} />
          <ViewerGate><BackgroundCalibrationControls ready={ready} experimentActive={expActive} onPublished={() => void refreshConfig()} /></ViewerGate>
        </>
      ) : (
        <p className="mono" role="status">Processing runs on the PL for every frame. While an experiment runs its results are counted and recorded (see the sidebar and the Monitoring tab); this view shows the camera's previews.</p>
      )}

      <ConfigPanel />
    </>
  );
}
