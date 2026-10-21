// O3 Experiment · Plan (#716 A5; ADR 0015 items 1, 2, 4, 5): the sample format, the sample sheet,
// the run's identity and the selected operator. A single sample has an explicit "Start run"; a
// sample queue is the guided flow of QueuePlan. Editing the plan sends nothing; "Start run" runs the
// classic Start flow (pick the file, ask before a stopped Run's frames are discarded, readiness with
// the chosen file, start) with the plan's startRequestJson (bridge ABI 39); the reply's start outcome
// code decides what it means (ABI 40, experimentStart.ts startReplyState). Prepare's verdict still
// governs: while Prepare's checklist blocks, Start is disabled with Prepare's own words
// (prepareVerdict, confirmBlockers). On the PZ7035 the Plan offers the explicit switch to Run.
import { Button, Card, ConditionSwatch, DataTable, FactList, Notice, StateChip, type DataTableColumn } from "@mib/ui";
import { EXPERIMENT_START_REQUEST as B } from "../bridgeContract";
import {
  SINGLE_SAMPLE_RUN_INDEX,
  buildStartRequest,
  conditionSlots,
  concentrationText,
  invalidRequestReason,
  issuesFor,
  type PlanCameraMode,
  type SampleRow,
} from "./planModel";
import { FormatCard, NOT_DECLARED, OperatorFields, CameraModeNotice, SampleEditor, StartBlockText, startBlocks } from "./planParts";
import { QueuePlan } from "./QueuePlan";
import { startConfirmation, type ConfirmationState, type Verdict } from "./prepareModel";
import type { PlanController, PlanStartResult } from "./usePlan";

export interface PlanViewProps {
  plan: PlanController;
  /** Why Start is unavailable apart from the plan and Prepare (the classic Start button's reasons:
   *  backend, camera or Run mode, a run is active, a start pending); "" when it is available. */
  lockedReason: string;
  /** Prepare's one verdict, and why its checklist blocks (confirmBlockers), null when it does not. */
  verdict: Verdict;
  blockers: { tone: "blocked" | "unknown" | "warn"; lines: string[] } | null;
  /** The explicit switch to the mode a run records in: the PZ7035's Run, a desktop camera's
   *  experiment window after the full sensor (null when none is needed). */
  cameraMode?: PlanCameraMode | null;
  onCameraMode?: () => Promise<unknown>;
  /** Prepare's Preflight confirmation for this setup, shown beside Start while it is not given
   *  (advisory: kept per browser tab, Start does not require it; #763 F23). Absent: not shown. */
  confirmation?: ConfirmationState;
  /** Show Experiment · Live (navigation only), offered with a started run's outcome. */
  onShowLive?: () => void;
}

export { startBlocks, type StartBlock } from "./planParts";

/** `live`: the outcome points to Live, where the run is followed and stopped (#763 F12). */
type NoticeText = { tone: "confirmed" | "warn" | "blocked" | "info"; title: string; text: string; live?: boolean };

/** The Notice for the last Start's result, decided by the start reply alone; the InvalidRequest
 *  refusal is shown by the sheet. */
export function resultNotice(r: PlanStartResult | null): NoticeText | null {
  if (!r) return null;
  switch (r.kind) {
    case "started": return { tone: "confirmed", title: `Run ${SINGLE_SAMPLE_RUN_INDEX} started · ${r.path}`, text: `Its identity and the planned annotation are recorded in the file.${r.notice ? ` ${r.notice}` : ""} Follow it in Experiment · Live, and stop it there with Stop & finalize run.`, live: true };
    case "unknown": return r.code !== undefined
      ? { tone: "blocked", title: `Not started · its file may exist${r.path ? ` · ${r.path}` : ""}`, text: `The backend did not start the run (${r.message}), but it can answer this way after it opened the run's file. This plan's identity is not sent again: check the file, then make a new plan.` }
      : { tone: "blocked", title: `Start outcome not known${r.path ? ` · ${r.path}` : ""}`, text: `The start request was sent but no reply was read (${r.message}). The run may have started: Experiment · Live shows the run the status reports. This plan's identity is not sent again.`, live: true };
    case "not-ready": return { tone: "warn", title: "Not started · readiness with the chosen file", text: r.message };
    case "refused": return invalidRequestReason(r.message, r.code) ? null : { tone: "blocked", title: "Not started · the backend refused the Start", text: `${r.path}: ${r.message}` };
    case "error": return { tone: "blocked", title: "Not started · an error before the start was sent", text: r.message };
    case "busy": return { tone: "info", title: "Not started · a start request is already pending", text: "Wait for it to finish." };
    case "withdrawn": return { tone: "warn", title: "Not started · control was lost while the Start was pending", text: "Nothing was sent. Press Start run again." };
    default: return null;
  }
}

/** A failed status read after the start reply, said apart: it never changes the outcome. */
export function refreshNotice(r: PlanStartResult | null): NoticeText | null {
  if (!r || !("refreshError" in r) || !r.refreshError) return null;
  return { tone: "warn", title: "Run status not refreshed", text: `${r.refreshError}. The start reply decides the outcome above; the status is read again on the next poll.` };
}

export function PlanView(props: PlanViewProps) {
  return props.plan.draft.format === "queue" ? <QueuePlan {...props} /> : <SinglePlan {...props} />;
}

function SinglePlan({ plan, lockedReason, verdict, blockers, cameraMode, onCameraMode, confirmation, onShowLive }: PlanViewProps) {
  const { draft, started, starting } = plan;
  const row = draft.rows[0];
  const build = buildStartRequest(draft, 0, SINGLE_SAMPLE_RUN_INDEX);
  const locked = !!started || starting;
  const blocks = startBlocks({ started: started?.outcome ?? null, notStarted: started?.code !== undefined, starting, verdict, blockers, lockedReason, issues: build.issues, cameraReason: cameraMode?.startReason, controlHeld: plan.queue.controlHeld });
  const refusal = plan.result?.kind === "refused" ? invalidRequestReason(plan.result.message, plan.result.code) : null;
  const outcome = resultNotice(plan.result);
  const refresh = refreshNotice(plan.result);
  const confirm = confirmation ? startConfirmation(confirmation) : null;
  const slots = conditionSlots(draft.rows);
  const editRow = (change: (r: SampleRow) => SampleRow) => plan.edit((d) => ({ ...d, rows: d.rows.map((r, i) => (i === 0 ? change(r) : r)) }));
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
        <FormatCard draft={draft} locked={locked} onFormat={(format) => plan.edit((d) => ({ ...d, format }))} />

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
          <OperatorFields plan={plan} issues={operatorIssues} locked={locked} />
        </Card>
      </div>

      <Card title="Start" subtitle="Start run picks the file, checks readiness with it and starts this plan's run" data-testid="plan-start"
        status={<>
          <StateChip tone={verdict.tone} title={verdict.detail || undefined}>{`Prepare: ${verdict.title}`}</StateChip>
          {confirm ? <StateChip tone={confirm.tone} data-testid="plan-confirmation">{confirm.chip}</StateChip> : null}
        </>}
        actions={<>
          {started ? <Button onClick={plan.newPlan}>New plan</Button> : null}
          <Button variant="primary" disabled={blocks.length > 0} onClick={() => { if (build.json !== null) void plan.start(build.json, SINGLE_SAMPLE_RUN_INDEX); }}>
            {starting ? "Starting…" : "Start run"}
          </Button>
        </>}>
        <CameraModeNotice mode={cameraMode} onSwitch={onCameraMode} />
        {outcome ? <Notice tone={outcome.tone} title={outcome.title} data-testid="plan-outcome"
          actions={outcome.live && onShowLive ? <Button size="small" onClick={onShowLive} data-testid="plan-to-live">Go to Live</Button> : undefined}>{outcome.text}</Notice> : null}
        {refresh ? <Notice tone={refresh.tone} title={refresh.title} data-testid="plan-refresh">{refresh.text}</Notice> : null}
        {blocks.map((b) => (
          <Notice key={b.id} tone={b.tone} title={b.title} data-testid={`plan-blocked-${b.id}`}><StartBlockText block={b} /></Notice>
        ))}
        {confirm ? <p className="ws-note" data-testid="plan-confirmation-note">{confirm.note}</p> : null}
        <p className="ws-note">
          {`Nothing starts on navigation. The request is ${build.bytes.toLocaleString("en-US")} of at most ${B.max_request_bytes.toLocaleString("en-US")} bytes; the backend checks it again and refuses, never truncates.`}
        </p>
      </Card>
    </div>
  );
}
