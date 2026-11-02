// O2 Align quick controls as SFNC bindings (#806 E-3; goal E02). A quick control is a standard
// GenICam SFNC feature name to look up in the camera's node map, as the backend enumerated it
// (camera_feature_tree, bridge ABI 49). The table below names what to look for; it never
// decides what exists or is writable. The tree does: an absent node shows "not available on this
// camera", a node that is not writable now is read-only with the device's access reason, and a
// writable node takes a draft that is written only on Write. A write's outcome (readback,
// written_unconfirmed, refused, stale, unsupported, error, unknown_write_outcome) is shown as the
// backend returned it. Nothing here computes or checks a value: bounds, increments, enum entries
// and access are the backend's to enforce, and its refusal reason is shown. Pure: no I/O.
import type {
  CameraFeatureDescriptor,
  CameraFeatureEntry,
  CameraFeatureModule,
  CameraFeatureModuleTree,
  CameraFeatureOutcome,
  CameraFeatureReply,
  CameraFeatureSession,
  CameraFeatureTreeReply,
  CameraFeatureValue,
} from "../bridge";

export type BindingId = "exposure" | "gain" | "frameRate" | "trigger";
export type RoiFeature = "Width" | "Height" | "OffsetX" | "OffsetY";

/** A quick control's standard SFNC name. Only names: existence and access come from the tree. */
export interface SfncBinding {
  id: BindingId;
  label: string;
  module: CameraFeatureModule;
  feature: string;
}

/** SFNC 2.x names of the remote device (the camera), in the order the view lists them. */
export const SFNC_BINDINGS: readonly SfncBinding[] = [
  { id: "exposure", label: "Exposure", module: "remote", feature: "ExposureTime" },
  { id: "gain", label: "Gain", module: "remote", feature: "Gain" },
  { id: "frameRate", label: "Frame rate", module: "remote", feature: "AcquisitionFrameRate" },
  { id: "trigger", label: "Trigger mode", module: "remote", feature: "TriggerMode" },
];

/** The SFNC image-format names of the sensor ROI. Shown against the camera ROI row; the ROI is
 *  still written by save_camera_roi, which also saves the experiment window. */
export const SFNC_ROI_FEATURES: readonly RoiFeature[] = ["Width", "Height", "OffsetX", "OffsetY"];

export const NOT_AVAILABLE = "not available on this camera";

export type Resolution =
  /** No tree yet, a tree that could not be read, or a node the backend could not describe. */
  | { state: "unknown"; reason: string }
  /** The tree's session changed while it was read; it is being read again. */
  | { state: "stale"; reason: string }
  /** The camera has no GenApi node map. */
  | { state: "unsupported"; reason: string }
  | { state: "absent"; reason: string }
  | { state: "readOnly"; reason: string; descriptor: CameraFeatureDescriptor; session: CameraFeatureSession | null }
  | { state: "writable"; descriptor: CameraFeatureDescriptor; session: CameraFeatureSession };

/** Find a feature of a module in the tree. Undefined: the module lists no such feature. */
export function featureEntry(module: CameraFeatureModuleTree | undefined, feature: string): CameraFeatureEntry | undefined {
  return module?.features.find((f) => f.name === feature);
}

/** Resolve one SFNC name against the backend's tree reply. */
export function resolveBinding(tree: CameraFeatureTreeReply | null, module: CameraFeatureModule, feature: string): Resolution {
  if (!tree) return { state: "unknown", reason: "the feature tree has not been read yet" };
  switch (tree.outcome) {
    case "read": break;
    case "stale": return { state: "stale", reason: tree.reason || "the camera session changed; reading the tree again" };
    case "unsupported": return { state: "unsupported", reason: tree.reason || "the camera has no GenApi node map" };
    default: return { state: "unknown", reason: tree.reason || `the feature tree was not read (${tree.outcome})` };
  }
  const m = tree.modules.find((x) => x.module === module);
  if (!m) return { state: "unknown", reason: `the ${module} module was not enumerated` };
  if (!m.enumerated) return { state: "unknown", reason: m.reason || `the ${module} module could not be enumerated` };
  const entry = featureEntry(m, feature);
  if (!entry) return { state: "absent", reason: m.truncated ? `${NOT_AVAILABLE} (the tree was truncated)` : NOT_AVAILABLE };
  if (entry.error) return { state: "unknown", reason: entry.error };
  const d = entry.descriptor;
  if (!d) return { state: "unknown", reason: "the backend did not describe this feature" };
  if (!d.implemented) return { state: "absent", reason: `${NOT_AVAILABLE} (not implemented)` };
  if (!d.writable || !tree.session) {
    return { state: "readOnly", descriptor: d, session: tree.session,
      reason: d.access_reason || (d.readable ? "read-only on this camera now" : "neither readable nor writable now") };
  }
  return { state: "writable", descriptor: d, session: tree.session };
}

/** A value as the backend sent it, with the unit. Integers stay decimal strings. */
export function valueText(v: CameraFeatureValue | null | undefined, unit = ""): string {
  if (!v) return "—";
  const u = unit ? ` ${unit}` : "";
  switch (v.type) {
    case "integer": return `${v.value}${u}`;
    case "float": return v.value === null ? "not finite" : `${v.value}${u}`;
    case "boolean": return v.value ? "true" : "false";
    case "string": return v.value;
  }
}

/** The device's current range and step as text ("10–1000000 µs · step 1"); "" when it declares none. */
export function rangeText(d: CameraFeatureDescriptor): string {
  const parts: string[] = [];
  if (d.minimum || d.maximum) parts.push(`${valueText(d.minimum)}–${valueText(d.maximum, d.unit)}`);
  if (d.increment) parts.push(`step ${valueText(d.increment)}`);
  if (d.kind === "enumeration" && d.entries.length) parts.push(d.entries.join(" | "));
  return parts.join(" · ");
}

/** The access as the tree reports it, in short form. */
export function accessText(d: CameraFeatureDescriptor | null): string {
  if (!d) return "not described";
  if (!d.implemented) return "not implemented";
  return d.readable && d.writable ? "read/write" : d.writable ? "write-only" : d.readable ? "read-only" : "no access now";
}

/** The draft's text for a descriptor's current value (what the input starts from). */
export function draftOf(d: CameraFeatureDescriptor): string {
  const v = d.value;
  if (!v) return "";
  if (v.type === "float") return v.value === null ? "" : String(v.value);
  return v.type === "boolean" ? String(v.value) : v.value;
}

/** The typed request value for a draft, by the node's kind. Only the encoding is chosen here: the
 *  backend checks type, bounds, increment and enum entries and refuses with the reason. */
export function encodeDraft(d: CameraFeatureDescriptor, text: string): { ok: true; value: CameraFeatureValue } | { ok: false; reason: string } {
  const t = text.trim();
  switch (d.kind) {
    case "integer":
      return /^-?\d{1,19}$/.test(t) ? { ok: true, value: { type: "integer", value: t } } : { ok: false, reason: "an integer is required" };
    case "float": {
      const n = t === "" ? NaN : Number(t);
      return Number.isFinite(n) ? { ok: true, value: { type: "float", value: n } } : { ok: false, reason: "a number is required" };
    }
    case "boolean":
      return t === "true" || t === "false" ? { ok: true, value: { type: "boolean", value: t === "true" } } : { ok: false, reason: "true or false" };
    case "string":
    case "enumeration":
      return { ok: true, value: { type: "string", value: text } };
    default:
      return { ok: false, reason: `a ${d.kind} feature is not written from here` };
  }
}

export type FeatureTone = "confirmed" | "warn" | "blocked" | "unknown" | "unsupported" | "info" | "neutral";

/** The chip of a binding before (or apart from) any write. */
export function resolutionStatus(r: Resolution): { tone: FeatureTone; label: string; title: string } {
  switch (r.state) {
    case "writable": return { tone: "neutral", label: "Writable", title: `Writable: ${accessText(r.descriptor)}` };
    case "readOnly": return { tone: "neutral", label: `Read-only · ${r.reason}`, title: `Read-only: ${r.reason}` };
    case "absent": return { tone: "unsupported", label: "Not available on this camera", title: r.reason };
    case "unsupported": return { tone: "unsupported", label: `Unsupported · ${r.reason}`, title: r.reason };
    case "stale": return { tone: "warn", label: "Stale · reading the tree again", title: r.reason };
    case "unknown": return { tone: "unknown", label: `Unknown · ${r.reason}`, title: r.reason };
  }
}

const OUTCOME_LABEL: Record<CameraFeatureOutcome, { tone: FeatureTone; label: string }> = {
  read: { tone: "neutral", label: "Read" },
  readback: { tone: "confirmed", label: "Written · readback" },
  written_unconfirmed: { tone: "warn", label: "Written · no readback" },
  refused: { tone: "blocked", label: "Refused" },
  stale: { tone: "warn", label: "Stale" },
  unsupported: { tone: "unsupported", label: "Unsupported" },
  error: { tone: "blocked", label: "Error" },
  unknown_write_outcome: { tone: "unknown", label: "Unknown outcome · it may have reached the camera" },
};

/** A write reply as the backend returned it: outcome, requested and readback kept apart. */
export interface WriteSummary { tone: FeatureTone; label: string; requested: string; readback: string; reason: string }
/** `treeUnit`: the node's unit from the tree, for a reply that carries no readback descriptor. */
export function writeSummary(reply: CameraFeatureReply, treeUnit = ""): WriteSummary {
  const o = OUTCOME_LABEL[reply.outcome] ?? { tone: "unknown" as const, label: `Unknown reply (${String(reply.outcome)})` };
  const unit = reply.readback?.unit ?? treeUnit;
  return {
    tone: o.tone,
    label: o.label,
    requested: reply.requested ? valueText(reply.requested, unit) : "—",
    readback: reply.readback?.value ? valueText(reply.readback.value, unit) : "no readback",
    reason: reply.reason,
  };
}

/** One line of the read-only feature tree browser. */
export interface TreeLine {
  key: string;
  depth: number;
  kind: "category" | "feature";
  name: string;
  displayName: string;
  /** Categories: why the backend could not read it. Features: why it could not be described. */
  error: string;
  descriptor: CameraFeatureDescriptor | null;
}

/** The module's categories and features in the node map's order (roots first, depth first), then
 *  the features outside every category. A category is listed once even if the map repeats it. */
export function treeLines(m: CameraFeatureModuleTree): TreeLine[] {
  const categories = new Map(m.categories.map((c) => [c.name, c]));
  const features = new Map(m.features.map((f) => [f.name, f]));
  const out: TreeLine[] = [];
  const seen = new Set<string>();
  const feature = (name: string, depth: number, parent: string) => {
    const f = features.get(name);
    out.push({ key: `${parent}/${name}`, depth, kind: "feature", name, displayName: f?.display_name ?? "", error: f?.error ?? "",
      descriptor: f?.descriptor ?? null });
  };
  const walk = (name: string, depth: number) => {
    if (seen.has(name)) return;
    seen.add(name);
    const c = categories.get(name);
    out.push({ key: `category/${name}`, depth, kind: "category", name, displayName: c?.display_name ?? "", error: c?.error ?? "", descriptor: null });
    if (!c) return;
    for (const child of c.categories) walk(child, depth + 1);
    for (const f of c.features) feature(f, depth + 1, name);
  };
  for (const root of m.roots) walk(root, 0);
  for (const f of m.uncategorized) feature(f, 0, "");
  return out;
}
