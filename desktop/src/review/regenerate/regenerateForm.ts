// The Qt BatchMaskDialog's form rules (#867 REVIEW-057/-059/-060/-061/-062/-063), shared by
// both Tauri Review surfaces: YOFO Review's Regenerate masks dialog (../RegenerateMasks.tsx)
// and MIB Studio's Review stage panel (../../reanalysisControls.tsx). Pure; tested in
// regenerateForm.test.ts.

/** The Processing Config group's eight fields (config.json `image_processing` names). */
export interface RegenConfig {
  gaussian_blur_size: number;
  bg_subtract_threshold: number;
  morph_kernel_size: number;
  morph_iterations: number;
  area_threshold_min: number;
  area_threshold_max: number;
  deformability_threshold_min: number;
  deformability_threshold_max: number;
}

export type RegenConfigKey = keyof RegenConfig;

export interface RegenConfigField {
  key: RegenConfigKey;
  label: string;
  group: "image" | "validation";
  min: number;
  max: number;
  step: number;
  /** Kernels run odd (the pipeline's toOdd); Qt coerces the shown value too. */
  odd?: boolean;
  decimals?: number;
  suffix?: string;
}

/** Qt's spin boxes: labels, ranges, steps (BatchMaskDialog.cpp:179-223). */
export const REGEN_CONFIG_FIELDS: readonly RegenConfigField[] = [
  { key: "gaussian_blur_size", label: "Blur kernel", group: "image", min: 1, max: 99, step: 2, odd: true },
  { key: "bg_subtract_threshold", label: "BG threshold", group: "image", min: 0, max: 255, step: 1 },
  { key: "morph_kernel_size", label: "Morph kernel", group: "image", min: 1, max: 99, step: 2, odd: true },
  { key: "morph_iterations", label: "Morph iters", group: "image", min: 1, max: 20, step: 1 },
  { key: "area_threshold_min", label: "Area min", group: "validation", min: 0, max: 10000, step: 1, suffix: "µm²" },
  { key: "area_threshold_max", label: "Area max", group: "validation", min: 0, max: 10000, step: 1, suffix: "µm²" },
  { key: "deformability_threshold_min", label: "Deform min", group: "validation", min: 0, max: 1, step: 0.001, decimals: 4 },
  { key: "deformability_threshold_max", label: "Deform max", group: "validation", min: 0, max: 1, step: 0.001, decimals: 4 },
];

/** The kernel's ProcessingConfig defaults (ProcessingTypes.h), when nothing else is known. */
export const DEFAULT_REGEN_CONFIG: RegenConfig = {
  gaussian_blur_size: 3,
  bg_subtract_threshold: 8,
  morph_kernel_size: 3,
  morph_iterations: 1,
  area_threshold_min: 60,
  area_threshold_max: 290,
  deformability_threshold_min: 0,
  deformability_threshold_max: 1,
};

/** Qt's toOdd: an even kernel becomes the next odd one. */
export function toOdd(v: number): number {
  return v % 2 === 0 ? v + 1 : v;
}

/** One field's value as Qt's spin box keeps it: clamped to its range, an integer unless it
 * has decimals (then rounded to them), odd for the kernels. NaN keeps `fallback`. */
export function coerceField(field: RegenConfigField, value: number, fallback: number): number {
  if (!Number.isFinite(value)) return fallback;
  const scale = 10 ** (field.decimals ?? 0);
  let v = Math.round(value * scale) / scale;
  v = Math.min(field.max, Math.max(field.min, v));
  if (field.odd) v = Math.min(field.max, toOdd(v));
  return v;
}

/** Every field coerced (a recorded or live config may hold an even kernel; Qt shows it odd). */
export function coerceConfig(config: RegenConfig): RegenConfig {
  const out = { ...config };
  for (const f of REGEN_CONFIG_FIELDS) out[f.key] = coerceField(f, Number(config[f.key]), DEFAULT_REGEN_CONFIG[f.key]);
  return out;
}

/** The eight fields of an `image_processing` object (absent or non-numeric ones from
 * `fallback`), coerced. */
export function configFromImageProcessing(json: unknown, fallback: RegenConfig = DEFAULT_REGEN_CONFIG): RegenConfig {
  const source = (json && typeof json === "object" ? json : {}) as Record<string, unknown>;
  const out = { ...fallback };
  for (const f of REGEN_CONFIG_FIELDS) {
    const v = source[f.key];
    if (typeof v === "number" && Number.isFinite(v)) out[f.key] = v;
  }
  return coerceConfig(out);
}

/** The partial `image_processing` the run sends: the eight fields, nothing else, so every
 * other setting stays the base config's (recorded, live or default). */
export function imageProcessingOverride(config: RegenConfig): Record<RegenConfigKey, number> {
  const out = {} as Record<RegenConfigKey, number>;
  for (const f of REGEN_CONFIG_FIELDS) out[f.key] = config[f.key];
  return out;
}

export function sameConfig(a: RegenConfig, b: RegenConfig): boolean {
  return REGEN_CONFIG_FIELDS.every((f) => a[f.key] === b[f.key]);
}

// ---- ROI ------------------------------------------------------------------------------

export interface Roi {
  x: number;
  y: number;
  w: number;
  h: number;
}

/** A drag from `a` to `b` (image pixels) as an ROI inside a `width` × `height` frame; null
 * for a click without area (Qt's RoiDrawCanvas keeps the previous ROI then). */
export function roiFromDrag(a: [number, number], b: [number, number], width: number, height: number): Roi | null {
  const clampX = (v: number) => Math.max(0, Math.min(width, Math.round(v)));
  const clampY = (v: number) => Math.max(0, Math.min(height, Math.round(v)));
  const x0 = clampX(Math.min(a[0], b[0])), x1 = clampX(Math.max(a[0], b[0]));
  const y0 = clampY(Math.min(a[1], b[1])), y1 = clampY(Math.max(a[1], b[1]));
  return x1 > x0 && y1 > y0 ? { x: x0, y: y0, w: x1 - x0, h: y1 - y0 } : null;
}

/** Why an ROI cannot run on a `width` × `height` source ("" = fine; null = the full frame). */
export function roiError(roi: Roi | null, width?: number, height?: number): string {
  if (!roi) return "";
  const values = [roi.x, roi.y, roi.w, roi.h];
  if (values.some((v) => !Number.isSafeInteger(v) || v < 0)) return "ROI values must be whole numbers, 0 or more";
  if (roi.w === 0 || roi.h === 0) return "";
  if (width && height && (roi.x + roi.w > width || roi.y + roi.h > height))
    return `The ROI reaches past the ${width} × ${height} frame`;
  return "";
}

export function roiLabel(roi: Roi | null): string {
  return roi && roi.w > 0 && roi.h > 0 ? `ROI ${roi.x}, ${roi.y}, ${roi.w} × ${roi.h}` : "ROI: full frame";
}

// ---- preview window (Qt getSourceFrameCount / loadPreviewFrame) ------------------------------

/** The frames the preview steps through: the whole set, or `count` frames from `start`
 * (0 = to the end) clamped to it. `offset` maps preview frame i to source index offset + i. */
export function previewWindow(setSize: number, whole: boolean, start: number, count: number): { offset: number; total: number } {
  if (whole) return { offset: 0, total: Math.max(0, setSize) };
  const s = Number.isSafeInteger(start) && start > 0 ? start : 0;
  if (s >= setSize) return { offset: s, total: 0 };
  const rest = setSize - s;
  const n = Number.isSafeInteger(count) && count > 0 ? Math.min(count, rest) : rest;
  return { offset: s, total: n };
}

/** Qt's navigation label. */
export function frameLabel(index: number, total: number | null): string {
  if (total === 0) return "No frames";
  return total === null ? `Frame ${index + 1}` : `Frame ${index + 1} / ${total}`;
}

// ---- background (Qt Set as Background / Clear) ------------------------------------------------

/** recorded = the file's saved background (Tauri keeps it by default, see the PR's decisions);
 * none = Qt's "Background: none" (synthetic when the box is ticked); frame = Set as
 * Background: source index `index` of `dataset`, shown as preview frame `label`. */
export type RegenBackground =
  | { kind: "recorded" }
  | { kind: "none" }
  | { kind: "frame"; index: number; dataset: string; label: number };

export function backgroundLabel(bg: RegenBackground, synthetic: boolean): string {
  if (bg.kind === "frame") return `Background: Frame ${bg.label}`;
  if (bg.kind === "recorded") return "Background: the file's saved background";
  return synthetic ? "Background: none (a synthetic one is built)" : "Background: none";
}

// ---- output (Qt computeAutoOutputPath) -----------------------------------------------------

function splitPath(p: string): { dir: string; name: string; sep: string } {
  const sep = p.includes("\\") && !p.includes("/") ? "\\" : "/";
  const trimmed = p.replace(/[\\/]+$/, "");
  const i = Math.max(trimmed.lastIndexOf("/"), trimmed.lastIndexOf("\\"));
  return { dir: i >= 0 ? trimmed.slice(0, i) : "", name: i >= 0 ? trimmed.slice(i + 1) : trimmed, sep };
}

/** Qt's default output: `<base>_remasked.h5` beside an HDF5 or AVI source, and
 * `<folder name>_remasked.h5` inside an image folder ("batch" when it has no name). */
export function remaskedDefault(kind: "hdf" | "avi" | "folder", sourcePath: string): { dir: string; base: string } {
  if (!sourcePath) return { dir: "", base: "masks" };
  const { dir, name } = splitPath(sourcePath);
  if (kind === "folder") return { dir: sourcePath.replace(/[\\/]+$/, ""), base: name || "batch" };
  const dot = name.lastIndexOf(".");
  return { dir, base: dot > 0 ? name.slice(0, dot) : name };
}

export function joinDir(dir: string, name: string): string {
  if (!dir) return name;
  const sep = dir.includes("\\") && !dir.includes("/") ? "\\" : "/";
  return dir.endsWith("/") || dir.endsWith("\\") ? dir + name : dir + sep + name;
}

// ---- processing core (Qt BatchMaskDialog.cpp:892-912) ------------------------------------------

/** Qt's processing-core mismatch text, for a host that compares the identities itself. */
export function coreMismatchText(recorded: { version: string; contract?: number | null }, active: { version: string; contract?: number | null }): string {
  const c = (v?: number | null) => (v === undefined || v === null ? "unknown" : String(v));
  return (
    `This file was recorded with processing core ${recorded.version} (contract ${c(recorded.contract)}), but ` +
    `regeneration will use the currently active core ${active.version} (contract ${c(active.contract)}).\n\n` +
    "The application will not switch cores automatically; the regenerated output records the active identity."
  );
}
