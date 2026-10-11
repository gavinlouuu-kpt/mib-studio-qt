// The Run frame rate and exposure of the PZ7035 (bridge ABI 38): types and the texts. The numbers come from the backend (PzRunTiming.h) through
// fetch_run_fps and check_pz_camera_document; nothing here recomputes sensor timing.

export interface RunTimingFields {
  fps: number;
  /** "auto": SHS 64, the exposure follows the frame length; "manual": a number of µs inside the strobe window. */
  exposure_mode: "auto" | "manual";
  exposure_us: number;
  hmax: number;
  vmax: number;
  shs: number;
  exposure_lines: number;
  actual_fps: number;
  /** µs after XVS. */
  exposure_start_us: number;
  exposure_end_us: number;
  strobe_start_us: number;
  strobe_end_us: number;
  strobe_overlaps: boolean;
  strobe_straddles_exposure_start: boolean;
  strobe_warning: string;
  allowed_exposure_min_us: number;
  allowed_exposure_max_us: number;
}

export interface RunFpsStatus extends Partial<RunTimingFields> {
  available: boolean;
  reason?: string;
  min_fps: number;
  max_fps: number;
  default_fps: number;
  qualified_fps: number;
  /** Only 5000 fps with the automatic exposure is the qualified setting. */
  qualified?: boolean;
  in_run?: boolean;
  /** The setting of the last Run entry; null outside Run. */
  applied_fps?: number | null;
  applied_exposure_us?: number | null;
  /** The camera runs at another setting than the configured one: enter Run again. */
  pending?: boolean;
  /** A stopped Run's ring is held: changing the setting discards its buffered frames. */
  held?: boolean;
}

/** check_pz_camera_document: the validation of a document text, with the timing it gives when it is valid. */
export type PzCameraCheck = ({ ok: true; qualified: boolean } & RunTimingFields) | { ok: false; error: string };

export const QUALIFIED_NOTE = "5,000 fps is the qualified rate; others are not yet validated on the board.";

export function exposureText(us: number): string {
  return us >= 1000 ? `${(us / 1000).toFixed(2)} ms` : `${Math.round(us)} µs`;
}

export function fpsText(fps: number, digits = 0): string {
  return fps.toLocaleString("en-US", { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

/** "1,000 fps · exposure auto 950 µs (SHS 64) · the sensor runs 1,000.1 fps" */
export function rateSummary(t: Pick<RunTimingFields, "fps" | "exposure_mode" | "exposure_us" | "shs" | "actual_fps">): string {
  const mode = t.exposure_mode === "auto" ? "auto " : "";
  return `${fpsText(t.fps)} fps · exposure ${mode}${exposureText(t.exposure_us)} (SHS ${t.shs}) · the sensor runs ${fpsText(t.actual_fps, 1)} fps`;
}

/** Where the exposure and the (Service-only) LED strobe fall in a frame. */
export function strobeSummary(t: Pick<RunTimingFields, "exposure_start_us" | "exposure_end_us" | "strobe_start_us" | "strobe_end_us">): string {
  const r = (v: number) => Math.round(v);
  return `exposure ${r(t.exposure_start_us)} to ${r(t.exposure_end_us)} µs after XVS, LED strobe ${r(t.strobe_start_us)} to ${r(t.strobe_end_us)} µs`;
}

/** Shown beside a valid document: the qualification note when it is not the qualified setting. */
export function qualificationNote(qualified: boolean | undefined): string {
  return qualified ? "" : QUALIFIED_NOTE;
}

/** What the operator is told before applying: whether the camera restarts, and what a held ring loses. */
export function applyNotice(status: RunFpsStatus | null | undefined, ringFrames: number): string {
  if (!status?.available) return "";
  const parts: string[] = [];
  if (status.in_run) parts.push("Applying re-enters Run (the camera restarts for a few seconds).");
  else parts.push("The setting applies when the instrument next enters Run.");
  if (status.in_run && status.held) parts.push(`The Run is stopped to review ${ringFrames.toLocaleString("en-US")} buffered frames: applying discards them.`);
  return parts.join(" ");
}

/** The question asked before a change that discards a stopped Run's frames; empty when nothing is lost. */
export function discardQuestion(status: RunFpsStatus | null | undefined, ringFrames: number): string {
  if (!status?.available || !status.in_run || !status.held) return "";
  return `Applying the camera document discards the ${ringFrames.toLocaleString("en-US")} buffered frames of the stopped Run. Continue?`;
}

/** Shown while the camera runs at another setting than the configured one. */
export function pendingNotice(status: RunFpsStatus | null | undefined): string {
  if (!status?.available || !status.pending || status.applied_fps == null || status.fps == null) return "";
  return `The camera runs at ${fpsText(status.applied_fps)} fps; ${fpsText(status.fps)} fps is set and applies after Run is entered again.`;
}
