// The statistics side panel and the status bar line, as Qt words them (epic #866: MON-095, 098, 100, 104;
// also SHELL-029 and SHELL-033). Qt: utils/StatisticsPanel.cpp, utils/StatsDisplayManager.cpp,
// MainWindow::renderStats. The shell views (Sidebar, StatusBar) only place what these return.
// Pure: no React or Tauri imports.
import { metricNumber } from "../../metricFormat";

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

/** A telemetry sample with its validity (Qt backend::services::MetricSample). */
export interface MetricSample { value: number; validity: "valid" | "stale" | "unavailable" | "unsupported" | "error"; age_us?: number }

/** Qt StatsDisplayManager::formatMetric: an unavailable, unsupported, stale or failed reading is written as text,
 *  never shown as a number that could be mistaken for a measurement. */
export function formatMetricSample(m: MetricSample, digits = 0): string {
  switch (m.validity) {
    case "valid": return m.value.toFixed(digits);
    case "stale": return `${m.value.toFixed(digits)} (stale ${((m.age_us ?? 0) / 1e6).toFixed(1)} s)`;
    case "unavailable": return "n/a";
    case "unsupported": return "unsupported";
    case "error": return "error";
  }
}

/** Camera Status row (MON-098): Running in green, Stopped in grey. */
export function cameraStatusReadout(running: boolean): Readout {
  return running ? { text: "Running", cls: "ok" } : { text: "Stopped", cls: "dim" };
}

const MEASURED_HERE = "Measured from the frames this window received. The backend does not report the capture rate yet.";

/** Camera FPS and Data Rate rows. A backend capture sample is shown as text with its validity; until the bridge
 *  carries one, the rate measured from the frames this window received stands in (and says so). A stopped
 *  camera reads zero, as in Qt. */
export function cameraRateReadouts(i: {
  running: boolean; displayFps: number; dataRateMBs: number; captureFps?: MetricSample | null; captureMBs?: MetricSample | null;
}): { fps: Readout; dataRate: Readout } {
  if (!i.running) return { fps: { text: "0.0" }, dataRate: { text: "0.0 MB/s" } };
  const fps: Readout = i.captureFps ? { text: formatMetricSample(i.captureFps) } : { text: metricNumber(i.displayFps, 1), title: MEASURED_HERE };
  const rate: Readout = i.captureMBs ? { text: `${formatMetricSample(i.captureMBs, 1)} MB/s` } : { text: `${metricNumber(i.dataRateMBs, 1)} MB/s`, title: MEASURED_HERE };
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
