import type { HTMLAttributes, ReactNode } from "react";
import { cx } from "./classNames";
import { StateChip } from "./StateChip";

/** Which recorded overlay to draw over the crop. */
export type CellOverlay = "none" | "contour" | "mask";

export interface CellImageProps extends Omit<HTMLAttributes<HTMLElement>, "onSelect"> {
  /** The record this crop belongs to (u64 ids travel as strings). */
  recordId: string;
  /** Recorded crop (data, blob or file URL). Omit when no image was recorded: the crop says so. */
  src?: string;
  /** Natural width of the crop in image pixels (contour coordinates use it). */
  imageWidth: number;
  /** Natural height of the crop in image pixels. */
  imageHeight: number;
  /** Recorded contour in image pixels, as `[x, y]` points of a closed outline. */
  contour?: Array<[number, number]>;
  /** Recorded mask image, the same size as the crop. */
  maskSrc?: string;
  /** Overlay to draw. When the requested overlay was not recorded, a chip says so. */
  overlay?: CellOverlay;
  /** Display size of the square box in CSS pixels. */
  size?: number;
  /** Corner label, e.g. "CONTOUR". */
  label?: ReactNode;
  /** Shows the record id (and `detail`) under the crop. */
  caption?: boolean;
  /** Second caption line, e.g. "frame 82 140 · object 02". */
  detail?: ReactNode;
  /** Pinned or inspected: drawn in the inspect colour. */
  selected?: boolean;
  /** Makes the crop a button that reports its record id. */
  onSelect?: (recordId: string) => void;
}

/**
 * One recorded cell crop with its contour or mask, identified by its record.
 * Pixels are shown as recorded (no smoothing). Nothing is drawn that was not
 * recorded: a missing image or overlay is stated, never replaced.
 */
export function CellImage({
  recordId,
  src,
  imageWidth,
  imageHeight,
  contour,
  maskSrc,
  overlay = "contour",
  size = 120,
  label,
  caption = false,
  detail,
  selected = false,
  onSelect,
  className,
  ...rest
}: CellImageProps) {
  const overlayMissing =
    src !== undefined &&
    ((overlay === "contour" && (!contour || contour.length < 3)) || (overlay === "mask" && !maskSrc));
  const points = contour?.map(([x, y]) => `${x},${y}`).join(" ");

  const box = (
    <span
      className={cx("mib-cell__box", src === undefined && "mib-cell__box--missing")}
      style={{ width: size, height: size }}
    >
      {src === undefined ? (
        <span className="mib-cell__missing">Image not recorded</span>
      ) : (
        <>
          <img className="mib-cell__img" src={src} alt={`Cell ${recordId}`} width={size} height={size} draggable={false} />
          {overlay === "mask" && maskSrc ? (
            <img className="mib-cell__mask" src={maskSrc} alt="" aria-hidden="true" width={size} height={size} draggable={false} />
          ) : null}
          {overlay === "contour" && points && contour && contour.length >= 3 ? (
            <svg
              className="mib-cell__overlay"
              viewBox={`0 0 ${imageWidth} ${imageHeight}`}
              preserveAspectRatio="xMidYMid meet"
              aria-hidden="true"
            >
              <polygon className="mib-cell__contour" points={points} vectorEffect="non-scaling-stroke" />
            </svg>
          ) : null}
          {label ? <span className="mib-cell__label">{label}</span> : null}
          {overlayMissing ? (
            <StateChip tone="unknown" size="small" className="mib-cell__overlay-missing">
              {overlay === "mask" ? "Mask not recorded" : "Contour not recorded"}
            </StateChip>
          ) : null}
        </>
      )}
    </span>
  );

  const captionContent = (
    <>
      <span className="mib-cell__id">{recordId}</span>
      {detail ? <span className="mib-cell__detail">{detail}</span> : null}
    </>
  );

  const classes = cx("mib-cell", selected && "mib-cell--selected", size < 90 && "mib-cell--small", className);

  if (onSelect) {
    return (
      <button
        {...(rest as HTMLAttributes<HTMLButtonElement>)}
        type="button"
        className={cx(classes, "mib-cell--button")}
        aria-pressed={selected}
        aria-label={`Cell ${recordId}`}
        onClick={() => onSelect(recordId)}
      >
        {box}
        {caption ? <span className="mib-cell__caption">{captionContent}</span> : null}
      </button>
    );
  }
  return (
    <figure {...rest} className={classes}>
      {box}
      {caption ? <figcaption className="mib-cell__caption">{captionContent}</figcaption> : null}
    </figure>
  );
}

export interface CellStripItem extends Omit<CellImageProps, "size" | "caption" | "selected" | "onSelect" | "label"> {}

export interface CellStripProps extends Omit<HTMLAttributes<HTMLDivElement>, "onSelect"> {
  /** Crops the backend kept, newest first. */
  cells: CellStripItem[];
  /** The pinned record id. */
  pinnedId?: string;
  /** Pins a record; acquisition and populations are unaffected. */
  onPin?: (recordId: string) => void;
  /** Crop size in CSS pixels. */
  size?: number;
  /** What to say with no cells, e.g. "No cells detected yet in this run." */
  empty?: ReactNode;
}

/** The live detected-cell strip: newest first, click a crop to pin it. */
export function CellStrip({ cells, pinnedId, onPin, size = 64, empty = "No cells yet.", className, ...rest }: CellStripProps) {
  return (
    <div {...rest} className={cx("mib-cellstrip", className)} role="list">
      {cells.length === 0 ? (
        <div className="mib-cellstrip__empty" role="listitem">
          {empty}
        </div>
      ) : (
        cells.map((cell) => (
          <div key={cell.recordId} role="listitem" className="mib-cellstrip__item">
            <CellImage {...cell} size={size} caption selected={cell.recordId === pinnedId} onSelect={onPin} />
          </div>
        ))
      )}
    </div>
  );
}
