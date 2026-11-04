// @vitest-environment jsdom
// Qt's sort-trigger counters in the status bar (HW-046): shown only on instruments with a host trigger, live on every tab.
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { SortTriggerStats } from './SortTriggerStats';
import { StudioContext } from '../../studio/StudioContext';
import type { Studio } from '../../studio/useStudioController';
import { bridge, type TriggerStatus } from '../../bridge';

vi.mock('../../bridge', () => ({ bridge: { fetchTriggerStatus: vi.fn() } }));
let host: HTMLDivElement, root: Root;
const status = (change: Partial<TriggerStatus> = {}): TriggerStatus => ({ valid: true, camera_attached: true, pulse_duration_us: 1, trigger_count: 0, last_onset_us: 0, last_object_id: -1, last_track_id: -1, periodic_active: false, periodic_interval_ms: 1000,
  valid_objects: 100, target_group_objects: 10, unserved_target_group_objects: 0, dropped_requests: 0, dropped_pulses: 0, dropped_valid_frames: 0, avg_target_latency_us: 300, max_target_latency_us: 700, ...change });
const render = async (studio: Partial<Studio>) => { await act(async () => root.render(<StudioContext.Provider value={studio as Studio}><SortTriggerStats /></StudioContext.Provider>)); await act(async () => { await Promise.resolve(); await Promise.resolve(); }); };
beforeEach(() => { Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true }); vi.clearAllMocks(); vi.mocked(bridge.fetchTriggerStatus).mockResolvedValue(status()); host = document.createElement('div'); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe('SortTriggerStats', () => {
  it("shows Qt's counters for the sort trigger", async () => {
    await render({ ready: true, caps: { trigger: true } as Studio['caps'] });
    expect(host.textContent).toContain('Targets=10 (10.0% of valid), served=100.0%, lost=0 | SortLat=300 us (max 700)');
  });
  it('renders nothing and does not poll without a host trigger (PZ7035) or before the backend is ready', async () => {
    await render({ ready: true, caps: { trigger: false } as Studio['caps'] });
    await render({ ready: false, caps: { trigger: true } as Studio['caps'] });
    expect(host.textContent).toBe(''); expect(bridge.fetchTriggerStatus).not.toHaveBeenCalled();
  });
  it('renders nothing for a server that does not report the counters', async () => {
    vi.mocked(bridge.fetchTriggerStatus).mockResolvedValue(status({ valid_objects: undefined }));
    await render({ ready: true, caps: { trigger: true } as Studio['caps'] });
    expect(host.textContent).toBe('');
  });
});
