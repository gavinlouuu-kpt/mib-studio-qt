// The HDF5 review module (UI-4, #269; plan 2026-10-01-standalone-review-app).
//
// One React tree serves two products: MIB Studio mounts this panel in its
// Review tab, YOFO Review (`review.html` → `ReviewApp.tsx`) mounts it as the
// whole window. The panel owns its review state and the "review" slot of the
// shared FramePullScheduler; the host supplies backend readiness, the
// PlaybackPosition range it drains from `poll_events`, a log sink and the
// hooks it needs to stay in sync (file open, metadata).
//
// Behaviour is today's panel moved out of App.tsx unchanged. The frames /
// charts / exports rewrites arrive with the later PRs of the plan.

import { forwardRef, useCallback, useEffect, useImperativeHandle, useRef, useState } from "react";
import { open, save } from "@tauri-apps/plugin-dialog";
import {
  bridge,
  mono8ToImageData,
  type BridgeEvent,
  type FramePacket,
  type ReviewMetadata,
  type ReviewMetricsPage,
} from "../bridge";
import { decimalU64 } from "../framePacket";
import type { FramePullScheduler } from "../framePullScheduler";

export const H5_FILTER = [{ name: "HDF5", extensions: ["h5", "hdf5"] }];
export const METRICS_PAGE_SIZE = 50;

// Standard reason string for controls whose backend surface is not bridged
// yet (BE-6, #276; replaced by the plan's PR 1). The control stays visible but
// cannot be activated and never fakes backend state.
const PENDING_REVIEW = "HDF5 export/batch/regenerate jobs are not bridged yet — backend issue BE-6 (#276)";

export interface PlaybackRange {
  earliest: string;
  latest: string;
  count: string;
}

export interface ReviewPanelProps {
  /** Backend initialized (load is refused otherwise). */
  ready: boolean;
  /** Shared scheduler; the panel mounts its own "review" slot on it. */
  scheduler: FramePullScheduler;
  /** PlaybackPosition range the host drained from poll_events. */
  range: PlaybackRange;
  /** Image scaling preference shared with the host's live canvases. */
  fitWindow: boolean;
  /** Log drawer sink. */
  log: (line: string) => void;
  /** Host-side event application for the events drained right after load. */
  applyEvents: (events: BridgeEvent[]) => void;
  /** Called before a load replaces the current source (MIB Studio stops the
   * live preview loop here). */
  beforeLoad?: () => void;
  /** The loaded file path changed (host invalidates its scheduler views). */
  onFileChange?: (path: string) => void;
  /** Fresh metadata after a load (workflow facts in MIB Studio). */
  onMetadata?: (meta: ReviewMetadata) => void;
}

export interface ReviewPanelHandle {
  /** The File ▸ Open… action. */
  openFile: () => Promise<void>;
}

export const ReviewPanel = forwardRef<ReviewPanelHandle, ReviewPanelProps>(function ReviewPanel(props, ref) {
  const { ready, scheduler, range, fitWindow, log, applyEvents, beforeLoad, onFileChange, onMetadata } = props;

  const [reviewPath, setReviewPath] = useState("");
  const [reviewing, setReviewing] = useState(false);
  const [reviewTab, setReviewTab] = useState<"raw" | "valid" | "invalid" | "charts">("raw");
  const [reviewIndex, setReviewIndex] = useState("0");
  // Paged review (bridge schema v9, BE-6).
  const [reviewMeta, setReviewMeta] = useState<ReviewMetadata | null>(null);
  const [metricsPage, setMetricsPage] = useState<ReviewMetricsPage | null>(null);
  const [metricsOffset, setMetricsOffset] = useState(0);
  const [reviewImgIndex, setReviewImgIndex] = useState(0);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  // The host's range at call time, without re-creating the load callback.
  const rangeRef = useRef(range);
  rangeRef.current = range;

  // Draw only the pixels owned by this exact immutable pull response.
  const draw = useCallback((meta: FramePacket) => {
    const canvas = canvasRef.current;
    if (!meta.valid || !canvas) return;
    canvas.width = meta.width;
    canvas.height = meta.height;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.putImageData(mono8ToImageData(meta.data, meta.width, meta.height, meta.stride_bytes), 0, 0);
  }, []);

  useEffect(() => {
    return scheduler.mount("review", draw, (e) => log(`review frame error: ${e}`));
  }, [scheduler, draw, log]);

  const onScrub = useCallback(
    (input: number | string) => {
      const idx = decimalU64(input);
      setReviewIndex(idx);
      scheduler.request("review", async () => {
        const result = await bridge.seekIndex(idx);
        if (!result.ok) throw new Error(result.message);
        return bridge.fetchFrameByIndex(idx);
      });
    },
    [scheduler],
  );

  const loadMetricsPage = useCallback(
    async (valid: boolean, offset: number) => {
      try {
        const page = await bridge.fetchReviewMetricsPage(valid, offset, METRICS_PAGE_SIZE);
        if (page.valid) {
          setMetricsPage(page);
          setMetricsOffset(offset);
        }
      } catch (e) {
        log(`metrics page error: ${e}`);
      }
    },
    [log],
  );

  const drawReviewImage = useCallback(
    (dataset: number, index: number) => {
      scheduler.request("review", () => bridge.fetchReviewImage(dataset, index));
    },
    [scheduler],
  );

  const openFile = useCallback(async () => {
    const picked = await open({ title: "Open recording", filters: H5_FILTER, multiple: false });
    if (typeof picked !== "string") return;
    beforeLoad?.();
    setReviewPath(picked);
    onFileChange?.(picked);
    try {
      const res = await bridge.loadRecording(picked);
      if (!res.ok) return log(`load failed: ${res.message}`);
      setReviewing(true);
      log(`loaded ${picked}`);
      applyEvents(await bridge.pollEvents());
      const meta = await bridge.fetchReviewMetadata();
      setReviewMeta(meta);
      onMetadata?.(meta);
      setReviewTab(meta.recording_file ? "raw" : "valid");
      setReviewImgIndex(0);
      await loadMetricsPage(true, 0);
      if (meta.recording_file) {
        onScrub(rangeRef.current.earliest);
      } else if (meta.valid_images.present && meta.valid_images.count > 0) {
        drawReviewImage(0, 0);
      }
    } catch (e) {
      log(`load error: ${e}`);
    }
  }, [applyEvents, beforeLoad, drawReviewImage, loadMetricsPage, log, onFileChange, onMetadata, onScrub]);

  useImperativeHandle(ref, () => ({ openFile }), [openFile]);

  const onExportCsv = useCallback(async () => {
    try {
      const picked = await save({
        title: "Export Metrics to CSV",
        filters: [{ name: "CSV", extensions: ["csv"] }],
        defaultPath: "metrics.csv",
      });
      if (!picked) return;
      const res = await bridge.reviewExportCsv(picked);
      log(res.ok ? `CSV export started (operation ${res.operation_id})` : `export failed: ${res.message}`);
    } catch (e) {
      log(`export error: ${e}`);
    }
  }, [log]);

  const imageCount = reviewTab === "valid" ? reviewMeta?.valid_images.count ?? 0 : reviewMeta?.invalid_images.count ?? 0;

  return (
    <>
      <div className="toolbar">
        <button onClick={openFile} disabled={!ready} title={ready ? undefined : "Backend is not initialized"}>
          Select HDF File…
        </button>
        <button disabled title="Loading a new file replaces the current one">Close File</button>
        <button
          onClick={onExportCsv}
          disabled={!reviewMeta?.file_open}
          title={reviewMeta?.file_open ? "Export frame/object metrics as a cancellable job" : "No file loaded"}
        >
          Export Metrics to CSV…
        </button>
        <button disabled title={PENDING_REVIEW}>Export All…</button>
        <button disabled title={PENDING_REVIEW}>Batch Metrics…</button>
        <button disabled title={PENDING_REVIEW}>Regenerate masks…</button>
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
      </div>
      <div className="subtabs" role="tablist" aria-label="Review views">
        <button className={reviewTab === "raw" ? "active" : ""} onClick={() => setReviewTab("raw")}>
          Raw Frames
        </button>
        <button
          className={reviewTab === "valid" ? "active" : ""}
          disabled={!reviewMeta?.valid_images.present}
          title={reviewMeta?.valid_images.present ? undefined : "No valid-frame images in this file"}
          onClick={async () => {
            setReviewTab("valid");
            setReviewImgIndex(0);
            await loadMetricsPage(true, 0);
            drawReviewImage(0, 0);
          }}
        >
          Valid Frames
        </button>
        <button
          className={reviewTab === "invalid" ? "active" : ""}
          disabled={!reviewMeta?.invalid_images.present}
          title={reviewMeta?.invalid_images.present ? undefined : "No invalid-frame images in this file"}
          onClick={async () => {
            setReviewTab("invalid");
            setReviewImgIndex(0);
            await loadMetricsPage(false, 0);
            drawReviewImage(1, 0);
          }}
        >
          Invalid Frames
        </button>
        <button disabled title="Chart rendering lands with UI-4 (#269)">Charts</button>
      </div>
      <div className="subtab-body">
        <div className="review-split">
          <div className="frames">
            <div className="canvas-wrap">
              {!reviewing && <span className="canvas-hint">No recording loaded — Select HDF File…</span>}
              <canvas ref={canvasRef} className={fitWindow ? "fit" : ""} />
            </div>
            {reviewing && reviewTab === "raw" && BigInt(range.count) > 0n && (
              <>
                <input
                  type="range"
                  className="scrub"
                  min={range.earliest}
                  max={range.latest}
                  value={reviewIndex}
                  onChange={(e) => onScrub(e.target.value)}
                  disabled={BigInt(range.latest) > BigInt(Number.MAX_SAFE_INTEGER)}
                  title={BigInt(range.latest) > BigInt(Number.MAX_SAFE_INTEGER) ? "Range exceeds exact browser slider precision" : "Choose frame"}
                  aria-label="Frame scrubber"
                />
                <span className="mono">
                  frame {reviewIndex} of [{range.earliest}…{range.latest}] ({range.count} available)
                </span>
              </>
            )}
            {reviewing && (reviewTab === "valid" || reviewTab === "invalid") && (
              <>
                <input
                  type="range"
                  className="scrub"
                  min={0}
                  max={Math.max(0, imageCount - 1)}
                  value={reviewImgIndex}
                  onChange={(e) => {
                    const idx = Number(e.target.value);
                    setReviewImgIndex(idx);
                    drawReviewImage(reviewTab === "valid" ? 0 : 1, idx);
                  }}
                  aria-label="Review image scrubber"
                />
                <span className="mono">
                  image {reviewImgIndex + 1} of {imageCount}
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
                  <th>Deformability</th>
                  <th>Ring ratio</th>
                  <th>E (kPa)</th>
                </tr>
              </thead>
              <tbody>
                {(metricsPage?.rows ?? []).map((r) => (
                  <tr key={`${r.frame_index}:${r.object_id}`}>
                    <td>{r.frame_index}</td>
                    <td>{r.object_id}</td>
                    <td>{r.track_id}</td>
                    <td>{r.area.toFixed(1)}</td>
                    <td>{r.deformability.toFixed(3)}</td>
                    <td>{r.ring_ratio.toFixed(3)}</td>
                    <td>{r.youngs_modulus.toFixed(2)}</td>
                  </tr>
                ))}
                {(metricsPage?.rows?.length ?? 0) === 0 && (
                  <tr>
                    <td colSpan={7} style={{ color: "#777" }}>
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
    </>
  );
});
