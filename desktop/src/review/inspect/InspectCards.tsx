// R3 Inspect's right column (#717 B3a, #269; Penpot R3 board
// dcdbaa6b-9790-4e69-bea4-936747f588b1): the selected record and what the
// file says produced it, on the @mib/ui kit.
//
// - RecordCard ("Selected measurement"): the record named by frame and
//   object (files carry no record id; nothing is invented), the run it
//   belongs to, its image as Raw / Contour / Mask crops (the backend draws
//   the overlay, the client cuts the bounding box) or the Full frame (the
//   toolbar's overlay and ROI; double-click opens the viewer), its stored
//   values, and what the file records for it. Previous / Next record and
//   Record details (the frame viewer: series, zoom, stepping).
// - StoredCard ("Stored vs reprocessed"): the core and calibration the
//   stored results came from. Reprocessing is the Regenerate masks job,
//   which writes a new file; this view lists no reprocessing history (#754).
//   The E-LUT is not in the review contract yet.
import type { ReactNode, RefObject } from "react";
import { Button, Card, CellImage, FactList, StateChip } from "@mib/ui";
import type { ReviewInfo, ReviewRow } from "../reviewBridge";
import {
  CELL_VIEWS,
  cropUnavailable,
  measurementFacts,
  recordedFacts,
  recordLabel,
  recordProvenance,
  recordWhere,
  storedFacts,
  type CellView,
} from "./inspectModel";

/** A crop of the selected record: being read, drawn, or not drawable. */
export type CropState = { status: "idle" | "loading" | "failed" } | { status: "ready"; src: string };

export interface RecordCardProps {
  info: ReviewInfo;
  valid: boolean;
  /** Set position of the selected record (-1 = none). */
  position: number;
  total: number;
  /** The selected record's row, when its page is loaded. */
  row: ReviewRow | null;
  view: CellView;
  onView: (view: CellView) => void;
  /** The Full frame canvas the panel's frame slot draws into. */
  canvasRef: RefObject<HTMLCanvasElement>;
  fitWindow: boolean;
  crop: CropState;
  /** The set has no stored images (a file written without them). */
  imagesMissing: boolean;
  onPrevious: () => void;
  onNext: () => void;
  onDetails: () => void;
  /** Shown when nothing is selected, e.g. "Click a point to inspect its record". */
  emptyHint: ReactNode;
}

const CROP_SIZE = 132;

export function RecordCard(props: RecordCardProps) {
  const { info, valid, position, total, row, view, onView, canvasRef, fitWindow, crop, imagesMissing } = props;
  const selected = position >= 0;
  const label = row ? recordLabel(row, info.recording_file) : selected ? `${info.recording_file ? "frame" : "record"} ${position + 1}` : "No record selected";
  const where = selected ? recordWhere(info, valid, position, total) : "";
  const provenance = recordProvenance(info);
  const subtitle = selected ? (info.recording_file ? "Recorded frame · stored evidence" : "Stored evidence") : props.emptyHint;

  const cropView = view !== "full";
  const cropLabel = CELL_VIEWS.find((v) => v.view === view)?.label.toUpperCase();
  let cropBody: ReactNode = null;
  if (cropView && selected) {
    if (imagesMissing) {
      cropBody = <CellImage recordId={label} imageWidth={1} imageHeight={1} overlay="none" size={CROP_SIZE} />;
    } else if (crop.status === "ready") {
      cropBody = <CellImage recordId={label} src={crop.src} imageWidth={1} imageHeight={1} overlay="none" label={cropLabel} size={CROP_SIZE} />;
    } else {
      cropBody = (
        <div className="rv-crop-pending" style={{ width: CROP_SIZE, height: CROP_SIZE }} role="status">
          {crop.status === "failed" ? "The crop could not be drawn" : "Reading the image…"}
        </div>
      );
    }
  }

  return (
    <Card
      className="rv-record-card"
      title="Selected measurement"
      subtitle={subtitle}
      actions={
        <>
          <Button size="small" disabled={!selected || position <= 0} onClick={props.onPrevious}>
            Previous record
          </Button>
          <Button size="small" disabled={!selected || position + 1 >= total} onClick={props.onNext}>
            Next record
          </Button>
          <Button size="small" disabled={!selected} onClick={props.onDetails} title="The frame viewer: series, zoom, stepping">
            Record details
          </Button>
        </>
      }
    >
      {/* The kit's RecordIdentity needs a record id, which files do not
          carry (#715): its layout, with the frame and object as the name. */}
      <div className="mib-record rv-record">
        <div className="mib-record__id" data-testid="record-identity">
          <span className="mib-record__name">{label}</span>
          {where ? <span className="mib-record__where">{where}</span> : null}
          {provenance ? <span className="mib-record__provenance">{provenance}</span> : null}
        </div>
      </div>
      <div className="rv-cell-views" role="group" aria-label="Image view">
        {CELL_VIEWS.map((v) => {
          const why = cropUnavailable(v.view, info, valid, row);
          return (
            <Button
              key={v.view}
              size="small"
              variant={view === v.view ? "selected" : "secondary"}
              aria-pressed={view === v.view}
              disabled={!selected || why !== null}
              title={why ?? undefined}
              onClick={() => onView(v.view)}
            >
              {v.label}
            </Button>
          );
        })}
      </div>
      <div className="rv-record__body">
        <div className="rv-record__image">
          <div
            className="canvas-wrap preview"
            hidden={cropView}
            onDoubleClick={() => selected && props.onDetails()}
            title="Double-click to open the viewer"
          >
            <canvas ref={canvasRef} className={fitWindow ? "fit" : ""} />
            <span className="preview-caption mono">
              {selected ? `#${position + 1} of ${total}${row ? ` · frame ${row.frame_index}` : ""}` : "—"}
            </span>
          </div>
          {cropBody}
        </div>
        {row ? <FactList facts={measurementFacts(row, info)} /> : selected ? <p className="rv-note">Reading the record…</p> : null}
      </div>
      {selected ? (
        <div className="rv-recorded">
          <h4 className="rv-recorded__title">What is recorded?</h4>
          <FactList facts={recordedFacts(info, valid, row)} />
        </div>
      ) : null}
    </Card>
  );
}

export interface StoredCardProps {
  info: ReviewInfo;
  /** Opens the Regenerate masks dialog (a new, separately labelled file). */
  onReprocess: () => void;
  reprocessDisabled: boolean;
}

export function StoredCard({ info, onReprocess, reprocessDisabled }: StoredCardProps) {
  return (
    <Card
      className="rv-stored-card"
      title="Stored vs reprocessed"
      subtitle="Never silently overwritten"
      status={<StateChip tone="neutral" size="small">Recorded file · read only</StateChip>}
      actions={
        <Button size="small" disabled={reprocessDisabled} onClick={onReprocess} title="Regenerate masks into a new file">
          Reprocess into a new file…
        </Button>
      }
    >
      <FactList facts={storedFacts(info)} />
      <p className="rv-note">
        Reprocessing runs the processing core over the run into a new file, labelled separately; the stored results stay as
        they were recorded.
      </p>
    </Card>
  );
}
