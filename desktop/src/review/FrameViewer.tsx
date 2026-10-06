// Frame viewer overlay (plan 2026-10-01-standalone-review-app, PR 2): the
// Qt FrameViewerDialog — one frame at full resolution with the backend
// overlay / ROI, frame prev/next, multi-image series prev/next, zoom
// (fit / 1:1 / 2× / 4×) and the frame's key metrics. An in-app overlay, not
// a native window (scatter plan decision). ←/→ step frames, ↑/↓ step the
// series, Esc closes.
import { useCallback, useEffect, useRef, useState } from "react";
import type { FramePacket } from "../framePacket";
import type { FramePullScheduler } from "../framePullScheduler";
import { packetToImageData, reviewBridge, REVIEW_DATASET, type OverlayMode, type ReviewRow } from "./reviewBridge";

const ZOOMS = [0, 1, 2, 4] as const; // 0 = fit

export interface FrameViewerProps {
  scheduler: FramePullScheduler;
  valid: boolean;
  index: number;
  total: number;
  overlay: OverlayMode;
  roiOverlay: boolean;
  /** The frame's metrics row, when loaded. */
  row: ReviewRow | null;
  onIndexChange: (index: number) => void;
  onClose: () => void;
  log: (line: string) => void;
}

export function FrameViewer(props: FrameViewerProps) {
  const { scheduler, valid, index, total, overlay, roiOverlay, row, onIndexChange, onClose, log } = props;
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [seriesCount, setSeriesCount] = useState(0);
  const [seriesK, setSeriesK] = useState(-1); // -1 = the frame itself
  const [zoom, setZoom] = useState<(typeof ZOOMS)[number]>(0);
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
    setSeriesK(-1);
    if (!valid) {
      setSeriesCount(0);
      return;
    }
    let live = true;
    reviewBridge
      .seriesCount(index)
      .then((n) => live && setSeriesCount(n))
      .catch(() => live && setSeriesCount(0));
    return () => {
      live = false;
    };
  }, [valid, index]);

  useEffect(() => {
    scheduler.request("viewer", () =>
      seriesK >= 0
        ? reviewBridge.seriesFrame(index, seriesK, overlay, roiOverlay)
        : reviewBridge.frame(valid ? REVIEW_DATASET.ValidImage : REVIEW_DATASET.InvalidImage, index, overlay, roiOverlay),
    );
  }, [scheduler, valid, index, seriesK, overlay, roiOverlay]);

  const step = useCallback(
    (delta: number) => {
      if (total > 0) onIndexChange((index + delta + total) % total);
    },
    [index, onIndexChange, total],
  );
  const stepSeries = useCallback(
    (delta: number) => {
      if (seriesCount <= 0) return;
      setSeriesK((k) => {
        const n = seriesCount + 1; // -1 (frame) … seriesCount-1
        return ((k + 1 + delta + n) % n) - 1;
      });
    },
    [seriesCount],
  );

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
      else if (e.key === "ArrowRight") step(1);
      else if (e.key === "ArrowLeft") step(-1);
      else if (e.key === "ArrowDown") stepSeries(1);
      else if (e.key === "ArrowUp") stepSeries(-1);
      else return;
      e.preventDefault();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose, step, stepSeries]);

  const style = zoom === 0 ? { maxWidth: "100%", maxHeight: "100%" } : { width: size.w * zoom, height: size.h * zoom };

  return (
    <div className="modal-backdrop viewer-backdrop" onClick={onClose}>
      <div className="viewer" role="dialog" aria-label="Frame viewer" onClick={(e) => e.stopPropagation()}>
        <div className="toolbar">
          <button onClick={() => step(-1)} title="Previous frame (←)">◀ Prev</button>
          <button onClick={() => step(1)} title="Next frame (→)">Next ▶</button>
          <span className="mono">
            {valid ? "valid" : "invalid"} #{index + 1} of {total}
            {row ? ` · frame ${row.frame_index}` : ""}
          </span>
          {seriesCount > 0 && (
            <>
              <button onClick={() => stepSeries(-1)} title="Previous series image (↑)">▲</button>
              <button onClick={() => stepSeries(1)} title="Next series image (↓)">▼</button>
              <span className="mono">{seriesK < 0 ? "frame" : `series ${seriesK + 1}/${seriesCount}`}</span>
            </>
          )}
          <span className="right" />
          {ZOOMS.map((z) => (
            <button key={z} className={zoom === z ? "active" : ""} onClick={() => setZoom(z)}>
              {z === 0 ? "Fit" : `${z}×`}
            </button>
          ))}
          <button onClick={onClose} title="Close (Esc)">Close</button>
        </div>
        <div className={`viewer-image${zoom === 0 ? " fit" : ""}`}>
          <canvas ref={canvasRef} style={style} />
        </div>
        {row && (
          <div className="viewer-metrics mono">
            area {row.area.toFixed(1)} px² ({row.area_um2.toFixed(2)} µm²) · deformability {row.deformability.toFixed(4)} · ring
            ratio {row.ring_ratio.toFixed(3)} · E {row.youngs_modulus.toFixed(2)} kPa · object {row.object_id} · track {row.track_id}
            {row.target_group ? " · target" : ""}
          </div>
        )}
      </div>
    </div>
  );
}
