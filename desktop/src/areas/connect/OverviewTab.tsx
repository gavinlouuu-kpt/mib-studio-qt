// Connect and Overview (epic #863): the Overview (Camera & Alignment) stage: the camera window on the
// full sensor, the live canvas, the UX-4 quality gates and the Overview's own camera script (overviewConfig.js).
// Moved from App.tsx (#869 wave 0).
import { egrabberWindowLine, rateSummary, RUN_WINDOW, windowInputLimits } from "../../cameraAlignment";
import { RunWindowNotice } from "../../components/RunWindowNotice";
import { ViewerGate, viewerReason } from "../../components/ViewerGate";
import { GATE_STATUS_LABEL } from "../../quality";
import { useStudio } from "../../studio/StudioContext";
import { CameraStageNotice } from "./CameraStageNotice";
import { cameraModeLead, mindVisionModeLabel } from "./overviewInfo";
import { OverviewScriptEditor } from "./OverviewScriptEditor";
import { RecordingCapacityPanel } from "./RecordingCapacityPanel";
import { ROI_OVERLAY_TITLE, roiOverlayLabel } from "./roiOverlay";
import { WaferGridNotice, WaferGridToggle } from "./WaferGridControls";

export function OverviewTab() {
  const {
    instrumentModes, cameraWin, modeError, setTab, setModeRetry, runFrozen, ring, resumeFromBanner, experimentRequestBusy,
    viewerOnly, setFitWindow, fitWindow, cameraGeometry, cameraWindow, expActive, saveCameraWindow, ready, capacity,
    hostProcessing, roiFields, setRoiFields, onApplyRoi, lastMeta, liveCanvasRef, onWindowPointerDown, onWindowPointerMove,
    onWindowPointerUp, quality, qualityInput, alignBestRef, setAlignImage, caps, overviewScript, roiOverlay, toggleRoiOverlay,
    previewFpsLimit,
  } = useStudio();
  return (
    <>
      {instrumentModes && <RunWindowNotice tab="overview" windowPlaced={cameraWin.placed} modeError={modeError}
        onPlaceWindow={() => setTab("overview")} onRetry={() => setModeRetry((n) => n + 1)} />}
      {runFrozen && (
        <p className="pending-note" role="status" data-testid="stopped-banner">
          Run is stopped to review buffered frames. Resume Run to switch to Align.{" "}
          {ring?.hold_note && <><span className="warn" data-testid="hold-note">{ring.hold_note}</span>{" "}</>}
          <button onClick={() => void resumeFromBanner()} disabled={experimentRequestBusy || viewerOnly} title={viewerReason(viewerOnly, experimentRequestBusy ? "An experiment is starting" : undefined)}>Resume Run (discards the buffered frames)</button>{" "}
          <button onClick={() => setTab("experiment")}>Back to the playback</button>
        </p>
      )}
      <div className="toolbar">
        <button onClick={() => setFitWindow((f) => !f)}>{fitWindow ? "Fit: Window" : "Fit: 1:1"}</button>
        {/* Qt's ROI Overlay toggle: off until a camera is connected; the PZ7035 always shows its Run window. */}
        {cameraGeometry?.supported && !instrumentModes && (
          <button onClick={toggleRoiOverlay} aria-pressed={roiOverlay} title={ROI_OVERLAY_TITLE}>{roiOverlayLabel(roiOverlay)}</button>
        )}

        <ViewerGate>{cameraGeometry?.supported ? (
          <>
            {/* Camera window (ROI 1, sensor coordinates), placed on the full sensor. */}
            {(["x", "y", "width", "height"] as const).map((key) => (
              <label key={key}>
                {key === "x" ? "X" : key === "y" ? "Y" : key === "width" ? "W" : "H"}:{" "}
                <input
                  type="number"
                  aria-label={`Camera window ${key}`}
                  value={cameraWindow ? cameraWindow[key] : 0}
                  {...windowInputLimits(cameraGeometry, key, cameraWindow)}
                  disabled={!cameraWindow || expActive}
                  onChange={(e) => cameraWin.edit(key, Number(e.target.value) || 0)}
                  onBlur={cameraWin.flush}
                  onKeyDown={(e) => { if (e.key === "Enter") cameraWin.flush(); }}
                />
                {key === "width" || key === "height" ? " px" : ""}
              </label>
            ))}
            <button
              className="btn"
              onClick={() => cameraWindow && void saveCameraWindow(cameraWindow)}
              disabled={!ready || expActive || !cameraWindow || cameraGeometry.sensor_width === 0}
              title={cameraGeometry.camera === "egrabber" ? "Save the window now (it is also saved a moment after each change)" : "Save the experiment window; Experiment uses it"}
            >
              Save camera ROI
            </button>
          </>
        ) : hostProcessing ? (
          <>
          <label>
            X: <input type="number" value={roiFields.x} disabled={expActive} title={expActive ? "Stop the experiment before changing ROI" : undefined} onChange={(e) => setRoiFields((r) => ({ ...r, x: e.target.value }))} />
          </label>
          <label>
            Y: <input type="number" value={roiFields.y} disabled={expActive} title={expActive ? "Stop the experiment before changing ROI" : undefined} onChange={(e) => setRoiFields((r) => ({ ...r, y: e.target.value }))} />
          </label>
          <label>
            W: <input type="number" value={roiFields.w} disabled={expActive} title={expActive ? "Stop the experiment before changing ROI" : undefined} onChange={(e) => setRoiFields((r) => ({ ...r, w: e.target.value }))} /> px
          </label>
          <label>
            H: <input type="number" value={roiFields.h} disabled={expActive} title={expActive ? "Stop the experiment before changing ROI" : undefined} onChange={(e) => setRoiFields((r) => ({ ...r, h: e.target.value }))} /> px
          </label>
          <button className="btn" onClick={onApplyRoi} disabled={!ready || expActive} title={expActive ? "Stop the experiment before changing ROI" : undefined}>
            Apply ROI
          </button>
          </>
        ) : null}</ViewerGate>
        <WaferGridToggle />
      </div>
      <CameraStageNotice stage="overview" />
      <WaferGridNotice />
      {cameraGeometry?.supported && (
        <div className="camera-alignment-status" aria-label="Camera mode">
          {cameraGeometry.camera === "mindvision" ? (
            // Qt's MindVision mode label (CONNECT-055): the mode, the camera's measured rate, the display limit.
            <>
              <strong data-testid="mindvision-mode-label">{mindVisionModeLabel(cameraGeometry, previewFpsLimit)}</strong>
              {cameraGeometry.sensor_width > 0 && ` Sensor ${cameraGeometry.sensor_width}×${cameraGeometry.sensor_height}.`}
            </>
          ) : (
            <>
              <strong>{cameraModeLead(cameraGeometry)}</strong>
              {cameraGeometry.overview && cameraGeometry.camera !== "egrabber" && " — drag the yellow box or edit X/Y/W/H, then save; Experiment uses it."}
              {cameraGeometry.camera === "egrabber" && (
                <span>{cameraGeometry.overview ? " — drag the yellow box or edit W/H: each change is snapped and saved to the Experiment script (and to the profile's camera.window when it has one). " : " — "}{egrabberWindowLine(cameraGeometry)}</span>
              )}
              {cameraGeometry.overview && instrumentModes && ` The run window is ${RUN_WINDOW.width}×${RUN_WINDOW.height}: x a multiple of ${RUN_WINDOW.xStep}, y a multiple of ${RUN_WINDOW.yStep} (the producer's offset check).`}
              {rateSummary(cameraGeometry) && <span> {rateSummary(cameraGeometry)}</span>}
            </>
          )}
          {cameraGeometry.overview && !roiOverlay && !instrumentModes && <span data-testid="roi-overlay-hint"> The ROI box is hidden: switch ROI Overlay on to see and drag it.</span>}
        </div>
      )}
      <div className="canvas-wrap">
        {!lastMeta && <span className="canvas-hint">No frame yet — configure a camera and press Start Camera</span>}
        <canvas
          ref={liveCanvasRef}
          className={fitWindow ? "fit" : ""}
          onPointerDown={onWindowPointerDown}
          onPointerMove={onWindowPointerMove}
          onPointerUp={onWindowPointerUp}
          onPointerCancel={onWindowPointerUp}
        />
      </div>

      {/* ---- Recording capacity of the next run (#833): the backend's gates, polled once a second ---- */}
      {hostProcessing && !instrumentModes && <RecordingCapacityPanel view={capacity} />}

      {/* ---- UX-4 image quality gates ---- */}
      <div className="quality-panel" aria-label="Image quality gates">
        <div className="quality-head">
          <h4>Quality gates</h4>
          <span className="quality-summary">
            {quality.pass} pass · {quality.warn} warn · {quality.fail} fail
            {quality.unknown ? ` · ${quality.unknown} unknown` : ""}
          </span>
          {qualityInput.image && (
            <button className="btn small" title="Forget the best focus seen and start again (after changing the sample)"
              onClick={() => { alignBestRef.current = { best: null, box: null }; setAlignImage(null); }}>
              Restart focus peak
            </button>
          )}
        </div>
        <div className="quality-gates">
          {quality.gates.map((g) => (
            <div key={g.id} className={`quality-gate ${g.status}`} title={g.detail}>
              <span className="gate-top">
                <span className={`gate-dot ${g.status}`} aria-hidden="true" />
                <span className="gate-label">{g.label}</span>
              </span>
              <span className="gate-value">{g.value}</span>
              <span
                className="gate-status"
                aria-label={`${g.label}: ${GATE_STATUS_LABEL[g.status]}`}
              >
                {GATE_STATUS_LABEL[g.status]}
              </span>
            </div>
          ))}
        </div>
      </div>

      {/* The Overview's own full-sensor camera script (Qt's script area under the canvas); hidden for a MindVision camera. */}
      {caps.egrabber_script && <ViewerGate><OverviewScriptEditor model={overviewScript} /></ViewerGate>}
      <p className="mono">
        {lastMeta
          ? `#${lastMeta.frame_index} ${lastMeta.width}×${lastMeta.height} stride=${lastMeta.stride_bytes} bytes=${lastMeta.byte_len}`
          : "no frame yet"}
      </p>
    </>
  );
}
