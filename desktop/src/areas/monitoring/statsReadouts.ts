// The statistics side panel and the status bar line, as Qt words them (epic #866: MON-095, 098, 100, 104;
// also SHELL-029 and SHELL-033). Qt: utils/StatisticsPanel.cpp, utils/StatsDisplayManager.cpp,
// MainWindow::renderStats. The shell views (Sidebar, StatusBar) only place what these return.
// Pure: no React or Tauri imports.
import { metricNumber } from "../../metricFormat";
import { formatMetricSample, type MetricSample } from "../../monitoringDensity";

// The bridge decoder's sample ({state, value, age_ms}) and its Qt formatter are the ones used here too (PR E1).
export { formatMetricSample };
export type { MetricSample };

export interface Readout {
  text: string;
  /** "ok" green, "warn" orange, "dim" grey (stale or inactive). */
  cls?: "ok" | "warn" | "dim";
  title?: string;
}

/** Qt: a value last updated more than this long ago is greyed with "Stale: last update N ms ago". */
export const STALE_AFTER_MS = 1000;

/** hh:mm:ss, Qt formatRuntime: zero or less (and anything not a number) is 00:00:00. */
export function formatRuntime(seconds: number | null | undefined): string {
  if (typeof seconds !== "number" || !Number.isFinite(seconds) || seconds <= 0) return "00:00:00";
  const total = Math.floor(seconds);
  const pad = (n: number) => String(n).padStart(2, "0");
  return `${pad(Math.floor(total / 3600))}:${pad(Math.floor((total % 3600) / 60))}:${pad(total % 60)}`;
}

/** Runtime row (MON-100): counts only while a run is Starting, Active or Stopping; Qt shows 00:00:00 otherwise. */
export function runtimeText(runActive: boolean, elapsedSeconds: number | null): string {
  return runActive ? formatRuntime(elapsedSeconds) : "00:00:00";
}

/** Flush Status row: Qt "Flushing..." in orange while a flush runs, "Idle" in grey otherwise. */
export function flushReadout(flushing: boolean): Readout {
  return flushing ? { text: "Flushing…", cls: "warn" } : { text: "Idle", cls: "dim" };
}

/** Algo Time (MON-095): average microseconds per frame, greyed after STALE_AFTER_MS with Qt's tooltip.
 *  null when the backend does not report it: the bridge's processing stats carry rates only (algo_fps1s) until
 *  `algo_avg_us` and its age are added to them, and a row that always says "—" is noise. */
export function algoTimeReadout(avgUs: number | null | undefined, ageMs: number | null | undefined): Readout | null {
  if (typeof avgUs !== "number" || !Number.isFinite(avgUs)) return null;
  const stale = typeof ageMs === "number" && Number.isFinite(ageMs) && ageMs > STALE_AFTER_MS;
  return stale
    ? { text: `${avgUs.toFixed(1)} µs`, cls: "dim", title: `Stale: last update ${Math.trunc(ageMs!)} ms ago` }
    : { text: `${avgUs.toFixed(1)} µs` };
}

/** Camera Status row (MON-098): Running in green, Stopped in grey. */
export function cameraStatusReadout(running: boolean): Readout {
  return running ? { text: "Running", cls: "ok" } : { text: "Stopped", cls: "dim" };
}

const MEASURED_HERE = "Measured from the frames this window received. The backend sent no capture rate.";

/** Camera FPS and Data Rate rows. The backend's capture sample (processing_stats.camera, bridge ABI 46) is shown as
 *  text with its validity, whole numbers as Qt's formatMetric writes them ("n/a", "unsupported", "error", "<n>
 *  (stale 3.0 s)"); only when the backend sends no camera telemetry does the rate measured from the frames this
 *  window received stand in (and say so). A stopped camera reads zero, as in Qt. */
export function cameraRateReadouts(i: {
  running: boolean; displayFps: number; dataRateMBs: number; captureFps?: MetricSample | null; captureMBs?: MetricSample | null;
  /** The backend sent camera telemetry that broke the contract: the stand-in says so instead of passing for a measurement of the camera. */
  telemetryError?: string;
}): { fps: Readout; dataRate: Readout } {
  if (!i.running) return { fps: { text: "0.0" }, dataRate: { text: "0.0 MB/s" } };
  const why = i.telemetryError ? `The backend's camera telemetry could not be read (${i.telemetryError}). ${MEASURED_HERE}` : MEASURED_HERE;
  const fps: Readout = i.captureFps ? { text: formatMetricSample(i.captureFps) } : { text: metricNumber(i.displayFps, 1), title: why };
  const rate: Readout = i.captureMBs ? { text: `${formatMetricSample(i.captureMBs)} MB/s` } : { text: `${metricNumber(i.dataRateMBs, 1)} MB/s`, title: why };
  return { fps, dataRate: rate };
}

/** The one-line status bar text (MON-104), Qt MainWindow::renderStats:
 *  "Camera 30 fps · Valid 1.2/s · Invalid 3.4/s · Algo 85 µs · Run 12 s · buffered 40 (flushing)". The element
 *  that shows it elides the end, so a narrow window never grows. Without host processing (the PL does the
 *  science) the rates say so instead. */
export function compactStatusLine(i: {
  running: boolean; cameraFps: number; hostProcessing: boolean; validFps: number | null; invalidFps: number | null;
  algoAvgUs?: number | null; algoFps?: number | null; runActive: boolean; runSeconds: number | null; buffered: bigint | number; flushing: boolean;
}): string {
  let line = i.running ? `Camera ${Math.round(i.cameraFps)} fps` : "Camera stopped";
  if (i.hostProcessing) {
    line += ` · Valid ${metricNumber(i.validFps, 1)}/s · Invalid ${metricNumber(i.invalidFps, 1)}/s`;
    line += typeof i.algoAvgUs === "number" && Number.isFinite(i.algoAvgUs) ? ` · Algo ${i.algoAvgUs.toFixed(0)} µs` : ` · Algo ${metricNumber(i.algoFps, 0)}/s`;
  } else {
    line += " · Science on PL";
  }
  if (i.runActive) {
    line += ` · Run ${typeof i.runSeconds === "number" && i.runSeconds > 0 ? Math.round(i.runSeconds) : 0} s · buffered ${i.buffered}${i.flushing ? " (flushing)" : ""}`;
  }
  return line;
}
