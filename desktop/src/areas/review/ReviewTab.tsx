// Review (epic #867): MIB Studio's Review stage: open/close, exports, the frame and image scrubbers,
// charts and the metrics table. Moved from App.tsx without change (#869 wave 0).
import { bridge } from "../../bridge";
import { ResultMetricCells } from "../../components/ResultMetricCells";
import { ReviewCharts } from "../../components/ReviewCharts";
import { RunOutcomeNotice } from "../../components/RunOutcomeNotice";
import { SavedReviewImage } from "../../components/SavedReviewImage";
import { ViewerGate } from "../../components/ViewerGate";
import { ReviewExportOptions } from "../../exportControls";
import { ReanalysisControls } from "../../reanalysisControls";
import { noImagesNotice } from "../../review/noImages";
import { OpenFailureDialog, ReviewStatus, reviewStatusText } from "../../review/reviewOpen";
import { useStudio } from "../../studio/StudioContext";
import { reviewLockReason } from "./reviewLock";

export function ReviewTab() {
  const {
    onSelectHdf, ready, reviewSourceBusy, reviewMeta, expActive, recording, reviewSourcePending, setReviewSourceBusy,
    reviewSourceGeneration, append, clearReviewSource, reviewExport, stats, reviewing, reviewPath, reviewTab,
    metricsGeneration, setMetricsPage, setReviewTab, setReviewImgIndex, loadMetricsPage, reviewOutcome, caps, reanalysis,
    reviewImgIndex, fitWindow, reviewCanvasRef, reviewIndex, onScrub, metricsPage, metricsOffset, METRICS_PAGE_SIZE,
    reviewOpenPhase, reviewOpenFailure, setReviewOpenFailure,
  } = useStudio();
  // Qt locks the Review tab while an experiment is active (REVIEW-007); a raw recording refuses the load too.
  const reviewLock = reviewLockReason(expActive, recording);
  // A recording file (REVIEW-006): one "Frames" view, no valid/invalid sets, no metrics or charts.
  const recordingFile = !!reviewMeta?.recording_file;
  // The file's own px→µm factor (recorded, else the live one); an older backend does not say.
  const areaFactor = reviewMeta?.file_open && !recordingFile && (reviewMeta.pixel_to_micron ?? 0) > 0 ? reviewMeta.pixel_to_micron : undefined;
  return (
    <fieldset className="review-lock" disabled={!!reviewLock} title={reviewLock ?? undefined} data-tour="review-stage" data-review-lock={reviewLock ? "locked" : "open"}>
      <div className="toolbar">
        <ViewerGate>
        <button onClick={onSelectHdf} disabled={!ready || reviewSourceBusy || !!reviewLock} title={reviewLock ?? (ready ? undefined : "Backend is not initialized")}>
          Select HDF File…
        </button>
        <button disabled={!reviewMeta?.file_open || expActive || recording || reviewSourceBusy} onClick={async () => {
          if(reviewSourcePending.current)return;reviewSourcePending.current=true;setReviewSourceBusy(true);++reviewSourceGeneration.current;
          try {
          const result = await bridge.closeReview();
          if (!result.ok) return append(result.message);
          clearReviewSource();
          }catch(e){append(`Close review failed: ${e}`);}finally{reviewSourcePending.current=false;setReviewSourceBusy(false);}
        }}>Close File</button>
        <button
          onClick={() => void reviewExport.start("metrics_csv", stats?.valid ? stats.pixel_to_micron ?? undefined : undefined)}
          disabled={!reviewMeta?.file_open || reviewMeta.recording_file || reviewExport.busy || reviewSourceBusy}
          title={reviewMeta?.file_open ? "Export frame/object metrics as a cancellable job" : "No file loaded"}
        >
          Export Metrics to CSV…
        </button>
        <button disabled={!reviewMeta?.file_open || reviewExport.busy || reviewSourceBusy} onClick={() => void reviewExport.start("all", stats?.valid ? stats.pixel_to_micron ?? undefined : undefined)}>Export All…</button>
        <button disabled={!reviewMeta?.file_open || reviewExport.busy || reviewSourceBusy} onClick={() => void reviewExport.start("images", stats?.valid ? stats.pixel_to_micron ?? undefined : undefined)}>Export Images…</button>
        <button disabled={!reviewMeta?.file_open || reviewMeta.recording_file || reviewExport.busy || reviewSourceBusy} onClick={() => void reviewExport.start("charts")}>Export Charts…</button>
        <button disabled={!ready || reviewExport.busy || reviewSourceBusy} onClick={() => void reviewExport.start("metrics_csv", undefined, true)}>Batch Metrics…</button>
        <button disabled={!ready || reviewExport.busy || reviewSourceBusy} onClick={() => void reviewExport.start("all", undefined, true)}>Batch Export All…</button>
        </ViewerGate>

        <span className="legend">
          <span className="chip"><span className="swatch" style={{ background: "#2b6cb0" }} /> Target</span>
          <span className="chip"><span className="swatch" style={{ background: "#1a7f37" }} /> Valid</span>
          <span className="chip"><span className="swatch" style={{ background: "#b42318" }} /> Invalid</span>
        </span>
        <span className="path-label right">
          {reviewing
            ? `${reviewPath}${reviewMeta?.valid ? ` · ${reviewMeta.recording_file ? "recording" : "experiment"} · valid ${reviewMeta.total_valid}, invalid ${reviewMeta.total_invalid}${reviewMeta.has_core_identity ? ` · core v${reviewMeta.core_version}` : ""}` : ""}`
            : "No file selected"}
        </span>
        <ReviewStatus text={reviewStatusText(reviewOpenPhase, reviewMeta)} />
      </div>
      {reviewLock && <p className="pending-note" role="status" data-testid="review-locked" data-tour="review-locked">{reviewLock}</p>}
      {noImagesNotice(reviewMeta) && <p className="pending-note" role="status" data-testid="review-no-images">{noImagesNotice(reviewMeta)}</p>}
      <div className="subtabs" role="tablist" aria-label="Review views">
        <button className={reviewTab === "raw" ? "active" : ""} onClick={() => { ++metricsGeneration.current; setMetricsPage(null); setReviewTab("raw"); }}>
          {recordingFile ? "Frames" : "Raw Frames"}
        </button>
        {!recordingFile && <button
          className={reviewTab === "valid" ? "active" : ""}
          disabled={!reviewMeta?.valid_images.present}
          title={reviewMeta?.valid_images.present ? undefined : "No valid-frame images in this file"}
          onClick={async () => {
            setReviewTab("valid");
            setReviewImgIndex(0);
            await loadMetricsPage(true, 0);
          }}
        >
          Valid Frames
        </button>}
        {!recordingFile && <button
          className={reviewTab === "invalid" ? "active" : ""}
          disabled={!reviewMeta?.invalid_images.present}
          title={reviewMeta?.invalid_images.present ? undefined : "No invalid-frame images in this file"}
          onClick={async () => {
            setReviewTab("invalid");
            setReviewImgIndex(0);
            await loadMetricsPage(false, 0);
          }}
        >
          Invalid Frames
        </button>}
        <button className={reviewTab === "charts" ? "active" : ""} disabled={!reviewMeta?.file_open || reviewMeta.recording_file} onClick={() => setReviewTab("charts")}>Charts</button>
      </div>
      <div className="subtab-body">
        <RunOutcomeNotice outcome={reviewOutcome} />
        <ReviewExportOptions model={reviewExport}/>
        {caps.reanalysis && <ViewerGate><ReanalysisControls model={reanalysis} metadata={reviewMeta} blocked={reviewExport.busy || reviewSourceBusy || !ready}/></ViewerGate>}
        {reviewTab === "charts" && <ReviewCharts sourcePath={reviewMeta?.file_path ?? ""}/>}
        <div className="review-split" style={reviewTab === "charts" ? { display: "none" } : undefined}>
          <div className="frames">
            {reviewMeta?.file_open && (reviewTab === "valid" || reviewTab === "invalid") && <SavedReviewImage metadata={reviewMeta} valid={reviewTab === "valid"} index={reviewImgIndex} fit={fitWindow}/>}
            <div className="canvas-wrap" style={{display:reviewTab === "raw" ? undefined : "none"}}>
              {!reviewing && <span className="canvas-hint">No recording loaded — Select HDF File…</span>}
              <canvas ref={reviewCanvasRef} className={fitWindow ? "fit" : ""} />
            </div>
            {reviewing && reviewTab === "raw" && (reviewMeta?.recorded_images.count ?? 0) > 0 && (
              <>
                <input
                  type="range"
                  className="scrub"
                  min={0}
                  max={Math.max(0, (reviewMeta?.recorded_images.count ?? 0) - 1)}
                  value={reviewIndex}
                  onChange={(e) => onScrub(e.target.value)}
                  title="Choose recorded frame"
                  aria-label="Frame scrubber"
                />
                <span className="mono">
                  frame {reviewIndex} of {reviewMeta?.recorded_images.count ?? 0} recorded frames
                </span>
              </>
            )}
            {reviewing && (reviewTab === "valid" || reviewTab === "invalid") && (
              <>
                <input
                  type="range"
                  className="scrub"
                  min={0}
                  max={Math.max(
                    0,
                    (reviewTab === "valid"
                      ? reviewMeta?.valid_images.count ?? 0
                      : reviewMeta?.invalid_images.count ?? 0) - 1,
                  )}
                  value={reviewImgIndex}
                  onChange={async (e) => {
                    const idx = Number(e.target.value);
                    setReviewImgIndex(idx);
                  }}
                  aria-label="Review image scrubber"
                />
                <span className="mono">
                  image {reviewImgIndex + 1} of{" "}
                  {reviewTab === "valid"
                    ? reviewMeta?.valid_images.count ?? 0
                    : reviewMeta?.invalid_images.count ?? 0}
                </span>
              </>
            )}
          </div>
          <div className="table-panel">
            <table className="metrics-table">
              <thead>
                {recordingFile ? (
                  <tr>
                    <th>Index</th>
                    <th>Timestamp</th>
                  </tr>
                ) : (
                  <tr>
                    <th>Index</th>
                    <th>Object Id</th>
                    <th>Track Id</th>
                    <th>Area (px²)</th>
                    {areaFactor !== undefined && <th data-tour="review-area-um2" title="Area in px² × (the file's px→µm factor)²">Area (µm²)</th>}
                    <th>Deformability</th>
                    <th>Ring ratio</th>
                    <th>E (kPa)</th>
                  </tr>
                )}
              </thead>
              <tbody>
                {(metricsPage?.rows ?? []).map((r, rowIndex) => (
                  <tr key={`${r.frame_index}:${r.object_id}`} tabIndex={0}
                    aria-selected={reviewImgIndex === metricsOffset + rowIndex}
                    onClick={() => setReviewImgIndex(metricsOffset + rowIndex)}
                    onKeyDown={(event) => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); setReviewImgIndex(metricsOffset + rowIndex); } }}>

                    <td>{r.frame_index}</td>
                    {recordingFile ? <td>{r.timestamp_ns}</td> : <>
                      <td>{r.object_id}</td>
                      <td>{r.track_id}</td>
                      <ResultMetricCells row={r} areaUm2Factor={areaFactor} />
                    </>}
                  </tr>
                ))}
                {(metricsPage?.rows?.length ?? 0) === 0 && (
                  <tr>
                    <td colSpan={recordingFile ? 2 : areaFactor !== undefined ? 8 : 7} style={{ color: "#777" }}>
                      {reviewing ? "No metric rows in this table." : "Load a file to see frame/object metrics."}
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
            {reviewing && (metricsPage?.total ?? 0) > METRICS_PAGE_SIZE && (
              <div className="toolbar" style={{ padding: 4 }}>
                <button
                  className="btn"
                  disabled={metricsOffset === 0}
                  onClick={() => loadMetricsPage(reviewTab !== "invalid", Math.max(0, metricsOffset - METRICS_PAGE_SIZE))}
                >
                  ◀ Prev
                </button>
                <span className="mono">
                  {metricsOffset + 1}–{Math.min(metricsPage?.total ?? 0, metricsOffset + METRICS_PAGE_SIZE)} of {metricsPage?.total ?? 0}
                </span>
                <button
                  className="btn"
                  disabled={metricsOffset + METRICS_PAGE_SIZE >= (metricsPage?.total ?? 0)}
                  onClick={() => loadMetricsPage(reviewTab !== "invalid", metricsOffset + METRICS_PAGE_SIZE)}
                >
                  Next ▶
                </button>
              </div>
            )}
          </div>
        </div>
      </div>
      {reviewOpenFailure && <OpenFailureDialog failure={reviewOpenFailure} onClose={() => setReviewOpenFailure(null)} />}
    </fieldset>
  );
}
