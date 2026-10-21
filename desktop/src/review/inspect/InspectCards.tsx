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
//   The crop places the stored box through the file's object coordinate
//   space (B3c); a file whose space is unknown offers the full frame only.
// - TrackCard ("Linked cell records", B3c): the selected record's track,
//   read with `fetch_review_track_rows` (at most 256 rows) only when asked.
// - StoredCard ("Stored vs reprocessed"): the core, calibration, profile,
//   E-LUT and object coordinate space the stored results came from, and the
//   selected record reprocessed now (`review_reprocess_record`, B3c; sent
//   only by its button, never written). Reprocessing into a file is the
//   Regenerate masks job, which writes a new file; this view lists no
//   reprocessing history (#754).
import type { ReactNode, RefObject } from "react";
import { Button, Card, CellImage, DataTable, FactList, StateChip, type DataTableColumn } from "@mib/ui";
import { metricNumber } from "../../metricFormat";
import { REVIEW_REPROCESS } from "../reviewContract";
import type { ReviewInfo, ReviewReprocess, ReviewRow, ReviewTrackRows } from "../reviewBridge";
import {
  CELL_VIEWS,
  CROP_UNKNOWN_SPACE,
  cropUnavailable,
  measurementFacts,
  objectOffset,
  recordedFacts,
  recordLabel,
  recordProvenance,
  recordWhere,
  reprocessComparison,
  reprocessLutDiffers,
  reprocessMatchText,
  storedFacts,
  trackSummary,
  type CellView,
  type CompareLine,
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
      {selected && !info.recording_file && !objectOffset(info) ? <p className="rv-note rv-crop-note">{CROP_UNKNOWN_SPACE}</p> : null}
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

/** A request sent by a button: its answer, the refusal, or still waiting. */
export type Asked<T> = { status: "pending" } | { status: "refused"; message: string } | { status: "ready"; value: T };

export interface TrackCardProps {
  valid: boolean;
  row: ReviewRow | null;
  /** The selected record's set position (-1 = none). */
  position: number;
  /** The track's rows, when asked for this record's track. */
  track: Asked<ReviewTrackRows> | null;
  onShow: () => void;
  onSelect: (position: number) => void;
}

type TrackLine = { position: number; row: ReviewRow };

const TRACK_COLUMNS: DataTableColumn<TrackLine>[] = [
  { key: "position", header: "#", cell: (l) => String(l.position + 1), numeric: true },
  { key: "frame", header: "Frame", cell: (l) => l.row.frame_index, emphasis: true },
  { key: "object", header: "Object", cell: (l) => String(l.row.object_id), numeric: true },
  { key: "area", header: "Area (µm²)", cell: (l) => metricNumber(l.row.area_um2, 2), numeric: true },
  { key: "deformability", header: "Deformability", cell: (l) => metricNumber(l.row.deformability, 4), numeric: true },
  { key: "validation", header: "Validation", cell: (l) => (l.row.valid ? "valid" : "invalid") },
];

/** The selected record's linked records (its track), read only when asked. */
export function TrackCard({ valid, row, position, track, onShow, onSelect }: TrackCardProps) {
  const tracked = !!row && row.track_id >= 0;
  let body: ReactNode;
  if (!row) body = <p className="rv-note">Select a record</p>;
  else if (!tracked) body = <p className="rv-note">Not tracked: this record has no linked records</p>;
  else if (!track)
    body = (
      <p className="rv-note">
        Track {row.track_id} · {row.track_observation_count} {row.track_observation_count === 1 ? "observation" : "observations"} recorded
      </p>
    );
  else if (track.status === "pending") body = <p className="rv-note" role="status">Reading the track…</p>;
  else if (track.status === "refused") body = <p className="rv-note rv-refusal" role="alert">Not read: {track.message}</p>;
  else {
    const t = track.value;
    const lines: TrackLine[] = t.rows.map((r, k) => ({ position: Number(t.positions[k] ?? -1), row: r }));
    body = (
      <>
        <p className={`rv-note${t.truncated ? " rv-truncated" : ""}`} data-testid="track-summary">
          {trackSummary(t, row.track_id, valid)}
        </p>
        <div className="rv-track-table">
          <DataTable
            density="compact"
            columns={TRACK_COLUMNS}
            rows={lines}
            rowKey={(l) => String(l.position)}
            selectedKey={position >= 0 ? String(position) : undefined}
            onSelect={(_, l) => l.position >= 0 && onSelect(l.position)}
            empty="No records of this track in this set."
          />
        </div>
      </>
    );
  }
  return (
    <Card
      className="rv-track-card"
      title="Linked cell records"
      subtitle={tracked ? `The records of track ${row!.track_id} in this set` : "The records of the selected record's track"}
      actions={
        <Button size="small" disabled={!tracked || track?.status === "pending"} onClick={onShow} title={tracked ? "Read this track's records (at most 256)" : undefined}>
          Show linked records
        </Button>
      }
    >
      {body}
    </Card>
  );
}

export interface StoredCardProps {
  info: ReviewInfo;
  /** Opens the Regenerate masks dialog (a new, separately labelled file). */
  onReprocess: () => void;
  reprocessDisabled: boolean;
  /** Reprocess the selected record now (read only; sent only by the button). */
  onReprocessRecord: () => void;
  /** Why it cannot be sent, said under the button; null when it can. */
  reprocessRecordUnavailable: string | null;
  /** REVIEW_REPROCESS.settings value. */
  reprocessSettings: number;
  onReprocessSettings: (settings: number) => void;
  /** The selected record's answer, when asked for it. */
  reprocess: Asked<ReviewReprocess> | null;
}

const COMPARE_COLUMNS: DataTableColumn<CompareLine>[] = [
  { key: "label", header: "", cell: (l) => l.label, emphasis: true },
  { key: "stored", header: "Stored", cell: (l) => l.stored },
  { key: "reprocessed", header: "Reprocessed", cell: (l) => (l.warn ? <span className="rv-compare-warn" data-testid="reprocess-lut-differs">{l.reprocessed}</span> : l.reprocessed) },
];

function ReprocessResult({ info, result }: { info: ReviewInfo; result: Asked<ReviewReprocess> }) {
  if (result.status === "pending") return <p className="rv-note" role="status">Reprocessing the record…</p>;
  if (result.status === "refused") return <p className="rv-note rv-refusal" role="alert">Not reprocessed: {result.message}</p>;
  const r = result.value;
  const matched = r.match_index !== null;
  return (
    <div className="rv-reprocess-result" data-testid="reprocess-result">
      <p className={`rv-note${matched && !reprocessLutDiffers(r, info) ? "" : " rv-refusal"}`} data-testid="reprocess-match">
        {reprocessMatchText(r, info)}
      </p>
      {r.object_count_differs && r.stored ? (
        <p className="rv-note rv-truncated">
          The stored frame records {r.stored.object_count} {r.stored.object_count === 1 ? "object" : "objects"}; the kernel found{" "}
          {r.truncated ? `more than ${r.reprocessed.length}` : r.reprocessed.length}
        </p>
      ) : null}
      <div className="rv-compare">
        <DataTable className="rv-compare-table" density="compact" columns={COMPARE_COLUMNS} rows={reprocessComparison(r, info)} rowKey={(l) => l.label} />
      </div>
      <p className="rv-note">
        Settings: {r.settings_label} · core {r.core_version}
        {r.core_source ? ` (${r.core_source})` : ""}. Computed in memory; nothing is written.
      </p>
    </div>
  );
}

export function StoredCard(props: StoredCardProps) {
  const { info, onReprocess, reprocessDisabled, reprocess, reprocessRecordUnavailable } = props;
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
      <div className="rv-reprocess">
        <h4 className="rv-recorded__title">This record, reprocessed now</h4>
        <div className="rv-reprocess__controls">
          <select
            aria-label="Reprocess settings"
            value={props.reprocessSettings}
            onChange={(e) => props.onReprocessSettings(Number(e.target.value))}
            disabled={reprocessRecordUnavailable !== null}
          >
            <option value={REVIEW_REPROCESS.settings.recorded}>Recorded settings</option>
            <option value={REVIEW_REPROCESS.settings.default}>Default settings</option>
          </select>
          <Button
            size="small"
            disabled={reprocessRecordUnavailable !== null || reprocess?.status === "pending"}
            title={reprocessRecordUnavailable ?? "Run the processing kernel on this record's stored image; nothing is written"}
            onClick={props.onReprocessRecord}
          >
            Reprocess this record
          </Button>
        </div>
        {reprocessRecordUnavailable !== null && !info.recording_file ? (
          <p className="rv-note rv-reprocess-unavailable">{reprocessRecordUnavailable}</p>
        ) : null}
        {reprocess ? <ReprocessResult info={info} result={reprocess} /> : null}
      </div>
      <p className="rv-note">
        Reprocessing into a new file runs the processing core over the run, labelled separately; the stored results stay as
        they were recorded.
      </p>
    </Card>
  );
}
