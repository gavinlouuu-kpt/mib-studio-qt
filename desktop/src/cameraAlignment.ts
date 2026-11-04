// Camera & Alignment (YOFO Studio; Qt Overview-tab parity): the whole sensor is shown and the
// experiment window (ROI 1, sensor coordinates) is placed on it. Pure helpers, unit-tested.
import type {CameraGeometry} from "./bridge";

export interface Rect {x: number; y: number; width: number; height: number}

const snap = (value: number, step: number) => Math.round(value / Math.max(1, step)) * Math.max(1, step);

/** The window the Qt Overview starts from when an eGrabber script says nothing
 *  (EgrabberConfigParser::defaultRoiPosition / defaultRoiSize). */
export const EGRABBER_DEFAULT_WINDOW: Rect = {x: 704, y: 500, width: 512, height: 96};

const floorTo = (value: number, step: number) => (value <= 0 ? 0 : Math.floor(value / Math.max(1, step)) * Math.max(1, step));

/** An eGrabber camera window as the Qt Overview keeps it (OverviewTab::onRoiSizeChanged, SimpleImageCanvas
 *  drag, EgrabberConfigParser::snapToStep): the dragged offset is rounded to a pixel and then snapped DOWN
 *  to its step (X 16, Y 4); sizes snap down to width 16 / height 4 and stay within min..sensor; an offset
 *  that would push the window off the sensor is pulled back to the last step that fits. The backend does
 *  the same when it saves (CameraWindowProfile.cpp snapEgrabberWindow). */
export function snapEgrabberWindow(rect: Rect, g: CameraGeometry): Rect {
  const sw = g.sensor_width > 0 ? g.sensor_width : 1920, sh = g.sensor_height > 0 ? g.sensor_height : 1080;
  const width = Math.min(Math.max(floorTo(Math.round(rect.width), g.width_increment), g.min_width), Math.max(g.min_width, floorTo(sw, g.width_increment)));
  const height = Math.min(Math.max(floorTo(Math.round(rect.height), g.height_increment), g.min_height), Math.max(g.min_height, floorTo(sh, g.height_increment)));
  let x = floorTo(Math.round(rect.x), g.offset_x_increment), y = floorTo(Math.round(rect.y), g.offset_y_increment);
  if (x + width > sw) x = floorTo(sw - width, g.offset_x_increment);
  if (y + height > sh) y = floorTo(sh - height, g.offset_y_increment);
  return {x, y, width, height};
}

/** Snap a window to the camera's steps and keep it on the sensor (size first, then offset). An eGrabber
 *  camera snaps the way the Qt app does (down, not to the nearest step). */
export function snapWindow(rect: Rect, g: CameraGeometry): Rect {
  if (g.camera === "egrabber") return snapEgrabberWindow(rect, g);
  const sw = g.sensor_width, sh = g.sensor_height;
  const minW = Math.max(g.min_width, g.width_increment), minH = Math.max(g.min_height, g.height_increment);
  const fitStep = (limit: number, step: number) => Math.floor(limit / Math.max(1, step)) * Math.max(1, step);
  const width = Math.min(Math.max(snap(rect.width, g.width_increment), minW), sw > 0 ? fitStep(sw, g.width_increment) : Infinity);
  const height = Math.min(Math.max(snap(rect.height, g.height_increment), minH), sh > 0 ? fitStep(sh, g.height_increment) : Infinity);
  const maxX = sw > 0 ? fitStep(sw - width, g.offset_x_increment) : Infinity;
  const maxY = sh > 0 ? fitStep(sh - height, g.offset_y_increment) : Infinity;
  const x = Math.min(Math.max(snap(rect.x, g.offset_x_increment), 0), maxX);
  const y = Math.min(Math.max(snap(rect.y, g.offset_y_increment), 0), maxY);
  return {x, y, width, height};
}

/** The window shown when nothing is saved yet: the current experiment read-back or the
 *  centre of the sensor at the YOFO processing size (512 x 96). */
export function initialWindow(g: CameraGeometry): Rect {
  if (g.roi.width > 0 && g.roi.height > 0) return {...g.roi};
  if (g.camera === "egrabber") return snapEgrabberWindow(EGRABBER_DEFAULT_WINDOW, g);
  const region = g.session.region;
  if (region && !g.session.overview && region.width > 0) return {...region};
  return snapWindow({x: (g.sensor_width - 512) / 2, y: (g.sensor_height - 96) / 2, width: 512, height: 96}, g);
}

const LIMITS: Record<string, string> = {
  SensorGeometry: "window height",
  Profile: "processing profile",
  StoreBandwidth: "store bandwidth",
  BandReadout: "band readout to the PS",
  SensorRate: "sensor rate",
  PreviewRate: "preview rate (the PL sees every frame)",
};

/** One line for the operator: what the sensor runs at and what reaches this screen. */
export function rateSummary(g: CameraGeometry): string {
  const s = g.session;
  if (s.frame_rate_hz === undefined) return "";
  const parts = [`Sensor ${s.frame_rate_hz.toFixed(0)} Hz`];
  if (s.frame_rate_max_hz) parts[0] += ` (max ${s.frame_rate_max_hz.toFixed(0)} Hz: ${LIMITS[s.frame_rate_limit ?? ""] ?? "device"})`;
  if (s.frame_rate_clamped) parts.push("requested rate was clamped");
  if (s.delivered_frame_rate_hz) {
    const bands = s.band_count && s.band_count > 1 ? `, ${s.band_count} bands per image` : "";
    parts.push(`≥ ${s.delivered_frame_rate_hz.toFixed(s.delivered_frame_rate_hz < 100 ? 1 : 0)} images/s here` +
      ` (limit: ${LIMITS[s.delivered_limit ?? ""] ?? "device"}${bands})`);
  }
  return parts.join(" → ");
}

/** PZ7035 Run window (#501 P1): ROI 2 is the fixed 512x96 U-Net window; the producer takes x in
 *  steps of 8 and the sensor lands y on a multiple of 4 (pz7035-imx426 YOFO_HOST_INTERFACE.md). */
export const RUN_WINDOW = {width: 512, height: 96, xStep: 8, yStep: 4, sensorWidth: 816, sensorHeight: 624};

export function snapRunWindow(rect: Pick<Rect, "x" | "y">): Rect {
  const {width, height, xStep, yStep, sensorWidth, sensorHeight} = RUN_WINDOW;
  const maxX = Math.floor((sensorWidth - width) / xStep) * xStep, maxY = Math.floor((sensorHeight - height) / yStep) * yStep;
  const x = Math.min(Math.max(Math.round(rect.x / xStep) * xStep, 0), maxX);
  const y = Math.min(Math.max(Math.round(rect.y / yStep) * yStep, 0), maxY);
  return {x, y, width, height};
}

/** The limits of a window number box (Qt's spin boxes for an eGrabber camera: W 64..1920 in steps of 16, H 16..1080
 *  in steps of 4; the offsets in their own steps and on the sensor). Undefined for the other cameras, whose boxes
 *  keep the browser's defaults. */
export function windowInputLimits(g: CameraGeometry, key: keyof Rect, window: Rect | null): {min: number; max: number; step: number} | undefined {
  if (g.camera !== "egrabber") return undefined;
  const sw = g.sensor_width > 0 ? g.sensor_width : 1920, sh = g.sensor_height > 0 ? g.sensor_height : 1080;
  switch (key) {
    case "width": return {min: g.min_width, max: Math.max(g.min_width, floorTo(sw, g.width_increment)), step: g.width_increment};
    case "height": return {min: g.min_height, max: Math.max(g.min_height, floorTo(sh, g.height_increment)), step: g.height_increment};
    case "x": return {min: 0, max: floorTo(Math.max(0, sw - (window?.width ?? g.min_width)), g.offset_x_increment), step: g.offset_x_increment};
    default: return {min: 0, max: floorTo(Math.max(0, sh - (window?.height ?? g.min_height)), g.offset_y_increment), step: g.offset_y_increment};
  }
}

const rectText = (r: Rect) => `${r.width}×${r.height} at (${r.x}, ${r.y})`;

/** The eGrabber window line under the Overview toolbar: the window the box shows and where it comes from
 *  (the applied profile's camera.window wins over the Experiment script, #833). */
export function egrabberWindowLine(g: CameraGeometry): string {
  const has = g.roi.width > 0 && g.roi.height > 0;
  const script = (g.experiment_script_path ?? "").split(/[\\/]/).pop() ?? "";
  switch (g.window_source) {
    case "profile":
      return has ? `Window ${rectText(g.roi)}: the applied profile's camera.window, applied after the Experiment script.` : "";
    case "script":
      return has ? `Window ${rectText(g.roi)}: from the Experiment script${script ? ` ${script}` : ""}.` : "";
    default:
      return "No camera window is saved yet: the default is shown. Drag the yellow box or edit W/H to set it.";
  }
}
