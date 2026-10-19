// Parts of the Experiment · Plan sub-view (O3, #716 A5) shared by the single-sample plan
// (PlanView) and the sample queue (QueuePlan): the sheet's fields and row editor, the sample
// format choice, the operator fields and the PZ7035's explicit switch to Run. None of them acts on
// its own: editing changes the local plan, and the Run switch is a button.
import { useId, useState } from "react";
import { Button, Card, Notice, OptionGroup } from "@mib/ui";
import { EXPERIMENT_START_REQUEST as B } from "../bridgeContract";
import {
  FIELD_KEYS,
  KEY_LABELS,
  ROW_TEXT_FIELDS,
  freeRowIssues,
  issueLine,
  issuesFor,
  operatorText,
  type OperatorChoice,
  type PlanDraft,
  type PlanIssue,
  type PlanCameraMode,
  type RowTextField,
  type SampleFormat,
  type SampleRow,
} from "./planModel";
import type { Verdict } from "./prepareModel";
import type { PlanController } from "./usePlan";

/** Why Prepare's checklist blocks (confirmBlockers), null when it does not. */
export type StartBlockers = { tone: "blocked" | "unknown" | "warn"; lines: string[] } | null;

/** Why Start run is unavailable, each as its own Notice next to the button. */
export interface StartBlock {
  id: "started" | "starting" | "prepare" | "locked" | "sheet" | "control" | "status" | "paused" | "camera";
  tone: "blocked" | "unknown" | "warn" | "info";
  title: string;
  text: string;
}

/** Every reason Start run is unavailable, in plain words (empty = it is available). */
export function startBlocks(p: { started: "started" | "unknown" | null; notStarted?: boolean; starting: boolean; verdict: Verdict; blockers: StartBlockers; lockedReason: string; issues: readonly PlanIssue[]; cameraReason?: string }): StartBlock[] {
  const out: StartBlock[] = [];
  if (p.started === "started") out.push({ id: "started", tone: "info", title: "This plan's run has started", text: "Its identity is recorded in that run. A new plan mints a new experiment id." });
  if (p.started === "unknown") out.push({ id: "started", tone: "info", title: "This plan's identity may be recorded", text: p.notStarted
    ? "The backend did not start the run, but it may have opened the run's file first, so the identity is not sent again. A new plan mints a new experiment id."
    : "The start request was sent but its reply was lost, so it is not sent again. A new plan mints a new experiment id." });
  if (p.starting) out.push({ id: "starting", tone: "info", title: "Start request pending", text: "Waiting for the file dialog, readiness and the backend's answer." });
  // Prepare governs: the same verdict and the same words as Prepare's "Confirm readiness is unavailable".
  if (p.verdict.word === "Not ready")
    out.push({ id: "prepare", tone: p.blockers?.tone ?? "blocked", title: `Start is unavailable · Prepare: ${p.verdict.title}`, text: p.blockers?.lines.length ? `${p.blockers.lines.join(". ")}.` : p.verdict.detail });
  if (p.lockedReason) out.push({ id: "locked", tone: "info", title: "Start is unavailable", text: `${p.lockedReason.replace(/\.$/, "")}.` });
  if (p.cameraReason) out.push({ id: "camera", tone: "blocked", title: "Start is unavailable · camera mode", text: `${p.cameraReason}. Show the experiment window first (the button above).` });
  if (p.issues.length)
    out.push({ id: "sheet", tone: "blocked", title: `Start is unavailable · the plan has ${p.issues.length === 1 ? "1 issue" : `${p.issues.length} issues`}`, text: p.issues.map(issueLine).join(" ") });
  return out;
}

export const NOT_DECLARED = <span title="Not declared">–</span>;
const sameOperator = (a: OperatorChoice, b: OperatorChoice) => a.name.trim() === b.name.trim() && a.id.trim() === b.id.trim();

/** One labelled input of the sheet, with its issues under it. */
export function Field(p: {
  label: string; value: string; onChange: (v: string) => void; issues: PlanIssue[]; disabled: boolean;
  hint?: string; multiline?: boolean; numeric?: boolean; wide?: boolean; testId?: string;
}) {
  const id = useId();
  const invalid = p.issues.length > 0;
  const common = {
    id, value: p.value, disabled: p.disabled, placeholder: "Not declared", "aria-invalid": invalid || undefined,
    "aria-describedby": invalid || p.hint ? `${id}-note` : undefined, "data-testid": p.testId,
  };
  return (
    <div className={`ws-plan-field${p.wide ? " ws-plan-field--wide" : ""}`}>
      <label htmlFor={id}>{p.label}</label>
      {p.multiline
        ? <textarea {...common} rows={3} onChange={(e) => p.onChange(e.target.value)} />
        : <input {...common} type="text" inputMode={p.numeric ? "decimal" : undefined} autoComplete="off" onChange={(e) => p.onChange(e.target.value)} />}
      {invalid
        ? <span id={`${id}-note`} className="ws-plan-issue" role="alert">{p.issues.map((i) => i.text).join(" ")}</span>
        : p.hint ? <span id={`${id}-note`} className="ws-detail">{p.hint}</span> : null}
    </div>
  );
}

/** The editor of one sheet row: one field per member of `/experiment_annotation`. */
export function SampleEditor({ row, issues, disabled, onRow }: { row: SampleRow; issues: PlanIssue[]; disabled: boolean; onRow: (change: (r: SampleRow) => SampleRow) => void }) {
  const set = (field: RowTextField) => (v: string) => onRow((r) => ({ ...r, [field]: v }));
  const fieldIssues = (field: RowTextField, extra: string[] = []) => issuesFor(issues, [FIELD_KEYS[field], ...extra]);
  const used = row.free.filter((e) => e.key.trim() !== "" || e.value.trim() !== "").length;
  return (
    <div className="ws-plan-form" data-testid="plan-editor">
      {ROW_TEXT_FIELDS.map((f) => (
        <Field key={f.field} label={KEY_LABELS[f.key]} value={row[f.field]} onChange={set(f.field)} issues={fieldIssues(f.field)} disabled={disabled} hint={f.hint} testId={`plan-${f.field}`} />
      ))}
      <div className="ws-plan-field ws-plan-pair">
        <Field label="Concentration" value={row.concentrationValue} onChange={set("concentrationValue")} numeric
          issues={fieldIssues("concentrationValue", ["annotation.concentration"])} disabled={disabled} hint="value ≥ 0, with its unit" testId="plan-concentrationValue" />
        <Field label="Unit" value={row.concentrationUnit} onChange={set("concentrationUnit")} issues={fieldIssues("concentrationUnit")} disabled={disabled} hint="e.g. µM" testId="plan-concentrationUnit" />
      </div>
      <Field label={KEY_LABELS["annotation.temperature_c"]} value={row.temperatureC} onChange={set("temperatureC")} numeric issues={fieldIssues("temperatureC")}
        disabled={disabled} hint={`declared, ${B.temperature_c_min} to ${B.temperature_c_max} °C`} testId="plan-temperatureC" />
      <Field label="Notes" value={row.notes} onChange={set("notes")} multiline wide issues={fieldIssues("notes")} disabled={disabled} testId="plan-notes" />
      <div className="ws-plan-field ws-plan-field--wide" data-testid="plan-free">
        <span className="ws-plan-label">Key/values</span>
        {row.free.map((e, i) => {
          const rowIssues = freeRowIssues(issues, i);
          return (
            <div key={i} className="ws-plan-kv">
              <input type="text" aria-label={`Key ${i + 1}`} placeholder="key" value={e.key} disabled={disabled} aria-invalid={rowIssues.length > 0 || undefined} autoComplete="off"
                onChange={(ev) => { const key = ev.target.value; onRow((r) => ({ ...r, free: r.free.map((x, j) => (j === i ? { ...x, key } : x)) })); }} />
              <input type="text" aria-label={`Value ${i + 1}`} placeholder="value" value={e.value} disabled={disabled} aria-invalid={rowIssues.length > 0 || undefined} autoComplete="off"
                onChange={(ev) => { const value = ev.target.value; onRow((r) => ({ ...r, free: r.free.map((x, j) => (j === i ? { ...x, value } : x)) })); }} />
              <Button size="small" disabled={disabled} aria-label={`Remove key/value ${i + 1}`} onClick={() => onRow((r) => ({ ...r, free: r.free.filter((_, j) => j !== i) }))}>Remove</Button>
              {rowIssues.length ? <span className="ws-plan-issue" role="alert">{rowIssues.map((x) => `${x.label}: ${x.text}`).join(" ")}</span> : null}
            </div>
          );
        })}
        {issuesFor(issues, ["annotation.free"]).map((x) => <span key={x.text} className="ws-plan-issue" role="alert">{x.text}</span>)}
        <div>
          <Button size="small" disabled={disabled || row.free.length >= B.max_free_entries}
            title={row.free.length >= B.max_free_entries ? `At most ${B.max_free_entries} key/values` : undefined}
            onClick={() => onRow((r) => ({ ...r, free: [...r.free, { key: "", value: "" }] }))}>Add key/value</Button>
          <span className="ws-detail">{`Strings only, unique keys, ${used} of at most ${B.max_free_entries}.`}</span>
        </div>
      </div>
    </div>
  );
}

/** The sample format: a single sample, or a sample queue (one run and one file per sample). */
export function FormatCard({ draft, locked, onFormat }: { draft: PlanDraft; locked: boolean; onFormat: (format: SampleFormat) => void }) {
  const many = draft.rows.length > 1;
  return (
    <Card title="Sample format" subtitle="Each run is its own file" className="ws-plan__format">
      <OptionGroup
        legend="Sample format"
        hideLegend
        value={draft.format}
        disabled={locked}
        onChange={(format) => onFormat(format === "queue" ? "queue" : "single")}
        options={[
          { value: "single", label: "Single sample", description: "One run, one file",
            unavailable: many && draft.format === "queue" ? `The sheet has ${draft.rows.length} samples · remove all but one first` : undefined },
          { value: "queue", label: "Sample queue", description: "Manual tube swaps · one file per sample" },
        ]}
      />
      <p className="ws-note">A sample change is a new run (ADR 0015). Nothing here acts until you press Start.</p>
    </Card>
  );
}

/** The selected operator (#393 phase 1): typed or picked from the remembered list. */
export function OperatorFields({ plan, issues, locked }: { plan: PlanController; issues: (k: "name" | "id") => PlanIssue[]; locked: boolean }) {
  const { draft } = plan;
  const setOperator = (k: "name" | "id") => (v: string) => plan.edit((d) => ({ ...d, operator: { ...d.operator, [k]: v } }));
  return (
    <>
      <div className="ws-plan-form ws-plan-form--single">
        <Field label="Operator" value={draft.operator.name} onChange={setOperator("name")} issues={issues("name")} disabled={locked} testId="plan-operator-name"
          hint="Type a name, or pick one remembered below" />
        <Field label="Operator id" value={draft.operator.id} onChange={setOperator("id")} issues={issues("id")} disabled={locked} testId="plan-operator-id" hint="Optional" />
      </div>
      {plan.operators.length ? (
        <div className="ws-plan-operators" data-testid="plan-operators">
          <span className="ws-detail">Remembered:</span>
          {plan.operators.map((o) => (
            <Button key={`${o.name}\u0000${o.id}`} size="small" disabled={locked} variant={sameOperator(o, draft.operator) ? "selected" : "secondary"}
              onClick={() => plan.edit((d) => ({ ...d, operator: { ...o } }))}>{operatorText(o)}</Button>
          ))}
        </div>
      ) : null}
      <p className="ws-note" data-testid="plan-operator-note">
        The operator you select is recorded as selected (assurance "selected"). It is not a sign-in and not authentication. Names are remembered on this computer only.
      </p>
    </>
  );
}

/** The explicit switch to the mode a run records in (App's applyCameraMode("experiment"), as the
 *  classic Experiment tab on entry): the PZ7035's "Switch to Run", a desktop camera's "Show the
 *  experiment window". Navigation never switches. */
export function CameraModeNotice({ mode: runMode, onSwitch }: { mode: PlanCameraMode | null | undefined; onSwitch?: () => Promise<unknown> }) {
  const [switching, setSwitching] = useState(false);
  if (!runMode || !onSwitch) return null;
  const press = async () => {
    if (switching) return;
    setSwitching(true);
    try { await onSwitch(); } finally { setSwitching(false); }
  };
  return (
    <Notice tone={runMode.tone} title={`Camera · ${runMode.label}`} data-testid="plan-camera-mode"
      actions={<Button variant="primary" disabled={!!runMode.blocked || switching} title={runMode.blocked || undefined} onClick={() => void press()}>
        {switching ? "Switching…" : runMode.action}
      </Button>}>
      {runMode.blocked ? `${runMode.action} is unavailable: ${runMode.blocked}.` : runMode.detail}
    </Notice>
  );
}
