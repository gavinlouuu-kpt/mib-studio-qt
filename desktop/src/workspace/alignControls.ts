// O2 Align (P3, track A step A3): which quick controls this instrument offers, and the camera
// mode the image comes from. Pure: everything is read from the snapshots the classic Camera &
// Alignment tab already uses (fetch_camera_geometry, fetch_instrument_status, fetch_run_fps, the
// processing config). Only what the classic tab can write is writable here, through the same
// command: the camera ROI (save_camera_roi), the PZ7035 Run window (placed here, applied by the
// switch to Run) or the processing ROI (set_processing_roi). Exposure and frame rate are
// readbacks; gain is not exposed through the bridge. Nothing here sends a command.
import type { CameraGeometry } from "../bridge";
import { initialWindow, RUN_WINDOW, snapRunWindow } from "../cameraAlignment";
import { exposureText, fpsText, rateSummary as runRateSummary, type RunFpsStatus } from "../runFps";
import type { Size } from "./alignImageModel";
import type { Access, Rect, RectBounds } from "./quickControlModel";

export interface AlignFacts {
  pz7035: boolean;
  /** The PZ7035's backend-owned Align / Run modes (capabilities align_mode and run_mode). */
  instrumentModes: boolean;
  hostProcessing: boolean;
  geometry: CameraGeometry | null;
  /** fetch_instrument_status mode name ("align", "run", "unknown"); null when not reported. */
  instrumentMode: string | null;
  /** A stopped Run holds its frames for playback; a mode switch would discard them. */
  runFrozen: boolean;
  /** Why the last switch to Align (or the full sensor) was refused; "" when it was not. */
  modeError: string;
  /** PZ7035: the Run window the backend applied at the last Run entry (null before one). */
  appliedRunWindow: Rect | null;
  /** PZ7035: the Run window placed here or in the classic tab (null while only the default is shown). */
  placedRunWindow: Rect | null;
  /** The processing ROI in the applied processing config (desktop without camera geometry). */
  processingRoi: Rect | null;
  runFps: RunFpsStatus | null;
  experimentActive: boolean;
  running: boolean;
}

export type RoiKind = "camera" | "run" | "processing";

export interface ReadbackRow {
  id: "exposure" | "gain" | "frameRate" | "runTiming";
  label: string;
  access: Extract<Access, "ro" | "unavailable">;
  /** The device's value as text, or null when it reports none. */
  value: string | null;
  /** Secondary facts about the value ("max 4,512 Hz · 30.1 images/s here"); "" for none. */
  detail: string;
  /** The chip's short detail ("camera script", "not bridged"). */
  chip: string;
  /** Where it is set, or why it is unavailable, in full (the chip's title). */
  why: string;
}

export interface RoiRow {
  id: "roi";
  label: string;
  kind: RoiKind | null;
  access: Extract<Access, "rw" | "unavailable">;
  readback: Rect | null;
  /** Shown in the draft before any readback exists (the default or the placed window). */
  initial: Rect | null;
  bounds: RectBounds | null;
  /** What the readback is, or why the control is unavailable. */
  why: string;
  /** For a write whose effect comes later (PZ7035: the switch to Run). */
  deferred: string;
}

export const CLASSIC_SETTINGS = {
  desktop: "set in the camera script or configuration (classic view)",
  pz7035: "set in Config → Camera document (classic view)",
} as const;

/** The camera ROI's bounds from the geometry: the sensor, the camera's minimum size and steps. */
export function cameraRoiBounds(g: CameraGeometry): RectBounds {
  return {
    areaWidth: g.sensor_width, areaHeight: g.sensor_height,
    minWidth: Math.max(g.min_width, g.width_increment, 1), minHeight: Math.max(g.min_height, g.height_increment, 1),
    widthIncrement: Math.max(1, g.width_increment), heightIncrement: Math.max(1, g.height_increment),
    xIncrement: Math.max(1, g.offset_x_increment), yIncrement: Math.max(1, g.offset_y_increment),
  };
}

/** The PZ7035 Run window: 512×96 on the 816×624 sensor, x in steps of 8, y in steps of 4. */
export const RUN_WINDOW_BOUNDS: RectBounds = {
  areaWidth: RUN_WINDOW.sensorWidth, areaHeight: RUN_WINDOW.sensorHeight,
  minWidth: RUN_WINDOW.width, minHeight: RUN_WINDOW.height, widthIncrement: 1, heightIncrement: 1,
  xIncrement: RUN_WINDOW.xStep, yIncrement: RUN_WINDOW.yStep, fixedSize: { width: RUN_WINDOW.width, height: RUN_WINDOW.height },
};

/** The processing ROI is in frame pixels; 0 × 0 means the whole frame. */
export function processingRoiBounds(frame: Size | null): RectBounds {
  const w = frame?.width ?? 65_535, h = frame?.height ?? 65_535;
  return { areaWidth: w, areaHeight: h, minWidth: 0, minHeight: 0, widthIncrement: 1, heightIncrement: 1, xIncrement: 1, yIncrement: 1 };
}

const roiOf = (r: { x: number; y: number; width: number; height: number } | undefined | null): Rect | null =>
  r && r.width > 0 && r.height > 0 ? { x: r.x, y: r.y, width: r.width, height: r.height } : null;

export function roiRow(f: AlignFacts, frame: Size | null): RoiRow {
  const g = f.geometry;
  if (f.instrumentModes) {
    const centre = snapRunWindow({ x: (RUN_WINDOW.sensorWidth - RUN_WINDOW.width) / 2, y: (RUN_WINDOW.sensorHeight - RUN_WINDOW.height) / 2 });
    return {
      id: "roi", label: `Run window (${RUN_WINDOW.width} × ${RUN_WINDOW.height})`, kind: "run", access: "rw",
      readback: f.appliedRunWindow, initial: f.placedRunWindow ?? centre, bounds: RUN_WINDOW_BOUNDS,
      why: f.appliedRunWindow ? "applied at the last Run entry" : "no Run entry yet",
      deferred: "applied when Experiment opens (the switch to Run)",
    };
  }
  if (g?.supported) {
    if (!(g.sensor_width > 0 && g.sensor_height > 0))
      return { id: "roi", label: "Camera ROI", kind: "camera", access: "unavailable", readback: null, initial: null, bounds: null,
        why: "the camera has not reported its sensor size yet", deferred: "" };
    return {
      id: "roi", label: "Camera ROI (sensor)", kind: "camera", access: "rw", readback: roiOf(g.roi), initial: initialWindow(g),
      bounds: cameraRoiBounds(g), why: "the saved window the experiment uses", deferred: "",
    };
  }
  if (f.hostProcessing) {
    return {
      id: "roi", label: "Processing ROI (frame)", kind: "processing", access: "rw", readback: f.processingRoi, initial: f.processingRoi,
      bounds: processingRoiBounds(frame), why: "from the applied processing config; 0 × 0 is the whole frame", deferred: "",
    };
  }
  return { id: "roi", label: "ROI", kind: null, access: "unavailable", readback: null, initial: null, bounds: null,
    why: "this camera has no window the workspace can set", deferred: "" };
}

/** An exposure readback without losing short exposures: "2.5 µs", "900 µs", "1.20 ms". */
export function exposureReadback(us: number): string {
  return us < 100 ? `${Number(us.toFixed(2))} µs` : exposureText(us);
}

export function readbackRows(f: AlignFacts): ReadbackRow[] {
  const s = f.geometry?.supported ? f.geometry.session : undefined;
  const where = f.pz7035 ? CLASSIC_SETTINGS.pz7035 : CLASSIC_SETTINGS.desktop;
  const setBy = f.pz7035 ? "Align producer" : "camera script";
  const setByWhy = f.pz7035 ? "set by the Align producer" : where;
  const notReported = "not reported by this camera; " + where;
  const rows: ReadbackRow[] = [];
  rows.push(s?.exposure_us !== undefined
    ? { id: "exposure", label: "Exposure", access: "ro", value: exposureReadback(s.exposure_us), detail: "", chip: setBy, why: setByWhy }
    : { id: "exposure", label: "Exposure", access: "unavailable", value: null, detail: "", chip: "not reported", why: notReported });
  rows.push({ id: "gain", label: "Gain", access: "unavailable", value: null, detail: "", chip: "not bridged",
    why: f.pz7035 ? "not exposed through the bridge" : `not exposed through the bridge; ${where}` });
  if (s?.frame_rate_hz !== undefined) {
    const details: string[] = [];
    if (s.frame_rate_max_hz) details.push(`max ${fpsText(s.frame_rate_max_hz)} Hz`);
    if (s.frame_rate_clamped) details.push("requested rate was clamped");
    if (s.delivered_frame_rate_hz) details.push(`≥ ${fpsText(s.delivered_frame_rate_hz, s.delivered_frame_rate_hz < 100 ? 1 : 0)} images/s here`);
    rows.push({ id: "frameRate", label: "Frame rate", access: "ro", value: `${fpsText(s.frame_rate_hz, s.frame_rate_hz < 100 ? 1 : 0)} Hz`,
      detail: details.join(" · "), chip: setBy, why: setByWhy });
  } else {
    rows.push({ id: "frameRate", label: "Frame rate", access: "unavailable", value: null, detail: "", chip: "not reported", why: notReported });
  }
  if (f.instrumentModes) {
    const r = f.runFps;
    if (r?.available && r.fps != null) {
      const timing = runRateSummary({ fps: r.fps, exposure_mode: r.exposure_mode ?? "auto", exposure_us: r.exposure_us ?? 0, shs: r.shs ?? 64,
        actual_fps: r.actual_fps ?? r.fps }).split(" · ");
      rows.push({ id: "runTiming", label: "Run rate and exposure", access: "ro", value: timing[0], detail: timing.slice(1).join(" · "),
        chip: "camera document", why: CLASSIC_SETTINGS.pz7035 });
    } else {
      rows.push({ id: "runTiming", label: "Run rate and exposure", access: "unavailable", value: null, detail: "", chip: "not read",
        why: r?.reason || "not read yet" });
    }
  }
  return rows;
}

export type ModeTone = "confirmed" | "warn" | "blocked" | "unknown" | "neutral";

/** The camera mode the live image comes from, and the explicit switch the classic tab does on entry. */
export interface AlignMode {
  label: string;
  tone: ModeTone;
  /** The image is not the full sensor: offer the switch (never done on navigation). */
  switchNeeded: boolean;
  action: string;
  /** What pressing it does. */
  detail: string;
  /** Why it cannot be pressed now; "" when it can. */
  blocked: string;
}

export function alignMode(f: AlignFacts): AlignMode {
  const locked = f.experimentActive ? "a run is active" : "";
  if (f.instrumentModes) {
    const name = f.instrumentMode;
    const detail = "Switches the PZ7035 to Align: the full sensor, live, with the LED on the Align preset. The classic Camera & Alignment tab does this on entry; here only this button does.";
    const blocked = locked || (f.runFrozen ? "Run is stopped to review the buffered frames: Resume Run in the classic Experiment tab first" : "");
    if (name === "align") return { label: "Align · full sensor", tone: "confirmed", switchNeeded: false, action: "", detail: "", blocked: "" };
    return {
      label: f.modeError ? `Align refused · ${f.modeError}` : name === "run" ? "Run · no live camera image" : name ? `Mode · ${name}` : "Mode · not read yet",
      tone: f.modeError ? "blocked" : name ? "neutral" : "unknown",
      switchNeeded: true, action: "Switch to Align", detail, blocked,
    };
  }
  const g = f.geometry;
  if (g?.supported) {
    if (g.overview) return { label: `Full sensor ${g.sensor_width || "?"} × ${g.sensor_height || "?"}`, tone: "confirmed", switchNeeded: false, action: "", detail: "", blocked: "" };
    return {
      label: f.modeError ? `Full sensor refused · ${f.modeError}` : "Experiment window", tone: f.modeError ? "blocked" : "neutral", switchNeeded: true,
      action: "Show the full sensor",
      detail: "Switches the camera to the whole sensor (a running camera restarts), as the classic Camera & Alignment tab does on entry; here only this button does.",
      blocked: locked,
    };
  }
  return { label: g ? "Camera window · as configured" : "Camera mode · not read yet", tone: g ? "neutral" : "unknown", switchNeeded: false, action: "", detail: "", blocked: "" };
}

/** The ROI to outline on the image: only when the frame is the area the ROI is placed on. */
export function roiOverlay(row: RoiRow, shown: Rect | null, frame: Size | null): Rect | null {
  if (!shown || !frame || !row.bounds || row.access !== "rw") return null;
  if (row.kind === "processing") return shown.width > 0 && shown.height > 0 ? shown : null;
  return frame.width === row.bounds.areaWidth && frame.height === row.bounds.areaHeight ? shown : null;
}

