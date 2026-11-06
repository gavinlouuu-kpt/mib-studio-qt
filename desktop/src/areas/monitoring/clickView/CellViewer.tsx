// The picture and the words of the selected cell (epic #866, PR E1: MON-044): what the pane and the floating
// window both show. The pixels are the backend's object-centred crop (cut from the processing ROI) with the
// mask drawn over it when the valid strip's Show Overlay is on, the same composition as the strip thumbnails
// (cellStripsModel.composeThumbnail); Qt shows the whole ROI frame, the crop is the accepted Tauri difference
// (#880). Without pixels it says why, with the measurements of the row, never a blank.
import { useEffect, useRef } from "react";
import type { MonitoringRow } from "../../../bridge";
import { composeThumbnail, cellTint, type CellPixels } from "../cellStripsModel";
import type { CellViewState } from "./useCellView";
import { measurementsLine } from "./selection";

function paint(canvas: HTMLCanvasElement, pixels: CellPixels, row: Pick<MonitoringRow, "target_group">, overlay: boolean) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  canvas.width = pixels.width;
  canvas.height = pixels.height;
  const rgba = composeThumbnail(pixels.image, pixels.mask, pixels.width, pixels.height, cellTint({ valid: true, target_group: row.target_group }), overlay);
  const data = ctx.createImageData(pixels.width, pixels.height);
  data.data.set(rgba);
  ctx.putImageData(data, 0, 0);
}

export function CellViewer({ row, state, overlay }: { row: MonitoringRow; state: CellViewState; overlay: boolean }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const pixels = state.phase === "ready" ? state.pixels : null;
  const targetGroup = row.target_group;
  // Redrawn when its own pixels, the overlay flag or the classification changed, never per poll.
  useEffect(() => {
    if (canvasRef.current && pixels) paint(canvasRef.current, pixels, { target_group: targetGroup }, overlay);
  }, [pixels, overlay, targetGroup]);

  return (
    <div className="mon-cell-viewer" data-phase={state.phase}>
      <div className="mon-cell-image">
        {state.phase === "ready" && pixels ? (
          <canvas ref={canvasRef} className="mon-cell-canvas" role="img"
            aria-label={`Frame ${row.frame_index}, object ${row.object_id}, object-centred crop${overlay && pixels.mask ? " with mask overlay" : ""}`} />
        ) : state.phase === "loading" ? (
          <p className="pending-note mon-cell-note" role="status">Loading the cell…</p>
        ) : state.phase === "measurements" ? (
          <p className="pending-note mon-cell-note" role="status">No image: {state.reason}</p>
        ) : state.phase === "refused" ? (
          <p className="mon-cell-note mon-cell-refused" role="alert">{state.message}</p>
        ) : null}
      </div>
      <p className="mon-cell-metrics mono">{measurementsLine(row)}</p>
    </div>
  );
}
