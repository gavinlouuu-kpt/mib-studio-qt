// Connect and Overview (epic #863): the words of the Overview's camera-mode line and of the "ROI: W x H @ (x, y)"
// summary. Pure, so Qt's wording is unit-tested without a camera. Qt references: OverviewTab::onTick and
// initializeRoiFromConfig (the MindVision mode label), MainWindow's roiLabel_ (the ROI summary).
import type { CameraGeometry } from "../../bridge";
import type { Rect } from "../../cameraAlignment";

/** The trigger marker Qt puts before the MindVision label for an illuminated-live profile (the Overview runs the
 *  generator at its 400 Hz minimum, MindVision `overviewConfig`). */
export const TRIGGER_PREFIX = "Trigger: 400 Hz | ";

/** Qt's MindVision mode label.
 *  - The profile cannot be read: the reason (Qt: "Cannot read MindVision experiment profile: ..." or the parser's own).
 *  - Overview: "Full sensor overview | Capture: N fps | Display: up to D fps. Drag the ROI for the experiment."
 *    N is the camera's measured rate (CaptureStats::lastFrameRate: 0 while stopped), D the preview rate limit
 *    (Qt's Overview paints at a fixed 50 fps; here it is the configured limit).
 *  - Experiment window: the saved ROI.
 *  An illuminated-live profile gets "Trigger: 400 Hz | " in front. */
export function mindVisionModeLabel(g: CameraGeometry, displayFps: number): string {
  if (g.profile_error) return g.profile_error;
  const trigger = g.illuminated_live ? TRIGGER_PREFIX : "";
  if (g.overview) {
    const capture = Math.round(g.capture_fps ?? 0);
    return `${trigger}Full sensor overview | Capture: ${capture} fps | Display: up to ${Math.round(displayFps)} fps. Drag the ROI for the experiment.`;
  }
  return `${trigger}Experiment ROI: ${g.roi.width}x${g.roi.height}.`;
}

/** The bold lead of the mode line for the cameras that are not MindVision. */
export function cameraModeLead(g: CameraGeometry): string {
  if (g.overview) return `Full sensor ${g.sensor_width || "?"}×${g.sensor_height || "?"}`;
  // An eGrabber camera shows nothing of a stage until a script is applied: do not call that the Experiment window.
  if (g.camera === "egrabber" && !g.script_role) return "No camera script applied yet";
  return "Experiment window";
}

export interface RoiSummary { text: string; title?: string }

interface ProcessingRoi { x: string; y: string; w: string; h: string }

const processingText = (r: ProcessingRoi): string =>
  Number(r.w) > 0 ? `${r.w} × ${r.h} @ (${r.x}, ${r.y})` : "full frame";

/** The "ROI: W × H @ (x, y)" summary (Qt MainWindow roiLabel_, set from the Overview rectangle): with a camera window on
 *  the Overview it is that window, as Qt shows it; otherwise (a camera without a window, the PZ7035) the processing ROI
 *  as before. The processing ROI is a different setting (it applies to the frames after the camera crops them), so when
 *  the label shows the window the tooltip says what the processing ROI is. */
export function roiSummary(s: { geometry: CameraGeometry | null; window: Rect | null; instrumentModes: boolean; processing: ProcessingRoi }): RoiSummary {
  const processing = processingText(s.processing);
  if (!s.instrumentModes && s.geometry?.supported && s.window && s.window.width > 0 && s.window.height > 0) {
    const w = s.window;
    return { text: `ROI: ${w.width} × ${w.height} @ (${w.x}, ${w.y})`, title: `Camera window on the Overview. Processing ROI: ${processing}` };
  }
  return { text: `ROI: ${processing}` };
}
