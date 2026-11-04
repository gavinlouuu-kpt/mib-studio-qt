// The reference contour's file side (epic #866 PR E2: MON-061, 062, 086): what to say when a file does not
// give a contour, and what to do with the remembered path. Qt reference: ExperimentMonitoringTab::
// loadKdeReferenceFromFile and the start-up restore in its constructor, MonitoringSettingsDialog.cpp (the
// error dialog). Pure.
//
// Qt reads the file with a reader of its own and drops the remembered path whenever the load fails. The
// bridge reads it only while the instrument is idle (bridge-contract.json `kde_reference_from_file` rules): a
// run in progress, a review job or a raw recording writing HDF5 refuses the read. That refusal says nothing
// about the file, so the remembered path stays and the load is tried again when the instrument is idle.
import { kdeReferenceLabel, type KdeReference } from "../../../monitoringDensity";
import type { DensityReference } from "./densityModel";

/** The Qt dialog's title. */
export const REFERENCE_FILE_DIALOG_TITLE = "Reference core contour from experiment";
export const REFERENCE_ERROR_TITLE = "Reference contour";
/** Qt: "Experiment files (*.h5 *.hdf5)". */
export const REFERENCE_FILE_FILTERS = [{ name: "Experiment files", extensions: ["h5", "hdf5"] }];

// The bridge's own refusals that are about the moment, not the file (bridge-contract.json kde_reference_from_file).
const TRY_AGAIN_LATER = [
  /^a review job is running/,
  /^a raw recording is writing/,
  /^an experiment is running or starting/,
];

/** True for a refusal that a later read can succeed after: the instrument is busy, the file is not at fault. */
export function isBusyRefusal(reason: string | null | undefined): boolean {
  return typeof reason === "string" && TRY_AGAIN_LATER.some((re) => re.test(reason));
}

/** Qt's warning text: "No reference contour loaded from\n<path>\n\n<reason>". */
export function referenceErrorText(path: string, reason: string): string {
  return `No reference contour loaded from\n${path}\n\n${reason}`;
}

/** The reason of a load that did not give a contour. */
export function referenceFailureReason(reply: KdeReference): string {
  return reply.reason ?? reply.error ?? "unknown reason";
}

/** The reference a successful reply stands for (Qt's label), or null when it is not a usable one. */
export function referenceOfReply(reply: KdeReference): DensityReference | null {
  const label = kdeReferenceLabel(reply);
  return label !== null && reply.contours.length > 0 ? { label, contours: reply.contours } : null;
}

export type RestoreOutcome =
  | { kind: "restored"; reference: DensityReference }
  /** The instrument is busy: keep the path, try again when it is idle. */
  | { kind: "retry"; why: string }
  /** The file is gone or has no contour: forget the path (Qt: one info line). */
  | { kind: "drop"; why: string };

/** What the start-up restore does with the remembered path after `fetch_kde_reference_from_file` answered. */
export function restoreOutcome(reply: KdeReference): RestoreOutcome {
  const reference = reply.ok ? referenceOfReply(reply) : null;
  if (reference) return { kind: "restored", reference };
  const why = referenceFailureReason(reply);
  return isBusyRefusal(why) ? { kind: "retry", why } : { kind: "drop", why };
}

/** The same for a call that threw: an argument the bridge refuses (an empty or oversize path) is not worth keeping;
 *  anything else (a lost connection) may pass. */
export function restoreOutcomeOfError(error: unknown): RestoreOutcome {
  const why = error instanceof Error ? error.message : String(error);
  return /^INVALID_ARGUMENTS/.test(why) ? { kind: "drop", why } : { kind: "retry", why };
}

/** The folder a "From file…" picker starts in: where the remembered file is (Qt: QFileInfo(path).absolutePath()). */
export function referenceStartDir(path: string): string | undefined {
  const at = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return at > 0 ? path.slice(0, at) : undefined;
}
