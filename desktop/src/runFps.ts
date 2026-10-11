// The Run frame rate (bridge ABI 38): types, the sensor timing the backend derives from a rate, and the texts. PzRunTiming.h is the source of the
// numbers; the tests pin both sides to the same vectors (HMAX 58 forced, SHS 64, so the exposure follows the frame length).

export interface RunFpsStatus {
  available: boolean;
  reason?: string;
  min_fps: number;
  max_fps: number;
  default_fps: number;
  qualified_fps: number;
  fps?: number;
  exposure_us?: number;
  hmax?: number;
  vmax?: number;
  shs?: number;
  exposure_lines?: number;
  actual_fps?: number;
  qualified?: boolean;
  in_run?: boolean;
  /** The rate of the last Run entry; null outside Run. */
  applied_fps?: number | null;
  /** The camera runs at another rate than the configured one: enter Run again. */
  pending?: boolean;
  /** A stopped Run's ring is held: changing the rate discards its buffered frames. */
  held?: boolean;
}

export const QUALIFIED_NOTE = "5,000 fps is the qualified rate; others are not yet validated on the board.";
const SENSOR_CLOCK_HZ = 74.25e6, HMAX = 58, SHS = 64;

export interface DraftTiming { vmax: number; lines: number; exposureUs: number; actualFps: number }

/** The sensor timing for a rate (the same arithmetic as pz::runTiming). */
export function draftTiming(fps: number): DraftTiming {
  const vmax = Math.floor(SENSOR_CLOCK_HZ / (HMAX * fps));
  const lines = vmax - SHS;
  return { vmax, lines, exposureUs: fps === 5000 ? 150 : (lines * HMAX) / SENSOR_CLOCK_HZ * 1e6, actualFps: SENSOR_CLOCK_HZ / (HMAX * vmax) };
}

export type RateInput = { ok: true; fps: number } | { ok: false; reason: string };

/** The operator's text as a Run rate: a whole number of frames per second within the limits. */
export function parseRunFps(text: string, min = 100, max = 5000): RateInput {
  const t = text.trim().replace(/[,\s]/g, "");
  if (!/^\d+$/.test(t)) return { ok: false, reason: `enter a whole number of frames per second from ${min} to ${max}` };
  const fps = Number(t);
  if (fps < min || fps > max) return { ok: false, reason: `the Run rate must be from ${min} to ${max} fps` };
  return { ok: true, fps };
}

export function exposureText(us: number): string {
  return us >= 1000 ? `${(us / 1000).toFixed(2)} ms` : `${Math.round(us)} µs`;
}

export function fpsText(fps: number, digits = 0): string {
  return fps.toLocaleString("en-US", { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

/** "1,000 fps · exposure 950 µs · the sensor runs 1,000.1 fps" */
export function rateSummary(t: { fps: number; exposure_us: number; actual_fps: number }): string {
  return `${fpsText(t.fps)} fps · exposure ${exposureText(t.exposure_us)} · the sensor runs ${fpsText(t.actual_fps, 1)} fps`;
}

/** What the operator is told before applying: whether the camera restarts, and what a held ring loses. */
export function applyNotice(status: RunFpsStatus | null | undefined, ringFrames: number): string {
  if (!status?.available) return "";
  const parts: string[] = [];
  if (status.in_run) parts.push("Applying re-enters Run (the camera restarts for a few seconds).");
  else parts.push("The rate applies when the instrument next enters Run.");
  if (status.held) parts.push(`The Run is stopped to review ${ringFrames.toLocaleString("en-US")} buffered frames: applying discards them.`);
  return parts.join(" ");
}

/** The question asked before a rate change that discards a stopped Run's frames; empty when nothing is lost. */
export function discardQuestion(status: RunFpsStatus | null | undefined, ringFrames: number): string {
  if (!status?.held) return "";
  return `Changing the Run rate discards the ${ringFrames.toLocaleString("en-US")} buffered frames of the stopped Run. Continue?`;
}

/** Shown while the camera runs at another rate than the one set. */
export function pendingNotice(status: RunFpsStatus | null | undefined): string {
  if (!status?.available || !status.pending || status.applied_fps == null || status.fps == null) return "";
  return `The camera runs at ${fpsText(status.applied_fps)} fps; ${fpsText(status.fps)} fps is set and applies after Run is entered again.`;
}
