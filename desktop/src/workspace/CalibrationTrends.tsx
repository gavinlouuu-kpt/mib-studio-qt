import { useId, useState } from "react";
import { DataTable, Notice, type DataTableColumn } from "@mib/ui";
import type { CalibrationRecord } from "../calibrationRecords";
import { calibrationTrend, latestTrendRecords, TREND_METRICS, trendSetupKey, trendSetupLabel, type TrendMetricId, type TrendPoint } from "./calibrationTrendModel";

/** Record values only. Selection reuses calibration_record_get; no Apply/threshold/exclusion API. */
export function CalibrationTrends({ records, selectedId, onSelect }: {
  records: readonly CalibrationRecord[]; selectedId?: string; onSelect: (id: string) => void;
}) {
  const id = useId();
  const [metric, setMetric] = useState<TrendMetricId>("diameter_cv");
  const [setup, setSetup] = useState("all");
  const latest = latestTrendRecords(records);
  const setups = new Map(latest.map(r => [trendSetupKey(r), r]));
  const activeSetup = setups.has(setup) ? setup : "all";
  const spec = TREND_METRICS.find(m => m.id === metric)!;
  const points = calibrationTrend(activeSetup === "all" ? latest : latest.filter(r => trendSetupKey(r) === activeSetup), metric);
  const plotted = points.filter(p => p.value !== null && p.timestamp !== null);
  const values = plotted.flatMap(p => p.limit === null ? [p.value!] : [p.value!, p.limit]);
  const minY = values.length ? Math.min(...values) : 0, maxY = values.length ? Math.max(...values) : 1;
  const minX = plotted[0]?.timestamp ?? 0, maxX = plotted[plotted.length - 1]?.timestamp ?? 1;
  // Coordinate transforms only, not scientific calculations; never pool or fit measurements.
  const x = (p: TrendPoint) => maxX === minX ? 320 : 60 + 520 * (p.timestamp! - minX) / (maxX - minX);
  const y = (v: number) => maxY === minY ? 95 : 170 - 150 * ((v - minY) / (maxY - minY));
  const columns: DataTableColumn<TrendPoint>[] = [
    { key: "record", header: "Record · version", cell: p => `${p.record.record_id} · v${p.record.version}` },
    { key: "date", header: "Derived", cell: p => p.timestamp === null ? "Date unavailable" : p.record.created_at_utc },
    { key: "value", header: `${spec.label} (${spec.unit})`, cell: p => p.value ?? p.unavailable, numeric: true },
    { key: "limit", header: "Recorded gate limit", cell: p => p.limit, numeric: true },
    { key: "gate", header: "Recorded gate verdict", cell: p => p.passed === null ? "No gate recorded" : `${p.passed ? "Pass" : "Fail"}${p.gateReason ? ` · ${p.gateReason}` : ""}` },
    { key: "status", header: "Record status", cell: p => p.record.status },
    { key: "source", header: "Source run", cell: p => p.record.source?.run_id ?? "Not recorded" },
    { key: "setup", header: "Recorded setup / method", cell: p => trendSetupLabel(p.record) },
  ];
  return <section className="ws-cal-section" data-testid="calibration-trends">
    <h4 className="ws-cal-heading">Calibration measurement history</h4>
    <label htmlFor={`${id}-metric`}>Metric </label>
    <select id={`${id}-metric`} value={metric} onChange={e => setMetric(e.target.value as TrendMetricId)}>
      {TREND_METRICS.map(m => <option key={m.id} value={m.id}>{m.label} ({m.unit})</option>)}
    </select>
    <label htmlFor={`${id}-setup`}> Setup and method revision </label>
    <select id={`${id}-setup`} value={activeSetup} onChange={e => setSetup(e.target.value)} disabled={!setups.size}>
      <option value="all">All records · separate measurements, not a compatible comparison</option>
      {[...setups].map(([key, r]) =>
        <option key={key} value={key}>{trendSetupLabel(r)} · {r.record_id}</option>)}
    </select>
    <p className="ws-note">Recorded measurements, oldest first. Each point uses its own method revision and limits.
      Different or incomplete setup identities are not pooled; the all-record view does not assert compatibility. Cross marks are recorded limits, not a newly calculated threshold.
      Select a point or row for its record versions; nothing is applied.</p>
    {plotted.length ? <svg viewBox="0 0 640 220" role="group" aria-label={`${spec.label} history in ${spec.unit}`} style={{ width: "100%", maxWidth: 800 }}>
      <line x1="60" y1="20" x2="60" y2="170" stroke="currentColor" />
      <line x1="60" y1="170" x2="580" y2="170" stroke="currentColor" />
      <text x="2" y="25" fontSize="11">{maxY.toPrecision(4)}</text><text x="2" y="170" fontSize="11">{minY.toPrecision(4)}</text>
      <text x="60" y="195" fontSize="11">{new Date(minX).toISOString().slice(0, 10)}</text>
      <text x="580" y="210" textAnchor="end" fontSize="11">{new Date(maxX).toISOString().slice(0, 10)}</text>
      {plotted.map(p => <g key={p.record.record_id}>
        {p.limit !== null ? <path d={`M${x(p)-4},${y(p.limit)}h8 M${x(p)},${y(p.limit)-4}v8`} stroke="currentColor" opacity="0.55" /> : null}
        <circle cx={x(p)} cy={y(p.value!)} r={selectedId === p.record.record_id ? 7 : 5}
          fill={p.passed === false ? "none" : "currentColor"} stroke="currentColor" tabIndex={0} role="button"
          aria-label={`Select ${p.record.record_id}: ${p.value} ${spec.unit}`}
          onClick={() => onSelect(p.record.record_id)}
          onKeyDown={e => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); onSelect(p.record.record_id); } }}>
          <title>{`${p.record.record_id} v${p.record.version} · ${p.record.created_at_utc} · ${p.value} ${spec.unit} · ${p.passed === null ? "no gate" : p.passed ? "recorded pass" : "recorded fail"}`}</title>
        </circle>
      </g>)}
    </svg> : <Notice tone="info" title="No plottable recorded measurements">Missing measurements and dates remain in the table; no values are inferred.</Notice>}
    <DataTable columns={columns} rows={points} rowKey={p => p.record.record_id} selectedKey={selectedId}
      onSelect={key => onSelect(key)} density="compact" empty="No calibration records for this setup." />
    <p className="ws-note">Open markers mean a recorded gate failure. These are recorded verdicts, not current readiness.
      Trend exclusions and new threshold policies are not edited here; records and source recordings remain unchanged.</p>
  </section>;
}
