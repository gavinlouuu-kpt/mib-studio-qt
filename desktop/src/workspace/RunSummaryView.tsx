// O5 Experiment · Run summary (#716 A7b, aligned with its Penpot board in A7c): the hand-off after a
// run closed. One closed run of the plan (the newest by default, or the one chosen here or in Quick
// look): its Outcome, Counts, Provenance written, an Area–Deformability snapshot and the Next card.
// The medians and deltas are in Quick look (O6), not here.
//
// - Outcome: a lifecycle checklist from what this page observed (runSummaryModel outcomeChecklist:
//   the start reply, the run's own statuses, the terminal status, the operator's close confirmation,
//   or a single sample's operation end); a step it did not observe reads unknown, never ticked. The
//   stop reason as observed, the duration from the terminal status's recorded start and end ("Not
//   recorded" otherwise), the file, the experiment and run, and the writer.
// - Counts and provenance: the file's, from the run quick look (bridge ABI 44 `run_quick_look`),
//   shown as sent (quickLookModel formats; nothing here computes on them).
// - Snapshot: the run's display-only subsample from its quick look; the Review app does the analysis.
// - Next: Next sample (with the next sample's name), Repeat this sample, End queue (summaryParts
//   decisionsFor), and for a calibration run the record the backend derived from it (matched by its
//   source run_id in `calibration_records_list`), with Apply and Dismiss leading to Prepare's
//   Calibration card: nothing is ever applied from here.
//
// Reads, never polled: `run_quick_look` for the shown run on showing, when the shown run changes,
// once idle for a deferral, and on Refresh; `calibration_records_list` for a run that may be a
// calibration run, on showing, when the run changes, and on Refresh.
import { useMemo, useState, type ReactNode } from "react";
import { AreaDeformChart, Button, Card, FactList, Notice, StatTile, StateChip, Toolbar, type AreaDeformPoint, type ConditionSlot } from "@mib/ui";
import { counterText, countFacts, NOT_RECORDED, NUMBERS_AT_CLOSE, planClosedRuns, provenanceFacts, runFor, statusChip, type PlanRun } from "./quickLookModel";
import type { ExperimentSubView } from "./ExperimentView";
import { applyAvailability, factorChange, factorText, recordFactor, statusText, statusTone } from "./calibrationModel";
import { chartDomain, runInProgress } from "./liveModel";
import { conditionSlots } from "./planModel";
import {
  cvText,
  DURATION_SOURCE,
  durationText,
  gateLines,
  noRecordReason,
  outcomeChecklist,
  recordForRun,
  sampleName,
  STOP_REASON_UNKNOWN,
  stopReason,
  writerText,
  type CheckStep,
} from "./runSummaryModel";
import { basename, Chips, ControlNotice, DecisionReasons, decisionsFor, NoNumbers, ReadNotices, runName, what, type ReviewAction } from "./summaryParts";
import type { PlanController } from "./usePlan";
import { useQuickLook, type QuickLookRead } from "./useQuickLook";
import { useRunCalibration, type CalibrationListRead } from "./useRunCalibration";
import type { CalibrationRecordsList } from "../calibrationRecords";
import type { RunQuickLookRun } from "../runQuickLook";
import { formatByteCount } from "../ssdView";

export interface RunSummaryViewProps {
  /** The Experiment sub-view buttons (Plan · Live · Run summary · Quick look), first in the toolbar. */
  tabs: ReactNode;
  plan: PlanController;
  /** A run is starting, active or stopping (App's expActive): the transition to idle re-reads a deferral. */
  experimentActive: boolean;
  onView: (view: ExperimentSubView) => void;
  /** The run shown (its path; null: the newest closed run), held by the Experiment stage with Quick look. */
  inspected: string | null;
  onInspect: (path: string | null) => void;
  /** Prepare's Calibration card, with this record selected (navigation and a read; nothing applied). */
  onPrepare: (recordId: string | null) => void;
  /** Open in Review is Quick look's; kept here for the decisions' shared rules. */
  review: ReviewAction;
  read?: QuickLookRead;
  readCalibration?: CalibrationListRead;
}

const SNAPSHOT_W = 600, SNAPSHOT_H = 260;
const NO_REFERENCE = null;

export function RunSummaryView({ tabs, plan, experimentActive, onView, inspected, onInspect, onPrepare, review, read, readCalibration }: RunSummaryViewProps) {
  const runs = useMemo(() => planClosedRuns({ draft: plan.draft, queue: plan.queue.state, started: plan.started }),
    [plan.draft, plan.queue.state, plan.started]);
  const sel = runs.find((r) => r.path === inspected) ?? runs[runs.length - 1] ?? null;
  const paths = useMemo(() => (sel ? [sel.path] : []), [sel?.path]); // one path: a new array only when the run changes
  const quick = useQuickLook(paths, NO_REFERENCE, experimentActive, read);
  const q = sel ? runFor(quick.reply, sel.path) : null;
  // A run of a measurement method derives no record: the list is read only for the others.
  const calKey = sel && sel.methodPurpose !== "measurement" ? sel.path : null;
  const cal = useRunCalibration(calKey, readCalibration);
  const d = decisionsFor(sel, plan, review, onView);
  const active = runInProgress(plan.queue.status);
  const refresh = () => { quick.refresh(); if (calKey) cal.refresh(); };

  return (
    <div className="ws-view" data-testid="workspace-run-summary">
      <Toolbar title="Run summary">
        {tabs}
        <Button variant="primary" disabled={d.next.reason !== ""} title={d.next.reason || d.next.title} onClick={d.next.act} data-testid="o5-next-sample">Next sample</Button>
        <Button disabled={d.end.reason !== ""} title={d.end.reason || d.end.title} onClick={d.end.act} data-testid="o5-end-queue">End queue</Button>
        <Button onClick={refresh} disabled={sel === null || quick.loading} title="Reads the run's quick look (and, for a calibration run, the calibration records) again: a read, nothing is sent to the instrument" data-testid="summary-refresh">
          {quick.loading ? "Reading…" : "Refresh"}
        </Button>
      </Toolbar>
      <div className="ws-body" data-testid="summary-body">
        <ReadNotices error={quick.error} left={0} read={0} />
        {sel === null ? (
          <Notice tone="info" title="No closed run in this plan yet" data-testid="summary-empty"
            actions={<Button onClick={() => onView("Plan")}>Go to Plan</Button>}>
            {active
              ? `The run in progress is summarised once the backend has finalized it and closed its file (${NUMBERS_AT_CLOSE}).`
              : "A run's summary appears here once the backend has finalized it and closed its file. Showing this view reads nothing until then."}
          </Notice>
        ) : (
          <>
            {runs.length > 1 ? (
              <label className="ws-o5__pick">
                <span className="ws-plan-label">Run shown</span>
                <select value={sel.path} onChange={(e) => onInspect(e.target.value)} data-testid="o5-run-select">
                  {runs.map((r) => <option key={r.path} value={r.path}>{`${runName(r)}${what(r) ? ` · ${what(r)}` : ""} · ${basename(r.path)}`}</option>)}
                </select>
              </label>
            ) : null}
            <div className="ws-o5__row ws-o5__row--three">
              <OutcomeCard run={sel} q={q} loading={quick.loading} experimentId={plan.draft.experimentId} />
              <CountsCard q={q} />
              <ProvenanceCard run={sel} q={q} />
            </div>
            <div className="ws-o5__row ws-o5__row--two">
              <SnapshotCard run={sel} q={q} slot={sel.row ? conditionSlots(plan.draft.rows).get(sel.row.condition.trim()) : undefined} />
              <Card title="Next" data-testid="o5-next"
                subtitle="Hand-off = the file and its identities · statistics: Quick look (descriptive) and the Review app">
                <NextSampleLine plan={plan} next={d.next.row} />
                <ControlNotice d={d} />
                <div className="ws-ql__decision-row">
                  <Button variant="primary" disabled={d.next.reason !== ""} title={d.next.reason || d.next.title} onClick={d.next.act} data-testid="decision-next">Next sample</Button>
                  <Button disabled={d.repeat.reason !== ""} title={d.repeat.reason || d.repeat.title} onClick={d.repeat.act} data-testid="decision-repeat">Repeat this sample</Button>
                  <Button disabled={d.end.reason !== ""} title={d.end.reason || d.end.title} onClick={d.end.act} data-testid="decision-end-queue">End queue</Button>
                </div>
                <DecisionReasons d={d} items={[["Next sample", d.next], ["Repeat this sample", d.repeat], ["End queue", d.end]]} />
                <CalibrationBlock run={sel} q={q} list={cal.list} error={cal.error} listRead={calKey !== null && (cal.list !== null || cal.error !== "")}
                  onPrepare={onPrepare} />
              </Card>
            </div>
          </>
        )}
      </div>
    </div>
  );
}

const STEP_MARK: Readonly<Record<CheckStep["state"], string>> = { done: "✓", current: "…", unknown: "?" };
const STEP_WORD: Readonly<Record<CheckStep["state"], string>> = { done: "done", current: "in progress", unknown: "unknown · not observed" };

function OutcomeCard({ run, q, loading, experimentId }: { run: PlanRun; q: RunQuickLookRun | null; loading: boolean; experimentId: string }) {
  const steps = outcomeChecklist(run);
  const reason = stopReason(run);
  const id = q?.status === "computed" ? q.statistics?.identity ?? null : null;
  const chip = statusChip(q, loading);
  // The file's size in decimal units, the exact byte count in the tooltip (display units only).
  const size = formatByteCount(q?.file?.size_bytes);
  const exact = q?.file?.size_bytes ? `${counterText(q.file.size_bytes)} bytes` : undefined;
  const name = what(run);
  return (
    <Card title="Outcome" data-testid="o5-outcome"
      subtitle={`${runName(run)}${name ? ` · ${name}` : ""} · backend-confirmed or observed by this page · nothing here is invented by the UI`}
      status={<>
        <StateChip tone="neutral" data-testid="summary-outcome">{run.outcome.cancelled ? "Finalized · cancelled" : "Finalized · file closed"}</StateChip>
        {run.confirmed ? null : <StateChip tone="info" data-testid="summary-awaiting">Awaiting your close confirmation</StateChip>}
        <StateChip tone={chip.tone} data-testid="summary-status">{chip.text}</StateChip>
      </>}>
      <ol className="ws-o5__checklist" data-testid="o5-lifecycle">
        {steps.map((s) => (
          <li key={s.key} className={`ws-o5__step ws-o5__step--${s.state}`} data-state={s.state} data-testid={`o5-step-${s.key}`} title={s.detail}>
            <span className="ws-o5__mark" aria-hidden="true">{STEP_MARK[s.state]}</span>
            <span className="ws-o5__step-text">
              <span>{s.label}</span>
              <span className="ws-detail">{`${STEP_WORD[s.state]} · ${s.detail}`}</span>
            </span>
          </li>
        ))}
      </ol>
      <StateChip tone="neutral" data-testid="o5-stop-reason" title={reason ? undefined : STOP_REASON_UNKNOWN}>
        {`Stop reason: ${reason ?? "not observed here"}`}
      </StateChip>
      {run.outcome.headline ? <p className="ws-note" data-testid="summary-headline">{run.outcome.headline}</p> : null}
      <FactList missing={NOT_RECORDED} data-testid="o5-outcome-facts" facts={[
        { label: "Duration", value: <span title={DURATION_SOURCE} data-testid="o5-duration">{durationText(run)}</span> },
        { label: "File", value: <span data-testid="o5-file" title={exact}>{`${basename(run.path)}${size ? ` · ${size}` : ""}`}</span> },
        { label: "Experiment / run", value: <span data-testid="o5-experiment-run">{id
          ? `${id.experiment_id ?? NOT_RECORDED} · run ${id.run_index ?? NOT_RECORDED} · sample ${id.sample_ref ?? NOT_RECORDED}`
          : `${experimentId} · run ${run.runIndex} · sample ${run.ref || NOT_RECORDED} (as planned; the file's identity is read with its quick look)`}</span> },
        { label: "Writer", value: <span data-testid="o5-writer">{`${writerText(run)}${q?.status === "computed" && q.statistics?.accounting ? ` · accounting ${q.statistics.accounting.completion}` : ""}`}</span> },
      ]} />
    </Card>
  );
}

function CountsCard({ q }: { q: RunQuickLookRun | null }) {
  const s = q?.status === "computed" ? q.statistics : null;
  return (
    <Card title="Counts" data-testid="o5-counts" subtitle="The file's counts and accounting, read by the backend · shown as sent"
      status={s ? <Chips s={s} q={null} /> : undefined}>
      <NoNumbers q={q} testId="summary" />
      {s ? (
        <div className="ws-o5__counts" data-testid="summary-counts">
          {countFacts(s).map((f) => <StatTile key={f.label} label={f.label} value={f.value} missing={NOT_RECORDED} />)}
        </div>
      ) : !q ? <p className="ws-note">Read with the run's quick look.</p> : null}
    </Card>
  );
}

function ProvenanceCard({ run, q }: { run: PlanRun; q: RunQuickLookRun | null }) {
  const s = q?.status === "computed" ? q.statistics : null;
  const snap = s?.identity.has_snapshot ? `snapshot v${s.identity.snapshot_version ?? "?"}` : s ? "no run snapshot (legacy file)" : "read with the run's quick look";
  return (
    <Card title="Provenance written" data-testid="o5-provenance" subtitle={`The run's identity as its file records it · ${snap}`}>
      {s ? <FactList facts={provenanceFacts(s, run.path)} missing={NOT_RECORDED} data-testid="summary-provenance" />
        : <p className="ws-note">{q ? "Not read: the quick look has no identity for this file." : "Read with the run's quick look."}</p>}
    </Card>
  );
}

function SnapshotCard({ run, q, slot }: { run: PlanRun; q: RunQuickLookRun | null; slot: ConditionSlot | undefined }) {
  const o = q?.status === "computed" ? q.statistics?.overlay ?? null : null;
  const points: AreaDeformPoint[] = [];
  o?.area_um2?.forEach((area, i) => points.push(slot ? { area, deformability: o.deformability[i], condition: slot } : { area, deformability: o.deformability[i] }));
  const domain = chartDomain(points);
  const cond = run.row?.condition.trim();
  return (
    <Card title="Area–Deformability snapshot" data-testid="o5-snapshot"
      subtitle="The run's display-only subsample from its quick look · display only, the Review app does the analysis">
      <div className="ws-live__chart" data-testid="o5-snapshot-chart">
        <AreaDeformChart points={points} width={SNAPSHOT_W} height={SNAPSHOT_H} legend={slot && cond ? [{ condition: slot, label: cond }] : undefined}
          xDomain={domain.x} yDomain={domain.y} xTicks={domain.xTicks} yTicks={domain.yTicks} />
      </div>
      {!o ? <p className="ws-note">No points: read with the run's quick look.</p>
        : !o.area_um2 ? <p className="ws-note">Not drawn: px→µm not recorded (the snapshot is in µm²).</p> : null}
      <p className="ws-note" data-testid="o5-snapshot-note">{`Display only: at most 2000 points at a fixed stride, outside the numbers' parity with Review. The medians are in Quick look; the analysis is the Review app's.`}</p>
    </Card>
  );
}

function NextSampleLine({ plan, next }: { plan: PlanController; next: ReturnType<typeof decisionsFor>["next"]["row"] }) {
  if (plan.draft.format !== "queue") return <p className="ws-note" data-testid="o5-next-name">A single-sample plan: a new plan runs the next sample. Nothing starts until you press Start in the Plan.</p>;
  if (!next) return <p className="ws-note" data-testid="o5-next-name">No sample is left to run in this plan.</p>;
  return <>
    <p className="ws-o5__next-name" data-testid="o5-next-name">{`Next · ${sampleName(next)}`}</p>
    <p className="ws-note">{`Load ${next.ref}, then Next sample: the Plan shows its Start, which checks readiness with its file. Nothing starts until you press Start there.`}</p>
  </>;
}

function CalibrationBlock({ run, q, list, error, listRead, onPrepare }: {
  run: PlanRun; q: RunQuickLookRun | null; list: CalibrationRecordsList | null; error: string; listRead: boolean;
  onPrepare: (recordId: string | null) => void;
}) {
  const [dismissed, setDismissed] = useState<string | null>(null);
  const runIdKnown = q?.status === "computed" && !!q.statistics;
  const runId = runIdKnown ? q!.statistics!.identity.run_id : null;
  const storeError = list?.valid ? list.store?.error ?? null : null;
  const usable = list?.valid && !storeError ? list : null;
  const record = recordForRun(usable, runId);
  if (run.methodPurpose !== "calibration" && !record) return null;
  if (!record) {
    // "None was derived" only once the records were read, the store is readable and the run's id is
    // known: otherwise a record may exist, and the block says it is not known.
    const resolved = listRead && !error && !storeError && usable !== null && runIdKnown && runId !== null;
    const reason = noRecordReason({ listError: error, storeError, listRead, listValid: list ? list.valid : undefined, runIdKnown, runId });
    return resolved ? (
      <Notice tone="info" title="No calibration record was derived for this run" data-testid="o5-calibration-none">
        {`This run used a calibration method${run.methodPurpose === "calibration" ? " (the readiness report at its Start)" : ""}: ${reason}.`}
      </Notice>
    ) : (
      <Notice tone="unknown" title="Calibration record not known" data-testid="o5-calibration-unknown">
        {`This run used a calibration method${run.methodPurpose === "calibration" ? " (the readiness report at its Start)" : ""}: ${reason}. Refresh reads the records again.`}
      </Notice>
    );
  }
  const proposed = record.status === "proposed";
  const avail = usable ? applyAvailability(record, usable) : { action: null, reason: "" };
  // Dismissal holds for this version of a proposal only: a record applied, invalidated or revised
  // since (by another client, read on Refresh) shows again.
  const dismissKey = `${record.record_id}:${record.version}:${record.sequence}`;
  if (proposed && dismissed === dismissKey) {
    return (
      <div className="ws-o5__calibration" data-testid="o5-calibration">
        <p className="ws-note" data-testid="o5-calibration-dismissed">{`Dismissed here · ${record.record_id} stays proposed in Prepare's list and changes nothing until it is applied there. Nothing was sent.`}</p>
        <Button size="small" onClick={() => onPrepare(record.record_id)} data-testid="o5-calibration-show">Show in Prepare</Button>
      </div>
    );
  }
  return (
    <div className="ws-o5__calibration" data-testid="o5-calibration">
      <span className="ws-plan-label">{`Calibration run · the record derived from it`}</span>
      <StateChip size="small" tone={statusTone(record, usable!)} data-testid="o5-calibration-status">{statusText(record, usable!)}</StateChip>
      <FactList missing={NOT_RECORDED} facts={[
        { label: "Record", value: <span className="ws-plan-id" data-testid="o5-calibration-record">{record.record_id}</span> },
        { label: "Pixel → µm", value: <span data-testid="o5-calibration-factor">{record.status === "proposed" || record.status === "insufficient"
          ? `${factorChange(usable!.current_factor, record.results?.proposed_pixel_to_micron)} · current → proposed${record.status === "insufficient" ? " · never applicable" : ""}`
          : factorText(recordFactor(record))}</span> },
        { label: "Diameter CV", value: <span data-testid="o5-calibration-cv">{cvText(record)}</span> },
        { label: "Gates", value: <ul className="ws-issue-list" data-testid="o5-calibration-gates">{gateLines(record).map((g) => <li key={g.id}>{g.text}</li>)}</ul> },
        { label: "Derived", value: record.created_at_utc || null },
      ]} />
      <div className="ws-ql__decision-row">
        {proposed && avail.action ? <>
          <Button size="small" variant="primary" onClick={() => onPrepare(record.record_id)} data-testid="o5-calibration-apply"
            title="Shows Prepare's Calibration card with this record: Apply is there, with the factor it reads. Nothing is applied from here.">Apply in Prepare…</Button>
          <Button size="small" onClick={() => setDismissed(dismissKey)} data-testid="o5-calibration-dismiss"
            title="Hides the proposal here; it stays proposed in Prepare's list. Sends nothing.">Dismiss</Button>
        </> : <Button size="small" onClick={() => onPrepare(record.record_id)} data-testid="o5-calibration-show">Show in Prepare</Button>}
      </div>
      {proposed && avail.reason ? <p className="ws-note">{`Apply in Prepare is unavailable for now: ${avail.reason}.`}</p> : null}
      <p className="ws-note">Nothing is applied from here or automatically: Apply is on Prepare's Calibration card.</p>
    </div>
  );
}
