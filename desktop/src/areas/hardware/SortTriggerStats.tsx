// Hardware (epic #868, HW-046): Qt's sort-trigger counters in the status bar. It reads the trigger status itself
// (once a second) so the numbers stay live on every tab, not only while the Monitoring view polls; rendered only on
// instruments with a host sorter trigger (capabilities.trigger).
import { useEffect, useState } from "react";
import { bridge, type TriggerStatus } from "../../bridge";
import { useStudio } from "../../studio/StudioContext";
import { sortTriggerSummary } from "./sortTriggerModel";

export function SortTriggerStats() {
  const { ready, caps } = useStudio();
  const [status, setStatus] = useState<TriggerStatus | null>(null);
  const enabled = ready && caps.trigger;
  useEffect(() => {
    if (!enabled) { setStatus(null); return; }
    let live = true, polling = false;
    const poll = async () => {
      if (polling) return;
      polling = true;
      try { const next = await bridge.fetchTriggerStatus(); if (live) setStatus(next); }
      catch { /* backend gone: the next tick reports it */ }
      finally { polling = false; }
    };
    void poll();
    const timer = window.setInterval(() => void poll(), 1000);
    return () => { live = false; window.clearInterval(timer); };
  }, [enabled]);
  const summary = sortTriggerSummary(status);
  return enabled && summary ? <span className="sort-trigger-stats" title={`Sort trigger (the status bar clips long lines): ${summary}. Targets: valid objects in the target group; served: targets that got a pulse; lost: unserved extras, evicted requests and undriven pulses; SortLat: acquisition to pulse.`}> | {summary}</span> : null;
}
