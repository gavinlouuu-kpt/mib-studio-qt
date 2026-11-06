// @vitest-environment jsdom
// The Monitoring trigger tests (epic #868, D4: Service mode + arm stay; HW-043..HW-045): Qt's limits, the pulse duration applied
// live and before firing, a locked interval, refused input with a message, and the Operator-mode stop fallback.
import { act, useState } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { TriggerTestControls } from './TriggerTestControls';
import { StudioContext } from '../../studio/StudioContext';
import type { Studio } from '../../studio/useStudioController';
import { bridge, type TriggerStatus } from '../../bridge';
import { canActuate, canStartPeriodic, canStopPeriodic, type OperatingMode } from '../../commissioning';

vi.mock('../../bridge', () => ({ bridge: { triggerManualPulse: vi.fn(), triggerSetPulseDuration: vi.fn(), triggerPeriodicStart: vi.fn(), triggerPeriodicStop: vi.fn(), fetchTriggerStatus: vi.fn() } }));
vi.mock('../../transport/useControlRole', () => ({ useControlRole: () => ({ viewerOnly: false }) }));

let host: HTMLDivElement, root: Root;
const append = vi.fn(), setArmed = vi.fn();
const status = (change: Partial<TriggerStatus> = {}): TriggerStatus => ({ valid: true, camera_attached: true, pulse_duration_us: 1, trigger_count: 0, last_onset_us: 0, last_object_id: -1, last_track_id: -1, periodic_active: false, periodic_interval_ms: 1000, ...change });
const ok = (message = 'ok') => ({ ok: true, command: 8, operation_id: '0', message });
const button = (name: string) => [...host.querySelectorAll('button')].find(x => x.textContent === name)! as HTMLButtonElement;
const input = (label: string) => host.querySelector(`[aria-label="${label}"]`) as HTMLInputElement;
const setValue = (el: HTMLInputElement, value: string) => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(el, value); el.dispatchEvent(new Event('input', { bubbles: true })); };
const type = async (label: string, value: string) => act(async () => setValue(input(label), value));
const click = async (name: string) => act(async () => button(name).click());
const flush = async () => act(async () => { for (let i = 0; i < 6; i++) await Promise.resolve(); });
const calls = (fn: unknown) => (fn as { mock: { calls: unknown[][] } }).mock.calls;

function Harness({ mode, armed, trigStatus, expActive = false }: { mode: OperatingMode; armed: boolean; trigStatus: TriggerStatus; expActive?: boolean }) {
  const [pulseUs, setPulseUs] = useState('1'), [periodicMs, setPeriodicMs] = useState('1000'), [live, setLive] = useState(trigStatus);
  const input = { mode, experimentActive: expActive, triggerAttached: live.camera_attached, armed };
  const studio = { caps: { trigger: true }, operatingMode: mode, triggerArmed: armed, setTriggerArmed: setArmed, expActive, trigStatus: live, setTrigStatus: setLive, append,
    pulseUs, setPulseUs, periodicMs, setPeriodicMs, actuateCheck: canActuate(input), startPeriodicCheck: canStartPeriodic(input), stopPeriodicCheck: canStopPeriodic({ periodicActive: live.periodic_active }) } as unknown as Studio;
  return <StudioContext.Provider value={studio}><TriggerTestControls /></StudioContext.Provider>;
}
const render = async (props: Partial<Parameters<typeof Harness>[0]> = {}) => {
  // A new key remounts the harness so its state starts from the props.
  await act(async () => root.render(<Harness key={JSON.stringify(props)} mode="service" armed trigStatus={status()} {...props} />)); await flush();
};
beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true }); vi.clearAllMocks(); vi.useFakeTimers({ shouldAdvanceTime: true });
  vi.mocked(bridge.triggerSetPulseDuration).mockResolvedValue(ok('Pulse duration set')); vi.mocked(bridge.triggerManualPulse).mockResolvedValue(ok('Manual pulse requested'));
  vi.mocked(bridge.triggerPeriodicStart).mockResolvedValue(ok('Periodic test started')); vi.mocked(bridge.triggerPeriodicStop).mockResolvedValue(ok('Periodic test stopped'));
  vi.mocked(bridge.fetchTriggerStatus).mockResolvedValue(status());
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

describe('Service mode and the arm stay (D4)', () => {
  it('Operator mode shows only the hint, and a Stop button while a periodic test runs', async () => {
    await render({ mode: 'operator', armed: false });
    expect(host.textContent).toContain('Trigger tests are in Service / Commissioning mode'); expect(button('Sort Trigger')).toBeUndefined();
    await render({ mode: 'operator', armed: false, trigStatus: status({ periodic_active: true }) });
    await click('Stop Periodic Test'); await flush();
    expect(bridge.triggerPeriodicStop).toHaveBeenCalledOnce();
  });
  it('Sort Trigger needs the arm', async () => {
    await render({ armed: false });
    expect(button('Sort Trigger').disabled).toBe(true); expect(button('Sort Trigger').title).toContain('Arm');
  });
});

describe('pulse duration (HW-044)', () => {
  it('is applied live a moment after the operator stops typing, not on every keystroke', async () => {
    await render();
    await type('Pulse duration (µs)', '2'); await type('Pulse duration (µs)', '25'); await type('Pulse duration (µs)', '250');
    expect(bridge.triggerSetPulseDuration).not.toHaveBeenCalled();
    await act(async () => { await vi.advanceTimersByTimeAsync(350); });
    expect(calls(bridge.triggerSetPulseDuration)).toEqual([[250]]);
    expect(host.textContent).toContain('applied');
  });
  it('an out-of-range value is refused with a message and never applied or replaced (blank used to become 1)', async () => {
    await render();
    for (const bad of ['', '0', '1000001', '1.5']) {
      await type('Pulse duration (µs)', bad);
      expect(host.querySelector('[role="alert"]')!.textContent, JSON.stringify(bad)).toMatch(/between 1 and 1,000,000/);
      expect(input('Pulse duration (µs)').value).toBe(bad);
      expect(button('Sort Trigger').disabled).toBe(true);
    }
    await act(async () => { await vi.advanceTimersByTimeAsync(500); });
    expect(bridge.triggerSetPulseDuration).not.toHaveBeenCalled();
  });
  it('Sort Trigger applies the typed duration first, then fires, and logs the duration that was applied', async () => {
    await render();
    await type('Pulse duration (µs)', '40');
    await click('Sort Trigger'); await flush();
    expect(calls(bridge.triggerSetPulseDuration)).toEqual([[40]]);
    expect(bridge.triggerManualPulse).toHaveBeenCalledOnce();
    expect(vi.mocked(bridge.triggerSetPulseDuration).mock.invocationCallOrder[0]).toBeLessThan(vi.mocked(bridge.triggerManualPulse).mock.invocationCallOrder[0]);
    expect(append).toHaveBeenCalledWith('manual sort pulse fired (40 µs)');
    expect(setArmed).toHaveBeenCalledWith(false);
    // The pending live apply was folded into the click: nothing is applied twice.
    await act(async () => { await vi.advanceTimersByTimeAsync(500); });
    expect(bridge.triggerSetPulseDuration).toHaveBeenCalledOnce();
  });
  it('does not fire, and does not log a duration, when the backend refuses it', async () => {
    vi.mocked(bridge.triggerSetPulseDuration).mockResolvedValue({ ...ok(), ok: false, message: 'Pulse duration must be >= 1 us' });
    await render();
    await click('Sort Trigger'); await flush();
    expect(bridge.triggerManualPulse).not.toHaveBeenCalled();
    expect(append).toHaveBeenCalledWith('pulse duration failed: Pulse duration must be >= 1 us; sort trigger not fired');
    expect(append).not.toHaveBeenCalledWith(expect.stringContaining('manual sort pulse fired'));
  });
  it('there is no separate Set Pulse button any more', async () => {
    await render();
    expect(button('Set Pulse')).toBeUndefined();
  });
});

describe('periodic test (HW-045)', () => {
  it('starts at the typed interval after applying the pulse duration, and logs both', async () => {
    await render();
    await type('Periodic interval (ms)', '250'); await type('Pulse duration (µs)', '20');
    await click('Periodic Test'); await flush();
    expect(calls(bridge.triggerSetPulseDuration)).toEqual([[20]]);
    expect(calls(bridge.triggerPeriodicStart)).toEqual([[250]]);
    expect(append).toHaveBeenCalledWith('periodic test started (250 ms, 20 µs pulses)');
  });
  it('an interval outside 10-60000 ms is refused with a message (blank used to become 1000, 1 ms was allowed)', async () => {
    await render();
    for (const bad of ['', '0', '9', '60001']) {
      await type('Periodic interval (ms)', bad);
      expect(host.textContent, JSON.stringify(bad)).toMatch(/between 10 and 60000/);
      expect(button('Periodic Test').disabled).toBe(true);
    }
    expect(bridge.triggerPeriodicStart).not.toHaveBeenCalled();
  });
  it('the interval is locked while the test runs and the button stops it', async () => {
    await render({ trigStatus: status({ periodic_active: true }) });
    expect(input('Periodic interval (ms)').disabled).toBe(true);
    expect(button('Stop Periodic Test')).toBeTruthy();
    await click('Stop Periodic Test'); await flush();
    expect(bridge.triggerPeriodicStop).toHaveBeenCalledOnce(); expect(append).toHaveBeenCalledWith('periodic test stopped');
  });
  it('cannot start while an experiment is active or without the arm, but can always be stopped', async () => {
    await render({ expActive: true });
    expect(button('Periodic Test').disabled).toBe(true);
    await render({ armed: false });
    expect(button('Periodic Test').disabled).toBe(true);
    await render({ armed: false, trigStatus: status({ periodic_active: true }), expActive: true });
    expect(button('Stop Periodic Test').disabled).toBe(false);
  });
});
