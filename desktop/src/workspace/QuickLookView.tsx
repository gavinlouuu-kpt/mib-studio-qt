// O6 Experiment · Quick look (#716 A7b; its own sub-view, aligned with the Penpot board O6 in A7c;
// the issue's A7 rulings, exec plan P4 "Quick look", S5). Over the backend's run quick look (bridge
// ABI 44 `run_quick_look`, #716 A7a): one C++ function computes every number for this view and for
// Review, at close or on demand while the instrument is idle. This view only shows them
// (quickLookModel.ts formats; it computes nothing).
//
// - The experiment board, in plan order: the plan's closed runs side by side (n, medians, "% in
//   declared target group", event rate, flags, a mini scatter of the backend's overlay points), the
//   run in progress where it belongs ("numbers at close"), and a grey "Queued" placeholder with no
//   numbers for each run not yet made (runSummaryModel boardEntries).
// - The operator's reference run (none by default; held by App with the plan, not persisted) and the
//   backend's deltas (withheld ones with their reason, `differs` flagged).
// - The overlay on the kit's AreaDeformChart: the backend's point subsample, coloured by condition,
//   display only (Q4 a: no density contours).
// - "Refine the experiment" beside it: Continue · load the next sample, Repeat, Adjust next sample…
//   and Open in Review (summaryParts decisionsFor), with descriptive helper text only. Nothing here
//   assesses a run.
//
// Reads: useQuickLook (on showing, when the plan's closed runs or the reference change, once idle for
// a deferral, and on Refresh). No polling.
import { useMemo, useState, type ReactNode } from "react";
import {
  AreaDeformChart,
  Button,
  Card,
  ConditionSwatch,
  StatTile,
  StateChip,
  Toolbar,
  type AreaDeformPoint,
  type ConditionSlot,
} from "@mib/ui";
import type { RunQuickLookReply, RunQuickLookRun, RunStatistics } from "../runQuickLook";
import type { ExperimentSubView } from "./ExperimentView";
import { chartDomain, runCounts, runInProgress, type ChartDomain } from "./liveModel";
import { conditionSlots, type SampleRow } from "./planModel";
import { currentIndex, rowState } from "./queueModel";
import {
  NOT_RECORDED,
  NUMBERS_AT_CLOSE,
  planClosedRuns,
  quickLookPaths,
  referenceArgs,
  referenceOf,
  runFor,
  statusChip,
  TARGET_GROUP_LABEL,
  type PlanRun,
} from "./quickLookModel";
import { QuickLookThumb } from "./QuickLookThumb";
import { boardEntries, sampleName, type BoardEntry } from "./runSummaryModel";
import { basename, Chips, ControlNotice, DecisionReasons, decisionsFor, NoNumbers, ReadNotices, runName, Tiles, what, type ReviewAction } from "./summaryParts";
import type { PlanController } from "./usePlan";
import { useQuickLook, type QuickLookRead } from "./useQuickLook";

export interface QuickLookViewProps {
  /** The Experiment sub-view buttons (Plan · Live · Run summary · Quick look), first in the toolbar. */
  tabs: ReactNode;
  plan: PlanController;
  /** A run is starting, active or stopping (App's expActive): the transition to idle re-reads deferrals. */
  experimentActive: boolean;
  /** Show another Experiment sub-view (navigation only). */
  onView: (view: ExperimentSubView) => void;
  /** Adjust next: show the workspace's Align stage (navigation only). */
  onAlign: () => void;
  /** The inspected run (its path; null: the newest), shared with the Run summary. */
  inspected: string | null;
  onInspect: (path: string | null) => void;
  /** Open in Review: the classic Review tab with this file (`load_recording`, a control command). */
  review: ReviewAction;
  /** The read (tests pass their own; default bridge.runQuickLook). */
  read?: QuickLookRead;
}

const CHART_W = 560, CHART_H = 300;
/** At most this many runs are overlaid besides the reference (2000 points each, display only). */
export const OVERLAY_MAX_RUNS = 4;
const REFERENCE_SLOT: ConditionSlot = 5;

export function QuickLookView({ tabs, plan, experimentActive, onView, onAlign, inspected, onInspect, review, read }: QuickLookViewProps) {
  const runs = useMemo(() => planClosedRuns({ draft: plan.draft, queue: plan.queue.state, started: plan.started }),
    [plan.draft, plan.queue.state, plan.started]);
  const { paths, left } = quickLookPaths(runs);
  const shown = runs.slice(runs.length - paths.length);
  const quick = useQuickLook(paths, referenceArgs(plan.reference), experimentActive, read);
  const reply = quick.reply;
  const slots = conditionSlots(plan.draft.rows);
  const slotOfRow = (row: SampleRow | null): ConditionSlot | undefined => (row ? slots.get(row.condition.trim()) : undefined);
  const slotOf = (r: PlanRun) => slotOfRow(r.row);

  const sel = shown.find((r) => r.path === inspected) ?? shown[shown.length - 1] ?? null;
  // The runs overlaid: the operator's choice, else the inspected run.
  const [overlayChoice, setOverlayChoice] = useState<string[] | null>(null);
  const overlay = (overlayChoice ?? (sel ? [sel.path] : [])).filter((p) => shown.some((r) => r.path === p));
  const toggleOverlay = (path: string) => setOverlayChoice((prev) => {
    const cur = prev ?? (sel ? [sel.path] : []);
    return cur.includes(path) ? cur.filter((p) => p !== path) : cur.length >= OVERLAY_MAX_RUNS ? cur : [...cur, path];
  });

  const active = runInProgress(plan.queue.status);
  const entries = boardEntries({ draft: plan.draft, queue: plan.queue.state, started: plan.started }, shown, active);
  const ref = plan.reference;
  // The reference's own numbers only once the backend confirmed it is the run chosen (its run_id).
  const refConfirmed = !!ref && !!reply?.reference?.confirmed && reply.reference.requested.path === ref.path;
  const refRun = refConfirmed ? reply!.reference!.run : null;
  const d = decisionsFor(sel, plan, review, onView);
  // The tiles' mini scatters share one domain (display only).
  const thumbDomain = chartDomain(shown.flatMap((r) => thumbPoints(runFor(reply, r.path)?.statistics ?? null)));

  return (
    <div className="ws-view" data-testid="workspace-quick-look">
      <Toolbar title="Quick look">
        {tabs}
        <Button variant="primary" disabled={d.next.reason !== ""} title={d.next.reason || d.next.title} onClick={d.next.act} data-testid="o6-next-sample">Next sample</Button>
        <Button disabled={d.review.reason !== ""} title={d.review.reason || d.review.title} onClick={d.review.act} data-testid="o6-open-review">Open in Review</Button>
        <Button onClick={quick.refresh} disabled={paths.length === 0 || quick.loading} title="Reads the quick look again (a read; nothing is sent to the instrument)" data-testid="summary-refresh">
          {quick.loading ? "Reading…" : "Refresh"}
        </Button>
      </Toolbar>
      <div className="ws-body" data-testid="summary-body">
        <ReadNotices error={quick.error} left={left} read={paths.length} />
        <Card title="Experiment board" data-testid="summary-board"
          subtitle={`Runs of ${plan.draft.experimentId} in plan order · numbers from the backend (descriptive only); deltas are its run − reference; no tests, fits or % change`}
          status={<>
            <ReferenceChip reply={reply} hasReference={ref !== null} />
            {ref ? <span className="ws-detail" data-testid="summary-reference-name">{`Reference · run ${ref.runIndex ?? "?"} · ${ref.ref ?? basename(ref.path)}`}</span> : null}
          </>}
          actions={ref ? <Button size="small" onClick={() => plan.setReference(null)} data-testid="summary-reference-clear">Clear reference</Button> : null}>
          {reply?.reference && !reply.reference.confirmed ? (
            <p className="ws-note" data-testid="summary-reference-unconfirmed">
              {`Reference not confirmed · no deltas: ${reply.reference.message || reply.reference.run?.message || "the backend did not confirm the reference run."}`}
            </p>
          ) : null}
          {shown.length === 0 ? (
            <p className="ws-note" data-testid="summary-empty">
              {active ? `No closed run in this plan yet: the run in progress is shown with its status counts (${NUMBERS_AT_CLOSE}).`
                : "No closed run in this plan yet: a run's numbers appear here once the backend has finalized it and closed its file. Showing this view reads nothing until then."}
            </p>
          ) : null}
          <div className="ws-ql__board" data-testid="summary-runs">
            {entries.map((e) => <Entry key={entryKey(e)} e={e} reply={reply} loading={quick.loading} slotOf={slotOf} slotOfRow={slotOfRow}
              sel={sel} refPath={ref?.path ?? null} overlay={overlay} thumbDomain={thumbDomain} plan={plan}
              onInspect={(p) => onInspect(p)} onSummary={(p) => { onInspect(p); onView("Run summary"); }} onOverlay={toggleOverlay}
              onReference={(r) => { const next = referenceOf(r, runFor(reply, r.path)); if (next) plan.setReference(next); }} />)}
          </div>
          <p className="ws-note">{`Deltas are against the reference run you choose (Set as reference on a closed run). ${TARGET_GROUP_LABEL}: the run's own recorded target group, no polygon. Counts are the file's; "${NOT_RECORDED}" is a value the file does not record (never 0). Review recomputes on the same file with the same function.`}</p>
        </Card>

        <div className="ws-o6__row">
          {shown.length ? (
            <OverlayCard runs={shown.filter((r) => overlay.includes(r.path) && r.path !== ref?.path)} reply={reply} slotOf={slotOf}
              reference={ref ? { run: refRun, confirmed: refConfirmed, name: `run ${ref.runIndex ?? "?"} · ${ref.ref ?? basename(ref.path)}` } : null} />
          ) : <div />}
          <Card title="Refine the experiment" data-testid="o6-refine" subtitle="Decisions, not conclusions · each is one step">
            <p className="ws-note" data-testid="o6-refine-about">
              {sel ? `For ${runName(sel)}${what(sel) ? ` · ${what(sel)}` : ""} (the inspected run; choose another with Inspect).` : "No closed run is inspected yet."}
            </p>
            <ControlNotice d={d} />
            <div className="ws-o6__decisions" data-testid="summary-decisions">
              <Button variant="primary" disabled={d.next.reason !== ""} title={d.next.reason || d.next.title} onClick={d.next.act} data-testid="decision-continue">
                {d.next.row ? `Continue · load ${d.next.row.ref}` : "Continue · Next sample"}
              </Button>
              <span className="ws-note">{d.next.row ? `Follow the plan as written: the Plan shows ${sampleName(d.next.row)}'s Start` : "Follow the plan as written: the Plan shows what is next"}</span>
              <Button disabled={d.repeat.reason !== ""} title={d.repeat.reason || d.repeat.title} onClick={d.repeat.act} data-testid="decision-repeat">
                {sel?.ref ? `Repeat ${sel.ref}` : "Repeat sample"}
              </Button>
              <span className="ws-note">New run, same sheet row and sample ref; both files kept</span>
              <span className="ws-o6__pair">
                <Button onClick={() => onView("Plan")} title="Shows the Plan to edit the next sample's row (navigation only; sends nothing)" data-testid="decision-adjust">Adjust next sample…</Button>
                <Button size="small" onClick={onAlign} title="Shows Align to adjust the next sample's settings (navigation only; sends nothing)" data-testid="decision-adjust-align">Align…</Button>
              </span>
              <span className="ws-note">Edit the next row in the Plan, or the settings in Align; sends nothing</span>
              <Button disabled={d.review.reason !== ""} title={d.review.reason || d.review.title} onClick={d.review.act} data-testid="decision-review">Open in Review (PC)</Button>
              <span className="ws-note">This file in the classic Review tab: populations, statistics with units, comparison</span>
            </div>
            <DecisionReasons d={d} items={[["Continue", d.next], ["Repeat", d.repeat], ["Open in Review", d.review]]} />
            <p className="ws-note">Nothing here changes a closed file or the method, and none of these acts until you press it.</p>
          </Card>
        </div>
      </div>
    </div>
  );
}

const entryKey = (e: BoardEntry) => (e.kind === "run" ? e.run.path : e.kind === "active" ? `active-${e.ref ?? ""}` : `queued-${e.ref}`);
const thumbPoints = (s: RunStatistics | null) => (s?.overlay.area_um2 ? s.overlay.area_um2.map((area, i) => ({ area, deformability: s.overlay.deformability[i] })) : []);

function Entry({ e, reply, loading, slotOf, slotOfRow, sel, refPath, overlay, thumbDomain, plan, onInspect, onSummary, onOverlay, onReference }: {
  e: BoardEntry; reply: RunQuickLookReply | null; loading: boolean; slotOf: (r: PlanRun) => ConditionSlot | undefined;
  slotOfRow: (row: SampleRow | null) => ConditionSlot | undefined; sel: PlanRun | null; refPath: string | null; overlay: string[];
  thumbDomain: ChartDomain; plan: PlanController;
  onInspect: (path: string) => void; onSummary: (path: string) => void; onOverlay: (path: string) => void; onReference: (r: PlanRun) => void;
}) {
  if (e.kind === "active") return <ActiveRunTile plan={plan} />;
  if (e.kind === "placeholder") {
    const slot = slotOfRow(e.row);
    const name = e.row.sample.trim() || e.row.condition.trim();
    return (
      <Card variant="tile" className="ws-ql__run ws-ql__run--placeholder" data-testid={`summary-queued-${e.ref}`}
        title={slot ? <ConditionSwatch condition={slot}>{e.ref}</ConditionSwatch> : e.ref}
        subtitle={name || "no sample named"}
        status={<StateChip size="small" tone={e.tone} data-testid={`summary-queued-${e.ref}-chip`}>{e.text}</StateChip>}>
        <p className="ws-ql__dash" aria-label="no numbers">—</p>
        <p className="ws-note">No numbers until its run is closed.</p>
      </Card>
    );
  }
  const r = e.run;
  const q = runFor(reply, r.path);
  return <RunTile run={r} q={q} loading={loading} slot={slotOf(r)} inspected={sel?.path === r.path} isReference={refPath === r.path}
    overlaid={overlay.includes(r.path)} overlayFull={overlay.length >= OVERLAY_MAX_RUNS} thumbDomain={thumbDomain}
    onInspect={() => onInspect(r.path)} onSummary={() => onSummary(r.path)} onOverlay={() => onOverlay(r.path)} onReference={() => onReference(r)} />;
}

function ReferenceChip({ reply, hasReference }: { reply: RunQuickLookReply | null; hasReference: boolean }) {
  if (!hasReference) return <StateChip tone="neutral" data-testid="summary-reference-chip">Reference · none chosen</StateChip>;
  if (!reply?.reference) return <StateChip tone="unknown" data-testid="summary-reference-chip">Reference · not read yet</StateChip>;
  return reply.reference.confirmed
    ? <StateChip tone="info" data-testid="summary-reference-chip">Reference · confirmed by the backend</StateChip>
    : <StateChip tone="unknown" data-testid="summary-reference-chip">Reference · not confirmed</StateChip>;
}

function RunTile({ run, q, loading, slot, inspected, isReference, overlaid, overlayFull, thumbDomain, onInspect, onSummary, onOverlay, onReference }: {
  run: PlanRun; q: RunQuickLookRun | null; loading: boolean; slot: ConditionSlot | undefined; inspected: boolean; isReference: boolean;
  overlaid: boolean; overlayFull: boolean; thumbDomain: ChartDomain; onInspect: () => void; onSummary: () => void; onOverlay: () => void; onReference: () => void;
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
        <Button size="small" variant={inspected ? "selected" : "secondary"} aria-pressed={inspected} onClick={onInspect} data-testid={`${id}-inspect`}>Inspect</Button>
        <Button size="small" onClick={onSummary} title="Shows this run's Run summary (navigation only)" data-testid={`${id}-summary`}>Run summary</Button>
        <Button size="small" variant={isReference ? "selected" : "secondary"} aria-pressed={isReference} disabled={!s || isReference}
          title={s ? (isReference ? "This is the reference run" : "Use as the reference: the backend computes each run's deltas against it (a read; sends nothing to the instrument)") : "Only a run with numbers can be the reference"}
          onClick={onReference} data-testid={`${id}-set-reference`}>{isReference ? "Reference" : "Set as reference"}</Button>
        <Button size="small" variant={overlaid ? "selected" : "secondary"} aria-pressed={overlaid} disabled={!canOverlay || (!overlaid && overlayFull)}
          title={!s ? "No numbers to overlay" : !canOverlay ? "px→µm not recorded: the overlay is in µm²" : !overlaid && overlayFull ? `At most ${OVERLAY_MAX_RUNS} runs besides the reference` : "Overlay on the chart (display only)"}
          onClick={onOverlay} data-testid={`${id}-overlay`}>Overlay</Button>
      </>}>
      <NoNumbers q={q} testId={id} />
      {s?.overlay.area_um2 ? <QuickLookThumb area={s.overlay.area_um2} deformability={s.overlay.deformability} domain={thumbDomain} condition={slot} testId={`${id}-thumb`} /> : null}
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
  reference: { run: RunQuickLookRun | null; confirmed: boolean; name: string } | null;
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
  else if (reference) notes.push(`The reference (${reference.name}) is not drawn: ${!reference.confirmed ? "the backend has not confirmed it" : reference.run?.status === "computed" ? "px→µm not recorded" : "its numbers were not read"}.`);
  for (const r of runs) {
    const q = runFor(reply, r.path);
    const slot = slotOf(r);
    if (!add(q?.statistics, slot)) { notes.push(`${runName(r)} is not drawn: ${q?.status === "computed" ? "px→µm not recorded" : "no numbers read"}.`); continue; }
    const cond = r.row?.condition.trim();
    if (slot && cond && !legend.some((l) => l.condition === slot)) legend.push({ condition: slot, label: cond });
    if (!slot) notes.push(`${runName(r)} has no condition colour (grey).`);
  }
  const domain = chartDomain(points);
  const names = runs.map((r) => `run ${r.runIndex}`).join(", ");
  return (
    <Card title={`Overlay${names ? ` · ${names}` : ""}${reference ? ` vs reference ${reference.name}` : ""}`} data-testid="summary-overlay"
      subtitle={`Point subsample from the backend, coloured by condition (no density contours) · the reference and the runs chosen with Overlay, at most ${OVERLAY_MAX_RUNS} besides the reference`}>
      <div className="ws-live__chart" data-testid="summary-chart">
        <AreaDeformChart points={points} width={CHART_W} height={CHART_H} legend={legend.length ? legend : undefined}
          xDomain={domain.x} yDomain={domain.y} xTicks={domain.xTicks} yTicks={domain.yTicks} />
      </div>
      {notes.map((n) => <p key={n} className="ws-note">{n}</p>)}
      <p className="ws-note" data-testid="summary-overlay-note">Display only: each run's subsample of at most 2000 points at a fixed stride, outside the numbers' parity with Review. Area in µm² at each file's recorded px→µm. Descriptive only: no test, no fit, cells are not replicates.</p>
    </Card>
  );
}
