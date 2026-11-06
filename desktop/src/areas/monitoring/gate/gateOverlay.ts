// The draft target-group gate drawn over the Monitoring scatter (epic #866, PR E1: MON-064, MON-068). The Qt
// original is GateOverlayItem in src/frontend/tabs/ExperimentMonitoringTab.cpp: a tinted "would be target"
// region, the dashed area/deformability box and, with the E gate on and a table loaded, the dash-dotted iso-E
// contours with value labels, and a pill with the gate's name ("Target group" / "Target group (unapplied)").
// With the E gate on and no table a red pill says so; the box is tinted and there are no contours and no E
// handles. Lines carry dash patterns and text, never colour alone.
//
// Pure canvas 2D, no React: `GateOverlayModel.draw` is what the scatter's `overlay.draw` calls with the live
// axis transform. The model keeps the two things worth caching between redraws: the iso-E contours (by level,
// at most 8, cleared with the table: a drag walks through many levels) and the sampled mask of the tint (by
// gate, table and viewport, so a redraw that changes nothing about them does not sample the table again).
import type { Viewport } from "../../../review/charts/chartMath";
import type { PreparedLut } from "../emodulusLut";
import {
  computeMask, contourLabelAnchor, eLayerFilters, hitTest, isoContour, NO_CONTOURS, NO_HIT, sameGate, toPixel, viewportValid,
  type Contour, type EContours, type GateSpec, type HandleHit, type HandleKind, type Mask,
} from "./targetGate";

/** What the model draws from. `spec` is the DRAFT gate (an unapplied edit previews live). */
export interface GateView {
  spec: GateSpec;
  /** Any target-group field differs from the applied baseline. */
  unapplied: boolean;
  lut: PreparedLut | null;
  /** The table state behind `lut`: the pill for an enabled E gate without one. */
  table: "loaded" | "none" | "unknown" | "error";
  tableError: string;
  /** The handle under the pointer, and the one being dragged. */
  hover: HandleKind;
  dragging: HandleKind;
  /** While dragging: "Target group area maximum (µm²): 330 µm²". */
  dragText: string;
}

export const GATE_LABEL = "Target group";
export const GATE_LABEL_UNAPPLIED = "Target group (unapplied)";
export const NO_TABLE_NOTICE = "No E-modulus table loaded: the target group uses the box only";
export const tableErrorNotice = (why: string) => `The E-modulus table could not be read (${why}): the preview shows the box only`;

/** Sampled-mask cell size in pixels (Qt kMaskStepPx). */
export const MASK_STEP_PX = 3;
/** Contours kept by level (Qt: at most 8, cleared with the table). */
export const CONTOUR_CACHE = 8;

// Qt's colours (GateOverlayItem): tint QColor(0,120,255,44), edge (0,70,180), E (150,40,170), active (220,110,0), warn (170,30,30).
export const GATE_TINT = "rgba(0, 120, 255, 0.1725)";
export const GATE_EDGE = "rgb(0, 70, 180)";
export const GATE_EMOD = "rgb(150, 40, 170)";
export const GATE_ACTIVE = "rgb(220, 110, 0)";
export const GATE_WARN = "rgb(170, 30, 30)";
const PILL_FILL = "rgba(255, 255, 255, 0.88)";
const FONT = "11px system-ui, sans-serif";
const LINE_HEIGHT = 14;

/** The notice pill of a view: an enabled E gate without a table to apply it. */
export function gateNotice(view: Pick<GateView, "spec" | "table" | "tableError">): string {
  if (!view.spec.enabled || !view.spec.emodEnabled) return "";
  if (view.table === "none") return NO_TABLE_NOTICE;
  if (view.table === "error") return tableErrorNotice(view.tableError || "unknown error");
  return "";
}

export const gateLabel = (unapplied: boolean): string => (unapplied ? GATE_LABEL_UNAPPLIED : GATE_LABEL);

interface Rect { x: number; y: number; w: number; h: number }
const intersects = (a: Rect, b: Rect) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;

/** The tinted cells of a sampled mask as horizontal runs [x, y, w, h] in pixels, so they replay as one path. */
function maskRuns(m: Mask, vp: Viewport): Float32Array {
  const out: number[] = [];
  for (let r = 0; r < m.rows; r++) {
    let start = -1;
    for (let c = 0; c <= m.cols; c++) {
      const on = c < m.cols && m.on[r * m.cols + c] !== 0;
      if (on && start < 0) start = c;
      else if (!on && start >= 0) {
        out.push(vp.left + start * m.stepPx, vp.top + r * m.stepPx, (c - start) * m.stepPx, m.stepPx);
        start = -1;
      }
    }
  }
  return Float32Array.from(out);
}

interface MaskKey {
  spec: GateSpec;
  lut: PreparedLut | null;
  vp: Viewport;
}
const sameViewport = (a: Viewport, b: Viewport) =>
  a.x0 === b.x0 && a.x1 === b.x1 && a.y0 === b.y0 && a.y1 === b.y1 && a.left === b.left && a.top === b.top && a.width === b.width && a.height === b.height;

export class GateOverlayModel {
  private lut: PreparedLut | null = null;
  private readonly contourCache = new Map<number, Contour>();
  private maskKey: MaskKey | null = null;
  private maskCache: { runs: Float32Array; on: number } | null = null;
  /** How often the table was sampled for a tint, for the tests that check a redraw does not resample. */
  maskComputations = 0;
  /** How many contours were computed (cache misses). */
  contourComputations = 0;

  /** A different table drops everything derived from the old one. */
  private adopt(lut: PreparedLut | null): void {
    if (lut === this.lut) return;
    this.lut = lut;
    this.contourCache.clear();
    this.maskKey = null;
    this.maskCache = null;
  }

  /** The iso-E contour of a level (cached; cleared when the table changes). Empty for no table. */
  contourFor(lut: PreparedLut | null, level: number): Contour | null {
    if (!lut) return null;
    this.adopt(lut);
    const hit = this.contourCache.get(level);
    if (hit) return hit;
    if (this.contourCache.size >= CONTOUR_CACHE) this.contourCache.clear(); // a drag walks through many levels
    const contour = isoContour(lut, level);
    this.contourComputations++;
    this.contourCache.set(level, contour);
    return contour;
  }

  /** The contours of the draft's two E levels: only while the gate and its E range are on and a table is loaded. */
  contoursOf(spec: GateSpec, lut: PreparedLut | null): EContours {
    if (!spec.enabled || !spec.emodEnabled || !lut) return NO_CONTOURS;
    return { min: this.contourFor(lut, spec.emodMin), max: this.contourFor(lut, spec.emodMax) };
  }

  /** The handle under the pointer, or none (also none while the gate is off). */
  hit(view: Pick<GateView, "spec" | "lut">, vp: Viewport, px: number, py: number): HandleHit {
    if (!view.spec.enabled) return NO_HIT;
    return hitTest(view.spec, this.contoursOf(view.spec, view.lut), vp, px, py);
  }

  /** The sampled mask of the tint as runs, cached by gate / table / viewport. */
  private tint(spec: GateSpec, lut: PreparedLut | null, vp: Viewport) {
    this.adopt(lut);
    const k = this.maskKey;
    if (!k || !this.maskCache || !sameGate(k.spec, spec) || k.lut !== lut || !sameViewport(k.vp, vp)) {
      const m = computeMask(spec, lut, vp, MASK_STEP_PX);
      let on = 0;
      for (let i = 0; i < m.on.length; i++) on += m.on[i];
      this.maskCache = { runs: maskRuns(m, vp), on };
      this.maskKey = { spec: { ...spec }, lut, vp: { ...vp } };
      this.maskComputations++;
    }
    return this.maskCache;
  }

  draw(g: CanvasRenderingContext2D, vp: Viewport, view: GateView): void {
    const { spec, lut } = view;
    if (!spec.enabled || !viewportValid(vp)) return;
    const plot: Rect = { x: vp.left, y: vp.top, w: vp.width, h: vp.height };
    const [bx0, by0] = toPixel(vp, spec.areaMin, spec.deformMax);
    const [bx1, by1] = toPixel(vp, spec.areaMax, spec.deformMin);
    const box: Rect = { x: Math.min(bx0, bx1), y: Math.min(by0, by1), w: Math.abs(bx1 - bx0), h: Math.abs(by1 - by0) };
    const visible: Rect = {
      x: Math.max(box.x, plot.x), y: Math.max(box.y, plot.y),
      w: Math.min(box.x + box.w, plot.x + plot.w) - Math.max(box.x, plot.x),
      h: Math.min(box.y + box.h, plot.y + plot.h) - Math.max(box.y, plot.y),
    };
    const hasVisibleBox = visible.w > 0 && visible.h > 0;
    const tableLoaded = lut !== null;

    g.save();
    g.beginPath();
    g.rect(plot.x, plot.y, plot.w, plot.h);
    g.clip();
    g.font = FONT;
    g.textAlign = "left";
    g.textBaseline = "alphabetic";

    // 1. The tint: the exact rectangle when the E layer does not filter (no E gate, or no table: the group is the
    //    box alone), otherwise the sampled predicate (box ^ E range) clipped to the exact box.
    if (hasVisibleBox) {
      g.fillStyle = GATE_TINT;
      if (!eLayerFilters(spec, tableLoaded)) {
        g.fillRect(visible.x, visible.y, visible.w, visible.h);
      } else {
        const tint = this.tint(spec, lut, vp);
        if (tint.on > 0) {
          g.save();
          g.beginPath();
          g.rect(visible.x, visible.y, visible.w, visible.h);
          g.clip();
          g.beginPath();
          for (let i = 0; i < tint.runs.length; i += 4) g.rect(tint.runs[i], tint.runs[i + 1], tint.runs[i + 2], tint.runs[i + 3]);
          g.fill();
          g.restore();
        }
      }
    }

    // 2. The dashed box. A hovered or dragged edge is drawn solid and heavier.
    const edge = (kind: HandleKind, x0: number, y0: number, x1: number, y1: number) => {
      const active = view.hover === kind || view.dragging === kind;
      g.strokeStyle = active ? GATE_ACTIVE : GATE_EDGE;
      g.lineWidth = active ? 3.5 : 2;
      g.setLineDash(active ? [] : [8, 4]);
      g.beginPath();
      g.moveTo(x0, y0);
      g.lineTo(x1, y1);
      g.stroke();
    };
    edge("areaMin", box.x, box.y, box.x, box.y + box.h);
    edge("areaMax", box.x + box.w, box.y, box.x + box.w, box.y + box.h);
    edge("deformMax", box.x, box.y, box.x + box.w, box.y);
    edge("deformMin", box.x, box.y + box.h, box.x + box.w, box.y + box.h);

    // 3. The gate's name (and unapplied state) and the E-gate notice sit in the plot's top-left corner; their
    //    rectangles are reserved first so the contour labels can move out of their way.
    const measure = (s: string) => g.measureText(s)?.width ?? s.length * 6;
    const maxPill = Math.max(60, plot.w - 12);
    const reserved: Rect[] = [];
    const placed: { text: string; rect: Rect; lines: string[]; color: string }[] = [];
    let cursorY = plot.y + 6;
    const stack = (text: string, color: string) => {
      if (!text) return;
      const lines = wrap(measure, text, maxPill - 10);
      const rect = pillRect(measure, lines, plot.x + 6, cursorY);
      reserved.push(rect);
      cursorY = rect.y + rect.h + 4;
      placed.push({ text, rect, lines, color });
    };
    stack(gateLabel(view.unapplied), GATE_EDGE);
    stack(gateNotice(view), GATE_WARN);
    stack(view.dragging !== "none" ? view.dragText : "", GATE_ACTIVE);

    // 4. The iso-E contours and their value labels.
    const contours = this.drawnContours(view, vp);
    for (const c of contours) {
      const active = view.hover === c.kind || view.dragging === c.kind;
      g.strokeStyle = active ? GATE_ACTIVE : GATE_EMOD;
      g.lineWidth = active ? 3.5 : 2;
      g.setLineDash(active ? [] : [8, 4, 2, 4]);
      for (const line of c.contour) {
        g.beginPath();
        line.forEach(([x, y], i) => {
          const [px, py] = toPixel(vp, x, y);
          if (i === 0) g.moveTo(px, py);
          else g.lineTo(px, py);
        });
        g.stroke();
      }
    }
    g.setLineDash([]);
    for (const c of contours) {
      const active = view.hover === c.kind || view.dragging === c.kind;
      const lines = wrap(measure, c.text, maxPill - 10);
      const probe = pillRect(measure, lines, 0, 0);
      const r: Rect = { x: c.at[0] - probe.w - 4, y: c.at[1] - probe.h - 2, w: probe.w, h: probe.h };
      for (let tries = 0; tries < 6 && reserved.some((o) => intersects(o, r)); tries++) r.y += probe.h + 2;
      r.x = Math.min(Math.max(r.x, plot.x + 2), Math.max(plot.x + 2, plot.x + plot.w - r.w - 2));
      r.y = Math.min(Math.max(r.y, plot.y + 2), Math.max(plot.y + 2, plot.y + plot.h - r.h - 2));
      reserved.push(r);
      drawPill(g, r, lines, active ? GATE_ACTIVE : GATE_EMOD);
    }
    for (const p of placed) drawPill(g, p.rect, p.lines, p.color);
    g.restore();
  }

  private drawnContours(view: GateView, vp: Viewport) {
    const out: { kind: HandleKind; contour: Contour; text: string; at: readonly [number, number] }[] = [];
    const { spec, lut } = view;
    if (!spec.enabled || !spec.emodEnabled || !lut) return out;
    const add = (kind: HandleKind, contour: Contour | null, text: string) => {
      if (!contour || contour.length === 0) return;
      const at = contourLabelAnchor(contour, vp);
      if (at) out.push({ kind, contour, text, at });
    };
    add("emodMin", this.contourFor(lut, spec.emodMin), `E ≥ ${spec.emodMin.toFixed(2)} kPa`);
    add("emodMax", this.contourFor(lut, spec.emodMax), `E ≤ ${spec.emodMax.toFixed(2)} kPa`);
    return out;
  }

  /** What a draw would put on the plot, for tests and the text alternative: the pills' texts and the contour labels. */
  describe(view: GateView, vp: Viewport) {
    return {
      label: view.spec.enabled ? gateLabel(view.unapplied) : "",
      notice: gateNotice(view),
      contourLabels: this.drawnContours(view, vp).map((c) => c.text),
      contourLines: this.drawnContours(view, vp).reduce((n, c) => n + c.contour.length, 0),
    };
  }
}

/** Word-wrap `text` to `width` pixels of the current font. */
function wrap(measure: (s: string) => number, text: string, width: number): string[] {
  const lines: string[] = [];
  let line = "";
  for (const word of text.split(" ")) {
    const next = line ? `${line} ${word}` : word;
    if (line && measure(next) > width) {
      lines.push(line);
      line = word;
    } else line = next;
  }
  if (line) lines.push(line);
  return lines;
}

function pillRect(measure: (s: string) => number, lines: string[], x: number, y: number): Rect {
  const w = Math.max(0, ...lines.map(measure)) + 10;
  return { x, y, w, h: lines.length * LINE_HEIGHT + 4 };
}

function drawPill(g: CanvasRenderingContext2D, r: Rect, lines: string[], color: string): void {
  const rad = 3;
  g.save();
  g.setLineDash([]);
  g.lineWidth = 1;
  g.strokeStyle = color;
  g.fillStyle = PILL_FILL;
  g.beginPath();
  g.moveTo(r.x + rad, r.y);
  g.arcTo(r.x + r.w, r.y, r.x + r.w, r.y + r.h, rad);
  g.arcTo(r.x + r.w, r.y + r.h, r.x, r.y + r.h, rad);
  g.arcTo(r.x, r.y + r.h, r.x, r.y, rad);
  g.arcTo(r.x, r.y, r.x + r.w, r.y, rad);
  g.closePath();
  g.fill();
  g.stroke();
  g.fillStyle = color;
  g.textAlign = "left";
  g.textBaseline = "alphabetic";
  lines.forEach((line, i) => g.fillText(line, r.x + 5, r.y + 2 + (i + 1) * LINE_HEIGHT - 3));
  g.restore();
}
