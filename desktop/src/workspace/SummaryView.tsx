// O5 Experiment · Run summary and O6 Quick look (#716 A7b; the issue's A7 rulings, exec plan P4 "Quick
// look", S5). Over the backend's run quick look (bridge ABI 44 `run_quick_look`, #716 A7a): one C++
// function computes every number for this view and for Review, at close or on demand while the
// instrument is idle. This view only shows them (quickLookModel.ts formats; it computes nothing).
//
// - O5 Run summary, for a closed run of the plan (the single sample, or a queue row once the backend
//   finalized it): the outcome, the counts and the accounting, the provenance from the identity, the
//   medians as StatTiles and the flags as descriptive chips; then the decisions.
// - O6 Quick look: the plan's closed runs side by side (n, medians, "% in declared target group",
//   event rate, flags), the operator's reference run (none by default; held by App with the plan,
//   not persisted), the backend's deltas (withheld ones with their reason, `differs` flagged), and
//   the overlay on the kit's AreaDeformChart (display only).
// - The decisions (the A7 ruling 8), each one explicit step: Repeat sample (re-queues the same
//   sample_ref as the next run; sends nothing), Adjust next (navigates to Align or the Plan; sends
//   nothing), Continue (the queue's next step: confirms the finalized run's close, then the Plan
//   shows the next Start), Open in Review (the classic Review tab with the file; loading it is a
//   control command). A viewer's acting decisions are disabled with the shared control block; reads
//   and navigation are allowed.
// - During a run: the earlier runs from the backend's cache, and the active run's status counts,
//   labelled "numbers at close".
//
// Reads: useQuickLook (on showing, when the plan's closed runs or the reference change, once idle
// for a deferral, and on Refresh). No polling.
import { useMemo, useState, type ReactNode } from "react";
import {
  AreaDeformChart,
  Button,
  Card,
  ConditionSwatch,
  FactList,
  Notice,
  StatTile,
  StateChip,
  Toolbar,
  type AreaDeformPoint,
  type ConditionSlot,
} from "@mib/ui";
import type { RunQuickLookReply, RunQuickLookRun, RunStatistics } from "../runQuickLook";
import type { ExperimentSubView } from "./ExperimentView";
import { chartDomain, runCounts, runInProgress } from "./liveModel";
import { conditionSlots } from "./planModel";
import { controlBlock } from "./planParts";
import { currentIndex, repeatReason, rowState } from "./queueModel";
import {
  countFacts,
  DEFERRED_TEXT,
  differsChips,
  flagChips,
  NOT_RECORDED,
  NUMBERS_AT_CLOSE,
  planClosedRuns,
  provenanceFacts,
  quickLookPaths,
  quickTiles,
  referenceArgs,
  referenceOf,
  runFor,
  statusChip,
  TARGET_GROUP_LABEL,
  type PlanRun,
} from "./quickLookModel";
import type { PlanController } from "./usePlan";
import { useQuickLook, type QuickLookRead } from "./useQuickLook";

export interface SummaryViewProps {
  /** The Experiment sub-view buttons (Plan · Live · Summary), first in the toolbar. */
  tabs: ReactNode;
  plan: PlanController;
  /** A run is starting, active or stopping (App's expActive): the transition to idle re-reads deferrals. */
  experimentActive: boolean;
  /** Show another Experiment sub-view (navigation only). */
  onView: (view: ExperimentSubView) => void;
  /** Adjust next: show the workspace's Align stage (navigation only). */
  onAlign: () => void;
  /** Open in Review: the classic Review tab with this file (`load_recording`, a control command);
   *  `reason` says why it is unavailable ("" when it is available). */
  review: { reason: string; open: (path: string) => void };
  /** The read (tests pass their own; default bridge.runQuickLook). */
  read?: QuickLookRead;
}

const CHART_W = 560, CHART_H = 300;
/** At most this many runs are overlaid besides the reference (2000 points each, display only). */
export const OVERLAY_MAX_RUNS = 4;
const REFERENCE_SLOT: ConditionSlot = 5;
const basename = (path: string) => path.split(/[\\/]/).pop() || path;
const runName = (r: PlanRun) => `Run ${r.runIndex} · ${r.ref || "sample"}`;
const what = (r: PlanRun) => r.row ? (r.row.sample.trim() || r.row.condition.trim()) : "";
/** Who controls the decisions that act: a viewer's are disabled, and nothing is sent. */
export const DECISIONS_CONTROL = controlBlock("Repeat sample, Continue and Open in Review");

export function SummaryView({ tabs, plan, experimentActive, onView, onAlign, review, read }: SummaryViewProps) {
  const runs = useMemo(() => planClosedRuns({ draft: plan.draft, queue: plan.queue.state, started: plan.started }),
    [plan.draft, plan.queue.state, plan.started]);
  const { paths, left } = quickLookPaths(runs);
  const shown = runs.slice(runs.length - paths.length);
  const quick = useQuickLook(paths, referenceArgs(plan.reference), experimentActive, read);
  const reply = quick.reply;
  const slots = conditionSlots(plan.draft.rows);
  const slotOf = (r: PlanRun): ConditionSlot | undefined => (r.row ? slots.get(r.row.condition.trim()) : undefined);

  const [inspected, setInspected] = useState<string | null>(null);
  const sel = shown.find((r) => r.path === inspected) ?? shown[shown.length - 1] ?? null;
  // The runs overlaid: the operator's choice, else the inspected run.
  const [overlayChoice, setOverlayChoice] = useState<string[] | null>(null);
  const overlay = (overlayChoice ?? (sel ? [sel.path] : [])).filter((p) => shown.some((r) => r.path === p));
  const toggleOverlay = (path: string) => setOverlayChoice((prev) => {
    const cur = prev ?? (sel ? [sel.path] : []);
    return cur.includes(path) ? cur.filter((p) => p !== path) : cur.length >= OVERLAY_MAX_RUNS ? cur : [...cur, path];
  });

  const status = plan.queue.status;
  const active = runInProgress(status);
  const ref = plan.reference;
  const refRun = reply?.reference?.run ?? null;

  return (
    <div className="ws-view" data-testid="workspace-summary">
      <Toolbar title="Run summary · Quick look">
        {tabs}
        <Button onClick={quick.refresh} disabled={paths.length === 0 || quick.loading} title="Reads the quick look again (a read; nothing is sent to the instrument)" data-testid="summary-refresh">
          {quick.loading ? "Reading…" : "Refresh"}
        </Button>
      </Toolbar>
      <div className="ws-body" data-testid="summary-body">
        {quick.error ? <Notice tone="unknown" title="Quick look not read" data-testid="summary-read-error">{quick.error}</Notice> : null}
        {left > 0 ? (
          <Notice tone="info" title={`The newest ${paths.length} runs are shown`} data-testid="summary-truncated">
            {`${left} earlier ${left === 1 ? "run is" : "runs are"} not read here: one read covers at most ${paths.length} files. Review opens them all.`}
          </Notice>
        ) : null}
        {sel === null ? (
          <Notice tone="info" title="No closed run in this plan yet" data-testid="summary-empty"
            actions={<Button onClick={() => onView("Plan")}>Go to Plan</Button>}>
            {active
              ? `The run in progress is summarised once the backend has finalized it and closed its file (${NUMBERS_AT_CLOSE}).`
              : "A run's summary appears here once the backend has finalized it and closed its file. Showing this view reads nothing until then."}
          </Notice>
        ) : (
          <RunSummary run={sel} q={runFor(reply, sel.path)} loading={quick.loading} plan={plan} isReference={ref?.path === sel.path}
            onView={onView} onAlign={onAlign} review={review} />
        )}

        {shown.length || active ? (
          <Card title="Quick look · experiment board" data-testid="summary-board"
            subtitle="This plan's closed runs side by side · descriptive only: n and medians from the backend, deltas are its run − reference; no tests, fits or % change"
            status={<>
              <ReferenceChip reply={reply} hasReference={ref !== null} />
              {ref ? <span className="ws-detail" data-testid="summary-reference-name">{`Reference · run ${ref.runIndex ?? "?"} · ${ref.ref ?? basename(ref.path)}`}</span> : null}
            </>}
            actions={ref ? <Button size="small" onClick={() => plan.setReference(null)} data-testid="summary-reference-clear">Clear reference</Button> : null}>
            {reply?.reference && !reply.reference.confirmed ? (
              <Notice tone="unknown" title="Reference not confirmed · no deltas" data-testid="summary-reference-unconfirmed">
                {reply.reference.message || reply.reference.run?.message || "The backend did not confirm the reference run."}
              </Notice>
            ) : null}
            <div className="ws-ql__board" data-testid="summary-runs">
              {shown.map((r) => (
                <RunTile key={r.path} run={r} q={runFor(reply, r.path)} loading={quick.loading} slot={slotOf(r)}
                  inspected={sel?.path === r.path} isReference={ref?.path === r.path} overlaid={overlay.includes(r.path)}
                  overlayFull={overlay.length >= OVERLAY_MAX_RUNS}
                  onInspect={() => setInspected(r.path)} onOverlay={() => toggleOverlay(r.path)}
                  onReference={() => { const next = referenceOf(r, runFor(reply, r.path)); if (next) plan.setReference(next); }} />
              ))}
              {active ? <ActiveRunTile plan={plan} /> : null}
            </div>
            <p className="ws-note">{`${TARGET_GROUP_LABEL}: the run's own recorded target group, no polygon. Counts are the file's; "${NOT_RECORDED}" is a value the file does not record (never 0). Review recomputes on the same file with the same function.`}</p>
          </Card>
        ) : null}

        {shown.length ? (
          <OverlayCard runs={shown.filter((r) => overlay.includes(r.path) && r.path !== ref?.path)} reply={reply} slotOf={slotOf} reference={ref ? { run: refRun, name: `run ${ref.runIndex ?? "?"} · ${ref.ref ?? basename(ref.path)}` } : null} />
        ) : null}
      </div>
    </div>
  );
}

function ReferenceChip({ reply, hasReference }: { reply: RunQuickLookReply | null; hasReference: boolean }) {
  if (!hasReference) return <StateChip tone="neutral" data-testid="summary-reference-chip">Reference · none chosen</StateChip>;
  if (!reply?.reference) return <StateChip tone="unknown" data-testid="summary-reference-chip">Reference · not read yet</StateChip>;
  return reply.reference.confirmed
    ? <StateChip tone="info" data-testid="summary-reference-chip">Reference · confirmed by the backend</StateChip>
    : <StateChip tone="unknown" data-testid="summary-reference-chip">Reference · not confirmed</StateChip>;
}

/** What the backend says when it has no numbers for a run (deferred, raw, error). */
function NoNumbers({ q, testId }: { q: RunQuickLookRun | null; testId: string }) {
  if (!q) return null;
  if (q.status === "deferred") return (
    <Notice tone="unknown" title={`Quick look ${DEFERRED_TEXT}`} data-testid={`${testId}-deferred`}>
      {"The backend reads a file it has not cached only while no run or review job is in progress. It is read again here when the run in progress ends; Refresh reads it again now."}
    </Notice>
  );
  if (q.status === "not_available") return (
    <Notice tone="info" title="Not available until exported to HDF5" data-testid={`${testId}-not-available`}>
      {`${q.message || "A raw recording has no quick look"}. Export it to HDF5 to see its numbers.`}
    </Notice>
  );
  if (q.status === "error") return <Notice tone="unknown" title="Quick look not read for this file" data-testid={`${testId}-error`}>{q.message || NOT_RECORDED}</Notice>;
  return null;
}

function Tiles({ s, q, hideDeltas, testId }: { s: RunStatistics; q: RunQuickLookRun; hideDeltas?: boolean; testId: string }) {
  return (
    <div className="ws-live__stats ws-ql__tiles" data-testid={testId}>
      {quickTiles(s, hideDeltas ? null : q.delta).map((t) => (
        <StatTile key={t.key} label={t.label} value={t.value} missing={t.missing} delta={t.delta} title={t.detail} data-testid={`${testId}-${t.key}`} />
      ))}
    </div>
  );
}

function Chips({ s, q }: { s: RunStatistics | null; q: RunQuickLookRun | null }) {
  return <>
    {flagChips(s).map((c) => <StateChip key={c.key} size="small" tone={c.tone} data-testid={`flag-${c.key}`}>{c.text}</StateChip>)}
    {differsChips(q?.delta ?? null).map((c) => <StateChip key={`d-${c.key}`} size="small" tone={c.tone} data-testid={`differs-${c.key}`}>{c.text}</StateChip>)}
  </>;
}

function RunSummary({ run, q, loading, plan, isReference, onView, onAlign, review }: {
  run: PlanRun; q: RunQuickLookRun | null; loading: boolean; plan: PlanController; isReference: boolean;
  onView: (v: ExperimentSubView) => void; onAlign: () => void; review: SummaryViewProps["review"];
}) {
  const s = q?.status === "computed" ? q.statistics : null;
  const chip = statusChip(q, loading);
  const name = what(run);
  return (
    <Card title={`Run summary · ${runName(run)}${name ? ` · ${name}` : ""}`} data-testid="summary-run"
      subtitle={`${basename(run.path)} · closed by the backend · the numbers are the file's, read by the backend`}
      status={<>
        <StateChip tone="neutral" data-testid="summary-outcome">{run.outcome.cancelled ? "Finalized · cancelled" : "Finalized · file closed"}</StateChip>
        {run.confirmed ? null : <StateChip tone="info" data-testid="summary-awaiting">Awaiting your close confirmation</StateChip>}
        <StateChip tone={chip.tone} data-testid="summary-status">{chip.text}</StateChip>
        {isReference ? <StateChip tone="info">Reference run</StateChip> : null}
        <Chips s={s} q={isReference ? null : q} />
      </>}>
      {run.outcome.headline ? <p className="ws-note" data-testid="summary-headline">{run.outcome.headline}</p> : null}
      <NoNumbers q={q} testId="summary" />
      {s && q ? (
        <>
          <Tiles s={s} q={q} hideDeltas={isReference} testId="summary-tiles" />
          <div className="ws-ql__facts">
            <div>
              <span className="ws-plan-label">Counts and accounting</span>
              <FactList facts={countFacts(s)} missing={NOT_RECORDED} data-testid="summary-counts" />
            </div>
            <div>
              <span className="ws-plan-label">Provenance · the run's identity</span>
              <FactList facts={provenanceFacts(s, run.path)} missing={NOT_RECORDED} data-testid="summary-provenance" />
            </div>
          </div>
        </>
      ) : null}
      <Decisions run={run} plan={plan} onView={onView} onAlign={onAlign} review={review} />
    </Card>
  );
}

/** The decisions (A7 ruling 8): each one explicit step. */
function Decisions({ run, plan, onView, onAlign, review }: {
  run: PlanRun; plan: PlanController; onView: (v: ExperimentSubView) => void; onAlign: () => void; review: SummaryViewProps["review"];
}) {
  const queue = plan.queue, q = queue.state;
  const queueMode = plan.draft.format === "queue";
  const order = plan.draft.rows.map((r) => r.ref);
  const controlHeld = queue.controlHeld;
  const busy = queue.pending !== null;
  const control = controlHeld ? "" : DECISIONS_CONTROL.title;

  // Repeat sample: the run's own row, its latest run only.
  let repeat = "";
  if (!queueMode) repeat = "a single-sample plan has one run: a sample queue repeats a sample";
  else {
    const row = rowState(q, run.ref);
    const last = row.attempts[row.attempts.length - 1];
    repeat = control || (busy ? "a Start or a stop is pending" : "")
      || (last && last.runIndex !== run.runIndex ? `${run.ref} was re-queued after this run (run ${last.runIndex})` : repeatReason(order, q, run.ref));
  }

  // Continue: the queue's next step from here.
  const cur = queueMode ? currentIndex(order, q) : -1;
  const curRef = cur >= 0 ? order[cur] : null;
  const curState = curRef ? rowState(q, curRef) : null;
  let next: { label: string; act: () => void; reason: string; title: string };
  if (queueMode && curState?.phase === "finalized") {
    next = { label: "Continue · Next sample", reason: control || (busy || q.paused ? "the queue is paused or a request is pending" : ""),
      title: `Confirms ${curRef}'s close (sends nothing), then the Plan shows the next Start`, act: () => { queue.confirmClosed(); onView("Plan"); } };
  } else if (queueMode && curState && ["starting", "running", "stopping"].includes(curState.phase)) {
    next = { label: "Continue · Next sample", reason: `${curRef} is ${curState.phase}: Next sample is in the Plan and Live`, title: "", act: () => {} };
  } else {
    next = { label: "Continue · Next sample", reason: "", act: () => onView("Plan"),
      title: queueMode ? (curRef ? `Shows the Plan, where ${curRef} waits for its Start (navigation only)` : "Shows the Plan: add a sample or start a new plan (navigation only)")
        : "Shows the Plan: a new plan runs the next sample (navigation only)" };
  }
  const reviewReason = control || review.reason;

  return (
    <div className="ws-ql__decisions" data-testid="summary-decisions">
      <span className="ws-plan-label">Decisions · each is one step</span>
      {!controlHeld ? <Notice tone={DECISIONS_CONTROL.tone} title={DECISIONS_CONTROL.title} data-testid="summary-control">{DECISIONS_CONTROL.text}</Notice> : null}
      <div className="ws-ql__decision-row">
        <Button disabled={repeat !== ""} title={repeat || `Re-queues ${run.ref} as the next run (run ${q.nextRunIndex}) with the same sample ref; Start picks a new file. Sends nothing.`}
          onClick={() => queue.repeat(run.ref)} data-testid="decision-repeat">Repeat sample</Button>
        <Button onClick={onAlign} title="Shows Align to adjust the next sample's settings (navigation only; sends nothing)" data-testid="decision-adjust-align">Adjust next · Align</Button>
        <Button onClick={() => onView("Plan")} title="Shows the Plan to edit the next sample's row (navigation only; sends nothing)" data-testid="decision-adjust-plan">Adjust next · Plan</Button>
        <Button variant="primary" disabled={next.reason !== ""} title={next.reason || next.title} onClick={next.act} data-testid="decision-continue">{next.label}</Button>
        <Button disabled={reviewReason !== ""} title={reviewReason || "Opens this file in the classic Review tab (it loads the file there). While a file is open in Review, Start is refused: close it in Review first."}
          onClick={() => review.open(run.path)} data-testid="decision-review">Open in Review</Button>
      </div>
      {[repeat && queueMode ? `Repeat sample: ${repeat}.` : "", next.reason ? `Continue: ${next.reason}.` : "", reviewReason && controlHeld ? `Open in Review: ${reviewReason}.` : ""]
        .filter((t) => t).map((t) => <p key={t} className="ws-note ws-ql__reason">{t}</p>)}
      <p className="ws-note">Descriptive numbers only: nothing here recommends a decision, and none acts until you press it.</p>
    </div>
  );
}

function RunTile({ run, q, loading, slot, inspected, isReference, overlaid, overlayFull, onInspect, onOverlay, onReference }: {
  run: PlanRun; q: RunQuickLookRun | null; loading: boolean; slot: ConditionSlot | undefined; inspected: boolean; isReference: boolean;
  overlaid: boolean; overlayFull: boolean; onInspect: () => void; onOverlay: () => void; onReference: () => void;
}) {
  const s = q?.status === "computed" ? q.statistics : null;
  const chip = statusChip(q, loading);
  const name = what(run);
  const id = `summary-run-${run.runIndex}`;
  const canOverlay = s?.overlay.area_um2 != null;
  return (
    <Card variant={inspected ? "selected" : "tile"} data-testid={id} className="ws-ql__run"
      title={slot ? <ConditionSwatch condition={slot}>{runName(run)}</ConditionSwatch> : runName(run)}
      subtitle={`${name ? `${name} · ` : ""}${basename(run.path)}`}
      status={<>
        <StateChip size="small" tone={chip.tone} data-testid={`${id}-status`}>{chip.text}</StateChip>
        {isReference ? <StateChip size="small" tone="info" data-testid={`${id}-reference`}>Reference</StateChip> : null}
        <Chips s={s} q={isReference ? null : q} />
      </>}
      actions={<>
        <Button size="small" variant={inspected ? "selected" : "secondary"} aria-pressed={inspected} onClick={onInspect} data-testid={`${id}-inspect`}>Summary</Button>
        <Button size="small" variant={isReference ? "selected" : "secondary"} aria-pressed={isReference} disabled={!s || isReference}
          title={s ? (isReference ? "This is the reference run" : "Use as the reference: the backend computes each run's deltas against it (a read; sends nothing to the instrument)") : "Only a run with numbers can be the reference"}
          onClick={onReference} data-testid={`${id}-set-reference`}>{isReference ? "Reference" : "Set as reference"}</Button>
        <Button size="small" variant={overlaid ? "selected" : "secondary"} aria-pressed={overlaid} disabled={!canOverlay || (!overlaid && overlayFull)}
          title={!s ? "No numbers to overlay" : !canOverlay ? "px→µm not recorded: the overlay is in µm²" : !overlaid && overlayFull ? `At most ${OVERLAY_MAX_RUNS} runs besides the reference` : "Overlay on the chart (display only)"}
          onClick={onOverlay} data-testid={`${id}-overlay`}>Overlay</Button>
      </>}>
      <NoNumbers q={q} testId={id} />
      {s && q ? <Tiles s={s} q={q} hideDeltas={isReference} testId={`${id}-tiles`} /> : null}
    </Card>
  );
}

/** The active run: its status counts now; its medians are the quick look of its file, at close. */
function ActiveRunTile({ plan }: { plan: PlanController }) {
  const status = plan.queue.status;
  const counts = runCounts(status);
  const order = plan.draft.rows.map((r) => r.ref);
  const cur = plan.draft.format === "queue" ? currentIndex(order, plan.queue.state) : -1;
  const curRef = cur >= 0 ? order[cur] : null;
  const attempts = curRef ? rowState(plan.queue.state, curRef).attempts : [];
  const last = attempts.length ? attempts[attempts.length - 1] : undefined;
  const title = curRef && last ? `Run ${last.runIndex} · ${curRef} · in progress` : "Run in progress";
  return (
    <Card variant="tile" className="ws-ql__run" data-testid="summary-active-run" title={title}
      subtitle={status?.output_path ? basename(status.output_path) : "Its file is being written"}
      status={<StateChip size="small" tone="info">{`Medians: ${NUMBERS_AT_CLOSE}`}</StateChip>}>
      {counts ? (
        <div className="ws-live__stats ws-ql__tiles" data-testid="summary-active-counts">
          <StatTile label="Valid saved" value={counts.saved} />
          <StatTile label="Invalid saved" value={counts.invalidSaved} />
          <StatTile label="Pending" value={counts.pending} />
          <StatTile label="Dropped" value={counts.dropped} />
        </div>
      ) : null}
      <p className="ws-note">{`The run's status counts as the backend reports them now. Its medians, target group and event rate are read from its file once it is closed (${NUMBERS_AT_CLOSE}).`}</p>
    </Card>
  );
}

function OverlayCard({ runs, reply, slotOf, reference }: {
  runs: PlanRun[]; reply: RunQuickLookReply | null; slotOf: (r: PlanRun) => ConditionSlot | undefined;
  reference: { run: RunQuickLookRun | null; name: string } | null;
}) {
  const points: AreaDeformPoint[] = [];
  const legend: { condition: ConditionSlot; label: string }[] = [];
  const notes: string[] = [];
  const add = (s: RunStatistics | null | undefined, condition: ConditionSlot | undefined) => {
    const o = s?.overlay;
    if (!o?.area_um2) return false;
    o.area_um2.forEach((area, i) => points.push(condition ? { area, deformability: o.deformability[i], condition } : { area, deformability: o.deformability[i] }));
    return true;
  };
  if (reference?.run?.statistics && add(reference.run.statistics, REFERENCE_SLOT)) legend.push({ condition: REFERENCE_SLOT, label: `Reference · ${reference.name}` });
  else if (reference) notes.push(`The reference (${reference.name}) is not drawn: ${reference.run?.status === "computed" ? "px→µm not recorded" : "its numbers were not read"}.`);
  for (const r of runs) {
    const q = runFor(reply, r.path);
    const slot = slotOf(r);
    if (!add(q?.statistics, slot)) { notes.push(`${runName(r)} is not drawn: ${q?.status === "computed" ? "px→µm not recorded" : "no numbers read"}.`); continue; }
    const cond = r.row?.condition.trim();
    if (slot && cond && !legend.some((l) => l.condition === slot)) legend.push({ condition: slot, label: cond });
    if (!slot) notes.push(`${runName(r)} has no condition colour (grey).`);
  }
  const domain = chartDomain(points);
  return (
    <Card title="Overlay · Area–Deformability" data-testid="summary-overlay"
      subtitle={`The reference and the runs chosen with Overlay, coloured by condition · at most ${OVERLAY_MAX_RUNS} runs besides the reference`}>
      <div className="ws-live__chart" data-testid="summary-chart">
        <AreaDeformChart points={points} width={CHART_W} height={CHART_H} legend={legend.length ? legend : undefined}
          xDomain={domain.x} yDomain={domain.y} xTicks={domain.xTicks} yTicks={domain.yTicks} />
      </div>
      {notes.map((n) => <p key={n} className="ws-note">{n}</p>)}
      <p className="ws-note" data-testid="summary-overlay-note">Display only: each run's subsample of at most 2000 points at a fixed stride, outside the numbers' parity with Review. Area in µm² at each file's recorded px→µm.</p>
    </Card>
  );
}
