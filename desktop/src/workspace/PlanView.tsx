// O3 Experiment · Plan (#716 A5; ADR 0015 items 1, 4, 5): a single-sample plan with its sample
// sheet, the run's identity and the selected operator, and an explicit "Start run". Editing the
// plan sends nothing; "Start run" runs the classic Start flow (pick the file, ask before a stopped
// Run's frames are discarded, readiness with the chosen file, start) with the plan's
// startRequestJson (bridge ABI 39). Prepare's verdict still governs: while Prepare's checklist
// blocks, Start run is disabled with Prepare's own words (prepareVerdict, confirmBlockers).
import { useId } from "react";
import { Button, Card, ConditionSwatch, DataTable, FactList, Notice, OptionGroup, StateChip, type DataTableColumn } from "@mib/ui";
import { EXPERIMENT_START_REQUEST as B } from "../bridgeContract";
import {
  FIELD_KEYS,
  KEY_LABELS,
  ROW_TEXT_FIELDS,
  SINGLE_SAMPLE_RUN_INDEX,
  buildStartRequest,
  conditionSlots,
  concentrationText,
  freeRowIssues,
  invalidRequestReason,
  issueLine,
  issuesFor,
  operatorText,
  type OperatorChoice,
  type PlanIssue,
  type RowTextField,
  type SampleRow,
} from "./planModel";
import type { Verdict } from "./prepareModel";
import type { PlanController, PlanStartResult } from "./usePlan";

export interface PlanViewProps {
  plan: PlanController;
  /** Why Start is unavailable apart from the plan and Prepare (the classic Start button's reasons:
   *  backend, camera or Run mode, a run is active, a start pending); "" when it is available. */
  lockedReason: string;
  /** Prepare's one verdict, and why its checklist blocks (confirmBlockers), null when it does not. */
  verdict: Verdict;
  blockers: { tone: "blocked" | "unknown" | "warn"; lines: string[] } | null;
}

/** Why Start run is unavailable, each as its own Notice next to the button. */
export interface StartBlock {
  id: "started" | "starting" | "prepare" | "locked" | "sheet";
  tone: "blocked" | "unknown" | "warn" | "info";
  title: string;
  text: string;
}

/** Every reason Start run is unavailable, in plain words (empty = it is available). */
export function startBlocks(p: { started: "started" | "unknown" | null; starting: boolean; verdict: Verdict; blockers: PlanViewProps["blockers"]; lockedReason: string; issues: readonly PlanIssue[] }): StartBlock[] {
  const out: StartBlock[] = [];
  if (p.started === "started") out.push({ id: "started", tone: "info", title: "This plan's run has started", text: "Its identity is recorded in that run. A new plan mints a new experiment id." });
  if (p.started === "unknown") out.push({ id: "started", tone: "info", title: "This plan's identity may be recorded", text: "The start request was sent but its reply was lost, so it is not sent again. A new plan mints a new experiment id." });
  if (p.starting) out.push({ id: "starting", tone: "info", title: "Start request pending", text: "Waiting for the file dialog, readiness and the backend's answer." });
  // Prepare governs: the same verdict and the same words as Prepare's "Confirm readiness is unavailable".
  if (p.verdict.word === "Not ready")
    out.push({ id: "prepare", tone: p.blockers?.tone ?? "blocked", title: `Start is unavailable · Prepare: ${p.verdict.title}`, text: p.blockers?.lines.length ? `${p.blockers.lines.join(". ")}.` : p.verdict.detail });
  if (p.lockedReason) out.push({ id: "locked", tone: "info", title: "Start is unavailable", text: `${p.lockedReason.replace(/\.$/, "")}.` });
  if (p.issues.length)
    out.push({ id: "sheet", tone: "blocked", title: `Start is unavailable · the plan has ${p.issues.length === 1 ? "1 issue" : `${p.issues.length} issues`}`, text: p.issues.map(issueLine).join(" ") });
  return out;
}

type NoticeText = { tone: "confirmed" | "warn" | "blocked" | "info"; title: string; text: string };

/** The Notice for the last Start's result, decided by the start reply alone; the InvalidRequest
 *  refusal is shown by the sheet. */
export function resultNotice(r: PlanStartResult | null): NoticeText | null {
  if (!r) return null;
  switch (r.kind) {
    case "started": return { tone: "confirmed", title: `Run ${SINGLE_SAMPLE_RUN_INDEX} started · ${r.path}`, text: `Its identity and the planned annotation are recorded in the file.${r.notice ? ` ${r.notice}` : ""} Monitor and stop it in the classic view.` };
    case "unknown": return { tone: "blocked", title: `Start outcome not known${r.path ? ` · ${r.path}` : ""}`, text: `The start request was sent but no reply was read (${r.message}). The run may have started: check it in the classic view. This plan's identity is not sent again.` };
    case "not-ready": return { tone: "warn", title: "Not started · readiness with the chosen file", text: r.message };
    case "refused": return invalidRequestReason(r.message) ? null : { tone: "blocked", title: "Not started · the backend refused the Start", text: `${r.path}: ${r.message}` };
    case "error": return { tone: "blocked", title: "Not started · an error before the start was sent", text: r.message };
    case "busy": return { tone: "info", title: "Not started · a start request is already pending", text: "Wait for it to finish." };
    default: return null;
  }
}

/** A failed status read after the start reply, said apart: it never changes the outcome. */
export function refreshNotice(r: PlanStartResult | null): NoticeText | null {
  if (!r || !("refreshError" in r) || !r.refreshError) return null;
  return { tone: "warn", title: "Run status not refreshed", text: `${r.refreshError}. The start reply decides the outcome above; the status is read again on the next poll.` };
}

const NOT_DECLARED = <span title="Not declared">–</span>;
const sameOperator = (a: OperatorChoice, b: OperatorChoice) => a.name.trim() === b.name.trim() && a.id.trim() === b.id.trim();

/** One labelled input of the sheet, with its issues under it. */
function Field(p: {
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

function SampleEditor({ row, issues, disabled, onRow }: { row: SampleRow; issues: PlanIssue[]; disabled: boolean; onRow: (change: (r: SampleRow) => SampleRow) => void }) {
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

export function PlanView({ plan, lockedReason, verdict, blockers }: PlanViewProps) {
  const { draft, started, starting } = plan;
  const row = draft.rows[0];
  const build = buildStartRequest(draft, 0, SINGLE_SAMPLE_RUN_INDEX);
  const locked = !!started || starting;
  const blocks = startBlocks({ started: started?.outcome ?? null, starting, verdict, blockers, lockedReason, issues: build.issues });
  const refusal = plan.result?.kind === "refused" ? invalidRequestReason(plan.result.message) : null;
  const outcome = resultNotice(plan.result);
  const refresh = refreshNotice(plan.result);
  const slots = conditionSlots(draft.rows);
  const editRow = (change: (r: SampleRow) => SampleRow) => plan.edit((d) => ({ ...d, rows: d.rows.map((r, i) => (i === 0 ? change(r) : r)) }));
  const setOperator = (k: "name" | "id") => (v: string) => plan.edit((d) => ({ ...d, operator: { ...d.operator, [k]: v } }));
  const operatorIssues = (k: "name" | "id") => issuesFor(build.issues, [`operator.${k}`]);

  const columns: DataTableColumn<SampleRow>[] = [
    { key: "run", header: "Run", numeric: true, width: "44px", cell: () => SINGLE_SAMPLE_RUN_INDEX },
    { key: "ref", header: "Ref", emphasis: true, width: "52px" },
    { key: "sample", header: "Sample", cell: (r) => r.sample.trim() },
    { key: "condition", header: "Condition", cell: (r) => {
      const c = r.condition.trim(), slot = slots.get(c);
      return c ? (slot ? <ConditionSwatch condition={slot}>{c}</ConditionSwatch> : c) : "";
    } },
    { key: "unit", header: "Unit", cell: (r) => r.experimentalUnit.trim() },
    { key: "conc", header: "Conc.", cell: concentrationText },
    { key: "channel", header: "Channel", cell: (r) => r.channel.trim() },
    { key: "medium", header: "Medium", cell: (r) => r.medium.trim() },
    { key: "temp", header: "°C", numeric: true, cell: (r) => r.temperatureC.trim() },
  ];

  return (
    <div className="ws-body" data-testid="workspace-plan">
      <div className="ws-plan">
        <Card title="Sample format" subtitle="Each run is its own file" className="ws-plan__format">
          <OptionGroup
            legend="Sample format"
            hideLegend
            value={draft.format}
            disabled={locked}
            onChange={(format) => plan.edit((d) => ({ ...d, format: format === "queue" ? "queue" : "single" }))}
            options={[
              { value: "single", label: "Single sample", description: "One run, one file" },
              { value: "queue", label: "Sample queue", description: "Manual tube swaps · one file per sample", unavailable: "Not yet · the guided queue comes in the next workspace step" },
            ]}
          />
          <p className="ws-note">A sample change is a new run (ADR 0015). Nothing here acts until you press Start run.</p>
        </Card>

        <Card title="Sample sheet" className="ws-plan__sheet" data-testid="plan-sheet"
          subtitle="Fixed schema · written once, as planned, into the run's file at Start (/experiment_annotation)">
          {refusal ? (
            <Notice tone="blocked" title="Not started · the backend refused the plan" data-testid="plan-refusal">
              {`${refusal.label}: ${refusal.text}. No file was created. The plan is kept: correct it and press Start run again.`}
              {refusal.key ? <span className="ws-gate-id">{refusal.key}</span> : null}
            </Notice>
          ) : null}
          <DataTable columns={columns} rows={draft.rows} rowKey={(r) => r.ref} selectedKey={row.ref} density="compact" missing={NOT_DECLARED}
            caption="As it will be recorded · – = not declared" data-testid="plan-sheet-table" />
          <SampleEditor row={row} issues={build.issues} disabled={locked} onRow={editRow} />
          <p className="ws-note">
            Leave a field empty when it is not declared: it is recorded as not declared, never as an empty value. Spaces around a value are not
            recorded. Limits are bytes of UTF-8 (µ and most accented letters take 2); a longer value is refused, never cut off.
          </p>
        </Card>

        <Card title="Run identity" subtitle="Recorded in the run's snapshot (v3)" className="ws-plan__identity" data-testid="plan-identity">
          <FactList facts={[
            { label: "Experiment", value: <span className="ws-plan-id" data-testid="plan-experiment-id">{draft.experimentId}</span> },
            { label: "Run", value: `${SINGLE_SAMPLE_RUN_INDEX} of 1 · single sample` },
            { label: "Sample ref", value: row.ref },
            { label: "Calibration record", value: "Unavailable · calibration runs come in a later step; not recorded", tone: "unknown" },
            { label: "Destination", value: "Chosen when you press Start run" },
          ]} />
          <div className="ws-plan-form ws-plan-form--single">
            <Field label="Operator" value={draft.operator.name} onChange={setOperator("name")} issues={operatorIssues("name")} disabled={locked} testId="plan-operator-name"
              hint="Type a name, or pick one remembered below" />
            <Field label="Operator id" value={draft.operator.id} onChange={setOperator("id")} issues={operatorIssues("id")} disabled={locked} testId="plan-operator-id" hint="Optional" />
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
        </Card>
      </div>

      <Card title="Start" subtitle="Start run picks the file, checks readiness with it and starts this plan's run" data-testid="plan-start"
        status={<StateChip tone={verdict.tone} title={verdict.detail || undefined}>{`Prepare: ${verdict.title}`}</StateChip>}
        actions={<>
          {started ? <Button onClick={plan.newPlan}>New plan</Button> : null}
          <Button variant="primary" disabled={blocks.length > 0} onClick={() => { if (build.json !== null) void plan.start(build.json, SINGLE_SAMPLE_RUN_INDEX); }}>
            {starting ? "Starting…" : "Start run"}
          </Button>
        </>}>
        {outcome ? <Notice tone={outcome.tone} title={outcome.title} data-testid="plan-outcome">{outcome.text}</Notice> : null}
        {refresh ? <Notice tone={refresh.tone} title={refresh.title} data-testid="plan-refresh">{refresh.text}</Notice> : null}
        {blocks.map((b) => (
          <Notice key={b.id} tone={b.tone} title={b.title} data-testid={`plan-blocked-${b.id}`}>{b.text}</Notice>
        ))}
        <p className="ws-note">
          {`Nothing starts on navigation. The request is ${build.bytes.toLocaleString("en-US")} of at most ${B.max_request_bytes.toLocaleString("en-US")} bytes; the backend checks it again and refuses, never truncates.`}
        </p>
      </Card>
    </div>
  );
}
