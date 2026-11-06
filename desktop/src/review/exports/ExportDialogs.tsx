// Export dialogs (plan 2026-10-01-standalone-review-app, PR 4): the job
// progress dialog (progress, phase text, Cancel; then the outcome with
// "Show in folder") and the Export All series-range prompt — the Qt tab's
// QProgressDialog and series QMessageBox.
import { useState } from "react";
import { isRemote } from "../../transport";
import { revealItemInDir } from "../../transport/dialogs";
import { REVIEW_OPERATION_KINDS } from "../reviewContract";
import { batchSummaryText, eventMapPathOf, parseBatchSummary, parseSeriesRange, type SeriesChoice } from "./exportHelpers";
import type { TrackedJob } from "./useReviewJobs";

/** Why "Show in folder" is unavailable in a browser: the output is on the
 * instrument, which this computer's file manager cannot open. */
export const REMOTE_REVEAL_REASON = "The output is on the instrument, so this browser cannot open its folder.";

/** Path a completed job produced (the job's terminal message), or "". */
export function outputPathOf(job: TrackedJob): string {
  if (job.phase !== "completed") return "";
  if (job.kind === REVIEW_OPERATION_KINDS.ExportMetrics || job.kind === REVIEW_OPERATION_KINDS.ExportAll) return job.message;
  if (job.kind === REVIEW_OPERATION_KINDS.RegenerateMasks) return job.message.replace(/ \(\d+ images:.*\)$/, "");
  if (job.kind === REVIEW_OPERATION_KINDS.ExportCharts) return job.message.replace(/^exported \d+ chart\(s\) to /, "");
  if (job.kind === REVIEW_OPERATION_KINDS.ExportFcs) return parseFcsOutcome(job.message).path;
  return "";
}

/** Export FCS's terminal message: `<path> (N events: V valid, I invalid)`. */
export function parseFcsOutcome(message: string): { path: string; events?: number; valid?: number; invalid?: number } {
  const m = /^(.*) \((\d+) events: (\d+) valid, (\d+) invalid\)$/s.exec(message);
  return m ? { path: m[1], events: Number(m[2]), valid: Number(m[3]), invalid: Number(m[4]) } : { path: message };
}

/** The Qt success text of Export FCS ("Exported N FCS 3.1 events (V valid,
 * I invalid) to … Event map: …"). */
export function fcsOutcomeText(message: string): string {
  const o = parseFcsOutcome(message);
  const counts = o.events === undefined ? "" : ` ${o.events} FCS 3.1 events (${o.valid} valid, ${o.invalid} invalid)`;
  return `Exported${counts} to ${o.path}. Event map: ${eventMapPathOf(o.path)}`;
}

/** Progress text with paths shortened to their file name
 * ("valid_images: /long/.partial-x/valid_frame_000001.tiff" → "valid_images: valid_frame_000001.tiff"). */
export function shortProgress(message: string): string {
  return message.replace(/(?:[A-Za-z]:)?[\\/][^\s:]*[\\/]([^\\/\s]+)/g, "$1");
}

const capitalize = (t: string) => (t ? t[0].toUpperCase() + t.slice(1) : t);

export function outcomeText(job: TrackedJob): string {
  switch (job.phase) {
    case "running":
      return shortProgress(job.message) || "Starting…";
    case "cancelled":
      return `Cancelled. ${capitalize(job.message) || "Partial output discarded."}`;
    case "failed":
      return job.id === "0" ? `Not started: ${job.message}` : `Failed: ${job.message}`;
    case "completed": {
      if (job.kind === REVIEW_OPERATION_KINDS.BatchExport) {
        const s = parseBatchSummary(job.message);
        return s ? batchSummaryText(s) : job.message;
      }
      if (job.kind === REVIEW_OPERATION_KINDS.ExportCharts) return capitalize(job.message);
      if (job.kind === REVIEW_OPERATION_KINDS.ExportFcs) return fcsOutcomeText(job.message);
      const path = outputPathOf(job);
      return path ? `Written to ${path}` : capitalize(job.message);
    }
  }
}

export function JobDialog(props: { job: TrackedJob; onCancel: () => void; onClose: () => void; extraAction?: { label: string; onClick: () => void }; log?: (line: string) => void }) {
  const { job, onCancel, onClose, extraAction, log } = props;
  // "Show in folder" can fail (no file manager, a folder that is gone): say so here and in the log.
  const [revealError, setRevealError] = useState("");
  const reveal = (path: string) => {
    setRevealError("");
    revealItemInDir(path).catch((e) => {
      const message = String(e instanceof Error ? e.message : e);
      setRevealError(message);
      log?.(`show in folder failed: ${message}`);
    });
  };
  const running = job.phase === "running";
  const pct = job.total > 0 ? Math.min(100, Math.round((job.progress / job.total) * 100)) : null;
  const batch = job.kind === REVIEW_OPERATION_KINDS.BatchExport && job.phase === "completed" ? parseBatchSummary(job.message) : null;
  const path = outputPathOf(job);
  return (
    <div className="modal-backdrop">
      <div className="modal job-dialog" role="dialog" aria-label={job.title} aria-busy={running}>
        <h3>{job.title}</h3>
        {running && (
          <div className="job-progress" role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={pct ?? undefined}>
            <div className={`job-progress-bar${pct === null ? " indeterminate" : ""}`} style={pct === null ? undefined : { width: `${pct}%` }} />
          </div>
        )}
        <p className={`job-outcome ${job.phase}`}>{outcomeText(job)}</p>
        {batch && batch.failures.length > 0 && (
          <ul className="job-failures mono">
            {batch.failures.map((f) => (
              <li key={f.file}>
                {f.file}: {f.reason}
              </li>
            ))}
          </ul>
        )}
        {revealError && (
          <p className="form-error" role="alert">
            Could not open the folder: {revealError}
          </p>
        )}
        <div className="actions">
          {running ? (
            <button className="btn" onClick={onCancel} disabled={job.cancelling}>
              {job.cancelling && job.id ? "Cancelling…" : "Cancel"}
            </button>
          ) : (
            <>
              {path &&
                (isRemote ? (
                  <button className="btn" disabled title={REMOTE_REVEAL_REASON}>
                    Show in folder
                  </button>
                ) : (
                  <button className="btn" onClick={() => reveal(path)}>
                    Show in folder
                  </button>
                ))}
              {extraAction && (
                <button className="btn" onClick={extraAction.onClick}>
                  {extraAction.label}
                </button>
              )}
              <button className="btn" onClick={onClose} autoFocus>
                Close
              </button>
            </>
          )}
        </div>
      </div>
    </div>
  );
}

/** Export All: how to export multi-image series frames (Qt: all, a custom
 * 1-based range such as 9-15, or skip). One choice applies to every record. */
export function SeriesPrompt(props: { imagesPerRecord?: number; onChoose: (choice: SeriesChoice) => void; onCancel: () => void }) {
  const [mode, setMode] = useState<"all" | "range" | "skip">("all");
  const [text, setText] = useState("");
  const parsed = mode === "range" ? parseSeriesRange(text, props.imagesPerRecord) : null;
  const error = parsed && !parsed.ok ? parsed.error : "";
  const submit = () => {
    if (mode === "all") props.onChoose({ kind: "all" });
    else if (mode === "skip") props.onChoose({ kind: "skip" });
    else if (parsed?.ok) props.onChoose(parsed.choice);
  };
  return (
    <div className="modal-backdrop" onClick={props.onCancel}>
      <div className="modal series-prompt" role="dialog" aria-label="Export multi-image series" onClick={(e) => e.stopPropagation()}>
        <h3>Export multi-image series</h3>
        <p>
          This file has multi-image records{props.imagesPerRecord ? ` (${props.imagesPerRecord} images each)` : ""}. The choice applies to every
          record.
        </p>
        <label className="radio-row">
          <input type="radio" name="series" checked={mode === "all"} onChange={() => setMode("all")} /> All images
        </label>
        <label className="radio-row">
          <input type="radio" name="series" checked={mode === "range"} onChange={() => setMode("range")} /> Custom range
          <input
            type="text"
            className="series-range-input"
            placeholder="e.g. 9-15"
            value={text}
            aria-label="Series range (1-based)"
            onFocus={() => setMode("range")}
            onChange={(e) => setText(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && submit()}
          />
        </label>
        <label className="radio-row">
          <input type="radio" name="series" checked={mode === "skip"} onChange={() => setMode("skip")} /> Skip series images
        </label>
        {error && text && <p className="form-error">{error}</p>}
        <div className="actions">
          <button className="btn" onClick={props.onCancel}>
            Cancel
          </button>
          <button className="btn primary" onClick={submit} disabled={mode === "range" && !parsed?.ok}>
            Export
          </button>
        </div>
      </div>
    </div>
  );
}
