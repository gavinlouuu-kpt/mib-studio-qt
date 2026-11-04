// Live cell strips of the Monitoring sub-tab (epic #866 MON-052 .. MON-055): the last 25 valid and
// the last 25 invalid detections as 100 px thumbnails, five per row, scrollable, cropped to the
// object (the backend cuts the crops from the processing ROI), the invalid ones with the short
// rejection reasons under the image and a tooltip with the measured value and range, and a
// "Show Overlay" checkbox per strip that draws the mask (tint + outline: blue target, green valid,
// red invalid) over the image. The data and the pulls are useMonitoringCells.ts; the pixel logic is
// cellStripsModel.ts. Reads only: nothing here sends a command.
import { memo, useEffect, useRef, useState } from "react";
import { cellKeyString, type MonitoringCell, type MonitoringCellsListing } from "../../monitoringCells";
import {
  GRID_COLUMNS, THUMBNAIL_SIZE, cellTint, composeThumbnail, fitThumbnail, reasonsLine, reasonsTooltip,
  stripCount, type CellImageState, type CellPixels, type Rgb,
} from "./cellStripsModel";
import { useMonitoringCells } from "./useMonitoringCells";

/** Draws the thumbnail's pixels into its canvas at the crop's own size; the CSS size scales it. */
function paint(canvas: HTMLCanvasElement, pixels: CellPixels, tint: Rgb, overlay: boolean) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  canvas.width = pixels.width;
  canvas.height = pixels.height;
  const rgba = composeThumbnail(pixels.image, pixels.mask, pixels.width, pixels.height, tint, overlay);
  const data = ctx.createImageData(pixels.width, pixels.height);
  data.data.set(rgba);
  ctx.putImageData(data, 0, 0);
}

interface ThumbProps {
  cell: MonitoringCell;
  state: CellImageState | undefined;
  overlay: boolean;
}

/** One thumbnail: re-rendered only when its own pixels, overlay flag or classification changed. */
const CellThumb = memo(function CellThumb({ cell, state, overlay }: ThumbProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const tint = cellTint(cell);
  const pixels = state?.kind === "ready" ? state.pixels : null;
  useEffect(() => {
    if (canvasRef.current && pixels) paint(canvasRef.current, pixels, tint, overlay);
  }, [pixels, overlay, tint]);

  const reasons = cell.reasons;
  const line = cell.valid ? "" : reasonsLine(reasons);
  const tooltip = cell.valid ? `Frame ${cell.frame_index}, object ${cell.object_id}` : reasonsTooltip(reasons);
  const size = pixels ? fitThumbnail(pixels.width, pixels.height) : null;
  // Without pixels: the backend's reason (no image for the row), the pull's (gone, failed) or a wait mark.
  let missing: { text: string; why?: string } = { text: "…" };
  if (cell.image === null) missing = { text: "no image", why: cell.no_image_reason ?? undefined };
  else if (state?.kind === "none") missing = { text: "no image", why: state.reason };

  return (
    <div className="cell-thumb" title={tooltip || undefined} data-cell-key={cellKeyString(cell)} data-valid={cell.valid}>
      <div className="cell-thumb-frame" style={{ width: THUMBNAIL_SIZE, height: THUMBNAIL_SIZE }}>
        {pixels && size ? (
          <canvas ref={canvasRef} style={{ width: size.width, height: size.height }}
            role="img" aria-label={`Frame ${cell.frame_index}, object ${cell.object_id}${line ? `: ${line}` : ""}`} />
        ) : (
          <span className="cell-thumb-none" title={missing.why}>{missing.text}</span>
        )}
      </div>
      {line && <div className="cell-thumb-reasons" style={{ width: THUMBNAIL_SIZE }}>{line}</div>}
    </div>
  );
});

interface StripProps {
  kind: "valid" | "invalid";
  title: string;
  cells: MonitoringCell[];
  images: ReadonlyMap<string, CellImageState>;
  empty: string;
}

function CellStrip({ kind, title, cells, images, empty }: StripProps) {
  const [overlay, setOverlay] = useState(false); // Qt: off until ticked
  return (
    <section className={`cell-strip cell-strip-${kind}`} data-tour={`monitoring-${kind}-cells`} aria-label={title}>
      <div className="cell-strip-head">
        <h5>{title}</h5>
        <span className="mono cell-strip-count">{stripCount(cells.length)}</span>
        <label className="cell-strip-overlay">
          <input type="checkbox" checked={overlay} data-tour={`monitoring-${kind}-overlay`}
            onChange={e => setOverlay(e.target.checked)} />
          Show Overlay
        </label>
      </div>
      <div className="cell-strip-scroll">
        {cells.length === 0 ? (
          <p className="pending-note cell-strip-empty">{empty}</p>
        ) : (
          <div className="cell-grid" style={{ gridTemplateColumns: `repeat(${GRID_COLUMNS}, ${THUMBNAIL_SIZE}px)` }}>
            {cells.map(cell => {
              const key = cellKeyString(cell);
              return <CellThumb key={key} cell={cell} state={images.get(key)} overlay={overlay} />;
            })}
          </div>
        )}
      </div>
    </section>
  );
}

/** The line above the strips: why they are empty or unavailable, never a silent blank. */
export function cellStripsNotice(
  listing: MonitoringCellsListing | null, unavailable: boolean, error: string | null,
): string | null {
  if (error) return `Cell images unavailable: ${error}`;
  if (unavailable) return "Cell images are unavailable: the backend is not initialised.";
  if (!listing) return null;
  const change = listing.last_capture_change;
  if (change && change.capture_epoch === listing.ring_capture_epoch && listing.strip.length === 0
      && listing.invalid_strip.length === 0) {
    const dropped = change.strip_dropped + change.invalid_strip_dropped;
    return `A new capture began (${change.reason}); ${dropped > 0 ? `${dropped} earlier cell${dropped === 1 ? "" : "s"} cleared` : "no earlier cells were held"}.`;
  }
  return null;
}

export function CellStrips({ ready, visible }: { ready: boolean; visible: boolean }) {
  const { listing, unavailable, error, images } = useMonitoringCells({ ready, visible });
  const notice = cellStripsNotice(listing, unavailable, error);
  return (
    <div className="cell-strips" data-tour="monitoring-cell-strips">
      {notice && <p className="pending-note cell-strips-notice" role="status">{notice}</p>}
      <div className="cell-strips-grid">
        <CellStrip kind="valid" title="Valid cells" cells={listing?.strip ?? []} images={images}
          empty="No valid cells yet: cells appear while capture and realtime processing run with Monitoring visible." />
        <CellStrip kind="invalid" title="Invalid cells" cells={listing?.invalid_strip ?? []} images={images}
          empty="No invalid cells yet." />
      </div>
    </div>
  );
}
