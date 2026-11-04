// @vitest-environment node
// Qt's sort-trigger counters (StatsDisplayManager.cpp:80-111), HW-046.
import { describe, expect, it } from 'vitest';
import type { TriggerStatus } from '../../bridge';
import { sortTriggerFunnel, sortTriggerSummary } from './sortTriggerModel';

const status = (change: Partial<TriggerStatus> = {}): TriggerStatus => ({ valid: true, camera_attached: true, pulse_duration_us: 1, trigger_count: 0, last_onset_us: 0, last_object_id: -1, last_track_id: -1,
  periodic_active: false, periodic_interval_ms: 1000, valid_objects: 200, target_group_objects: 50, unserved_target_group_objects: 3, dropped_requests: 1, dropped_pulses: 1,
  dropped_valid_frames: 0, avg_target_latency_us: 412.4, max_target_latency_us: 903, ...change });

describe('sort-trigger funnel', () => {
  it("computes conversion (targets / valid), lost (unserved + dropped requests + dropped pulses) and served as Qt does", () => {
    expect(sortTriggerFunnel(status())).toEqual({ targets: 50, conversionPct: 25, servedPct: 90, lost: 5, latencyAvgUs: 412.4, latencyMaxUs: 903, droppedValidFrames: 0 });
  });
  it('is 0 % conversion without valid objects and 100 % served without targets; lost never exceeds the targets in the served share', () => {
    expect(sortTriggerFunnel(status({ valid_objects: 0, target_group_objects: 0 }))).toMatchObject({ conversionPct: 0, servedPct: 100 });
    expect(sortTriggerFunnel(status({ target_group_objects: 2, unserved_target_group_objects: 9 }))).toMatchObject({ servedPct: 0, lost: 11 });
  });
  it('is null for a server that does not report the counters, or an invalid status', () => {
    expect(sortTriggerFunnel(null)).toBeNull();
    expect(sortTriggerFunnel(status({ valid: false }))).toBeNull();
    expect(sortTriggerFunnel(status({ valid_objects: undefined }))).toBeNull();
  });
});
describe('summary text', () => {
  it("uses Qt's wording", () => {
    expect(sortTriggerSummary(status())).toBe('Targets=50 (25.0% of valid), served=90.0%, lost=5 | SortLat=412 us (max 903)');
  });
  it('adds DroppedValid only when frames were dropped', () => {
    expect(sortTriggerSummary(status({ dropped_valid_frames: 7 }))).toBe('Targets=50 (25.0% of valid), served=90.0%, lost=5 | SortLat=412 us (max 903) | DroppedValid=7');
    expect(sortTriggerSummary(null)).toBeNull();
  });
});
