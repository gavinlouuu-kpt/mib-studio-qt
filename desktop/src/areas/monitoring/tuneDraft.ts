// The Monitoring tune panel's model (epic #866, MON-069..081): the Qt panel's fields, ranges and
// draft semantics as pure functions, so they test without a DOM. The Qt originals are
// src/frontend/models/ProcessingConfigDraft.cpp (fields, draft, validation, state text) and
// src/frontend/tabs/ExperimentMonitoringTab.cpp (the panel, Apply, Revert).
//
// The panel edits a *draft* over a subset of image_processing: the cell acceptance criteria, the
// target group (sorting gate, with its Young's modulus range) and the multi-image setting. The
// draft holds the authoritative values it was loaded from (`baseline`), the values on screen
// (`draft`) and which fields the operator deliberately changed (`changed`). Apply sends only those
// fields, so a value a control displays with fewer decimals than the file holds is never rewritten
// by an Apply that did not touch it.
import { EXPERIMENT_STATES } from "../../bridgeContract";

export type TuneFieldId =
  | "areaEnabled" | "areaMin" | "areaMax"
  | "deformabilityEnabled" | "deformabilityMin" | "deformabilityMax"
  | "ringRatioEnabled" | "ringRatioMin" | "ringRatioMax"
  | "areaRatioEnabled" | "areaRatioMax"
  | "borderEnabled" | "singleInnerContourEnabled"
  | "targetGroupEnabled" | "targetAreaMin" | "targetAreaMax" | "targetDeformabilityMin" | "targetDeformabilityMax"
  | "targetEmodulusEnabled" | "targetEmodulusMin" | "targetEmodulusMax"
  | "multiImageEnabled" | "multiImageCount";

export type TuneGroup = "acceptance" | "target" | "multi";
export type TuneKind = "bool" | "int" | "float";
export type TuneValue = number | boolean;
export type TuneValues = Record<TuneFieldId, TuneValue>;

export interface TuneFieldSpec {
  id: TuneFieldId;
  /** Key path under `image_processing` (the backend's ProcessingConfigJson layout). */
  path: readonly string[];
  kind: TuneKind;
  group: TuneGroup;
  /** Full name with the unit where there is one: the control's accessible name and tooltip. */
  label: string;
  /** The row's own label inside its criterion box ("Minimum", "Images per trigger"). */
  rowLabel: string;
  /** Suffix shown after the value (" µm²", " kPa"), empty when unitless. */
  unit: string;
  min?: number;
  max?: number;
  step?: number;
  /** Decimals the control shows and compares at; 0 for integers and switches. */
  decimals: number;
}

const bool = (id: TuneFieldId, group: TuneGroup, label: string, path: string): TuneFieldSpec =>
  ({ id, group, label, rowLabel: label, kind: "bool", path: path.split("."), unit: "", decimals: 0 });
const int = (id: TuneFieldId, group: TuneGroup, label: string, rowLabel: string, unit: string, path: string,
  min: number, max: number, step: number): TuneFieldSpec =>
  ({ id, group, label, rowLabel, kind: "int", path: path.split("."), unit, min, max, step, decimals: 0 });
const float = (id: TuneFieldId, group: TuneGroup, label: string, rowLabel: string, unit: string, path: string,
  min: number, max: number, step: number, decimals: number): TuneFieldSpec =>
  ({ id, group, label, rowLabel, kind: "float", path: path.split("."), unit, min, max, step, decimals });

/** Presentation order = Qt's `kFields` (ProcessingConfigDraft.cpp) with the ranges and steps of
 * `setupTuneParamsPanel` (ExperimentMonitoringTab.cpp:741-826). */
export const TUNE_FIELDS: readonly TuneFieldSpec[] = [
  bool("areaEnabled", "acceptance", "Area filter enabled", "filters.enable_area_range_check"),
  int("areaMin", "acceptance", "Area minimum (µm²)", "Minimum", " µm²", "area_threshold_min", 0, 100000, 10),
  int("areaMax", "acceptance", "Area maximum (µm²)", "Maximum", " µm²", "area_threshold_max", 0, 100000, 10),
  bool("deformabilityEnabled", "acceptance", "Deformability filter enabled", "filters.enable_deformability_range_check"),
  float("deformabilityMin", "acceptance", "Deformability minimum", "Minimum", "", "deformability_threshold_min", 0, 1, 0.01, 2),
  float("deformabilityMax", "acceptance", "Deformability maximum", "Maximum", "", "deformability_threshold_max", 0, 1, 0.01, 2),
  bool("ringRatioEnabled", "acceptance", "Ring ratio filter enabled", "filters.enable_ring_ratio_check"),
  float("ringRatioMin", "acceptance", "Ring ratio minimum", "Minimum", "", "ring_ratio_min", 0, 100, 0.5, 1),
  float("ringRatioMax", "acceptance", "Ring ratio maximum", "Maximum", "", "ring_ratio_max", 0, 100, 0.5, 1),
  bool("areaRatioEnabled", "acceptance", "Area ratio filter enabled", "filters.enable_area_ratio_check"),
  float("areaRatioMax", "acceptance", "Area ratio maximum", "Maximum", "", "area_ratio_threshold_max", 0, 10, 0.1, 2),
  bool("borderEnabled", "acceptance", "Border exclusion enabled", "filters.enable_border_check"),
  bool("singleInnerContourEnabled", "acceptance", "Single inner contour required", "filters.require_single_inner_contour"),
  bool("targetGroupEnabled", "target", "Target group sorting gate enabled", "target_group.enabled"),
  int("targetAreaMin", "target", "Target group area minimum (µm²)", "Area minimum", " µm²", "target_group.area_min", 0, 100000, 10),
  int("targetAreaMax", "target", "Target group area maximum (µm²)", "Area maximum", " µm²", "target_group.area_max", 0, 100000, 10),
  float("targetDeformabilityMin", "target", "Target group deformability minimum", "Deformability minimum", "", "target_group.deformability_min", 0, 1, 0.01, 2),
  float("targetDeformabilityMax", "target", "Target group deformability maximum", "Deformability maximum", "", "target_group.deformability_max", 0, 1, 0.01, 2),
  bool("targetEmodulusEnabled", "target", "Target group Young's modulus gate enabled", "target_group.emodulus_enabled"),
  float("targetEmodulusMin", "target", "Target group Young's modulus minimum (kPa)", "Minimum", " kPa", "target_group.emodulus_min", 0, 1000, 0.1, 2),
  float("targetEmodulusMax", "target", "Target group Young's modulus maximum (kPa)", "Maximum", " kPa", "target_group.emodulus_max", 0, 1000, 0.1, 2),
  bool("multiImageEnabled", "multi", "Multi-image acquisition enabled", "multi_image.enabled"),
  int("multiImageCount", "multi", "Images per trigger", "Images per trigger", "", "multi_image.count", 1, 32, 1),
];

const SPEC = Object.fromEntries(TUNE_FIELDS.map((s) => [s.id, s])) as Record<TuneFieldId, TuneFieldSpec>;
export const tuneSpec = (id: TuneFieldId): TuneFieldSpec => SPEC[id];

/** The multi-image fields stay locked for the whole run: they fix the run's saved image layout and
 * the realtime mode Start chose (MainWindow.cpp:1542-1550 forces inline for a series), so the
 * backend refuses them mid-run (ProcessingConfigTransaction.cpp, processingPatchIsTuneOnly). */
export const RUN_LOCKED_FIELDS: readonly TuneFieldId[] = ["multiImageEnabled", "multiImageCount"];
export const RUN_LOCKED_REASON = "Locked while an experiment runs: it fixes the run's saved image layout.";

// ---------------------------------------------------------------------------------------------
// Values

const llround = (x: number) => Math.sign(x) * Math.round(Math.abs(x));

/** Equality at the field's display precision: the operator cannot tell 0.123456 from 0.12 in a
 * two-decimal control, so re-typing the displayed value is not an edit (Qt `tuneValuesEqual`). */
export function tuneValuesEqual(spec: TuneFieldSpec, a: TuneValue, b: TuneValue): boolean {
  if (spec.kind === "bool") return Boolean(a) === Boolean(b);
  if (spec.decimals === 0) return llround(Number(a)) === llround(Number(b));
  const scale = 10 ** spec.decimals;
  return llround(Number(a) * scale) === llround(Number(b) * scale);
}

/** What a spin box does with a typed number: round to its decimals, clamp to its range. Undefined
 * for something that is not a finite number (nothing changes). */
export function normalizeTuneValue(spec: TuneFieldSpec, raw: unknown): TuneValue | undefined {
  if (spec.kind === "bool") return typeof raw === "boolean" ? raw : undefined;
  const typed = typeof raw === "number" ? raw : typeof raw === "string" && raw.trim() !== "" ? Number(raw) : NaN;
  if (!Number.isFinite(typed)) return undefined;
  let v = spec.kind === "int" ? Math.round(typed) : Number(typed.toFixed(spec.decimals));
  if (spec.min !== undefined) v = Math.max(spec.min, v);
  if (spec.max !== undefined) v = Math.min(spec.max, v);
  return v;
}

/** The text a control shows for a value (fixed decimals, like a spin box). */
export function formatTuneValue(spec: TuneFieldSpec, value: TuneValue): string {
  return spec.kind === "bool" ? String(Boolean(value)) : Number(value).toFixed(spec.decimals);
}

/** The text of the "(applied value: x)" tooltip: Qt prints the value's own string form. */
export const appliedValueText = (value: TuneValue): string => String(value);

const dig = (root: unknown, path: readonly string[]): unknown =>
  path.reduce<unknown>((o, key) => (o && typeof o === "object" ? (o as Record<string, unknown>)[key] : undefined), root);

/** The exposed fields of an `image_processing` section (the live config document's, or the open
 * file's), or null while any of them is missing or of the wrong type: the panel then has no
 * authoritative baseline and stays read-only. */
export function readTuneValues(imageProcessing: unknown): TuneValues | null {
  const out: Partial<TuneValues> = {};
  for (const spec of TUNE_FIELDS) {
    const raw = dig(imageProcessing, spec.path);
    if (spec.kind === "bool") {
      if (typeof raw !== "boolean") return null;
      out[spec.id] = raw;
    } else {
      if (typeof raw !== "number" || !Number.isFinite(raw)) return null;
      out[spec.id] = spec.kind === "int" ? Math.round(raw) : raw;
    }
  }
  return out as TuneValues;
}

/** Cross-field range checks of the exposed criteria (Qt `validateTuneConfig`): a range is checked
 * only while its criterion is enabled; field-local messages, values are never changed. */
export function tuneIssues(v: TuneValues): string[] {
  const issues: string[] = [];
  const n = (id: TuneFieldId) => Number(v[id]);
  if (v.areaEnabled && n("areaMin") > n("areaMax")) issues.push(`Area: minimum (${n("areaMin")} µm²) exceeds maximum (${n("areaMax")} µm²)`);
  if (v.deformabilityEnabled && n("deformabilityMin") > n("deformabilityMax")) issues.push("Deformability: minimum exceeds maximum");
  if (v.ringRatioEnabled && n("ringRatioMin") > n("ringRatioMax")) issues.push("Ring ratio: minimum exceeds maximum");
  if (v.targetGroupEnabled) {
    if (n("targetAreaMin") > n("targetAreaMax")) issues.push("Target group: area minimum exceeds maximum");
    if (n("targetDeformabilityMin") > n("targetDeformabilityMax")) issues.push("Target group: deformability minimum exceeds maximum");
    if (v.targetEmodulusEnabled && n("targetEmodulusMin") > n("targetEmodulusMax")) issues.push("Target group: Young's modulus minimum exceeds maximum");
  }
  if (v.multiImageEnabled && n("multiImageCount") < 1) issues.push("Multi-image: at least one image per trigger is required");
  return issues;
}

/** Which of the exposed fields of two value sets differ at display precision. */
export function tuneDifferences(a: TuneValues, b: TuneValues): TuneFieldId[] {
  return TUNE_FIELDS.filter((s) => !tuneValuesEqual(s, a[s.id], b[s.id])).map((s) => s.id);
}

// ---------------------------------------------------------------------------------------------
// Where the experiment lets the panel apply

export interface TuneScope {
  /** Apply may run now. */
  allowed: boolean;
  /** A run is Active: the criteria apply live, the multi-image fields stay locked. */
  midRun: boolean;
  /** Why not (when !allowed), or the standing note for a run in progress. */
  reason: string;
}

/** The backend's rule (ExperimentCoordinator::withTuneConfiguration): Idle takes every field,
 * Active takes the criteria and the target group live, Starting, Stopping and Failed refuse, and a
 * PL instrument refuses during a run (the PL runs with the profile compiled at Start). */
export function tuneScope(expState: number, hostProcessing: boolean): TuneScope {
  switch (expState) {
    case EXPERIMENT_STATES.Idle: return { allowed: true, midRun: false, reason: "" };
    case EXPERIMENT_STATES.Active:
      return hostProcessing
        ? { allowed: true, midRun: true, reason: "The experiment is running: changes apply live. Multi-image stays locked until it ends." }
        : { allowed: false, midRun: true, reason: "The PL runs the science with the settings compiled at Start: tune between runs." };
    case EXPERIMENT_STATES.Starting: return { allowed: false, midRun: false, reason: "The experiment is starting: apply once it is running." };
    case EXPERIMENT_STATES.Stopping: return { allowed: false, midRun: false, reason: "The experiment is stopping: apply once it has ended." };
    case EXPERIMENT_STATES.Failed: return { allowed: false, midRun: false, reason: "The last experiment failed: acknowledge it before applying." };
    default: return { allowed: false, midRun: false, reason: "The experiment state is not known yet." };
  }
}

// ---------------------------------------------------------------------------------------------
// Draft state

/** Where the last confirmed Apply went. */
export type TuneTarget =
  | { kind: "file"; path: string }
  | { kind: "runtime"; why: string };

export interface TuneState {
  /** The authoritative values the draft started from; null until the running configuration is read. */
  baseline: TuneValues | null;
  draft: TuneValues | null;
  /** Fields the operator changed, in the order they first changed. */
  changed: TuneFieldId[];
  /** A newer authoritative set that arrived while the draft was dirty or applying. */
  pending: TuneValues | null;
  conflict: boolean;
  applying: boolean;
  /** The request in flight (0 = none) and the next id to hand out. */
  activeRequest: number;
  nextRequest: number;
  lastError: string;
  savedNotApplied: boolean;
  target: TuneTarget | null;
}

export const initialTuneState = (): TuneState => ({
  baseline: null, draft: null, changed: [], pending: null, conflict: false, applying: false, activeRequest: 0, nextRequest: 1,
  lastError: "", savedNotApplied: false, target: null,
});

export type ApplyOutcome =
  | { kind: "applied"; effective: TuneValues; target: TuneTarget }
  /** `savedNotApplied`: the file carries the patch but the running configuration does not. `conflict`:
   * the file (or the running configuration) changed under the draft; nothing was written. */
  | { kind: "failed"; error: string; savedNotApplied?: boolean; conflict?: boolean };

export type TuneAction =
  | { type: "external"; values: TuneValues }
  | { type: "edit"; field: TuneFieldId; value: unknown }
  | { type: "revert" }
  | { type: "begin"; requestId: number }
  | { type: "result"; requestId: number; outcome: ApplyOutcome };

const loadBaseline = (s: TuneState, values: TuneValues): TuneState => ({
  ...s, baseline: { ...values }, draft: { ...values }, changed: [], pending: null, conflict: false, lastError: "", savedNotApplied: false,
});

export const tuneDirty = (s: TuneState) => s.changed.length > 0;
export const tuneChanged = (s: TuneState, id: TuneFieldId) => s.changed.includes(id);

/** A new authoritative config arrived (a reload, another client, a profile): Qt `noteExternalBaseline`.
 * Clean: adopt it. Dirty and an exposed field differs: keep the draft and go to Conflict until
 * Revert. Dirty and only unexposed things changed: keep editing against the same values. During an
 * Apply the value is parked and reconciled when the result arrives. */
function noteExternal(s: TuneState, values: TuneValues): TuneState {
  if (s.applying) return { ...s, pending: { ...values } };
  if (!s.baseline || !s.draft) return loadBaseline(s, values);
  const differs = tuneDifferences(values, s.baseline).length > 0;
  if (!tuneDirty(s)) return differs || s.conflict ? loadBaseline(s, values) : s;
  if (!differs) return s;
  // The same newer set again (the panel polls): nothing to record.
  if (s.conflict && s.pending && tuneDifferences(values, s.pending).length === 0) return s;
  return { ...s, pending: { ...values }, conflict: true };
}

function edit(s: TuneState, field: TuneFieldId, raw: unknown): TuneState {
  if (s.applying || !s.baseline || !s.draft) return s; // edits are not accepted mid-apply
  const spec = SPEC[field];
  const value = normalizeTuneValue(spec, raw);
  if (value === undefined) return s;
  const back = tuneValuesEqual(spec, value, s.baseline[field]);
  // Back to what the operator sees as the baseline: restore the exact baseline value, never a rounded copy.
  const next = back ? s.baseline[field] : value;
  if (s.draft[field] === next && back === !s.changed.includes(field)) return s;
  const changed = back ? s.changed.filter((id) => id !== field) : s.changed.includes(field) ? s.changed : [...s.changed, field];
  return { ...s, draft: { ...s.draft, [field]: next }, changed, lastError: "", savedNotApplied: false };
}

function result(s: TuneState, requestId: number, outcome: ApplyOutcome): TuneState {
  if (!s.applying || requestId !== s.activeRequest) return s; // a stale result is ignored
  const done: TuneState = { ...s, applying: false, activeRequest: 0 };
  if (outcome.kind === "applied") return { ...loadBaseline(done, outcome.effective), target: outcome.target };
  // Failure or partial outcome: keep the draft (still dirty), record why; what was parked meanwhile is reconciled.
  const failed: TuneState = {
    ...done, conflict: done.conflict || Boolean(outcome.conflict), savedNotApplied: Boolean(outcome.savedNotApplied),
    lastError: outcome.error || (outcome.savedNotApplied ? "Saved to the configuration file but not applied by the processing service" : "Apply failed"),
  };
  return done.pending ? noteExternal({ ...failed, pending: null }, done.pending) : failed;
}

export function tuneReducer(s: TuneState, a: TuneAction): TuneState {
  switch (a.type) {
    case "external": return noteExternal(s, a.values);
    case "edit": return edit(s, a.field, a.value);
    case "revert": {
      if (s.applying) return s;
      const values = s.pending ?? s.baseline;
      return values ? loadBaseline(s, values) : s;
    }
    case "begin":
      return s.applying ? s : { ...s, applying: true, activeRequest: a.requestId, nextRequest: Math.max(s.nextRequest, a.requestId + 1), lastError: "", savedNotApplied: false };
    case "result": return result(s, a.requestId, a.outcome);
  }
}

/** Why Apply is held back beyond an invalid draft: the experiment's state, or a changed field that
 * a running experiment keeps locked. Empty when nothing holds it back. */
export function tuneApplyBlocker(s: TuneState, scope: TuneScope): string {
  if (!scope.allowed) return scope.reason;
  if (scope.midRun && s.changed.some((id) => RUN_LOCKED_FIELDS.includes(id)))
    return "Multi-image changes wait until the experiment ends: they fix the run's saved image layout.";
  return "";
}

/** Apply is enabled only for a valid, non-conflicting dirty draft with no apply in flight and an experiment that allows it. */
export function tuneCanApply(s: TuneState, scope: TuneScope): boolean {
  return Boolean(s.draft) && tuneDirty(s) && tuneIssues(s.draft!).length === 0 && !s.conflict && !s.applying && !tuneApplyBlocker(s, scope);
}

export const tuneCanRevert = (s: TuneState) => (tuneDirty(s) || s.conflict) && !s.applying;

/** The footer's state line (Qt `stateText`). */
export function tuneStateText(s: TuneState): string {
  if (!s.draft) return "Waiting for the running configuration…";
  if (s.applying) return "Applying…";
  if (s.conflict) return "Conflict: the configuration changed elsewhere. Revert to load it, or keep editing and Apply after reverting.";
  if (s.savedNotApplied) return `Saved, not applied: ${s.lastError}`;
  if (s.lastError) return `Not applied: ${s.lastError}`;
  const issues = tuneDirty(s) ? tuneIssues(s.draft) : [];
  if (issues.length) return `Invalid: ${issues[0]}`;
  if (tuneDirty(s)) return s.changed.length === 1 ? "1 unapplied change" : `${s.changed.length} unapplied changes`;
  return "Applied";
}

/** Every validation issue to list under the state line (only while the draft is dirty, like Qt). */
export const tuneValidationList = (s: TuneState): string[] => (s.draft && tuneDirty(s) ? tuneIssues(s.draft) : []);

// ---------------------------------------------------------------------------------------------
// The patch, the read-back and the open file

/** The `image_processing` patch of the changed fields only, in the backend's nested layout. */
export function tunePatch(s: TuneState): Record<string, unknown> {
  const patch: Record<string, unknown> = {};
  if (!s.draft) return patch;
  for (const id of s.changed) {
    const spec = SPEC[id];
    let node = patch;
    for (const key of spec.path.slice(0, -1)) node = (node[key] ??= {}) as Record<string, unknown>;
    node[spec.path[spec.path.length - 1]] = s.draft[id];
  }
  return patch;
}

/** True when every changed field reads back equal (at display precision) from `effective`. */
export const tunePatchSatisfiedBy = (s: TuneState, effective: TuneValues): boolean =>
  !!s.draft && s.changed.every((id) => tuneValuesEqual(SPEC[id], effective[id], s.draft![id]));

/** Key paths where an open configuration file's `image_processing` disagrees with the running one,
 * over the keys both carry (the file's unknown keys and the keys it lacks say nothing). The backend
 * applies a document's every known value to an idle instrument, so a file that disagrees with what
 * runs is not used to save a tune: the apply goes to the running configuration only. */
export function documentDisagreements(fileSection: unknown, runningSection: unknown): string[] {
  const out: string[] = [];
  const walk = (file: unknown, running: unknown, prefix: string) => {
    if (!file || typeof file !== "object" || Array.isArray(file) || !running || typeof running !== "object") return;
    for (const [key, value] of Object.entries(file as Record<string, unknown>)) {
      const other = (running as Record<string, unknown>)[key];
      if (other === undefined) continue;
      if (value && typeof value === "object" && !Array.isArray(value)) walk(value, other, `${prefix}${key}.`);
      else if (JSON.stringify(value) !== JSON.stringify(other)) out.push(`${prefix}${key}`);
    }
  };
  walk(fileSection, runningSection, "");
  return out;
}
