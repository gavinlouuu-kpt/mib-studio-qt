// The HDF5 review module (UI-4, #269; plan 2026-10-01-standalone-review-app).
//
// One React tree serves two products: MIB Studio mounts this panel in its
// Review tab, YOFO Review (`review.html` → `ReviewApp.tsx`) mounts it as the
// whole window. The panel owns its review state and the "review" slot of the
// shared FramePullScheduler, and talks only to the review bridge
// (`reviewBridge.ts` → src-tauri/src/review.rs → ReviewSession), so the
// review-only product needs no backend bridge at all.
//
// Scope today: open/close, info line, raw / valid / invalid scrubbing with
// backend-composed overlays, the metrics table. Thumbnails, charts, exports
// and jobs arrive with the plan's PR 2–4.

import { forwardRef, useCallback, useEffect, useImperativeHandle, useRef, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import type { FramePacket } from "../framePacket";
import type { FramePullScheduler } from "../framePullScheduler";
import {
  OVERLAY,
  REVIEW_DATASET,
  packetToImageData,
  reviewBridge,
  type OverlayMode,
  type ReviewInfo,
  type ReviewRows,
} from "./reviewBridge";

export const H5_FILTER = [{ name: "HDF5", extensions: ["h5", "hdf5"] }];
export const METRICS_PAGE_SIZE = 50;

// Jobs (exports, batch, regenerate masks) are bridged with the plan's PR 1b.
const PENDING_JOBS = "Export and regeneration jobs arrive with the next review bridge step (PR 1b)";

const OVERLAY_LABELS: { mode: OverlayMode; label: string }[] = [
  { mode: OVERLAY.None, label: "Overlay: None" },
  { mode: OVERLAY.AllContour, label: "Overlay: Contours" },
  { mode: OVERLAY.OuterInnerColorCoded, label: "Overlay: Outer/Inner" },
  { mode: OVERLAY.AllMask, label: "Overlay: Mask" },
  { mode: OVERLAY.FilteredMask, label: "Overlay: Filtered mask" },
];

export interface ReviewPanelProps {
  /** Backend initialized (open is refused otherwise). */
  ready: boolean;
  /** Shared scheduler; the panel mounts its own "review" slot on it. */
  scheduler: FramePullScheduler;
  /** Image scaling preference shared with the host's live canvases. */
  fitWindow: boolean;
  /** Log drawer sink. */
  log: (line: string) => void;
  /** Called before an open replaces the current source (MIB Studio stops the
   * live preview loop here). */
  beforeLoad?: () => void;
  /** The loaded file path changed (host invalidates its scheduler views). */
  onFileChange?: (path: string) => void;
  /** Fresh info after an open or close (workflow facts, status bar). */
  onInfo?: (info: ReviewInfo | null) => void;
}

export interface ReviewPanelHandle {
  /** The File ▸ Open… action. */
  openFile: () => Promise<void>;
  /** The File ▸ Close action. */
  closeFile: () => Promise<void>;
}

type ReviewTab = "raw" | "valid" | "invalid" | "charts";

export const ReviewPanel = forwardRef<ReviewPanelHandle, ReviewPanelProps>(function ReviewPanel(props, ref) {
  const { ready, scheduler, fitWindow, log, beforeLoad, onFileChange, onInfo } = props;

  const [info, setInfo] = useState<ReviewInfo | null>(null);
  const [reviewTab, setReviewTab] = useState<ReviewTab>("raw");
  const [rows, setRows] = useState<ReviewRows | null>(null);
  const [rowsOffset, setRowsOffset] = useState(0);
  const [imgIndex, setImgIndex] = useState(0);
  const [overlay, setOverlay] = useState<OverlayMode>(OVERLAY.None);
  const [roiOverlay, setRoiOverlay] = useState(false);
  const canvasRef = useRef<HTMLCanvasElement>(null);

  const reviewing = !!info?.file_open;
  const total = Number(rows?.total ?? 0);

  // Draw only the pixels owned by this exact immutable pull response.
  const draw = useCallback((p: FramePacket) => {
    const canvas = canvasRef.current;
    if (!p.valid || !canvas) return;
    canvas.width = p.width;
    canvas.height = p.height;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.putImageData(packetToImageData(p), 0, 0);
  }, []);

  useEffect(() => {
    return scheduler.mount("review", draw, (e) => log(`review frame error: ${e}`));
  }, [scheduler, draw, log]);

  const datasetFor = useCallback((tab: ReviewTab): number => {
    if (tab === "raw") return REVIEW_DATASET.RecordedImage;
    return tab === "invalid" ? REVIEW_DATASET.InvalidImage : REVIEW_DATASET.ValidImage;
  }, []);

  const imageCount = useCallback(
    (tab: ReviewTab): number => {
      if (!info) return 0;
      if (tab === "raw") return Number(info.recorded_images.count);
      return Number(tab === "invalid" ? info.invalid_images.count : info.valid_images.count);
    },
    [info],
  );

  const drawImage = useCallback(
    (tab: ReviewTab, index: number, mode: OverlayMode, roi: boolean) => {
      scheduler.request("review", () => reviewBridge.frame(datasetFor(tab), index, mode, roi));
    },
    [scheduler, datasetFor],
  );

  const loadRows = useCallback(
    async (valid: boolean, offset: number) => {
      try {
        const page = await reviewBridge.rows(valid, offset, METRICS_PAGE_SIZE);
        if (page.valid) {
          setRows(page);
          setRowsOffset(offset);
        }
      } catch (e) {
        log(`metrics page error: ${e}`);
      }
    },
    [log],
  );

  const selectTab = useCallback(
    async (tab: ReviewTab) => {
      setReviewTab(tab);
      setImgIndex(0);
      if (tab === "charts") return;
      await loadRows(tab !== "invalid", 0);
      if (imageCount(tab) > 0) drawImage(tab, 0, overlay, roiOverlay);
    },
    [drawImage, imageCount, loadRows, overlay, roiOverlay],
  );

  const closeFile = useCallback(async () => {
    try {
      await reviewBridge.close();
    } catch (e) {
      log(`close error: ${e}`);
    }
    scheduler.invalidate("review");
    setInfo(null);
    setRows(null);
    setRowsOffset(0);
    setImgIndex(0);
    setReviewTab("raw");
    onInfo?.(null);
    onFileChange?.("");
    const canvas = canvasRef.current;
    canvas?.getContext("2d")?.clearRect(0, 0, canvas.width, canvas.height);
  }, [log, onFileChange, onInfo, scheduler]);

  const openFile = useCallback(async () => {
    const picked = await open({ title: "Open recording", filters: H5_FILTER, multiple: false });
    if (typeof picked !== "string") return;
    beforeLoad?.();
    onFileChange?.(picked);
    try {
      const res = await reviewBridge.open(picked);
      if (!res.ok) return log(`open failed: ${res.message}`);
      const fresh = await reviewBridge.info();
      setInfo(fresh);
      onInfo?.(fresh);
      log(`opened ${picked}${fresh.accounting_summary}`);
      const tab: ReviewTab = fresh.recording_file ? "raw" : "valid";
      setReviewTab(tab);
      setImgIndex(0);
      await loadRows(true, 0);
      const count = fresh.recording_file ? Number(fresh.recorded_images.count) : Number(fresh.valid_images.count);
      if (count > 0) drawImage(tab, 0, overlay, roiOverlay);
    } catch (e) {
      log(`open error: ${e}`);
    }
  }, [beforeLoad, drawImage, loadRows, log, onFileChange, onInfo, overlay, roiOverlay]);

  useImperativeHandle(ref, () => ({ openFile, closeFile }), [openFile, closeFile]);

  const onOverlayChange = (mode: OverlayMode) => {
    setOverlay(mode);
    if (reviewing && reviewTab !== "charts" && imageCount(reviewTab) > 0) drawImage(reviewTab, imgIndex, mode, roiOverlay);
  };
  const onRoiChange = (on: boolean) => {
    setRoiOverlay(on);
    if (reviewing && reviewTab !== "charts" && imageCount(reviewTab) > 0) drawImage(reviewTab, imgIndex, overlay, on);
  };

  const count = reviewing && reviewTab !== "charts" ? imageCount(reviewTab) : 0;
  const isRecording = !!info?.recording_file;

  return (
    <>
      <div className="toolbar">
        <button onClick={openFile} disabled={!ready} title={ready ? undefined : "Backend is not initialized"}>
          Select HDF File…
        </button>
        <button onClick={closeFile} disabled={!reviewing} title={reviewing ? "Close the current file" : "No file loaded"}>
          Close File
        </button>
        <button disabled title={PENDING_JOBS}>Export Metrics to CSV…</button>
        <button disabled title={PENDING_JOBS}>Export All…</button>
        <button disabled title={PENDING_JOBS}>Batch Metrics…</button>
        <button disabled title={PENDING_JOBS}>Regenerate masks…</button>
        <select
          value={overlay}
          disabled={!reviewing || isRecording}
          title={isRecording ? "Recording files carry no masks" : "Overlay drawn by the backend"}
          onChange={(e) => onOverlayChange(Number(e.target.value) as OverlayMode)}
          aria-label="Overlay mode"
        >
          {OVERLAY_LABELS.map((o) => (
            <option key={o.mode} value={o.mode}>{o.label}</option>
          ))}
        </select>
        <label title={isRecording ? "Recording files carry no ROI" : "Draw the recorded ROI"}>
          <input type="checkbox" checked={roiOverlay} disabled={!reviewing || isRecording} onChange={(e) => onRoiChange(e.target.checked)} /> ROI
        </label>
        <span className="legend">
          <span className="chip"><span className="swatch" style={{ background: "#2b6cb0" }} /> Target</span>
          <span className="chip"><span className="swatch" style={{ background: "#1a7f37" }} /> Valid</span>
          <span className="chip"><span className="swatch" style={{ background: "#b42318" }} /> Invalid</span>
        </span>
        <span className="path-label right" title={info?.file_path}>
          {reviewing && info
            ? `${info.file_path} · ${info.recording_file ? "recording" : "experiment"} · valid ${info.total_valid}, invalid ${info.total_invalid}${info.has_core_identity ? ` · core v${info.core_version}` : ""} · px→µm ${info.pixel_to_micron.toFixed(4)}${info.pixel_to_micron_from_file ? "" : " (fallback)"}${info.accounting_summary}`
            : "No file selected"}
        </span>
      </div>
      <div className="subtabs" role="tablist" aria-label="Review views">
        <button
          className={reviewTab === "raw" ? "active" : ""}
          disabled={!info?.recorded_images.present}
          title={info?.recorded_images.present ? undefined : "No recorded raw frames in this file"}
          onClick={() => void selectTab("raw")}
        >
          Raw Frames
        </button>
        <button
          className={reviewTab === "valid" ? "active" : ""}
          disabled={!info?.valid_images.present}
          title={info?.valid_images.present ? undefined : "No valid-frame images in this file"}
          onClick={() => void selectTab("valid")}
        >
          {isRecording ? "Frames" : "Valid Frames"}
        </button>
        {!isRecording && (
          <button
            className={reviewTab === "invalid" ? "active" : ""}
            disabled={!info?.invalid_images.present}
            title={info?.invalid_images.present ? undefined : "No invalid-frame images in this file"}
            onClick={() => void selectTab("invalid")}
          >
            Invalid Frames
          </button>
        )}
        <button disabled title="Chart rendering lands with the plan's PR 3">Charts</button>
      </div>
      <div className="subtab-body">
        <div className="review-split">
          <div className="frames">
            <div className="canvas-wrap">
              {!reviewing && <span className="canvas-hint">No recording loaded — Select HDF File…</span>}
              <canvas ref={canvasRef} className={fitWindow ? "fit" : ""} />
            </div>
            {count > 0 && (
              <>
                <input
                  type="range"
                  className="scrub"
                  min={0}
                  max={Math.max(0, count - 1)}
                  value={imgIndex}
                  onChange={(e) => {
                    const idx = Number(e.target.value);
                    setImgIndex(idx);
                    drawImage(reviewTab, idx, overlay, roiOverlay);
                  }}
                  aria-label="Review image scrubber"
                />
                <span className="mono">
                  image {imgIndex + 1} of {count}
                </span>
              </>
            )}
          </div>
          <div className="table-panel">
            <table className="metrics-table">
              <thead>
                <tr>
                  <th>Index</th>
                  <th>Object Id</th>
                  <th>Track Id</th>
                  <th>Area (px²)</th>
                  <th>Area (µm²)</th>
                  <th>Deformability</th>
                  <th>Ring ratio</th>
                  <th>E (kPa)</th>
                </tr>
              </thead>
              <tbody>
                {(rows?.rows ?? []).map((r) => (
                  <tr key={`${r.frame_index}:${r.object_id}`}>
                    <td>{r.frame_index}</td>
                    <td>{r.object_id}</td>
                    <td>{r.track_id}</td>
                    <td>{r.area.toFixed(1)}</td>
                    <td>{r.area_um2.toFixed(2)}</td>
                    <td>{r.deformability.toFixed(3)}</td>
                    <td>{r.ring_ratio.toFixed(3)}</td>
                    <td>{r.youngs_modulus.toFixed(2)}</td>
                  </tr>
                ))}
                {(rows?.rows?.length ?? 0) === 0 && (
                  <tr>
                    <td colSpan={8} style={{ color: "#777" }}>
                      {reviewing ? "No metric rows in this table." : "Load a file to see frame/object metrics."}
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
            {reviewing && total > METRICS_PAGE_SIZE && (
              <div className="toolbar" style={{ padding: 4 }}>
                <button
                  className="btn"
                  disabled={rowsOffset === 0}
                  onClick={() => loadRows(reviewTab !== "invalid", Math.max(0, rowsOffset - METRICS_PAGE_SIZE))}
                >
                  ◀ Prev
                </button>
                <span className="mono">
                  {rowsOffset + 1}–{Math.min(total, rowsOffset + METRICS_PAGE_SIZE)} of {total}
                </span>
                <button
                  className="btn"
                  disabled={rowsOffset + METRICS_PAGE_SIZE >= total}
                  onClick={() => loadRows(reviewTab !== "invalid", rowsOffset + METRICS_PAGE_SIZE)}
                >
                  Next ▶
                </button>
              </div>
            )}
          </div>
        </div>
      </div>
    </>
  );
});
