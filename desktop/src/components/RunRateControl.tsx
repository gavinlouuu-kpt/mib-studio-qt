import { QUALIFIED_NOTE, pendingNotice, rateSummary, strobeSummary, type RunFpsStatus } from "../runFps";

// The Run frame rate and exposure of the PZ7035 (bridge ABI 38), read only: they are set by the PZ7035 camera document in Config → Camera document, the way a
// MIB camera is set by its script. Shows what the instrument is set to (and runs at), the strobe geometry, and the qualification note.
export function RunRateControl({ status }: { status: RunFpsStatus | null | undefined }) {
  if (!status?.available || status.fps == null) return null;
  const pending = pendingNotice(status);
  return (
    <div className="run-rate" data-testid="run-rate">
      <p className="mono" data-testid="run-rate-timing">
        Run: {rateSummary({ fps: status.fps, exposure_mode: status.exposure_mode ?? "auto", exposure_us: status.exposure_us ?? 0, shs: status.shs ?? 64, actual_fps: status.actual_fps ?? status.fps })}
      </p>
      {status.exposure_start_us != null && <p className="mono muted" data-testid="run-rate-strobe-window">
        {strobeSummary({ exposure_start_us: status.exposure_start_us, exposure_end_us: status.exposure_end_us ?? 0, strobe_start_us: status.strobe_start_us ?? 0, strobe_end_us: status.strobe_end_us ?? 0 })}
      </p>}
      {status.strobe_warning && <p role="alert" data-testid="run-rate-strobe">{status.strobe_warning}</p>}
      {pending && <p role="alert" data-testid="run-rate-pending">{pending}</p>}
      <p data-testid="run-rate-qualification" className={status.qualified ? "muted" : undefined}>{QUALIFIED_NOTE}</p>
      <p className="muted">Change the rate and exposure in Config → Camera document.</p>
    </div>
  );
}
