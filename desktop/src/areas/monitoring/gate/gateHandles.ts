// What the gate's handles say and allow (epic #866, PR E1: MON-065, MON-066): the cursor and tooltip of a
// handle under the pointer, whether it can be edited now, and whether the draft gate differs from the applied
// one. Pure; the Qt originals are ExperimentMonitoringTab::gateHandleToolTip / showGateHover / gateHandleEditable
// and the unapplied test in refreshGateOverlay.
import { formatTuneValue, tuneChanged, tuneSpec, type TuneFieldId, type TuneState, type TuneValues } from "../tuneDraft";
import { handleField, isEmodHandle, isHorizontalEdge, isVerticalEdge, type HandleKind } from "./targetGate";

/** The fields whose difference from the applied baseline makes the gate "(unapplied)" (Qt: the Target* fields). */
export const TARGET_GATE_FIELDS: readonly TuneFieldId[] = [
  "targetGroupEnabled", "targetAreaMin", "targetAreaMax", "targetDeformabilityMin", "targetDeformabilityMax",
  "targetEmodulusEnabled", "targetEmodulusMin", "targetEmodulusMax",
];

/** True when any target-group field of the draft differs from the applied baseline. */
export const gateUnapplied = (tune: TuneState): boolean => TARGET_GATE_FIELDS.some((id) => tuneChanged(tune, id));

/** Qt: horizontal resize for an area edge, vertical for a deformability edge, size-all for an E contour. */
export function handleCursor(kind: HandleKind): string {
  if (isVerticalEdge(kind)) return "ew-resize";
  if (isHorizontalEdge(kind)) return "ns-resize";
  return "move";
}

/** "Target group area maximum (µm²): 400 µm² — drag to change"; while dragging without the hint. */
export function handleToolTip(kind: Exclude<HandleKind, "none">, draft: TuneValues, dragging: boolean): string {
  const field = handleField(kind);
  const spec = tuneSpec(field);
  const value = `${formatTuneValue(spec, draft[field])}${spec.unit}`;
  return dragging ? `${spec.label}: ${value}` : `${spec.label}: ${value} — drag to change`;
}

/** A handle is editable exactly when the control it is bound to would be enabled: the running configuration is
 *  known, no apply is in flight, the gate (and for an E contour its E range) is on, and this client controls
 *  the instrument (the tune panel is disabled for a viewer). */
export function handleEditable(kind: HandleKind, tune: Pick<TuneState, "draft" | "applying">, viewerOnly: boolean): boolean {
  if (kind === "none" || viewerOnly || !tune.draft || tune.applying) return false;
  if (!tune.draft.targetGroupEnabled) return false;
  return !isEmodHandle(kind) || Boolean(tune.draft.targetEmodulusEnabled);
}
