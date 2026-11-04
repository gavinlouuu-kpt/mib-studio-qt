// @vitest-environment jsdom
// HW-034 (epic #868): the ring width in the sidebar dims with a tooltip when older than the configured staleness limit, and the
// status bar carries Qt's 'Ring width=' value.
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import type { AutofocusStatus } from '../../bridge';
import { DESKTOP_CAPABILITIES } from '../../platformCapabilities';
import { StudioContext } from '../../studio/StudioContext';
import type { Studio } from '../../studio/useStudioController';
import { StatusBar } from './StatusBar';
import { TelemetrySidebar } from './Sidebar';

const af = (over: Partial<AutofocusStatus>): AutofocusStatus => ({valid: true, connected: true, enabled: false, current_voltage: 10, com_port: 5, backend_name: 'oeabt', endpoint_id: '/dev/COM5',
  average_ring_ratio: 20.2, median_ring_ratio: 20.123, last_ring_ratio_update_us: 5, ring_ratio_age_us: 200_000, ...over});
const studio = (over: object) => ({
  sidebarCollapsed: false, caps: DESKTOP_CAPABILITIES, backgroundSet: false, displayFps: 30, hostProcessing: true, stats: null, algoFps: 0, validFps: 0, invalidFps: 0,
  running: false, camStatus: 'idle', dataRate: 0, pz7035: false, instrument: null, afStatus: af({}), afStaleMs: 1500, expState: 0, expActive: false, runOutcome: null,
  expStatus: null, elapsedWallSeconds: null, toggleSidebar: () => {}, abi: 1, ready: true, reviewing: false, recording: false, setShowLog: () => {}, showLog: false, log: [], ...over,
}) as unknown as Studio;
let host: HTMLDivElement, root: Root;
const render = async (value: Studio) => act(async () => root.render(<StudioContext.Provider value={value}><TelemetrySidebar /><StatusBar /></StudioContext.Provider>));
const ringValue = () => Array.from(host.querySelectorAll('.side-row')).find(row => row.textContent?.startsWith('Ring width:'))!.querySelector('.v') as HTMLElement;
beforeEach(() => { Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true}); host = document.createElement('div'); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe('ring width display (HW-034)', () => {
  it('shows the median ring width, not dimmed while the sample is newer than the configured limit', async () => {
    await render(studio({}));
    expect(ringValue().textContent).toBe('20.123');
    expect(ringValue().className).not.toContain('dim');
    expect(ringValue().title).toBe('');
  });
  it('dims the value and says how old it is once it is older than ring_ratio_stale_ms (not the fixed 1000 ms)', async () => {
    await render(studio({afStatus: af({ring_ratio_age_us: 1_200_000}), afStaleMs: 1500}));
    expect(ringValue().className).not.toContain('dim');                      // 1200 ms is inside a 1500 ms limit
    await render(studio({afStatus: af({ring_ratio_age_us: 1_700_000}), afStaleMs: 1500}));
    expect(ringValue().className).toContain('dim');
    expect(ringValue().title).toBe('Stale: last update 1700 ms ago');
    await render(studio({afStatus: af({ring_ratio_age_us: 600_000}), afStaleMs: 500}));
    expect(ringValue().className).toContain('dim');                          // a shorter configured limit counts
  });
  it('shows an em dash and a dimmed value until the first sample', async () => {
    await render(studio({afStatus: af({last_ring_ratio_update_us: 0, ring_ratio_age_us: 0})}));
    expect(ringValue().textContent).toBe('—');
    expect(ringValue().className).toContain('dim');
  });
  it('the status bar carries Qt\'s Ring width= value, with the same staleness note, only where there is a nanopositioner', async () => {
    await render(studio({}));
    expect(host.querySelector('footer .metrics')!.textContent).toContain('Ring width=20.123 | Experiment:');
    await render(studio({afStatus: af({ring_ratio_age_us: 2_000_000})}));
    expect(host.querySelector('footer .metrics span[title]')!.getAttribute('title')).toBe('Stale: last update 2000 ms ago');
    await render(studio({afStatus: af({last_ring_ratio_update_us: 0})}));
    expect(host.querySelector('footer .metrics')!.textContent).toContain('Ring width=— | Experiment:');
    await render(studio({caps: {...DESKTOP_CAPABILITIES, autofocus: false}}));
    expect(host.querySelector('footer .metrics')!.textContent).not.toContain('Ring width');
  });
});
