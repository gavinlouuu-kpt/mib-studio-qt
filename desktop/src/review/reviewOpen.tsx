// What the Qt Review tab tells the operator when it opens a file (HdfReviewTab.cpp:540-581, 598-728,
// resources/ui/HdfReviewTab.ui:213-218), shared by both Tauri surfaces: MIB Studio's Review stage
// (areas/review) and YOFO Review (ReviewPanel). Parity rows REVIEW-003 (the open-failure dialog) and
// REVIEW-004 (the status label) of epic #867.
import { useEffect, useId, useRef } from "react";

/** QMessageBox::critical title. */
export const OPEN_FAILURE_TITLE = "Cannot Open HDF5 File";
/** The detail when the file exists but the backend cannot read it (QFile::exists true). */
export const CORRUPT_FILE_DETAIL =
  "The file exists but its HDF5 metadata is corrupt, likely caused by an interrupted write (crash or forced close during an experiment).\n\n" +
  "Frame data may be partially recoverable using the h5recover tool from the HDF5 utilities package.";
/** The detail when there is no such file (QFile::exists false). */
export const MISSING_FILE_DETAIL = "File not found.";

/** The Qt status label before any file, after Close File and while nothing is wrong. */
export const STATUS_READY = "Ready";
export const STATUS_LOADING = "Loading...";
export const STATUS_ERROR = "Error loading file";

/** Where an open stands: nothing in flight (or done), reading the file, or failed. */
export type OpenPhase = "idle" | "loading" | "error";

export interface OpenFailure {
  path: string;
  /** The Qt detail paragraph(s): `MISSING_FILE_DETAIL` or `CORRUPT_FILE_DETAIL`. */
  detail: string;
}

/** Qt tells a missing file from a corrupt one by `QFile::exists`. The backend makes the same
 * call where the file is (ReviewSession::open: "File not found: <path>"; the facade's load
 * result: "Recording load failed: file not found"), so the browser needs no file system. */
export function isMissingFileMessage(message: string): boolean {
  return /file not found/i.test(message);
}

/** MIB Studio's load answers "Recording load failed" when the file cannot be opened; its other refusals
 * (an experiment running, a raw recording) are about the state of the instrument, not the file. */
export function isOpenFailureMessage(message: string): boolean {
  return /^Recording load failed/.test(message);
}

export function openFailureFor(path: string, backendMessage: string): OpenFailure {
  return { path, detail: isMissingFileMessage(backendMessage) ? MISSING_FILE_DETAIL : CORRUPT_FILE_DETAIL };
}

/** The dialog body, as Qt composes it: `Failed to open:\n<path>\n\n<detail>`. */
export function openFailureText(failure: OpenFailure): string {
  return `Failed to open:\n${failure.path}\n\n${failure.detail}`;
}

export interface OpenedCounts {
  file_open: boolean;
  recording_file: boolean;
  total_valid: number | string;
  total_invalid: number | string;
  /** Recording files: frames filtered (empty) at record time. */
  filtered_frames?: number | string;
}

/** The status label: "Loading..." and "Error loading file" while an open runs or failed, else what
 * the file holds as Qt words it ("Valid: N, Invalid: M", or "Recording: N frames, M empty
 * skipped"), "Ready" with no file. `summary` is the accounting text Qt appends (" · run ..."). */
export function reviewStatusText(phase: OpenPhase, opened: OpenedCounts | null, summary = ""): string {
  if (phase === "loading") return STATUS_LOADING;
  if (phase === "error") return STATUS_ERROR;
  if (!opened?.file_open) return STATUS_READY;
  if (opened.recording_file) return `Recording: ${opened.total_valid} frames, ${opened.filtered_frames ?? 0} empty skipped${summary}`;
  return `Valid: ${opened.total_valid}, Invalid: ${opened.total_invalid}${summary}`;
}

/** The status label element (tour anchor `review-status`). */
export function ReviewStatus({ text }: { text: string }) {
  return <span className="review-status" role="status" data-tour="review-status" data-testid="review-status">{text}</span>;
}

/** The QMessageBox::critical of a failed open. Modal: only OK or Escape dismisses it. */
export function OpenFailureDialog({ failure, onClose }: { failure: OpenFailure; onClose: () => void }) {
  const titleId = useId();
  const bodyId = useId();
  const ok = useRef<HTMLButtonElement>(null);
  useEffect(() => ok.current?.focus(), []);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => { if (e.key === "Escape") onClose(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);
  return (
    <div className="modal-backdrop">
      <div className="modal" role="alertdialog" aria-modal="true" aria-labelledby={titleId} aria-describedby={bodyId} data-tour="review-open-failure">
        <h3 id={titleId}>{OPEN_FAILURE_TITLE}</h3>
        <p id={bodyId} data-testid="open-failure-text" style={{ whiteSpace: "pre-wrap", overflowWrap: "anywhere" }}>{openFailureText(failure)}</p>
        <div className="actions">
          <button className="btn" ref={ok} onClick={onClose}>OK</button>
        </div>
      </div>
    </div>
  );
}
