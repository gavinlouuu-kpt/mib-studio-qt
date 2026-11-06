// The standing Start readiness (epic #866: MON-028): what would block Start and what only warns, with the reason
// and the remediation the backend gives. Qt shows the capacity gates only in the refusal dialog at Start and a
// one-line summary beside the button; Warn gates (camera.rate near its limit, a short buffer, the profile's E-LUT
// warning) were never visible to a Tauri operator before Start was pressed, and then only the Fail ones.
import { gateStatusLabel, type ReadinessSummary } from "./startReadiness";

export function ReadinessAdvisories({ summary, notice }: { summary: ReadinessSummary | null; notice: string }) {
  if (!summary && !notice) return null;
  const gates = summary ? [...summary.blocked, ...summary.warnings] : [];
  return (
    <div className={`start-readiness ${summary?.tone ?? "warn"}`} data-tour="monitoring-readiness" data-tone={summary?.tone}>
      {summary && <p className="start-readiness-line" role="status">Start readiness: <strong>{summary.line}</strong></p>}
      {notice && <p role="alert" className="start-readiness-error">{notice}</p>}
      {gates.length > 0 && (
        <ul className="start-readiness-gates" aria-label="Start readiness gates">
          {gates.map((g) => (
            <li key={g.id} className={summary!.blocked.includes(g) ? "blocked" : "warning"}>
              <strong>{gateStatusLabel(g.status)}</strong> — <code>{g.id}</code>: {g.reason}
              {g.remediation && <span className="remedy"> → {g.remediation}</span>}
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}
