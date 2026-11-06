// Wafer Grid on the Overview (CONNECT-054, epic #863): the pure part. The backend's DotGridService
// decodes the live frame against the dot-grid design registry; this module turns its pose document
// (`fetch_dot_grid_pose`) into the overlay Qt draws on the live canvas (SimpleImageCanvas): the
// detected dots, the image-centre cross and the pose text box, or the failure reason. Qt parity:
// OverviewTab::updateDotGridOverlay (text), SimpleImageCanvas::paintEvent (marks).
import type { DotGridPose } from "../../bridge";

/** The Overview polls the pose at about 4 Hz (the decoder runs every 250 ms). */
export const WAFER_GRID_POLL_MS = 250;

/** What the overlay shows: the text box always, the marks only for a valid pose. */
export interface WaferGridView {
  valid: boolean;
  text: string;
  /** Detected dots in image pixels (valid pose only). */
  dots: [number, number][];
  /** The image the pose was decoded from; the marks are drawn only on a frame of this size. */
  imageWidth: number;
  imageHeight: number;
}

export interface WaferGridState {
  /** The service decodes (config `enabled`, from the backend; the button follows it). */
  enabled: boolean;
  /** The service thread runs (false with MIB_DISABLED_SERVICES=dot_grid). */
  available: boolean;
  designs: number;
  /** Sequence of the pose `view` was copied from; 0 before the first. */
  sequence: number;
  view: WaferGridView | null;
}

export const INITIAL_WAFER_GRID: WaferGridState = { enabled: false, available: true, designs: 0, sequence: 0, view: null };

const fixed = (n: number | undefined, digits: number) => (n ?? 0).toFixed(digits);

/** The text Qt shows for a pose (OverviewTab::updateDotGridOverlay), or the failure reason. */
export function waferGridText(pose: DotGridPose): string {
  if (!pose.has_pose) return "Wafer grid: waiting for a frame";
  if (!pose.valid) return `Wafer grid: ${pose.reason || "no pose"} (${pose.dots ?? 0} dots)`;
  // "<design>   chip R3C2" tells the operator which chip design is under the objective.
  let chip = "";
  const design = pose.design_name || pose.design_id;
  if (design) chip = `${design}   `;
  if (pose.chip) chip += `chip ${pose.chip}   `;
  return (
    `Wafer X ${fixed(pose.centre_x_um, 1)} µm   Y ${fixed(pose.centre_y_um, 1)} µm\n` +
    `θ ${fixed(pose.theta_deg, 2)}°   ${fixed(pose.um_per_px, 4)} µm/px   ${pose.mirrored ? "mirrored" : "direct"}\n` +
    `${chip}${pose.dots ?? 0} dots   votes ${pose.votes ?? 0}   ${fixed(pose.decode_ms, 1)} ms`
  );
}

/** The overlay for a pose document that carries a pose. */
export function waferGridView(pose: DotGridPose): WaferGridView {
  const valid = pose.has_pose && pose.valid === true;
  return {
    valid,
    text: waferGridText(pose),
    dots: valid ? (pose.dots_px ?? []).filter((d) => d.length === 2) : [],
    imageWidth: pose.image_width ?? 0,
    imageHeight: pose.image_height ?? 0,
  };
}

/**
 * The next state after a poll. The dot list is copied only when the sequence moved (an `unchanged`
 * reply keeps the view); a disabled service resets the overlay (Qt resets it when `enabled` flips).
 */
export function applyDotGridPose(prev: WaferGridState, pose: DotGridPose): WaferGridState {
  const head = { enabled: pose.enabled, available: pose.available, designs: pose.designs };
  if (!pose.enabled) return { ...head, sequence: 0, view: null };
  // Nothing moved since the overlay was copied: keep it (the poll sent no dot list).
  if (pose.unchanged && prev.enabled && prev.view) return { ...prev, ...head };
  return { ...head, sequence: pose.sequence, view: waferGridView(pose) };
}

/** `since_sequence` for the next poll: the sequence the overlay was copied from. */
export function nextSinceSequence(state: WaferGridState): number {
  return state.enabled && state.view ? state.sequence : 0;
}

/**
 * Decode only while the Overview is on screen (Qt: OverviewTab showEvent / hideEvent): the
 * Overview stage is shown, the window is visible (not hidden or minimised) and no experiment runs
 * (Qt blocks the Overview during one), so the service spends nothing next to an experiment.
 */
export function waferGridActive(o: { ready: boolean; supported: boolean; tab: string; windowVisible: boolean; experimentActive: boolean }): boolean {
  return o.ready && o.supported && o.tab === "overview" && o.windowVisible && !o.experimentActive;
}

/** The text of a refused toggle (Qt: the Wafer Grid warning dialog). */
export function waferGridUnavailableText(reason: string): string {
  return `Dot-grid localization is not available: ${reason}`;
}

/** The button label (Qt: "Wafer Grid: On" / "Wafer Grid: Off"). */
export function waferGridButtonLabel(enabled: boolean): string {
  return enabled ? "Wafer Grid: On" : "Wafer Grid: Off";
}

// ---- Overlay geometry (SimpleImageCanvas::paintEvent) ----
// Qt sizes the marks in display pixels: dot radius max(3, 6 x scale), a +-14 px cross, the text box
// 8 px inside the image corner. The canvas here holds the image at its own resolution and the page
// scales it (fit window), so the marks are sized in display pixels and divided by the display scale.

export interface Segment { x1: number; y1: number; x2: number; y2: number }

export interface WaferGridGeometry {
  /** Dots as circles in image pixels. */
  dots: { x: number; y: number; r: number }[];
  dotLineWidth: number;
  cross: Segment[];
  crossLineWidth: number;
  /** Text box, image pixels (top-left corner, size is measured when drawn). */
  textX: number;
  textY: number;
  fontPx: number;
  paddingX: number;
  paddingY: number;
}

/** Display pixels per image pixel of the canvas (1 when it is not laid out). */
export function displayScale(canvas: { width: number; clientWidth: number }): number {
  return canvas.width > 0 && canvas.clientWidth > 0 ? canvas.clientWidth / canvas.width : 1;
}

/** True when the pose was decoded from a frame of the size now on the canvas. */
export function sameImageSize(view: WaferGridView, frameWidth: number, frameHeight: number): boolean {
  return view.imageWidth === frameWidth && view.imageHeight === frameHeight;
}

/**
 * Where the overlay draws on a frame of `frameWidth` x `frameHeight` shown at `scale` display pixels
 * per image pixel. The dots and the cross only for a valid pose decoded from a frame of this size
 * (a window change between decode and draw would put them in the wrong place); the text always.
 */
export function waferGridGeometry(view: WaferGridView, frameWidth: number, frameHeight: number, scale: number): WaferGridGeometry {
  const s = scale > 0 ? scale : 1;
  const marks = view.valid && sameImageSize(view, frameWidth, frameHeight);
  const cx = view.imageWidth / 2, cy = view.imageHeight / 2;
  const arm = 14 / s;
  return {
    dots: marks ? view.dots.map(([x, y]) => ({ x, y, r: Math.max(3, 6 * s) / s })) : [],
    dotLineWidth: 1 / s,
    cross: marks
      ? [{ x1: cx - arm, y1: cy, x2: cx + arm, y2: cy }, { x1: cx, y1: cy - arm, x2: cx, y2: cy + arm }]
      : [],
    crossLineWidth: 2 / s,
    textX: 8 / s,
    textY: 8 / s,
    fontPx: 13 / s,
    paddingX: 7 / s,
    paddingY: 5 / s,
  };
}

export const WAFER_GRID_COLORS = {
  dot: "rgb(255,160,0)",
  cross: "rgb(0,220,255)",
  box: "rgba(0,0,0,0.59)",
  textValid: "rgb(255,220,120)",
  textFailed: "rgb(255,130,130)",
} as const;

/** Draws the overlay on a canvas that already holds the frame (called last by the frame drawing). */
export function drawWaferGrid(ctx: CanvasRenderingContext2D, view: WaferGridView, scale: number): void {
  const canvas = ctx.canvas;
  const g = waferGridGeometry(view, canvas.width, canvas.height, scale);
  ctx.save();
  ctx.lineWidth = g.dotLineWidth;
  ctx.strokeStyle = WAFER_GRID_COLORS.dot;
  for (const d of g.dots) {
    ctx.beginPath();
    ctx.arc(d.x, d.y, d.r, 0, Math.PI * 2);
    ctx.stroke();
  }
  ctx.lineWidth = g.crossLineWidth;
  ctx.strokeStyle = WAFER_GRID_COLORS.cross;
  for (const c of g.cross) {
    ctx.beginPath();
    ctx.moveTo(c.x1, c.y1);
    ctx.lineTo(c.x2, c.y2);
    ctx.stroke();
  }
  // The text box last, on top of everything: the pose, or the failure reason in words (not colour alone).
  ctx.font = `${g.fontPx}px sans-serif`;
  ctx.textBaseline = "top";
  const lines = view.text.split("\n");
  const lineHeight = g.fontPx * 1.25;
  const width = Math.max(0, ...lines.map((l) => ctx.measureText(l).width));
  ctx.fillStyle = WAFER_GRID_COLORS.box;
  ctx.fillRect(g.textX, g.textY, width + 2 * g.paddingX, lines.length * lineHeight + 2 * g.paddingY);
  ctx.fillStyle = view.valid ? WAFER_GRID_COLORS.textValid : WAFER_GRID_COLORS.textFailed;
  lines.forEach((line, i) => ctx.fillText(line, g.textX + g.paddingX, g.textY + g.paddingY + i * lineHeight));
  ctx.restore();
}
