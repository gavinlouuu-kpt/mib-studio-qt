// O3 Experiment · Plan as a guided sample queue (#716 A5; ADR 0015 items 1, 2): N sheet rows, one
// run and one file each, under the plan's one experiment id, run in sheet order over the existing
// commands. Every step is a button the operator presses:
//
//   Start S1 → (running) Next sample → stop → drain/finalize (the status, with progress) →
//   Confirm closed → "Swap the tube for S2" → Start S2 (readiness with S2's file at Start) → …
//
// Nothing advances or starts on its own; a failure stops the flow at its step with the reason and
// the operator's choice (retry, skip, end); a lost controlling client pauses the flow until the
// operator resumes it. queueModel.ts is the state machine, usePlan the controller. Rows can be
// added at any time and reordered or removed only before they have started.
import { useEffect, useState, type ReactNode } from "react";
import { Button, Card, ConditionSwatch, DataTable, FactList, Notice, StateChip, Stepper, type DataTableColumn } from "@mib/ui";
import { experimentCounterRows } from "../experimentCounters";
import {
  CALIBRATION_REFUSAL_TITLE,
  KEY_LABELS,
  buildStartRequest,
  concentrationText,
  conditionSlots,
  emptyRow,
  issuesFor,
  nextSampleRef,
  type SampleRow,
} from "./planModel";
import { CONTROL_BLOCK, calibrationFacts, declaredCalibration, FormatCard, NOT_DECLARED, OperatorFields, CameraModeNotice, SampleEditor, StartBlockText, startBlocks, type StartBlock } from "./planParts";
import type { PlanViewProps } from "./PlanView";
import { startConfirmation } from "./prepareModel";
import {
  canEnd,
  canMove,
  canRemove,
  currentIndex,
  lifecycle,
  notStarted,
  ownRunActive,
  plannedRunIndex,
  queueBegun,
  queueInProgress,
  rowChip,
  rowEditable,
  rowState,
  type QueueRow,
} from "./queueModel";

const basename = (path: string) => path.split(/[\\/]/).pop() || path;
const describe = (r: SampleRow) => {
  const what = r.sample.trim() || r.condition.trim();
  return what ? `${r.ref} · ${what}` : r.ref;
};
const rowFile = (st: QueueRow) => st.end?.outputPath || st.outputPath || st.attempts[st.attempts.length - 1]?.path || "";

type NoticeText = { tone: "confirmed" | "warn" | "blocked" | "unknown" | "info"; title: string; text: string };

export function QueuePlan({ plan, lockedReason, verdict, blockers, cameraMode, onCameraMode, confirmation, calibration }: PlanViewProps) {
  const { draft, queue } = plan;
  const q = queue.state;
  const order = draft.rows.map((r) => r.ref);
  const cur = currentIndex(order, q);
  const curRow = cur >= 0 ? draft.rows[cur] : null;
  const curState = curRow ? rowState(q, curRow.ref) : null;
  const begun = queueBegun(q);
  const inProgress = queueInProgress(order, q);
  const pending = queue.pending;
  const paused = q.paused !== null;
  const actionsLocked = paused || !queue.controlHeld || pending !== null;

  const [selected, setSelected] = useState<string | null>(null);
  const selRef = selected !== null && order.includes(selected) ? selected : curRow?.ref ?? order[order.length - 1];
  const selIndex = order.indexOf(selRef);
  const selRow = draft.rows[selIndex];
  const selEditable = rowEditable(q, selRef) && pending !== "start";
  const selBuild = buildStartRequest(draft, selIndex, plannedRunIndex(order, q, selIndex) ?? q.nextRunIndex, declaredCalibration(calibration));
  // The calibration record shown as in effect is the one the next Start declares (#716 A4b).
  const declared = declaredCalibration(calibration);
  // A Start refused for a calibration mismatch reads the calibration again at once, as it says.
  const calibrationRefused = curState?.failure?.title === CALIBRATION_REFUSAL_TITLE ? curState.failure : null;
  const refreshCalibration = calibration?.refresh;
  useEffect(() => { if (calibrationRefused) refreshCalibration?.(); }, [calibrationRefused]);
  const curBuild = curRow ? buildStartRequest(draft, cur, q.nextRunIndex, declared) : null;
  const slots = conditionSlots(draft.rows);
  const confirm = confirmation ? startConfirmation(confirmation) : null;

  const editRow = (ref: string) => (change: (r: SampleRow) => SampleRow) => plan.edit((d) => ({ ...d, rows: d.rows.map((r) => (r.ref === ref ? change(r) : r)) }));
  const addRow = () => {
    const ref = nextSampleRef(draft.rows);
    plan.edit((d) => ({ ...d, rows: [...d.rows, emptyRow(ref)] }));
    setSelected(ref);
  };
  const move = (i: number, by: -1 | 1) => plan.edit((d) => {
    const rows = [...d.rows];
    [rows[i], rows[i + by]] = [rows[i + by], rows[i]];
    return { ...d, rows };
  });
  const remove = (i: number) => plan.edit((d) => ({ ...d, rows: d.rows.filter((_, j) => j !== i) }));

  const columns: DataTableColumn<SampleRow>[] = [
    { key: "run", header: "Run", numeric: true, width: "56px", cell: (r) => {
      const st = rowState(q, r.ref);
      const planned = plannedRunIndex(order, q, order.indexOf(r.ref));
      if (st.attempts.length) return [
        ...st.attempts.map((a) => (
          <span key={a.runIndex} title={a.outcome === "started" ? "Started" : a.code !== undefined ? "Not started, but its file may exist; its index is not used again" : "Reply lost: this run may exist; its index is not used again"}>{a.outcome === "unknown" ? `${a.runIndex}?` : a.runIndex}</span>
        )),
        // A repeated row (#716 A7b): its next run, planned.
        ...(st.phase === "planned" && planned !== null ? [<span key="planned" className="ws-detail" title="Repeat planned: the run index is assigned at Start">{planned}</span>] : []),
      ].reduce<ReactNode[]>((acc, el, i) => (i ? [...acc, ", ", el] : [el]), []);
      return planned === null ? "" : <span className="ws-detail" title="Planned: the run index is assigned at Start">{planned}</span>;
    } },
    { key: "ref", header: "Ref", emphasis: true, width: "48px" },
    { key: "sample", header: "Sample", cell: (r) => r.sample.trim() },
    { key: "condition", header: "Condition", cell: (r) => {
      const c = r.condition.trim(), slot = slots.get(c);
      return c ? (slot ? <ConditionSwatch condition={slot}>{c}</ConditionSwatch> : c) : "";
    } },
    { key: "unit", header: "Unit", cell: (r) => r.experimentalUnit.trim() },
    { key: "conc", header: "Conc.", cell: concentrationText },
    { key: "state", header: "State", width: "112px", cell: (r) => {
      const st = rowState(q, r.ref), chip = rowChip(st, q.ended);
      const i = order.indexOf(r.ref);
      const issues = rowEditable(q, r.ref) ? buildStartRequest(draft, i, plannedRunIndex(order, q, i) ?? q.nextRunIndex).issues.length : 0;
      return <span className="ws-queue-state" data-testid={`queue-state-${r.ref}`}>
        <StateChip size="small" tone={chip.tone}>{chip.text}</StateChip>
        {issues ? <StateChip size="small" tone="blocked">{issues === 1 ? "1 issue" : `${issues} issues`}</StateChip> : null}
      </span>;
    } },
    { key: "file", header: "File", cell: (r) => {
      const path = rowFile(rowState(q, r.ref));
      return path ? <span className="ws-queue-file" title={path} data-testid={`queue-file-${r.ref}`}>{basename(path)}</span> : "";
    } },
    { key: "order", header: <span className="mib-visually-hidden">Order</span>, width: "128px", cell: (r) => {
      const i = order.indexOf(r.ref);
      if (!rowEditable(q, r.ref)) return <span className="ws-detail" title="Started rows keep their place">locked</span>;
      return <span className="ws-queue-order">
        <Button size="small" aria-label={`Move ${r.ref} up`} disabled={pending === "start" || !canMove(order, q, i, -1)} onClick={() => move(i, -1)}>↑</Button>
        <Button size="small" aria-label={`Move ${r.ref} down`} disabled={pending === "start" || !canMove(order, q, i, 1)} onClick={() => move(i, 1)}>↓</Button>
        <Button size="small" aria-label={`Remove ${r.ref}`} disabled={pending === "start" || !canRemove(order, q, i)} onClick={() => remove(i)}>Remove</Button>
      </span>;
    } },
  ];

  // ---- the current step: its notice, its blocks and its buttons ----
  const prev = cur > 0 ? draft.rows[cur - 1] : null;
  const prevState = prev ? rowState(q, prev.ref) : null;
  const lastRun = curState?.attempts[curState.attempts.length - 1];
  const runStatus = queue.status?.valid && lastRun?.generation && queue.status.start_generation === lastRun.generation ? queue.status : null;
  const remaining = cur < 0 ? 0 : order.slice(cur + 1).length;
  const startable = !!curState && ["planned", "refused", "unknown", "failed", "interrupted"].includes(curState.phase);
  // Next sample sends experiment_stop, which stops whatever run is active: only once the status
  // shows this row's own run (its file, its generation) active.
  const ownActive = !!curState && ownRunActive(curState, queue.status);

  let notice: NoticeText | null = null;
  if (curRow && curState) {
    const file = rowFile(curState);
    switch (curState.phase) {
      case "planned": {
        // Repeat sample (#716 A7b): the row's earlier run is kept; its Start sends the same ref with the next run index.
        const kept = curState.history?.length ? curState.history[curState.history.length - 1] : null;
        if (kept) {
          notice = { tone: "info", title: `Repeat ${describe(curRow)} · load the tube again`,
            text: `Run ${kept.runIndex}'s file is kept (${kept.path}). Start ${curRow.ref} picks a new file, checks readiness with it and starts run ${q.nextRunIndex} with the same sample ref.` };
          break;
        }
        notice = prev && prevState && begun
          ? { tone: "info", title: `Swap the tube for ${describe(curRow)}`, text: `${prev.ref} ${prevState.phase === "closed" ? `is closed${rowFile(prevState) ? ` (${rowFile(prevState)})` : ""}` : "was skipped"}. Load the next tube, then press Start ${curRow.ref}: it picks the file, checks readiness with it and starts run ${q.nextRunIndex}.` }
          : { tone: "info", title: `Ready for ${describe(curRow)}`, text: `Start ${curRow.ref} picks the file, checks readiness with it and starts run ${q.nextRunIndex} of experiment ${draft.experimentId.slice(0, 8)}…` };
        break;
      }
      case "starting":
        notice = { tone: "info", title: `Starting ${curRow.ref} · run ${q.nextRunIndex}`, text: "Waiting for the file dialog, readiness with the chosen file and the backend's answer." };
        break;
      case "running":
        notice = curState.failure
          ? { tone: curState.failure.tone, title: curState.failure.title, text: curState.failure.text }
          : lastRun?.generation === null
          ? { tone: "info", title: `${curRow.ref} started · waiting for the status to report its run`,
            text: `${remaining ? "Next sample" : "Stop last sample"} is available once the status shows this run on ${curRow.ref}'s file (${file || "the chosen file"}).` }
          : { tone: "confirmed", title: `${curRow.ref} running · run ${lastRun?.runIndex ?? "?"}${file ? ` · ${file}` : ""}`,
            text: `${remaining ? "Next sample" : "Stop last sample"} stops this run, waits for the drain and finalize, then asks you to confirm the close. Nothing stops on its own here.` };
        break;
      case "stopping":
        notice = { tone: "info", title: `Stopping ${curRow.ref} · draining and finalizing`, text: pending === "stop" ? "The stop request is on its way." : `The backend is writing the last records and closing ${file || "the file"}. Wait for Finalized.` };
        break;
      case "finalized":
        notice = { tone: curState.end?.severity && !["ok", "partial"].includes(curState.end.severity) ? "warn" : "confirmed",
          title: `${curRow.ref} finalized · the backend closed ${curState.end?.outputPath || "the file"}`,
          text: `${curState.end?.cancelled ? "The run was cancelled. " : ""}${curState.end?.headline ? `${curState.end.headline} ` : ""}Confirm the close to go on; nothing else happens until then.` };
        break;
      case "refused": case "unknown": case "failed": case "interrupted":
        if (curState.failure) notice = { tone: curState.failure.tone, title: curState.failure.title,
          text: `${curState.failure.text}${curState.phase === "failed" && file ? ` The file is kept: ${file}.` : ""} ${choiceText(curState.phase, curRow.ref, q.nextRunIndex, notStarted(curState))}` };
        break;
      default:
        break;
    }
  }
  const complete = !inProgress && begun && cur < 0;
  const closedCount = order.filter((r) => rowState(q, r).phase === "closed").length;
  const skippedCount = order.filter((r) => rowState(q, r).phase === "skipped").length;
  const notRun = order.filter((r) => rowState(q, r).phase === "planned" || rowState(q, r).phase === "refused");
  const finished: NoticeText | null = !complete ? null : q.ended
    ? { tone: "info", title: `Queue ended · ${closedCount} closed${skippedCount ? `, ${skippedCount} skipped` : ""}`, text: `${notRun.length ? `${notRun.join(", ")} not run. ` : ""}${order.some((r) => ["running", "stopping"].includes(rowState(q, r).phase)) ? "A run is still active: stop it in the classic view. " : ""}New plan mints a new experiment id.` }
    : { tone: "confirmed", title: `Queue complete · ${closedCount} of ${order.length} closed${skippedCount ? `, ${skippedCount} skipped` : ""}`, text: "Every sample has its own file under this experiment id. Add a sample to go on, or start a new plan." };

  const blocks: StartBlock[] = startable && curBuild ? queueStartBlocks() : [];
  function queueStartBlocks(): StartBlock[] {
    const out = startBlocks({ started: null, starting: pending === "start", verdict, blockers, lockedReason, issues: curBuild!.issues, cameraReason: cameraMode?.startReason })
      .map((b) => (b.id === "sheet" ? { ...b, title: `Start is unavailable · ${curRow!.ref} has ${curBuild!.issues.length === 1 ? "1 issue" : `${curBuild!.issues.length} issues`}` } : b));
    if (!queue.controlHeld) out.push(CONTROL_BLOCK);
    if (!queue.status?.valid) out.push({ id: "status", tone: "unknown", title: "Start is unavailable · run status not read yet", text: "The queue follows each run in the experiment status, which is read every half second." });
    return out;
  }

  const primary = (() => {
    if (!curRow || !curState) return finished ? { label: "New plan", onClick: plan.newPlan, disabled: pending !== null, testId: "queue-new-plan" } : null;
    switch (curState.phase) {
      case "planned": case "refused":
        return { label: pending === "start" ? "Starting…" : `Start ${curRow.ref}`, onClick: () => void queue.startCurrent(declared), disabled: actionsLocked || blocks.length > 0, testId: "queue-start" };
      case "unknown": case "failed": case "interrupted":
        return { label: `Retry ${curRow.ref} as run ${q.nextRunIndex}`, onClick: () => void queue.startCurrent(declared), disabled: actionsLocked || blocks.length > 0, testId: "queue-retry" };
      case "starting":
        return { label: "Starting…", onClick: () => {}, disabled: true, testId: "queue-start" };
      case "running":
        return { label: remaining ? "Next sample" : "Stop last sample", onClick: () => void queue.stopCurrent(), disabled: actionsLocked || !ownActive, testId: "queue-next",
          title: ownActive ? `Stops ${curRow.ref}'s run (stop → drain → finalize), then asks you to confirm the close` : `Waiting for the status to show ${curRow.ref}'s run active` };
      case "stopping":
        return { label: "Stopping…", onClick: () => {}, disabled: true, testId: "queue-next" };
      case "finalized":
        return { label: "Confirm closed", onClick: queue.confirmClosed, disabled: actionsLocked, testId: "queue-confirm" };
      default:
        return null;
    }
  })();
  const showSkip = !!curState && ["refused", "unknown", "failed", "interrupted"].includes(curState.phase);
  const showEnd = canEnd(order, q) && begun;

  const steps = curState ? lifecycle(curState).map((s) => ({
    state: s.state,
    label: <span className="ws-queue-step">{s.label}{s.flag ? <StateChip size="small" tone={s.flag.tone}>{s.flag.text}</StateChip> : null}</span>,
  })) : [];

  // The queue card: below the sheet while planning, above it once the queue has begun, so the
  // step to press (Next sample, Confirm closed, Start S2) is in view during a run.
  const queueCard = (
    <Card title="Queue" data-testid="queue-card"
      subtitle="Guided: every step is a button you press. Nothing advances, stops or starts on its own."
      status={<>
        <StateChip tone={verdict.tone} title={verdict.detail || undefined}>{`Prepare: ${verdict.title}`}</StateChip>
        {confirm ? <StateChip tone={confirm.tone} data-testid="plan-confirmation">{confirm.chip}</StateChip> : null}
        <StateChip tone={paused ? "warn" : "neutral"} data-testid="queue-progress">{paused ? "Paused" : curRow ? `${curRow.ref} · ${cur + 1} of ${order.length}` : begun ? "Finished" : "Not started"}</StateChip>
      </>}
      actions={<>
        {showSkip ? <Button disabled={actionsLocked} onClick={queue.skip} data-testid="queue-skip">{`Skip ${curRow!.ref}`}</Button> : null}
        {showEnd ? <Button disabled={actionsLocked} onClick={queue.end} data-testid="queue-end">End queue</Button> : null}
        {primary ? <Button variant="primary" disabled={primary.disabled} title={"title" in primary ? primary.title : undefined} onClick={primary.onClick} data-testid={primary.testId}>{primary.label}</Button> : null}
      </>}>
      <CameraModeNotice mode={cameraMode} onSwitch={onCameraMode} />
      {paused ? (
        <Notice tone="warn" title="Queue paused · control lost" data-testid="queue-paused"
          actions={<Button disabled={!queue.controlHeld} title={queue.controlHeld ? undefined : "Take control of the instrument first"} onClick={queue.resume} data-testid="queue-resume">Resume queue</Button>}>
          {`Paused because ${q.paused}. It never resumes on its own and sends nothing while paused; with no client left the instrument's server stops and saves an active run (#711). Resuming sends nothing: you press each step again.`}
        </Notice>
      ) : null}
      <div className="ws-queue-body">
        {curRow && curState ? (
          <div className="ws-queue-lifecycle">
            <span className="ws-plan-label">{`Run lifecycle · ${describe(curRow)}${lastRun ? ` · run ${lastRun.runIndex}` : ""}`}</span>
            <Stepper steps={steps} data-testid="queue-stepper" />
          </div>
        ) : null}
        <div className="ws-queue-now">
          {notice ? <Notice tone={notice.tone} title={notice.title} data-testid="queue-step">{notice.text}</Notice> : null}
          {runStatus && curState && (curState.phase === "running" || curState.phase === "stopping") ? (
            <FactList data-testid="queue-progress-facts" facts={[
              ...experimentCounterRows(runStatus).map(([label, value]) => ({ label: label.replace(/:$/, ""), value })),
              { label: "Committed / admitted", value: `${runStatus.persistence_committed} / ${runStatus.persistence_admitted}${runStatus.flushing ? " · finalizing" : ""}` },
            ]} />
          ) : null}
          {finished ? <Notice tone={finished.tone} title={finished.title} data-testid="queue-finished">{finished.text}</Notice> : null}
          {blocks.map((b) => (
            <Notice key={b.id} tone={b.tone} title={b.title} data-testid={`plan-blocked-${b.id}`}><StartBlockText block={b} /></Notice>
          ))}
        </div>
      </div>
      {confirm ? <p className="ws-note" data-testid="plan-confirmation-note">{confirm.note}</p> : null}
      <p className="ws-note">{`Each Start sends this experiment id, the row's ref as sample_ref and the next run index; a sample change is a new run with its own file. ${KEY_LABELS.startRequest} is checked again by the backend.`}</p>
    </Card>
  );

  return (
    <div className="ws-body" data-testid="workspace-plan">
      {begun ? queueCard : null}
      <div className="ws-plan ws-plan--queue">
        <FormatCard draft={draft} locked={begun || q.ended || pending !== null} onFormat={(format) => plan.edit((d) => ({ ...d, format }))} />

        <Card title="Sample sheet" className="ws-plan__sheet" data-testid="plan-sheet"
          subtitle="One row per sample, one run and one file each · the fixed schema, written once, as planned, into each run's file at its Start">
          <DataTable columns={columns} rows={draft.rows} rowKey={(r) => r.ref} selectedKey={selRef} onSelect={(key) => setSelected(key)} density="compact" missing={NOT_DECLARED}
            caption="Run order · – = not declared or not known yet · select a row to edit it" data-testid="plan-sheet-table" />
          <div className="ws-queue-sheet-actions">
            <Button size="small" onClick={addRow} disabled={q.ended || pending === "start"} data-testid="queue-add">+ Sample</Button>
            <span className="ws-detail">{`${draft.rows.length} ${draft.rows.length === 1 ? "sample" : "samples"} · reorder or remove a row only before it has started.`}</span>
          </div>
          <h4 className="ws-queue-editing" data-testid="queue-editing">{`Editing ${selRow.ref}${selEditable ? "" : " · locked"}`}</h4>
          {!selEditable && rowState(q, selRef).attempts.length ? (
            <p className="ws-note" data-testid="queue-locked-note">{`${selRow.ref} is locked: its run started (run ${rowState(q, selRef).attempts.map((a) => a.runIndex).join(", ")}), so its identity and annotation are recorded in that file.`}</p>
          ) : null}
          <SampleEditor row={selRow} issues={selEditable ? selBuild.issues : []} disabled={!selEditable} onRow={editRow(selRow.ref)} />
          <p className="ws-note">
            Leave a field empty when it is not declared: it is recorded as not declared, never as an empty value. Limits are bytes of UTF-8; a longer
            value is refused, never cut off.
          </p>
        </Card>

        <Card title="Run identity" subtitle="Recorded in each run's snapshot (v3)" className="ws-plan__identity" data-testid="plan-identity">
          <FactList facts={[
            { label: "Experiment", value: <span className="ws-plan-id" data-testid="plan-experiment-id">{draft.experimentId}</span> },
            { label: "Runs", value: `${draft.rows.length} ${draft.rows.length === 1 ? "sample" : "samples"} · one run and one file each` },
            { label: "Next run index", value: <span data-testid="queue-next-run-index">{q.nextRunIndex}</span> },
            { label: "Run index rule", value: "Assigned at each Start; a started or unknown run's index is never sent again" },
            ...calibrationFacts(calibration),
            { label: "Destination", value: "Chosen at each Start" },
          ]} />
          <OperatorFields plan={plan} issues={(k) => (begun ? [] : issuesFor(curBuild?.issues ?? [], [`operator.${k}`]))} locked={begun || q.ended || pending !== null} />
        </Card>
      </div>

      {begun ? null : queueCard}
    </div>
  );
}

/** What the operator can do at a failure, in plain words. */
function choiceText(phase: "refused" | "unknown" | "failed" | "interrupted", ref: string, nextRunIndex: number, notStartedRun = false): string {
  if (phase === "unknown" && notStartedRun) return `Check whether the backend left the run's file on disk, then retry ${ref} as run ${nextRunIndex}, skip it, or end the queue.`;
  switch (phase) {
    case "interrupted": return `Retry ${ref} as run ${nextRunIndex} once the other run is over, skip ${ref}, or end the queue.`;
    case "refused": return `Correct ${ref} if needed and press Start ${ref} again, skip it, or end the queue.`;
    case "unknown": return `If a run is active it may be this sample's: stop it in the classic view before a retry. Retry as run ${nextRunIndex}, skip ${ref}, or end the queue.`;
    case "failed": return `Retry ${ref} as run ${nextRunIndex} (a new file), skip it, or end the queue. A fault may need acknowledging in the classic view before the next Start.`;
  }
}
