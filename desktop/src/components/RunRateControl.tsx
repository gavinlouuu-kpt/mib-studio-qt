import { useEffect, useState } from "react";
import { QUALIFIED_NOTE, applyNotice, discardQuestion, draftTiming, exposureText, fpsText, parseRunFps, pendingNotice, strobeNotice, type RunFpsStatus } from "../runFps";

// The Run frame rate of the PZ7035 (bridge ABI 38): the rate, its exposure (150 µs at every rate) and whether the LED strobe still falls in it, the 5,000 fps qualification note, and what applying does.
// The backend refuses a change during an experiment or recording and while a stopped Run's ring is held unless the operator agreed (the question below).
export function RunRateControl({ status, ringFrames, disabledReason, onApply, confirm }: {
  status: RunFpsStatus | null | undefined;
  /** Frames in the held ring (for the discard question). */
  ringFrames: number;
  /** Why the rate cannot be changed now (an experiment or recording runs); empty when it can. */
  disabledReason: string;
  onApply: (fps: number, acknowledgeDiscardRing: boolean) => Promise<{ ok: boolean; message: string }>;
  confirm: (message: string) => Promise<boolean>;
}) {
  const [draft, setDraft] = useState("");
  const [edited, setEdited] = useState(false);
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState<{ ok: boolean; message: string } | null>(null);
  useEffect(() => { if (!edited && status?.fps != null) setDraft(String(status.fps)); }, [status?.fps, edited]);
  if (!status?.available) return null;
  const parsed = parseRunFps(draft, status.min_fps, status.max_fps);
  const timing = parsed.ok ? draftTiming(parsed.fps) : null;
  const unchanged = parsed.ok && parsed.fps === status.fps;
  const apply = async () => {
    if (!parsed.ok || busy) return;
    const question = discardQuestion(status, ringFrames);
    if (question && !(await confirm(question))) return;
    setBusy(true);
    try {
      const r = await onApply(parsed.fps, !!question);
      setResult(r);
      if (r.ok) setEdited(false);
    } finally { setBusy(false); }
  };
  const pending = pendingNotice(status);
  const note = applyNotice(status, ringFrames);
  return (
    <div className="run-rate" data-testid="run-rate">
      <label>Run rate{" "}
        <input aria-label="Run rate in frames per second" inputMode="numeric" value={draft} disabled={busy || !!disabledReason}
          onChange={(e) => { setDraft(e.target.value); setEdited(true); setResult(null); }} />
        {" "}fps
      </label>
      <button onClick={() => void apply()} disabled={!parsed.ok || busy || !!disabledReason || (unchanged && !status.pending)}
        title={disabledReason || (parsed.ok ? undefined : parsed.reason)}>
        {busy ? "Applying…" : "Apply"}
      </button>
      {timing && (
        <span className="mono" data-testid="run-rate-timing">
          {" "}exposure {exposureText(timing.exposureUs)} · sensor {fpsText(timing.actualFps, 1)} fps
        </span>
      )}
      {timing && strobeNotice(timing) && <p role="alert" data-testid="run-rate-strobe">{strobeNotice(timing)}</p>}
      {!parsed.ok && draft.trim() !== "" && <p role="alert" data-testid="run-rate-invalid">{parsed.reason}</p>}
      {disabledReason && <p data-testid="run-rate-disabled">{disabledReason}</p>}
      {note && !disabledReason && <p data-testid="run-rate-note">{note}</p>}
      {pending && <p role="alert" data-testid="run-rate-pending">{pending}</p>}
      {!status.qualified && <p data-testid="run-rate-qualification">{QUALIFIED_NOTE}</p>}
      {status.qualified && <p data-testid="run-rate-qualification" className="muted">{QUALIFIED_NOTE}</p>}
      {result && <p role={result.ok ? "status" : "alert"} data-testid="run-rate-result">{result.message}</p>}
    </div>
  );
}
