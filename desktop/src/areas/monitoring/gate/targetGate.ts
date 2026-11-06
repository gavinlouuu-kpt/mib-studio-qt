// The target-group sorting gate, previewed and edited on the Monitoring scatter (epic #866, PR E1:
// MON-064..068). A pure TypeScript port of the Qt model include/frontend/models/TargetGatePreview.h +
// src/frontend/models/TargetGatePreview.cpp (cases of tests/frontend/target_gate_preview_test.cpp are in
// targetGate.test.ts), with the same names where a name carries over. No React, no canvas, no bridge.
//
//  - `isTarget` / `wouldBeTarget` are the exact predicate of ProcessingScience.cpp `isTargetGroupMember`
//    for a valid cell: the area and deformability lie in the box (inclusive) and the E layer passes. The E
//    layer filters only with the E gate on AND an E-modulus table loaded; then the cell's E must be a
//    number inside [emodMin, emodMax] (a NaN E fails). With no table the E layer is skipped and the group
//    is the box alone, whatever the E range says.
//  - `computeMask` samples that predicate over the plot area (the tinted region).
//  - `isoContour` extracts the iso-E lines of the table (marching squares on the table's own grid; the
//    crossings sit on grid edges, where the bilinear lookup is exactly linear).
//  - `hitTest` / `beginDrag` / `dragValue` / `constrainEdit` turn pointer positions into edits of one gate
//    field: the nearest box edge or E contour within a pixel tolerance, relative drag (no jump on press),
//    clamp to the control's range, round to the field's display precision, keep min <= max.
//  - `countTargets` / `previewText` give the live "N of M" count on the plotted cells.
//
// Pixel coordinates are the chart canvas's (the Viewport's `left/top/width/height` is the plot rectangle);
// data coordinates are (area µm², deformability).
import type { Viewport } from "../../../review/charts/chartMath";
import { insidePlot } from "../../../review/charts/scatterHitTest";
import { lookupEmodulus, type PreparedLut } from "../emodulusLut";
import { tuneSpec, type TuneFieldId, type TuneValues } from "../tuneDraft";

export type Pt = readonly [number, number];
/** A line of a contour, in data space: x = area (µm²), y = deformability. */
export type Polyline = Pt[];
/** One iso-E level: open lines and closed loops. */
export type Contour = Polyline[];

// ---- the gate -----------------------------------------------------------------

export interface GateSpec {
  enabled: boolean;
  areaMin: number;
  areaMax: number;
  deformMin: number;
  deformMax: number;
  emodEnabled: boolean;
  emodMin: number;
  emodMax: number;
}

/** The target-group fields of a tune draft (the values the operator sees, not the applied ones). */
export function gateSpecFromValues(v: TuneValues): GateSpec {
  return {
    enabled: Boolean(v.targetGroupEnabled),
    areaMin: Number(v.targetAreaMin),
    areaMax: Number(v.targetAreaMax),
    deformMin: Number(v.targetDeformabilityMin),
    deformMax: Number(v.targetDeformabilityMax),
    emodEnabled: Boolean(v.targetEmodulusEnabled),
    emodMin: Number(v.targetEmodulusMin),
    emodMax: Number(v.targetEmodulusMax),
  };
}

export const sameGate = (a: GateSpec, b: GateSpec): boolean =>
  a.enabled === b.enabled && a.areaMin === b.areaMin && a.areaMax === b.areaMax && a.deformMin === b.deformMin && a.deformMax === b.deformMax
  && a.emodEnabled === b.emodEnabled && a.emodMin === b.emodMin && a.emodMax === b.emodMax;

/** E (kPa) for a point, NaN without a table or outside its coverage. */
export const lookupE = lookupEmodulus;

/** Whether the E layer filters at all: the gate has an E range AND a table is loaded. Otherwise the target
 *  group is the box alone (the contours and the E part of the tint are not drawn either). */
export const eLayerFilters = (spec: GateSpec, tableLoaded: boolean): boolean => spec.emodEnabled && tableLoaded;

/** ProcessingScience semantics for a *valid* cell. `emodulus` is the cell's E (NaN = no value, i.e. outside
 *  the table); it is not read unless the E layer filters. With the layer filtering, a NaN E fails it.
 *  False whenever the gate is off. */
export function isTarget(spec: GateSpec, tableLoaded: boolean, areaUm: number, deformability: number, emodulus: number): boolean {
  if (!spec.enabled) return false;
  const tgArea = areaUm >= spec.areaMin && areaUm <= spec.areaMax;
  const tgDeform = deformability >= spec.deformMin && deformability <= spec.deformMax;
  const tgEmod = !eLayerFilters(spec, tableLoaded) || (!Number.isNaN(emodulus) && emodulus >= spec.emodMin && emodulus <= spec.emodMax);
  return tgArea && tgDeform && tgEmod;
}

/** Same with E looked up in `lut` (a null table skips the E layer). */
export function wouldBeTarget(spec: GateSpec, lut: PreparedLut | null, areaUm: number, deformability: number): boolean {
  const loaded = lut !== null;
  // E is only consulted when the E layer filters; skip the lookup otherwise.
  const e = spec.enabled && eLayerFilters(spec, loaded) ? lookupE(lut, areaUm, deformability) : NaN;
  return isTarget(spec, loaded, areaUm, deformability, e);
}

// ---- data <-> pixel -------------------------------------------------------------

export function viewportValid(v: Viewport): boolean {
  return v.x1 > v.x0 && v.y1 > v.y0 && v.width > 0 && v.height > 0
    && Number.isFinite(v.x0) && Number.isFinite(v.x1) && Number.isFinite(v.y0) && Number.isFinite(v.y1);
}

export const toPixel = (v: Viewport, x: number, y: number): Pt => [
  v.left + ((x - v.x0) * v.width) / (v.x1 - v.x0),
  v.top + ((v.y1 - y) * v.height) / (v.y1 - v.y0),
];

export const toDataPoint = (v: Viewport, px: number, py: number): Pt => [
  v.x0 + ((px - v.left) * (v.x1 - v.x0)) / v.width,
  v.y1 - ((py - v.top) * (v.y1 - v.y0)) / v.height,
];

// ---- tinted region ----------------------------------------------------------------

/** The predicate sampled at the centre of every step x step pixel cell of the plot rectangle (row-major
 *  from the top-left; cells at the right / bottom edge may overhang the plot). */
export interface Mask {
  cols: number;
  rows: number;
  stepPx: number;
  on: Uint8Array;
}

export const maskOnCount = (m: Mask): number => {
  let n = 0;
  for (let i = 0; i < m.on.length; i++) n += m.on[i];
  return n;
};

export const maskAt = (m: Mask, col: number, row: number): boolean =>
  col >= 0 && row >= 0 && col < m.cols && row < m.rows && m.on[row * m.cols + col] !== 0;

export function computeMask(spec: GateSpec, lut: PreparedLut | null, vp: Viewport, stepPx: number): Mask {
  const step = stepPx > 0.5 ? stepPx : 0.5;
  if (!viewportValid(vp)) return { cols: 0, rows: 0, stepPx: step, on: new Uint8Array(0) };
  const cols = Math.ceil(vp.width / step);
  const rows = Math.ceil(vp.height / step);
  const on = new Uint8Array(cols * rows);
  if (spec.enabled) {
    for (let r = 0; r < rows; r++) {
      const py = vp.top + (r + 0.5) * step;
      if (py > vp.top + vp.height) continue;
      for (let c = 0; c < cols; c++) {
        const px = vp.left + (c + 0.5) * step;
        if (px > vp.left + vp.width) continue;
        const [x, y] = toDataPoint(vp, px, py);
        if (wouldBeTarget(spec, lut, x, y)) on[r * cols + c] = 1;
      }
    }
  }
  return { cols, rows, stepPx: step, on };
}

// ---- iso-E contours ---------------------------------------------------------------

interface Node {
  next: [number, number];
  degree: number;
}

/** The `level` (kPa) contour of the table, in data space. Empty when the level lies outside its values or
 *  is not a number. Marching squares over the grid cells with all four corners valued; a saddle cell is
 *  resolved by the bilinear centre. */
export function isoContour(lut: PreparedLut, level: number): Contour {
  const out: Contour = [];
  if (!Number.isFinite(level)) return out;
  const na = lut.areaBins, nd = lut.deformBins, g = lut.values;
  if (na < 2 || nd < 2 || g.length !== na * nd) return out;
  const a0 = lut.areaMin, da = lut.areaStep, d0 = lut.deformMin, dd = lut.deformStep;
  const value = (ai: number, di: number) => g[di * na + ai];
  const inside = (v: number) => v >= level;
  // A crossing is identified by the grid edge it lies on: the low bit says vertical
  // ((ai, di) - (ai, di + 1)) or horizontal ((ai, di) - (ai + 1, di)).
  const edgeId = (ai: number, di: number, vertical: boolean) => (di * na + ai) * 2 + (vertical ? 1 : 0);
  const crossing = (id: number): Pt => {
    const vertical = id % 2 === 1;
    const cell = (id - (vertical ? 1 : 0)) / 2;
    const ai = cell % na, di = (cell - ai) / na;
    const aj = vertical ? ai : ai + 1, dj = vertical ? di + 1 : di;
    const fp = value(ai, di), fq = value(aj, dj);
    const t = (level - fp) / (fq - fp);
    return [a0 + da * (ai + t * (aj - ai)), d0 + dd * (di + t * (dj - di))];
  };

  const graph = new Map<number, Node>();
  const touch = (id: number): Node => {
    let n = graph.get(id);
    if (!n) graph.set(id, (n = { next: [0, 0], degree: 0 }));
    return n;
  };
  const link = (a: number, b: number) => {
    const A = touch(a);
    if (A.degree < 2) A.next[A.degree++] = b;
    const B = touch(b);
    if (B.degree < 2) B.next[B.degree++] = a;
  };

  for (let di = 0; di + 1 < nd; di++) {
    for (let ai = 0; ai + 1 < na; ai++) {
      const f0 = value(ai, di), f1 = value(ai + 1, di), f2 = value(ai + 1, di + 1), f3 = value(ai, di + 1);
      if (Number.isNaN(f0) || Number.isNaN(f1) || Number.isNaN(f2) || Number.isNaN(f3)) continue;
      const s0 = inside(f0), s1 = inside(f1), s2 = inside(f2), s3 = inside(f3);
      if (s0 === s1 && s1 === s2 && s2 === s3) continue;
      const e0 = edgeId(ai, di, false); // c0 - c1
      const e1 = edgeId(ai + 1, di, true); // c1 - c2
      const e2 = edgeId(ai, di + 1, false); // c3 - c2
      const e3 = edgeId(ai, di, true); // c0 - c3
      const crossed: number[] = [];
      if (s0 !== s1) crossed.push(e0);
      if (s1 !== s2) crossed.push(e1);
      if (s3 !== s2) crossed.push(e2);
      if (s0 !== s3) crossed.push(e3);
      if (crossed.length === 2) {
        link(crossed[0], crossed[1]);
      } else if (crossed.length === 4) {
        // Saddle: the bilinear centre value decides which diagonal pair is connected.
        const centre = inside(0.25 * (f0 + f1 + f2 + f3));
        if (centre === s0) {
          link(e0, e1); // cut off c1
          link(e2, e3); // cut off c3
        } else {
          link(e0, e3); // cut off c0
          link(e1, e2); // cut off c2
        }
      }
    }
  }

  const visited = new Set<number>();
  const walk = (start: number): Polyline => {
    const line: Polyline = [];
    let cur = start;
    for (;;) {
      visited.add(cur);
      line.push(crossing(cur));
      const node = graph.get(cur)!;
      let advanced = false;
      for (let i = 0; i < node.degree; i++) {
        const cand = node.next[i];
        if (visited.has(cand)) continue;
        cur = cand;
        advanced = true;
        break;
      }
      if (!advanced) break;
    }
    return line;
  };
  // Open lines first (they end on degree-1 crossings at the grid boundary), then closed loops.
  for (const [id, node] of graph) {
    if (node.degree === 1 && !visited.has(id)) {
      const line = walk(id);
      if (line.length >= 2) out.push(line);
    }
  }
  for (const id of graph.keys()) {
    if (!visited.has(id)) {
      const line = walk(id);
      if (line.length >= 3) line.push(line[0]); // close the loop
      if (line.length >= 2) out.push(line);
    }
  }
  return out;
}

// ---- handles ------------------------------------------------------------------------

export type HandleKind = "none" | "areaMin" | "areaMax" | "deformMin" | "deformMax" | "emodMin" | "emodMax";

export const DEFAULT_HANDLE_TOLERANCE_PX = 6;

const HANDLE_FIELD: Record<Exclude<HandleKind, "none">, TuneFieldId> = {
  areaMin: "targetAreaMin",
  areaMax: "targetAreaMax",
  deformMin: "targetDeformabilityMin",
  deformMax: "targetDeformabilityMax",
  emodMin: "targetEmodulusMin",
  emodMax: "targetEmodulusMax",
};

/** The tune field a handle edits (kind != "none"). */
export const handleField = (kind: Exclude<HandleKind, "none">): TuneFieldId => HANDLE_FIELD[kind];
/** An area edge: a vertical line, dragged horizontally. */
export const isVerticalEdge = (kind: HandleKind): boolean => kind === "areaMin" || kind === "areaMax";
/** A deformability edge: a horizontal line, dragged vertically. */
export const isHorizontalEdge = (kind: HandleKind): boolean => kind === "deformMin" || kind === "deformMax";
export const isEmodHandle = (kind: HandleKind): boolean => kind === "emodMin" || kind === "emodMax";

export function handleValue(spec: GateSpec, kind: HandleKind): number {
  switch (kind) {
    case "areaMin": return spec.areaMin;
    case "areaMax": return spec.areaMax;
    case "deformMin": return spec.deformMin;
    case "deformMax": return spec.deformMax;
    case "emodMin": return spec.emodMin;
    case "emodMax": return spec.emodMax;
    case "none": return NaN;
  }
}

/** Contours of the two E levels (null = not drawn / no table). */
export interface EContours {
  min: Contour | null;
  max: Contour | null;
}
export const NO_CONTOURS: EContours = { min: null, max: null };

export interface HandleHit {
  kind: HandleKind;
  distancePx: number;
  /** The nearest point on the handle (pixels). */
  anchor: Pt;
}
export const NO_HIT: HandleHit = { kind: "none", distancePx: Infinity, anchor: [0, 0] };

function distanceToSegment(px: number, py: number, a: Pt, b: Pt): { d: number; nearest: Pt } {
  const dx = b[0] - a[0], dy = b[1] - a[1];
  const len2 = dx * dx + dy * dy;
  let t = 0;
  if (len2 > 0) t = Math.min(1, Math.max(0, ((px - a[0]) * dx + (py - a[1]) * dy) / len2));
  const nearest: Pt = [a[0] + t * dx, a[1] + t * dy];
  return { d: Math.hypot(px - nearest[0], py - nearest[1]), nearest };
}

/** The box edges in pixels: x of the area edges, y of the deformability edges (the upper edge is the
 *  deformability maximum), and the part of each edge that lies inside the plot. */
function boxPixels(spec: GateSpec, vp: Viewport) {
  const right = vp.left + vp.width, bottom = vp.top + vp.height;
  const xMin = toPixel(vp, spec.areaMin, 0)[0];
  const xMax = toPixel(vp, spec.areaMax, 0)[0];
  const yTop = toPixel(vp, 0, spec.deformMax)[1];
  const yBottom = toPixel(vp, 0, spec.deformMin)[1];
  return {
    right, bottom, xMin, xMax, yTop, yBottom,
    vy0: Math.max(yTop, vp.top), vy1: Math.min(yBottom, bottom),
    hx0: Math.max(xMin, vp.left), hx1: Math.min(xMax, right),
  };
}

/** The handle nearest to (px, py) within `tolerancePx`; nothing unless the gate is on and the pointer is
 *  inside the plot rectangle. Edges win ties against E contours; of two coincident edges the one on the
 *  pointer's side is taken. */
export function hitTest(spec: GateSpec, contours: EContours, vp: Viewport, px: number, py: number, tolerancePx = DEFAULT_HANDLE_TOLERANCE_PX): HandleHit {
  let best: HandleHit = NO_HIT;
  if (!spec.enabled || !viewportValid(vp) || !insidePlot(vp, px, py)) return best;
  const consider = (kind: HandleKind, distance: number, anchor: Pt) => {
    if (distance <= tolerancePx && distance < best.distancePx) best = { kind, distancePx: distance, anchor };
  };
  const b = boxPixels(spec, vp);
  const vertical = (kind: HandleKind, x: number) => {
    if (x < vp.left || x > b.right || b.vy0 > b.vy1) return;
    const cy = Math.min(Math.max(py, b.vy0), b.vy1);
    consider(kind, Math.hypot(px - x, py - cy), [x, cy]);
  };
  const horizontal = (kind: HandleKind, y: number) => {
    if (y < vp.top || y > b.bottom || b.hx0 > b.hx1) return;
    const cx = Math.min(Math.max(px, b.hx0), b.hx1);
    consider(kind, Math.hypot(px - cx, py - y), [cx, y]);
  };
  if (Math.abs(b.xMax - b.xMin) < 1) vertical(px >= b.xMax ? "areaMax" : "areaMin", b.xMax);
  else {
    vertical("areaMin", b.xMin);
    vertical("areaMax", b.xMax);
  }
  if (Math.abs(b.yBottom - b.yTop) < 1) horizontal(py <= b.yTop ? "deformMax" : "deformMin", b.yTop);
  else {
    horizontal("deformMin", b.yBottom);
    horizontal("deformMax", b.yTop);
  }
  const contourHit = (kind: HandleKind, contour: Contour | null) => {
    if (!contour) return;
    for (const line of contour) {
      for (let i = 0; i + 1 < line.length; i++) {
        const { d, nearest } = distanceToSegment(px, py, toPixel(vp, line[i][0], line[i][1]), toPixel(vp, line[i + 1][0], line[i + 1][1]));
        if (d <= tolerancePx && insidePlot(vp, nearest[0], nearest[1])) consider(kind, d, nearest);
      }
    }
  };
  if (spec.emodEnabled) {
    contourHit("emodMin", contours.min);
    contourHit("emodMax", contours.max);
  }
  return best;
}

/** A representative pixel position on a handle, inside the plot (the middle of the visible edge, or a middle
 *  vertex of the E contour); null when it is not visible. */
export function handleAnchor(kind: HandleKind, spec: GateSpec, contours: EContours, vp: Viewport): Pt | null {
  if (!spec.enabled || !viewportValid(vp) || kind === "none") return null;
  const b = boxPixels(spec, vp);
  if (isVerticalEdge(kind)) {
    const x = kind === "areaMin" ? b.xMin : b.xMax;
    if (x < vp.left || x > b.right || b.vy0 > b.vy1) return null;
    return [x, 0.5 * (b.vy0 + b.vy1)];
  }
  if (isHorizontalEdge(kind)) {
    const y = kind === "deformMin" ? b.yBottom : b.yTop;
    if (y < vp.top || y > b.bottom || b.hx0 > b.hx1) return null;
    return [0.5 * (b.hx0 + b.hx1), y];
  }
  if (!spec.emodEnabled) return null;
  const contour = kind === "emodMin" ? contours.min : contours.max;
  if (!contour) return null;
  let best: Pt[] = [];
  for (const line of contour) {
    const visible: Pt[] = [];
    for (const d of line) {
      const p = toPixel(vp, d[0], d[1]);
      if (insidePlot(vp, p[0], p[1])) visible.push(p);
    }
    if (visible.length > best.length) best = visible;
  }
  return best.length ? best[Math.floor(best.length / 2)] : null;
}

/** Where to write the value label of an E contour: its right-most vertex inside the plot (null when no
 *  vertex is visible). */
export function contourLabelAnchor(contour: Contour, vp: Viewport): Pt | null {
  if (!viewportValid(vp)) return null;
  let best: Pt | null = null;
  for (const line of contour) {
    for (const d of line) {
      const p = toPixel(vp, d[0], d[1]);
      if (!insidePlot(vp, p[0], p[1])) continue;
      if (!best || p[0] > best[0]) best = p;
    }
  }
  return best;
}

// ---- editing ------------------------------------------------------------------------------

export interface Limits {
  lo: number;
  hi: number;
}

/** The tune panel's control range for a field (Qt reads the bound spin box's own range). */
export function fieldLimits(field: TuneFieldId): Limits {
  const spec = tuneSpec(field);
  return { lo: spec.min ?? 0, hi: spec.max ?? 0 };
}

const roundHalfAway = (x: number) => Math.sign(x) * Math.round(Math.abs(x));

/** Round to the field's display precision (its decimals). */
export function roundToField(field: TuneFieldId, value: number): number {
  const decimals = tuneSpec(field).decimals;
  if (decimals <= 0) return roundHalfAway(value);
  const scale = 10 ** decimals;
  return roundHalfAway(value * scale) / scale;
}

/** Clamp to `limits`, keep min <= max against the *other* bound of `current`, round to the field's precision. */
export function constrainEdit(kind: Exclude<HandleKind, "none">, raw: number, current: GateSpec, limits: Limits): number {
  let { lo, hi } = limits;
  switch (kind) {
    case "areaMin": hi = Math.min(hi, current.areaMax); break;
    case "areaMax": lo = Math.max(lo, current.areaMin); break;
    case "deformMin": hi = Math.min(hi, current.deformMax); break;
    case "deformMax": lo = Math.max(lo, current.deformMin); break;
    case "emodMin": hi = Math.min(hi, current.emodMax); break;
    case "emodMax": lo = Math.max(lo, current.emodMin); break;
  }
  if (hi < lo) hi = lo; // the other bound lies outside the limits: pin to the near limit
  return roundToField(handleField(kind), Math.min(Math.max(raw, lo), hi));
}

export interface DragSession {
  kind: HandleKind;
  valueAtPress: number;
  pressX: number;
  pressY: number;
  /** E handles: the nearest contour point minus the press. */
  anchorOffsetX: number;
  anchorOffsetY: number;
  eAtAnchor: number;
}

export const NO_DRAG: DragSession = { kind: "none", valueAtPress: 0, pressX: 0, pressY: 0, anchorOffsetX: 0, anchorOffsetY: 0, eAtAnchor: NaN };

export function beginDrag(hit: HandleHit, spec: GateSpec, lut: PreparedLut | null, vp: Viewport, pressX: number, pressY: number): DragSession {
  const s: DragSession = { ...NO_DRAG, kind: hit.kind, pressX, pressY, valueAtPress: handleValue(spec, hit.kind) };
  if (isEmodHandle(hit.kind)) {
    s.anchorOffsetX = hit.anchor[0] - pressX;
    s.anchorOffsetY = hit.anchor[1] - pressY;
    const [x, y] = toDataPoint(vp, hit.anchor[0], hit.anchor[1]);
    s.eAtAnchor = lookupE(lut, x, y);
  }
  return s;
}

/** The new value for the dragged field with the pointer at (nowX, nowY), already constrained. Relative to the
 *  press (no jump): an edge moves by the pointer's travel; an E contour by the change of E under the
 *  contour's anchor point carried along with the pointer. null when an E lookup has no value there (keep
 *  the last value). */
export function dragValue(s: DragSession, current: GateSpec, lut: PreparedLut | null, vp: Viewport, nowX: number, nowY: number, limits: Limits): number | null {
  if (s.kind === "none" || !viewportValid(vp)) return null;
  let raw = s.valueAtPress;
  if (isVerticalEdge(s.kind)) {
    raw += ((nowX - s.pressX) * (vp.x1 - vp.x0)) / vp.width;
  } else if (isHorizontalEdge(s.kind)) {
    raw -= ((nowY - s.pressY) * (vp.y1 - vp.y0)) / vp.height;
  } else {
    const [x, y] = toDataPoint(vp, nowX + s.anchorOffsetX, nowY + s.anchorOffsetY);
    const e = lookupE(lut, x, y);
    if (Number.isNaN(e) || Number.isNaN(s.eAtAnchor)) return null;
    raw += e - s.eAtAnchor;
  }
  return constrainEdit(s.kind, raw, current, limits);
}

// ---- live count -------------------------------------------------------------------------------

export interface PreviewCount {
  target: number;
  total: number;
}

export const previewPercent = (c: PreviewCount): number => (c.total > 0 ? (100 * c.target) / c.total : 0);

/** `xs` / `ys` are the plotted valid cells (µm², deformability), `emodulus` their E (NaN = none), same order;
 *  `tableLoaded` is whether the E-modulus table is loaded (when it is not, the E values are not read: the E
 *  layer is skipped). Counts the cells `isTarget` accepts. */
export function countTargets(spec: GateSpec, tableLoaded: boolean, xs: ArrayLike<number>, ys: ArrayLike<number>, emodulus?: ArrayLike<number>): PreviewCount {
  let target = 0;
  const readE = eLayerFilters(spec, tableLoaded);
  for (let i = 0; i < xs.length; i++) {
    const e = readE && emodulus && i < emodulus.length ? emodulus[i] : NaN;
    if (isTarget(spec, tableLoaded, xs[i], ys[i], e)) target++;
  }
  return { target, total: xs.length };
}

export function previewText(c: PreviewCount): string {
  if (c.total <= 0) return "Target group preview: no plotted valid cells yet";
  return `Target group preview: ${c.target} of ${c.total} plotted valid cells (${previewPercent(c).toFixed(1)}%)`;
}
