// Connect and Overview (epic #863, CONNECT-053): the Recording capacity panel under the Overview canvas.
// Qt's RecordingCapacityPanel: the planned window, the rate, the save stream, the buffer, the
// destination's rate and the four capacity gates. Words come from recordingCapacity.ts.
import type { RecordingCapacity } from "../../bridge";
import { describeRecordingCapacity } from "./recordingCapacity";

export function RecordingCapacityPanel({ view }: { view: RecordingCapacity | null }) {
  if (!view) {
    return <section className="capacity-panel" aria-label="Recording capacity" data-testid="recording-capacity">
      <strong>Recording capacity: <span className="capacity-status unknown">Unknown</span></strong>
      <p className="capacity-fact">Waiting for the backend…</p>
    </section>;
  }
  const text = describeRecordingCapacity(view);
  return <section className="capacity-panel" aria-label="Recording capacity" data-testid="recording-capacity" title={[...text.facts, ...text.gates.map((g) => g.text)].join("\n")}>
    <strong>Recording capacity: <span className={`capacity-status ${text.status.toLowerCase()}`}>{text.status}</span></strong>
    <ul className="capacity-facts">{text.facts.map((fact) => <li key={fact} className="capacity-fact">{fact}</li>)}</ul>
    <ul className="capacity-gates">{text.gates.map((gate) => <li key={gate.id} className={`capacity-gate ${gate.level}`}>{gate.text}</li>)}</ul>
  </section>;
}
