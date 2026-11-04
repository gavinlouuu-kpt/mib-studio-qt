// The run state widget (epic #866: MON-022, SHELL-059): Qt's RunStatusWidget, a glyph plus text in the Experiment
// toolbar. The glyph is decoration; the text always says the state, and the tooltip names the phase.
import type { RunPresentation } from "./runStatus";

export function RunStatusBadge({ presentation: p }: { presentation: RunPresentation }) {
  return (
    <span className={`run-status ${p.phase} tone-${p.tone}`} role="status" title={p.title} aria-label={`Run state: ${p.text}`} data-tour="monitoring-run-status" data-phase={p.phase}>
      <span className="run-status-glyph" aria-hidden="true">{p.glyph}</span>
      <span className="run-status-text">{p.text}</span>
    </span>
  );
}
