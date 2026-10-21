// Prepare's read of the authoritative backend readiness gates (#713): always without a
// destination (PREPARE_OUTPUT_PATH = ""), so the backend reports storage.output Unavailable and
// creates no probe file. Polled while Prepare is shown and no run is active; one request at a
// time (a poll meanwhile is skipped; an explicit refresh meanwhile drops the read in flight and
// reads again). A failed read clears the snapshot rather than showing stale gates as current.
import { useCallback, useEffect, useRef, useState } from "react";
import { bridge } from "../bridge";
import type { ExperimentReadiness } from "../eventAdapter";
import { PREPARE_OUTPUT_PATH, PREPARE_READINESS_POLL_MS } from "./prepareModel";

export interface PrepareReadiness {
  snapshot: ExperimentReadiness | null;
  error: string;
  /** Read again now (a read; it never names a path). */
  refresh: () => void;
}

export function usePrepareReadiness(active: boolean): PrepareReadiness {
  const [snapshot, setSnapshot] = useState<ExperimentReadiness | null>(null);
  const [error, setError] = useState("");
  const inFlight = useRef(false);
  const mounted = useRef(true);
  // An explicit refresh (Refresh, Read gates again, after a calibration apply) bumps it: a read in
  // flight from before is then dropped and read once more, so the refresh is never lost.
  const generation = useRef(0);
  const again = useRef(false);
  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  const read = useCallback(async (explicit = false) => {
    if (explicit) generation.current += 1;
    if (inFlight.current) { if (explicit) again.current = true; return; }
    inFlight.current = true;
    try {
      do {
        again.current = false;
        const started = generation.current;
        try {
          const r = await bridge.fetchExperimentReadiness(PREPARE_OUTPUT_PATH);
          if (mounted.current && started === generation.current) { setSnapshot(r); setError(""); }
        } catch (e) {
          if (mounted.current && started === generation.current) { setSnapshot(null); setError(String(e)); }
        }
      } while (again.current && mounted.current);
    } finally {
      inFlight.current = false;
    }
  }, []);

  useEffect(() => {
    if (!active) { setSnapshot(null); setError(""); return; }
    void read();
    const id = window.setInterval(() => void read(), PREPARE_READINESS_POLL_MS);
    return () => window.clearInterval(id);
  }, [active, read]);

  const refresh = useCallback(() => { if (active) void read(true); }, [active, read]);
  return { snapshot, error, refresh };
}
