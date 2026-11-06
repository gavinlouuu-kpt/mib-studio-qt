// Hardware (epic #868, HW-046): the sort-trigger counters of Qt's status bar (StatsDisplayManager.cpp:80-111):
//   Targets=N (x% of valid), served=y%, lost=L | SortLat=A us (max M) [| DroppedValid=D]
// Conversion is the share of valid detections that entered the sort target group; served is the share of those that
// got a pulse (the rest were extra targets in a frame, evicted requests or undriven pulses); latency is the live
// acquisition-to-pulse average. The backend reports the raw counters (fetch_trigger_status, ABI next (#881, set at merge)).
import type { TriggerStatus } from '../../bridge';

export interface SortTriggerFunnel {
  targets: number; conversionPct: number; servedPct: number; lost: number;
  latencyAvgUs: number; latencyMaxUs: number; droppedValidFrames: number;
}
const count = (value: unknown) => (typeof value === 'number' && Number.isFinite(value) && value >= 0 ? value : null);

/** null when the server does not report the counters (older than ABI next (#881, set at merge)) or the status is not valid. */
export function sortTriggerFunnel(status: TriggerStatus | null): SortTriggerFunnel | null {
  if (!status?.valid) return null;
  const valid = count(status.valid_objects), targets = count(status.target_group_objects), unserved = count(status.unserved_target_group_objects);
  const droppedRequests = count(status.dropped_requests), droppedPulses = count(status.dropped_pulses);
  if (valid === null || targets === null || unserved === null || droppedRequests === null || droppedPulses === null) return null;
  const lost = unserved + droppedRequests + droppedPulses;
  return {
    targets, lost,
    conversionPct: valid > 0 ? 100 * targets / valid : 0,
    servedPct: targets > 0 ? 100 * (targets - Math.min(lost, targets)) / targets : 100,
    latencyAvgUs: count(status.avg_target_latency_us) ?? 0,
    latencyMaxUs: count(status.max_target_latency_us) ?? 0,
    droppedValidFrames: count(status.dropped_valid_frames) ?? 0,
  };
}

export function sortTriggerSummary(status: TriggerStatus | null): string | null {
  const f = sortTriggerFunnel(status);
  if (!f) return null;
  let text = `Targets=${f.targets} (${f.conversionPct.toFixed(1)}% of valid), served=${f.servedPct.toFixed(1)}%, lost=${f.lost}`
    + ` | SortLat=${f.latencyAvgUs.toFixed(0)} us (max ${f.latencyMaxUs})`;
  if (f.droppedValidFrames > 0) text += ` | DroppedValid=${f.droppedValidFrames}`;
  return text;
}
