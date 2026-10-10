import { useEffect, useRef, useState } from "react";
import { stripView, type SsdStatus } from "../ssdView";

/** Seconds the state has read STOPPING (0 otherwise), re-rendered twice a second while it does. */
function useStoppingSeconds(stopping: boolean): number | undefined {
  const since = useRef<number | null>(null);
  const [, tick] = useState(0);
  if (stopping && since.current === null) since.current = Date.now();
  if (!stopping) since.current = null;
  useEffect(() => {
    if (!stopping) return undefined;
    const id = window.setInterval(() => tick((n) => n + 1), 500);
    return () => window.clearInterval(id);
  }, [stopping]);
  return stopping && since.current !== null ? (Date.now() - since.current) / 1000 : undefined;
}

// The storage strip (#667 S1): what the SATA SSD is doing and why, in one line, with the recovery note kept below it.
// While a run stops it also shows the elapsed time: the stop waits for the frame ring to drain (about 8 s at 5 kHz).
export function SsdStrip({ status }: { status: SsdStatus | null }) {
  const stoppingSeconds = useStoppingSeconds(status?.state === "STOPPING");
  if (!status) return null;
  const v = stripView(status, stoppingSeconds);
  return (
    <div className={`ssd-strip ${v.tone}`} data-testid="ssd-strip" role="status">
      <span data-testid="ssd-strip-text">{v.text}</span>
      {v.detail && <div className="pending-note" data-testid="ssd-strip-detail">{v.detail}</div>}
    </div>
  );
}
