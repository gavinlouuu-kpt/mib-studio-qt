import { useCallback, useEffect, useRef, useState } from "react";
import { bridge } from "../bridge";
import { emptyRunsText, runRow, type SsdRuns, type SsdStatus } from "../ssdView";

// The recorded runs of the SATA SSD, read only (#667 S1): the run table from pzrec, oldest first. Export, download and delete come with the
// storage layout and the export (S3). A run recovered after power loss shows its sentence and no numbers.
export function SsdRunsPanel({ status, active }: { status: SsdStatus | null; active: boolean }) {
  const [reply, setReply] = useState<SsdRuns | null>(null);
  const [error, setError] = useState("");
  const mounted = useRef(true);
  useEffect(() => { mounted.current = true; return () => { mounted.current = false; }; }, []);

  const load = useCallback(async () => {
    try {
      const next = await bridge.fetchSsdRuns();
      if (!mounted.current) return;
      setReply(next);
      setError("");
    } catch (e) {
      if (mounted.current) setError(e instanceof Error ? e.message : String(e));
    }
  }, []);

  // One read when opened, then every few seconds while visible; a run table changes only when a run starts or ends.
  useEffect(() => {
    if (!active) return;
    void load();
    const id = window.setInterval(() => void load(), 5000);
    return () => window.clearInterval(id);
  }, [active, load]);

  const rows = (reply?.ok ? reply.runs : []).map((r) => runRow(r, { open: status?.open_run, nowMs: Date.now() }));
  return (
    <div className="ssd-runs" data-testid="ssd-runs">
      {error && <p className="pending-note" role="alert">{error}</p>}
      {rows.length === 0 ? (
        <p className="mono" role="status" data-testid="ssd-runs-empty">{emptyRunsText(reply, status)}</p>
      ) : (
        <table className="ssd-table" aria-label="Runs on the SSD">
          <thead>
            <tr><th>#</th><th>Name</th><th>Started</th><th>Duration</th><th>Frames seen</th><th>Stored</th><th>Empty skipped</th><th>Dropped</th><th>Size</th><th>Completion</th></tr>
          </thead>
          <tbody>
            {rows.map((r) => (
              <tr key={r.id} className={r.deleted ? "deleted" : ""} data-testid={`ssd-run-${r.id}`}>
                <td>{r.id}</td>
                <td>{r.name}{r.deleted && " (deleted)"}</td>
                <td title={r.clockUnsynced ? "The board's clock was not synced when this run started: the date may be wrong." : undefined}>{r.started}{r.clockUnsynced && " *"}</td>
                <td>{r.duration}</td>
                <td>{r.seen}</td>
                <td>{r.stored}</td>
                <td>{r.empty}</td>
                <td>{r.dropped}</td>
                <td>{r.size}</td>
                <td className={`tone-${r.tone}`}>{r.completion}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </div>
  );
}
