// One poll of the pump and autofocus status for every consumer (#797). `HardwareControls` used to
// hold the only 1 s poll of `fetch_pump_status` (both slots) and `fetch_autofocus_status`; the
// telemetry sidebar now reads the pumps too. Rather than a second poll, the read lives here, in one
// module-level poller: the first subscriber starts the interval, the last one stops it, and every
// subscriber sees the same snapshot. `refresh()` reads now (after a command) and resolves when the
// read has settled; a reply from before the poll was stopped or restarted is dropped.
//
// Reads only: nothing here sends a command.
import { useCallback, useEffect, useState } from "react";
import { bridge, type AutofocusStatus, type PumpStatus } from "../bridge";

export const PUMP_STATUS_POLL_MS = 1000;

export interface PumpStatusSnapshot {
  /** Each slot's last valid status (Sample, Sheath); null when not read, invalid or failed. */
  pumps: readonly [PumpStatus | null, PumpStatus | null];
  /** The autofocus status read with them; null when not read, invalid or failed. */
  focus: AutofocusStatus | null;
  /** The last read's failure, "" when it succeeded (or was not made). */
  error: string;
  /** True once a read has answered since the poll started. */
  read: boolean;
}

const EMPTY: PumpStatusSnapshot = { pumps: [null, null], focus: null, error: "", read: false };

type Listener = (s: PumpStatusSnapshot) => void;

/** The module-level poller: one interval however many consumers. Exported for tests. */
export const pumpStatusPoller = {
  snapshot: EMPTY as PumpStatusSnapshot,
  listeners: new Set<Listener>(),
  timer: 0,
  generation: 0,
  inFlight: null as Promise<void> | null,
  /** How many reads were made since the poll last started (tests count the poll, not the consumers). */
  reads: 0,

  subscribe(l: Listener): () => void {
    this.listeners.add(l);
    if (this.listeners.size === 1) this.start();
    l(this.snapshot);
    return () => {
      this.listeners.delete(l);
      if (this.listeners.size === 0) this.stop();
    };
  },
  start() {
    this.generation += 1;
    this.reads = 0;
    this.set(EMPTY);
    void this.read();
    this.timer = window.setInterval(() => void this.read(), PUMP_STATUS_POLL_MS);
  },
  stop() {
    this.generation += 1;
    window.clearInterval(this.timer);
    this.timer = 0;
    this.inFlight = null;
    this.snapshot = EMPTY;
  },
  set(s: PumpStatusSnapshot) {
    this.snapshot = s;
    for (const l of this.listeners) l(s);
  },
  /** Read now; one read at a time (a read asked for while one is in flight joins it). */
  read(): Promise<void> {
    if (this.inFlight) return this.inFlight;
    const started = this.generation;
    this.reads += 1;
    this.inFlight = (async () => {
      try {
        const [sample, sheath, focus] = await Promise.all([bridge.fetchPumpStatus(0), bridge.fetchPumpStatus(1), bridge.fetchAutofocusStatus()]);
        if (started !== this.generation) return;
        this.set({ pumps: [sample.valid ? sample : null, sheath.valid ? sheath : null], focus: focus.valid ? focus : null, error: "", read: true });
      } catch (failure) {
        if (started !== this.generation) return;
        this.set({ pumps: [null, null], focus: null, error: String(failure), read: true });
      } finally {
        if (started === this.generation) this.inFlight = null;
      }
    })();
    return this.inFlight;
  },
};

export interface PumpStatusReader extends PumpStatusSnapshot {
  /** Read now (after a command); resolves once the read has settled. */
  refresh: () => Promise<void>;
}

/** The shared pump and autofocus status, polled every second while any consumer is mounted with
 *  `ready`; the empty snapshot otherwise. */
export function usePumpStatus(ready: boolean): PumpStatusReader {
  const [snapshot, setSnapshot] = useState<PumpStatusSnapshot>(EMPTY);
  useEffect(() => {
    if (!ready) { setSnapshot(EMPTY); return; }
    return pumpStatusPoller.subscribe(setSnapshot);
  }, [ready]);
  const refresh = useCallback(() => (ready && pumpStatusPoller.listeners.size ? pumpStatusPoller.read() : Promise.resolve()), [ready]);
  return { ...snapshot, refresh };
}
