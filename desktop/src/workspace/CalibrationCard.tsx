// Prepare's Calibration card (#716 A4b; ADR 0015 item 7, exec plan P2): the `calibration.record`
// gate and its Fix, the record in effect, proposed and insufficient records, and a plain list of
// every record (no trend chart). The one action is Apply / Re-apply, an explicit
// calibration_apply(record_id, expected_current_factor) with the factor this page last read; a
// viewer, a run or a pending apply disables it and nothing is sent. Showing, listing and polling
// never apply anything. A calibration itself is a normal run of a calibration method, started
// from Experiment · Plan; there is no separate acquisition path.
import { Button, Card, DataTable, FactList, Notice, StateChip, type DataTableColumn, type Fact } from "@mib/ui";
import type { CalibrationRecord, CalibrationRecordsList } from "../calibrationRecords";
import type { ReadinessGate } from "../eventAdapter";
import {
  CALIBRATION_LIST_ROWS,
  DERIVATION_TEXT,
  INSUFFICIENT_SHOWN,
  PURPOSE_GAP_TEXT,
  RUN_CALIBRATION_TEXT,
  ageText,
  applyAvailability,
  factorChange,
  factorText,
  failedGates,
  fixAction,
  identityText,
  methodText,
  notInEffectText,
  recordCv,
  recordFactor,
  sourceText,
  statusText,
  statusTone,
  type ApplyAction,
} from "./calibrationModel";
import { controlBlock } from "./planParts";
import { gateFactTone, gateRemediation, gateText, gateTone } from "./prepareModel";
import type { CalibrationController } from "./useCalibration";

export interface CalibrationCardProps {
  calibration: CalibrationController;
  /** The calibration.record gate of the path-less readiness read; null when not reported. */
  gate: ReadinessGate | null;
  /** Whether the readiness report was read (the gate's absence then means "not reported"). */
  gatesRead: boolean;
  controlHeld: boolean;
  experimentActive: boolean;
  /** This computer's clock, for the records' ages. */
  now: number;
}

/** Why every Apply is unavailable right now, apart from the record; "" when none. */
function lockReason(p: CalibrationCardProps): string {
  if (!p.controlHeld) return "another tab or client holds control";
  if (p.experimentActive) return "a run is active: calibration is applied only while the experiment is idle";
  if (p.calibration.applying) return `the apply of ${p.calibration.applying} is pending`;
  return "";
}

/** One Apply / Re-apply button: disabled with its reason as title (and said in the card). */
function ApplyButton({ p, record, action, list, primary }: { p: CalibrationCardProps; record: CalibrationRecord; action: ApplyAction; list: CalibrationRecordsList; primary?: boolean }) {
  const why = lockReason(p) || applyAvailability(record, list).reason;
  const factor = list.current_factor;
  return (
    <Button size="small" variant={primary ? "primary" : "secondary"} disabled={why !== "" || typeof factor !== "number"} title={why || undefined}
      data-testid={`calibration-${action === "Apply" ? "apply" : "reapply"}-${record.record_id}`}
      onClick={() => { if (typeof factor === "number") void p.calibration.apply(record.record_id, factor); }}>
      {p.calibration.applying === record.record_id ? "Applying…" : `${action} ${record.record_id}`}
    </Button>
  );
}

/** The facts of one record: what it stands for, measured on which setup, from which run. */
function recordFacts(r: CalibrationRecord, list: CalibrationRecordsList, now: number, proposal: boolean): Fact[] {
  const cv = recordCv(r);
  return [
    { label: "Record", value: <span className="ws-plan-id">{r.record_id}</span> },
    proposal
      ? { label: "Pixel → µm", value: `${factorChange(list.current_factor, r.results?.proposed_pixel_to_micron)} · current → proposed` }
      : { label: "Pixel → µm", value: factorText(recordFactor(r)) },
    { label: "Evidence", value: `${r.results?.valid_objects ?? "Not recorded"} valid objects · diameter CV ${cv === null ? "not recorded" : `${Number(cv.toFixed(2))} %`}` },
    { label: r.status === "applied" ? "Applied" : "Derived", value: `${ageText(r.status === "applied" ? r.status_at_utc : r.created_at_utc, now)} ago · ${r.status === "applied" ? r.status_at_utc : r.created_at_utc}` },
    { label: "Method", value: methodText(r.method ?? {}) },
    { label: "Identity", value: identityText(r.identity) },
    { label: "Source run", value: sourceText(r) },
  ];
}

export function CalibrationCard(p: CalibrationCardProps) {
  const { calibration: c, gate } = p;
  const list = c.list?.valid ? c.list : null;
  const records = list?.records ?? [];
  const inEffect = list ? records.find((r) => r.record_id === list.in_effect_record_id) ?? null : null;
  const applied = list ? records.find((r) => r.record_id === list.applied_record_id) ?? null : null;
  const appliedOut = applied && !inEffect && applied.status === "applied" ? applied : null;
  const proposed = records.filter((r) => r.status === "proposed");
  const insufficient = records.filter((r) => r.status === "insufficient");
  const fix = fixAction(gate, list);
  const fixText = gate ? gateRemediation(gate) : "";
  const anyAction = !!list && (!!appliedOut || proposed.some((r) => applyAvailability(r, list).action !== null));
  const sel = c.history?.recordId;

  const columns: DataTableColumn<CalibrationRecord>[] = [
    { key: "record_id", header: "Record", emphasis: true },
    { key: "status", header: "Status", cell: (r) => <StateChip size="small" tone={statusTone(r, list!)}>{statusText(r, list!)}</StateChip> },
    { key: "factor", header: "µm/px", numeric: true, cell: (r) => { const f = recordFactor(r); return f === null ? null : String(Number(f.toPrecision(6))); } },
    { key: "objects", header: "Objects", numeric: true, cell: (r) => r.results?.valid_objects ?? null },
    { key: "cv", header: "CV %", numeric: true, cell: (r) => { const v = recordCv(r); return v === null ? null : String(Number(v.toFixed(2))); } },
    { key: "created", header: "Derived", cell: (r) => r.created_at_utc || null },
    { key: "method", header: "Method", cell: (r) => methodText(r.method ?? {}) },
  ];

  return (
    <Card className="ws-card--wide" title="Calibration" data-testid="workspace-calibration"
      subtitle="Pixel → µm from a calibration run of rigid size beads. The records are the backend's; nothing applies on its own, and no method is changed."
      status={gate ? <StateChip tone={gateTone(gate.status)}>{gateText(gate)}</StateChip>
        : <StateChip tone="unknown">{p.gatesRead ? "Unknown · gate not reported" : "Unknown · gates not read yet"}</StateChip>}>
      <FactList data-testid="calibration-facts" facts={[
        {
          label: <span data-gate-id="calibration.record" title="calibration.record">Calibration gate<span className="ws-gate-id">calibration.record</span></span>,
          value: gate ? (
            <span className="ws-gate">
              <span>{gateText(gate)}</span>
              {fixText ? <span className="ws-gate-fix" data-testid="calibration-fix">{`Fix: ${fixText}`}</span> : null}
              {fix && list ? <ApplyButton p={p} record={fix.record} action={fix.action} list={list} primary /> : null}
            </span>
          ) : (p.gatesRead ? "Unknown · not reported" : "Unknown · not read yet"),
          // The state is the card's chip; the row says it in words, with the Fix and its button.
          tone: gate ? gateFactTone(gate.status) : "unknown",
        },
        {
          label: "In effect",
          value: !list ? (p.experimentActive ? "Unknown · not read while a run is active" : c.error ? `Unknown · ${c.error}` : "Unknown · not read yet")
            : inEffect ? `${inEffect.record_id} · ${factorText(recordFactor(inEffect))}` : "None in effect",
          tone: !list ? "unknown" : inEffect ? "confirmed" : undefined,
        },
        { label: "Current factor", value: list ? factorText(list.current_factor) : "Unknown", tone: list && typeof list.current_factor === "number" ? undefined : "unknown" },
        { label: "Current setup", value: list?.current_identity ? `${identityText(list.current_identity)}${list.current_identity.known ? "" : " · unknown, so no record can be confirmed"}` : "Unknown",
          tone: list?.current_identity?.known ? undefined : "unknown" },
      ]} />
      {list?.store?.error ? (
        <Notice tone="blocked" title="The calibration store cannot be read" data-testid="calibration-store-error">
          {`${list.store.error}. Every change is refused until it is repaired; nothing in it is lost.`}
        </Notice>
      ) : null}

      {c.outcome ? <Notice tone={c.outcome.tone} title={c.outcome.title} data-testid="calibration-outcome">{c.outcome.text}</Notice> : null}
      {!p.controlHeld && anyAction ? (() => { const b = controlBlock("Apply"); return (
        <Notice tone={b.tone} title={b.title} data-testid="calibration-control">{b.text}</Notice>
      ); })() : null}

      {inEffect && list ? (
        <section className="ws-cal-section" data-testid="calibration-in-effect">
          <h4 className="ws-cal-heading">In effect</h4>
          <FactList facts={recordFacts(inEffect, list, p.now, false)} />
        </section>
      ) : null}

      {appliedOut && list ? (
        <Notice tone="warn" title={`Applied, not in effect · ${appliedOut.record_id}`} data-testid="calibration-not-in-effect"
          actions={applyAvailability(appliedOut, list).action ? <ApplyButton p={p} record={appliedOut} action="Re-apply" list={list} /> : undefined}>
          {notInEffectText(appliedOut, list)}
          {applyAvailability(appliedOut, list).reason ? ` Re-apply is unavailable: ${applyAvailability(appliedOut, list).reason}.` : ""}
        </Notice>
      ) : null}

      {proposed.length && list ? (
        <section className="ws-cal-section" data-testid="calibration-proposed">
          <h4 className="ws-cal-heading">{`Proposed · ${proposed.length}`}</h4>
          {proposed.map((r) => {
            const a = applyAvailability(r, list);
            return (
              <div key={r.record_id} className="ws-cal-record" data-testid={`calibration-proposed-${r.record_id}`}>
                <FactList facts={[
                  ...recordFacts(r, list, p.now, true),
                  { label: "Applicable", value: a.action && !a.reason ? "Yes · on this setup" : `No · ${a.reason || "not applicable"}`, tone: a.action && !a.reason ? undefined : "warn" },
                ]} />
                {a.action ? <div className="ws-cal-actions"><ApplyButton p={p} record={r} action={a.action} list={list} /></div> : null}
              </div>
            );
          })}
        </section>
      ) : null}

      {insufficient.length ? (
        <section className="ws-cal-section" data-testid="calibration-insufficient">
          <h4 className="ws-cal-heading">{`Insufficient · ${insufficient.length} · never applicable`}</h4>
          {insufficient.slice(0, INSUFFICIENT_SHOWN).map((r) => (
            <div key={r.record_id} className="ws-cal-record" data-testid={`calibration-insufficient-${r.record_id}`}>
              <FactList facts={[
                { label: "Record", value: <span className="ws-plan-id">{r.record_id}</span> },
                { label: "Failed gates", value: <ul className="ws-issue-list">{failedGates(r).map((g) => <li key={g}>{g}</li>)}</ul>, tone: "blocked" },
                { label: "Derived", value: `${ageText(r.created_at_utc, p.now)} ago · ${r.created_at_utc}` },
                { label: "Source run", value: sourceText(r) },
              ]} />
            </div>
          ))}
          {insufficient.length > INSUFFICIENT_SHOWN ? <p className="ws-note">{`${insufficient.length - INSUFFICIENT_SHOWN} older insufficient records are in the list below.`}</p> : null}
        </section>
      ) : null}

      <Notice tone="info" title="Running a calibration" data-testid="calibration-run">
        {`${RUN_CALIBRATION_TEXT} ${DERIVATION_TEXT}`}
        <span className="ws-detail" data-testid="calibration-purpose-gap">{PURPOSE_GAP_TEXT}</span>
      </Notice>

      {list ? (
        <section className="ws-cal-section" data-testid="calibration-list">
          <DataTable columns={columns} rows={records.slice(0, CALIBRATION_LIST_ROWS)} rowKey={(r) => r.record_id} density="compact"
            selectedKey={sel} onSelect={(key) => c.select(key === sel ? null : key)} missing={<span title="Not recorded">–</span>}
            caption={`Every record, newest first: applied, superseded, invalidated, proposed, insufficient · – = not recorded · select one for its versions`}
            empty="No calibration records yet: run a calibration method to derive one." />
          {records.length > CALIBRATION_LIST_ROWS ? <p className="ws-note">{`${records.length - CALIBRATION_LIST_ROWS} older records are not shown.`}</p> : null}
          {c.history ? (
            <div className="ws-cal-record" data-testid="calibration-history">
              <h4 className="ws-cal-heading">{`Versions of ${c.history.recordId}`}</h4>
              {c.history.error ? <p className="ws-note">{`Unknown · ${c.history.error}`}</p>
                : !c.history.reply ? <p className="ws-note">Reading…</p>
                : !c.history.reply.found ? <p className="ws-note">{`Not found · ${c.history.reply.message}`}</p>
                : <FactList facts={c.history.reply.history.map((v) => ({
                  label: `v${v.version} · ${v.status}`,
                  value: `${v.status_at_utc}${v.status_reason ? ` · ${v.status_reason}` : ""}${v.apply ? ` · ${factorChange(v.apply.previous_factor, v.apply.new_factor)}` : ""}`,
                }))} />}
            </div>
          ) : null}
        </section>
      ) : null}
    </Card>
  );
}
