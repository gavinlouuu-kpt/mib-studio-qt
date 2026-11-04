// Frame viewer overlay (plan 2026-10-01-standalone-review-app, PR 2; Qt
// parity #867): the Qt FrameViewerDialog — one frame at full resolution with
// the backend overlay / ROI, an info header (index, timestamp, deformability,
// area, area ratio, ring ratio, Valid, inner count, series), previous / next
// frame, series previous / next, the overlay combo and ROI Overlay check
// (shared with the toolbar), Zoom Out / Fit to Window / Zoom In / Ctrl+wheel
// and Export as TIFF. An in-app overlay, not a native window (scatter plan
// decision). Keys, as in Qt: Left / Up = previous frame, Right / Down = next
// frame (both wrap), Esc closes. The series has its buttons and Shift+Up /
// Shift+Down (Qt has no key for it).
import { useCallback, useEffect, useRef, useState } from "react";
import type { FramePacket } from "../framePacket";
import { metricNumber } from "../metricFormat";
import type { FramePullScheduler } from "../framePullScheduler";
import { canvasToPng } from "./charts/chartExport";
import { frameTiffName } from "./exports/frameExport";
import { zoomedSize, useImageZoom } from "./imageZoom";
import { columnText } from "./metricsColumns";
import { OVERLAY, packetToImageData, reviewBridge, REVIEW_DATASET, type OverlayMode, type ReviewRow } from "./reviewBridge";

/** The overlay combo, with the texts of FrameViewerDialog.ui. */
export const VIEWER_OVERLAYS: { mode: OverlayMode; label: string }[] = [
  { mode: OVERLAY.None, label: "None" },
  { mode: OVERLAY.AllContour, label: "All contours" },
  { mode: OVERLAY.OuterInnerColorCoded, label: "Outer+inner (color)" },
  { mode: OVERLAY.AllMask, label: "All mask" },
  { mode: OVERLAY.FilteredMask, label: "Filtered mask" },
];

/** The info header (FrameViewerDialog::updateFrameInfo): Qt's fields, labels and
 * formats in Qt's order — deformability 3 dp, area 2 dp (px), area ratio 3 dp,
 * ring ratio 3 dp, Valid Yes / No — then the series indicator, then what only
 * YOFO Review shows (area in µm², E, object, track, target). Recording files
 * carry the index and timestamp only. */
export function viewerInfo(row: ReviewRow, series: { position: number; count: number } | null, recording = false): { label: string; value: string }[] {
  const out = [
    { label: "Frame Index", value: row.frame_index },
    { label: "Timestamp", value: `${row.timestamp_ns} ns` },
  ];
  if (!recording) {
    out.push(
      { label: "Deformability", value: columnText("deformability", row) },
      { label: "Area", value: columnText("area", row) },
      { label: "Area Ratio", value: columnText("area_ratio", row) },
      { label: "Ring Ratio", value: columnText("ring_ratio", row) },
      { label: "Valid", value: columnText("valid", row) },
      { label: "Inner Count", value: columnText("inner_contour_count", row) },
    );
  }
  if (series && series.count > 1) {
    out.push({
      label: "Series",
      value: series.position === 0 ? `Trigger image (1/${series.count})` : `Image ${series.position + 1}/${series.count} (metrics from trigger only)`,
    });
  }
  if (!recording) {
    out.push(
      { label: "Area (µm²)", value: columnText("area_um2", row) },
      { label: "E (kPa)", value: metricNumber(row.youngs_modulus, 3) },
      { label: "Object", value: columnText("object_id", row) },
      { label: "Track", value: columnText("track_id", row) },
    );
    if (row.target_group) out.push({ label: "Target", value: "Yes" });
  }
  return out;
}

export interface FrameViewerProps {
  scheduler: FramePullScheduler;
  valid: boolean;
  index: number;
  total: number;
  overlay: OverlayMode;
  roiOverlay: boolean;
  /** Recording files carry no masks or ROI: the overlay controls are disabled. */
  recording: boolean;
  /** The frame's metrics row, when loaded. */
  row: ReviewRow | null;
  onIndexChange: (index: number) => void;
  /** The overlay combo and the ROI check change the toolbar's state too. */
  onOverlayChange: (mode: OverlayMode) => void;
  onRoiOverlayChange: (on: boolean) => void;
  /** Export as TIFF…: the panel asks where, then writes the PNG as a TIFF. */
  onExportFrame: (defaultName: string, png: () => Promise<Uint8Array>) => void;
  onClose: () => void;
  log: (line: string) => void;
}

export function FrameViewer(props: FrameViewerProps) {
  const { scheduler, valid, index, total, overlay, roiOverlay, recording, row, onIndexChange, onOverlayChange, onRoiOverlayChange, onExportFrame, onClose, log } = props;
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [seriesCount, setSeriesCount] = useState(0);
  // The series image shown, 0-based like Qt (0 = the trigger image); it is the
  // trigger image again whenever the frame changes.
  const frameKey = `${valid}:${index}`;
  const [seriesAt, setSeriesAt] = useState({ key: "", position: 0 });
  const seriesPos = seriesAt.key === frameKey ? seriesAt.position : 0;
  const zoom = useImageZoom(canvasRef);
  // The series-count effect must not refetch when a host passes a new `log`.
  const logRef = useRef(log);
  logRef.current = log;
  const [size, setSize] = useState({ w: 0, h: 0 });

  const draw = useCallback((p: FramePacket) => {
    const canvas = canvasRef.current;
    if (!p.valid || !canvas) return;
    canvas.width = p.width;
    canvas.height = p.height;
    canvas.getContext("2d")?.putImageData(packetToImageData(p), 0, 0);
    setSize({ w: p.width, h: p.height });
  }, []);

  useEffect(() => scheduler.mount("viewer", draw, (e) => log(`viewer frame error: ${e}`)), [scheduler, draw, log]);

  // Series count for this frame (valid set only).
  useEffect(() => {
    if (!valid) {
      setSeriesCount(0);
      return;
    }
    let live = true;
    reviewBridge
      .seriesCount(index)
      .then((n) => live && setSeriesCount(n))
      .catch((e) => {
        if (!live) return;
        logRef.current(`series count error: ${e}`);
        setSeriesCount(0);
      });
    return () => {
      live = false;
    };
  }, [valid, index]);

  // A series image other than the trigger is shown raw (no overlay: the
  // metrics are the trigger's, Qt `updateImage`); the ROI is drawn on any.
  useEffect(() => {
    scheduler.request("viewer", () =>
      seriesPos > 0
        ? reviewBridge.seriesFrame(index, seriesPos, OVERLAY.None, roiOverlay)
        : reviewBridge.frame(valid ? REVIEW_DATASET.ValidImage : REVIEW_DATASET.InvalidImage, index, overlay, roiOverlay),
    );
  }, [scheduler, valid, index, seriesPos, overlay, roiOverlay]);

  const step = useCallback(
    (delta: number) => {
      if (total > 0) onIndexChange((index + delta + total) % total);
    },
    [index, onIndexChange, total],
  );
  const hasSeries = seriesCount > 1;
  // Qt: Series Prev / Next stop at the first / last image (no wrap).
  const stepSeries = useCallback(
    (delta: number) => {
      const next = seriesPos + delta;
      if (seriesCount > 1 && next >= 0 && next < seriesCount) setSeriesAt({ key: frameKey, position: next });
    },
    [frameKey, seriesCount, seriesPos],
  );

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
      // The overlay combo keeps its own arrow keys while it has the focus.
      else if (e.key.startsWith("Arrow") && e.target instanceof HTMLSelectElement) return;
      else if (e.key === "ArrowLeft") step(-1);
      else if (e.key === "ArrowRight") step(1);
      else if (e.key === "ArrowUp") (e.shiftKey ? stepSeries : step)(-1);
      else if (e.key === "ArrowDown") (e.shiftKey ? stepSeries : step)(1);
      else return;
      e.preventDefault();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose, step, stepSeries]);

  const style = zoom.zoom === null ? { maxWidth: "100%", maxHeight: "100%" } : { width: zoomedSize(size.w, zoom.zoom), height: zoomedSize(size.h, zoom.zoom) };
  const info = row ? viewerInfo(row, hasSeries ? { position: seriesPos, count: seriesCount } : null, recording) : [];

  return (
    <div className="modal-backdrop viewer-backdrop" onClick={onClose}>
      <div className="viewer" role="dialog" aria-label="Frame viewer" data-tour="review-viewer" onClick={(e) => e.stopPropagation()}>
        <div className="toolbar">
          <button onClick={() => step(-1)} title="Previous frame (←, ↑)">◀ Previous</button>
          <button onClick={() => step(1)} title="Next frame (→, ↓)">Next ▶</button>
          <span className="mono">
            {valid ? "valid" : "invalid"} #{index + 1} of {total}
            {row ? ` · frame ${row.frame_index}` : ""}
          </span>
          {hasSeries && (
            <span className="viewer-series" data-tour="review-viewer-series">
              <button onClick={() => stepSeries(-1)} disabled={seriesPos <= 0} title="Previous series image (Shift+↑)">&lt; Series Prev</button>
              <span className="mono">Series {seriesPos + 1}/{seriesCount}</span>
              <button onClick={() => stepSeries(1)} disabled={seriesPos >= seriesCount - 1} title="Next series image (Shift+↓)">Series Next &gt;</button>
            </span>
          )}
          <label className="viewer-overlay" title={recording ? "Recording files carry no masks" : "Overlay drawn by the backend (shared with the toolbar)"}>
            Overlay:
            <select
              value={overlay}
              disabled={recording}
              aria-label="Viewer overlay mode"
              data-tour="review-viewer-overlay"
              onChange={(e) => onOverlayChange(Number(e.target.value) as OverlayMode)}
            >
              {VIEWER_OVERLAYS.map((o) => (
                <option key={o.mode} value={o.mode}>{o.label}</option>
              ))}
            </select>
          </label>
          <label title={recording ? "Recording files carry no ROI" : "Draw the recorded ROI (shared with the toolbar)"}>
            <input type="checkbox" checked={roiOverlay} disabled={recording} data-tour="review-viewer-roi" onChange={(e) => onRoiOverlayChange(e.target.checked)} /> ROI Overlay
          </label>
          <span className="right" />
          <span className="viewer-zoom" data-tour="review-viewer-zoom">
            <button onClick={zoom.zoomOut} title="Zoom out (÷1.2, down to 10 %). Ctrl+wheel also zooms.">Zoom Out</button>
            <button className={zoom.zoom === null ? "active" : ""} onClick={zoom.fit} title="Scale the image to the window">Fit to Window</button>
            <button onClick={zoom.zoomIn} title="Zoom in (×1.2, up to 1000 %). Ctrl+wheel also zooms.">Zoom In</button>
            <button className={zoom.zoom === 1 ? "active" : ""} onClick={zoom.actual} title="Actual size (100 %)">1×</button>
          </span>
          <button
            disabled={!row}
            data-tour="review-viewer-export"
            title={row ? "Save the displayed frame, with overlay and ROI, as a TIFF" : "Waiting for the frame's metrics"}
            onClick={() => row && onExportFrame(frameTiffName(row.frame_index, hasSeries ? { position: seriesPos } : null), () => canvasToPng(canvasRef.current as HTMLCanvasElement))}
          >
            Export as TIFF...
          </button>
          <button onClick={onClose} title="Close (Esc)">Close</button>
        </div>
        {row && (
          <div className="viewer-info" data-tour="review-viewer-info">
            {info.map((f, i) => (
              <span key={f.label}>
                {i > 0 ? " | " : ""}
                <b>{f.label}:</b> {f.value}
              </span>
            ))}
          </div>
        )}
        <div ref={zoom.wrapRef} className={`viewer-image${zoom.zoom === null ? " fit" : ""}`}>
          <canvas ref={canvasRef} style={style} />
        </div>
      </div>
    </div>
  );
}
