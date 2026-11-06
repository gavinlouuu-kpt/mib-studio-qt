// @vitest-environment jsdom
// useServiceMode (epic #868): a backend refusal of the mode is shown, not swallowed (App.tsx:1368), and a running
// periodic test stops when the Monitoring view is hidden, as Qt's hideEvent does (HW-045).
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { setBackendServiceMode, useServiceMode } from './useHardware';
import { bridge, type TriggerStatus } from '../../bridge';
import { confirm } from '../../transport/dialogs';
import type { OperatingMode } from '../../commissioning';

vi.mock('../../bridge', () => ({ bridge: { setServiceMode: vi.fn(), triggerPeriodicStop: vi.fn(), fetchTriggerStatus: vi.fn() } }));
vi.mock('../../transport/dialogs', () => ({ confirm: vi.fn() }));

let host: HTMLDivElement, root: Root;
const append = vi.fn(), setOperatingMode = vi.fn(), setTriggerArmed = vi.fn(), setTrigStatus = vi.fn();
const status = (periodic_active: boolean): TriggerStatus => ({ valid: true, camera_attached: true, pulse_duration_us: 1, trigger_count: 0, last_onset_us: 0, last_object_id: -1, last_track_id: -1, periodic_active, periodic_interval_ms: 1000 });
let api: ReturnType<typeof useServiceMode>;
function Probe(props: { mode?: OperatingMode; instrumentModes?: boolean; monitoringVisible?: boolean; periodic?: boolean; ready?: boolean; viewerOnly?: boolean }) {
  api = useServiceMode({ append, instrumentModes: props.instrumentModes ?? false, ready: props.ready ?? true, operatingMode: props.mode ?? 'operator', setOperatingMode, triggerArmed: false, setTriggerArmed,
    trigStatus: status(props.periodic ?? false), setTrigStatus, expActive: false, monitoringVisible: props.monitoringVisible ?? true, viewerOnly: props.viewerOnly ?? false });
  return null;
}
const render = async (props: Parameters<typeof Probe>[0] = {}) => { await act(async () => root.render(<Probe {...props} />)); await act(async () => { await Promise.resolve(); }); };
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true }); vi.clearAllMocks();
  vi.mocked(confirm).mockResolvedValue(true);
  vi.mocked(bridge.setServiceMode).mockResolvedValue({ ok: true, command: 0, operation_id: '0', message: 'ok' });
  vi.mocked(bridge.triggerPeriodicStop).mockResolvedValue({ ok: true, command: 8, operation_id: '0', message: 'stopped' });
  vi.mocked(bridge.fetchTriggerStatus).mockResolvedValue(status(false));
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe('setBackendServiceMode', () => {
  it('resolves to the refusal, whether it comes as a result or as an error', async () => {
    expect(await setBackendServiceMode(true)).toBe('');
    vi.mocked(bridge.setServiceMode).mockResolvedValue({ ok: false, command: 0, operation_id: '0', message: 'run in progress' });
    expect(await setBackendServiceMode(true)).toBe('run in progress');
    vi.mocked(bridge.setServiceMode).mockResolvedValue({ ok: false, command: 0, operation_id: '0', message: '' });
    expect(await setBackendServiceMode(true)).toBe('the instrument refused');
    vi.mocked(bridge.setServiceMode).mockRejectedValue(new Error('VIEWER_ONLY: another client controls this instrument'));
    expect(await setBackendServiceMode(false)).toBe('VIEWER_ONLY: another client controls this instrument');
  });
});

describe('entering and leaving Service mode', () => {
  it('on an instrument a refused request leaves the app in Operator mode and says why', async () => {
    vi.mocked(bridge.setServiceMode).mockResolvedValue({ ok: false, command: 0, operation_id: '0', message: 'an experiment is active' });
    await render({ instrumentModes: true });
    await act(async () => { await api.enterMode('service'); });
    expect(bridge.setServiceMode).toHaveBeenCalledWith(true);
    expect(setOperatingMode).not.toHaveBeenCalled();
    expect(append).toHaveBeenCalledWith('Service / Commissioning mode refused: an experiment is active');
    expect(append).not.toHaveBeenCalledWith('entered Service / Commissioning mode');
  });
  it('an accepted request enters Service mode; a declined confirmation does nothing', async () => {
    await render({ instrumentModes: true });
    await act(async () => { await api.enterMode('service'); });
    expect(setOperatingMode).toHaveBeenCalledWith('service'); expect(append).toHaveBeenCalledWith('entered Service / Commissioning mode');
    setOperatingMode.mockClear(); vi.mocked(confirm).mockResolvedValue(false); vi.mocked(bridge.setServiceMode).mockClear();
    await act(async () => { await api.enterMode('service'); });
    expect(setOperatingMode).not.toHaveBeenCalled(); expect(bridge.setServiceMode).not.toHaveBeenCalled();
  });
  it('a desktop without instrument modes never asks the backend', async () => {
    await render();
    await act(async () => { await api.enterMode('service'); });
    expect(bridge.setServiceMode).not.toHaveBeenCalled(); expect(setOperatingMode).toHaveBeenCalledWith('service');
  });
  it('leaving Service mode is never blocked: the app returns to Operator and disarms at once, a backend refusal is logged', async () => {
    vi.mocked(bridge.setServiceMode).mockResolvedValue({ ok: false, command: 0, operation_id: '0', message: 'busy' });
    await render({ instrumentModes: true, mode: 'service' });
    await act(async () => { await api.enterMode('operator'); });
    expect(setOperatingMode).toHaveBeenCalledWith('operator'); expect(setTriggerArmed).toHaveBeenCalledWith(false);
    expect(append).toHaveBeenCalledWith(expect.stringContaining('the instrument did not leave Service mode: busy'));
  });
});

describe('a running periodic test stops when the Monitoring view is hidden', () => {
  it('stops it, logs it and refreshes the status', async () => {
    await render({ monitoringVisible: true, periodic: true });
    expect(bridge.triggerPeriodicStop).not.toHaveBeenCalled();
    await render({ monitoringVisible: false, periodic: true });
    await act(async () => { await Promise.resolve(); await Promise.resolve(); });
    expect(bridge.triggerPeriodicStop).toHaveBeenCalledOnce();
    expect(append).toHaveBeenCalledWith('periodic test stopped (the Monitoring view was hidden)');
    expect(setTrigStatus).toHaveBeenCalledWith(status(false));
  });
  it('does nothing when no test runs, when not ready, or for a viewer', async () => {
    await render({ monitoringVisible: false, periodic: false });
    await render({ monitoringVisible: false, periodic: true, ready: false });
    await render({ monitoringVisible: false, periodic: true, viewerOnly: true });
    expect(bridge.triggerPeriodicStop).not.toHaveBeenCalled();
  });
  it('reports a stop that failed', async () => {
    vi.mocked(bridge.triggerPeriodicStop).mockResolvedValue({ ok: false, command: 8, operation_id: '0', message: 'no camera' });
    await render({ monitoringVisible: false, periodic: true });
    await act(async () => { await Promise.resolve(); await Promise.resolve(); });
    expect(append).toHaveBeenCalledWith('periodic test stop failed: no camera');
  });
});
