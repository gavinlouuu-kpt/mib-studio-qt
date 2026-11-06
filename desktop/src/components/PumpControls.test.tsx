// @vitest-environment jsdom
// Pumps operate as in Qt (epic #868, bug #855): no Service mode / arm / experiment gate, Stop always available, the device is
// polled every 500 ms without a command lock, rate/unit/direction apply 300 ms after the last edit, hold-to-purge, exact
// syringe numbers, Connect from the saved pump settings, rate/unit/direction kept on the host (HW-003, HW-016).
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { PumpControls } from './PumpControls';
import { bridge } from '../bridge';
import type { CmdResult, HostSettingsDocument, PlatformCapabilities, PumpStatus } from '../bridge';
import { OPEN_PUMP_SETTINGS_EVENT } from './hardwareDialogEvents';

vi.mock('../bridge', () => ({bridge: {
  fetchPumpStatus: vi.fn(), pumpPollStatus: vi.fn(), pumpStart: vi.fn(), pumpStop: vi.fn(), pumpPurge: vi.fn(), pumpStopPurge: vi.fn(),
  pumpSetFlowRate: vi.fn(), pumpSetDirection: vi.fn(), pumpSetSyringeVolume: vi.fn(), pumpConnectModel: vi.fn(), pumpConnectEndpoint: vi.fn(),
  pumpDisconnect: vi.fn(), fetchHostSettings: vi.fn(), hostSettingsCommand: vi.fn(),
}}));
const ok = (message = 'ok'): CmdResult => ({ok: true, command: 10, message, operation_id: '0'});
const deferred = <T,>() => { let resolve!: (value: T) => void; const promise = new Promise<T>(r => { resolve = r; }); return {promise, resolve}; };
const base: PumpStatus = {valid: true, connected: true, run_status: 0, current_flow_rate: 0, accumulated_volume: 0, min_flow_rate: 0, max_flow_rate: 100, stalled: false, com_port: 3, baud_rate: 115200, modbus_address: 1, configured_flow_rate: 0, flow_rate_unit: 100, direction: 0, model: 0, port_name: 'COM3'};
let host: HTMLDivElement, root: Root;
let statuses: [PumpStatus, PumpStatus];
/** The host's saved settings: a stand-in for the backend that merges `set_pumps` patches like host_settings.json does. */
let saved: Record<string, unknown>;
const hostDocument = (): HostSettingsDocument => ({valid: true, path: '/data/host_settings.json', settings: structuredClone(saved), load_error: '', boot_env: '', pump_scan: {state: 'idle', operation_id: '0', port: '', found: [], error: ''}});
const lastPatch = () => { const all = savedPatches(); return all[all.length - 1]; };
const savedPatches = () => vi.mocked(bridge.hostSettingsCommand).mock.calls.map(([request]) => request as {action: string; settings: Record<string, unknown>}).filter(request => request.action === 'set_pumps').map(request => request.settings);
const append = vi.fn();

const panel = (slot: number) => Array.from(host.querySelectorAll('fieldset'))[slot];
const field = (text: string, slot = 0) => Array.from(panel(slot).querySelectorAll('label')).filter(node => node.textContent?.startsWith(text)).map(node => node.querySelector('select, input'))[0] as HTMLSelectElement & HTMLInputElement;
const button = (name: string | RegExp, slot = 0) => Array.from(panel(slot).querySelectorAll('button')).find(node => typeof name === 'string' ? node.textContent === name : name.test(node.textContent ?? ''))! as HTMLButtonElement;
const purgeButton = (slot = 0) => button(/^Hold to purge/, slot);
const alertText = (slot = 0) => panel(slot).querySelector('[role="alert"]')?.textContent ?? '';
const type = async (input: HTMLInputElement, value: string) => act(async () => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value); input.dispatchEvent(new Event('input', {bubbles: true})); });
const choose = async (select: HTMLSelectElement, value: string) => act(async () => { Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value')!.set!.call(select, value); select.dispatchEvent(new Event('change', {bubbles: true})); });
const fire = async (target: Element | Window | Document, type: string, init: object = {}) => act(async () => { target.dispatchEvent(Object.assign(new Event(type, {bubbles: true, cancelable: true}), init)); });
const key = async (target: Element, type: 'keydown' | 'keyup', k: string, repeat = false) => act(async () => { target.dispatchEvent(new KeyboardEvent(type, {key: k, repeat, bubbles: true, cancelable: true})); });
/** Qt's editingFinished: Enter in the rate field. */
const setRate = async (input: HTMLInputElement, value: string) => { await type(input, value); await key(input, 'keydown', 'Enter'); };
const tick = async (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const click = async (node: HTMLElement) => act(async () => node.click());
async function render(props: {ready?: boolean; capabilities?: PlatformCapabilities} = {}) {
  await act(async () => root.render(<PumpControls ready={props.ready ?? true} append={append} {...(props.capabilities ? {capabilities: props.capabilities} : {})} />));
}
beforeEach(() => {
  Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
  vi.useFakeTimers();
  vi.clearAllMocks();
  statuses = [{...base}, {...base, modbus_address: 2}];
  vi.mocked(bridge.fetchPumpStatus).mockImplementation(async slot => statuses[slot]);
  vi.mocked(bridge.pumpPollStatus).mockResolvedValue(ok('Pump status polled'));
  saved = {};
  vi.mocked(bridge.fetchHostSettings).mockImplementation(async () => hostDocument());
  vi.mocked(bridge.hostSettingsCommand).mockImplementation(async request => {
    const patch = (request as {settings: Record<string, unknown>}).settings;
    for (const [key, value] of Object.entries(patch)) { if (value === null) delete saved[key]; else saved[key] = value; }
    return ok('Pump settings saved');
  });
  for (const name of ['pumpStart', 'pumpStop', 'pumpPurge', 'pumpStopPurge', 'pumpSetFlowRate', 'pumpSetDirection', 'pumpSetSyringeVolume', 'pumpConnectModel', 'pumpConnectEndpoint', 'pumpDisconnect'] as const)
    vi.mocked(bridge[name] as (...a: unknown[]) => Promise<CmdResult>).mockResolvedValue(ok(name));
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

describe('operator controls need no Service mode, arm or idle experiment (D1)', () => {
  it('starts, purges and edits rate and direction with nothing but a connected pump', async () => {
    await render();
    for (const name of ['Run configured pump', 'Stop pump', 'Disconnect']) expect(button(name).disabled, name).toBe(false);
    expect(purgeButton().disabled).toBe(false);
    for (const label of ['Flow rate', 'Rate unit', 'Direction']) expect(field(label).disabled, label).toBe(false);
    await click(button('Run configured pump'));
    expect(bridge.pumpStart).toHaveBeenCalledWith(0);
    expect(append).toHaveBeenCalledWith('Start pump: pumpStart');
  });
  it('has no Apply rate, Apply direction or Read device status button, and the help says so', async () => {
    await render();
    const names = Array.from(host.querySelectorAll('button')).map(node => node.textContent);
    expect(names).not.toContain('Apply rate'); expect(names).not.toContain('Apply direction'); expect(names).not.toContain('Read device status');
    expect(host.textContent).toContain('need no Service mode');
  });
  it('disables a command only for an unready backend or a missing connection', async () => {
    statuses = [{...base, connected: false}, {...base}];
    await render();
    expect(button('Run configured pump', 0).disabled).toBe(true);
    expect(purgeButton(0).disabled).toBe(true);
    expect(field('Flow rate', 0).disabled).toBe(true);
    expect(button('Connect', 0).disabled).toBe(false);
    expect(button('Run configured pump', 1).disabled).toBe(false);
  });
  it('blocks everything but Stop when the status cannot be read', async () => {
    vi.mocked(bridge.fetchPumpStatus).mockRejectedValue(new Error('Transport lost'));
    await render();
    expect(button('Run configured pump').disabled).toBe(true);
    expect(purgeButton().disabled).toBe(true);
    expect(field('Flow rate').disabled).toBe(true);
    expect(button('Stop pump').disabled).toBe(false);
    expect(host.textContent).toContain('Transport lost');
  });
  it('shows rejected commands as failures, never optimistic success', async () => {
    vi.mocked(bridge.pumpStart).mockResolvedValue({...ok(), ok: false, message: 'Device stalled'});
    await render();
    await click(button('Run configured pump'));
    expect(alertText()).toContain('Device stalled');
    expect(append).not.toHaveBeenCalledWith('Start pump: pumpStart');
  });
});

describe('Stop always works (#855, D3)', () => {
  it('is disabled only when the backend is not ready', async () => {
    statuses = [{...base, connected: false}, {...base, connected: false}];
    await render();
    expect(button('Stop pump', 0).disabled).toBe(false);
    await render({ready: false});
    expect(button('Stop pump', 0).disabled).toBe(true);
  });
  it('is not held back by a command in flight, and stops a purge too', async () => {
    const slow = deferred<CmdResult>();
    vi.mocked(bridge.pumpStart).mockReturnValue(slow.promise);
    await render();
    await click(button('Run configured pump'));
    expect(button('Run configured pump').disabled).toBe(true);   // the button's own command is in flight
    expect(button('Stop pump').disabled).toBe(false);
    await click(button('Stop pump'));
    expect(bridge.pumpStop).toHaveBeenCalledWith(0);
    expect(bridge.pumpStopPurge).toHaveBeenCalledWith(0);        // the dLSP full-speed purge has its own register
    await act(async () => slow.resolve(ok()));
  });
  it('is not held back by a rate change that has not been answered', async () => {
    const slow = deferred<CmdResult>();
    vi.mocked(bridge.pumpSetFlowRate).mockReturnValue(slow.promise);
    await render();
    await setRate(field('Flow rate'), '20'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(1);
    await click(button('Stop pump'));
    expect(bridge.pumpStop).toHaveBeenCalledWith(0);
    await act(async () => slow.resolve(ok()));
  });
  it('is not held back by a status poll that has not been answered', async () => {
    await render();
    const slow = deferred<CmdResult>();
    vi.mocked(bridge.pumpPollStatus).mockReturnValue(slow.promise);
    await tick(500);                                              // a poll starts and hangs
    expect(bridge.pumpPollStatus).toHaveBeenCalled();
    expect(button('Run configured pump').disabled).toBe(false);   // polling is not a busy command
    await click(button('Stop pump'));
    expect(bridge.pumpStop).toHaveBeenCalledWith(0);
    await act(async () => slow.resolve(ok()));
  });
  it('reports a failed stop on the pump and still tries to clear the purge', async () => {
    vi.mocked(bridge.pumpStop).mockResolvedValue({...ok(), ok: false, message: 'Pump stop failed: timeout'});
    await render();
    await click(button('Stop pump'));
    expect(alertText()).toContain('Pump stop failed: timeout');
    expect(bridge.pumpStopPurge).toHaveBeenCalledWith(0);
  });
});

describe('Stop cancels what is still queued behind a slow command (#855)', () => {
  /** A rate apply is in flight on a slow bus; everything else the operator does queues behind it. */
  async function slowRateApply() {
    const slow = deferred<CmdResult>();
    vi.mocked(bridge.pumpSetFlowRate).mockReturnValueOnce(slow.promise);
    await render();
    await setRate(field('Flow rate'), '20'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(1);
    return slow;
  }
  const finish = async (slow: {resolve: (value: CmdResult) => void}) => { await act(async () => slow.resolve(ok('rate set'))); await tick(1000); };
  it('does not send a Start that was queued before Stop', async () => {
    const slow = await slowRateApply();
    await click(button('Run configured pump'));                   // queued behind the rate apply
    expect(bridge.pumpStart).not.toHaveBeenCalled();
    await click(button('Stop pump'));                             // sent at once
    expect(bridge.pumpStop).toHaveBeenCalledTimes(1);
    expect(bridge.pumpStopPurge).toHaveBeenCalledTimes(1);
    await finish(slow);
    expect(bridge.pumpStart).not.toHaveBeenCalled();
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(1);
    expect(alertText()).toContain('Start pump: cancelled by Stop');
    expect(button('Run configured pump').disabled).toBe(false);   // the cancelled command released its button
    expect(append).toHaveBeenCalledWith('Set flow rate: rate set');   // the command already in flight was not taken back
  });
  it('does not send a purge start that was queued before Stop, and the later release adds no stop of its own', async () => {
    const slow = await slowRateApply();
    await fire(purgeButton(), 'pointerdown', {button: 0}); await tick(0);
    expect(bridge.pumpPurge).not.toHaveBeenCalled();
    await click(button('Stop pump'));
    await finish(slow);
    await fire(purgeButton(), 'pointerup', {button: 0}); await tick(0);
    expect(bridge.pumpPurge).not.toHaveBeenCalled();
    expect(bridge.pumpStopPurge).toHaveBeenCalledTimes(1);        // only Stop's own
    expect(alertText()).toContain('Purge pump: cancelled by Stop');
  });
  it('does not send a rate or a direction that was queued before Stop', async () => {
    const slow = await slowRateApply();
    await setRate(field('Flow rate'), '30'); await tick(300);     // queued
    await choose(field('Direction'), '1'); await tick(300);       // queued
    await click(button('Stop pump'));
    await finish(slow);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(1);
    expect(bridge.pumpSetDirection).not.toHaveBeenCalled();
    expect(alertText()).toContain('cancelled by Stop');
  });
  it('sends nothing but the Stop and its purge stop after it, whatever was queued', async () => {
    const slow = await slowRateApply();
    await setRate(field('Flow rate'), '30'); await tick(300);
    await click(button('Run configured pump'));
    vi.mocked(bridge.pumpStop).mockClear(); vi.mocked(bridge.pumpStopPurge).mockClear();
    await click(button('Stop pump'));
    await finish(slow);
    for (const name of ['pumpStart', 'pumpPurge', 'pumpSetDirection', 'pumpSetSyringeVolume', 'pumpDisconnect'] as const) expect(bridge[name], name).not.toHaveBeenCalled();
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(1);
    expect(bridge.pumpStop).toHaveBeenCalledTimes(1);
    expect(bridge.pumpStopPurge).toHaveBeenCalledTimes(1);
  });
  it('lets a command given after Stop through', async () => {
    const slow = await slowRateApply();
    await click(button('Stop pump'));
    await finish(slow);
    await click(button('Run configured pump'));
    expect(bridge.pumpStart).toHaveBeenCalledWith(0);
  });
  it('drops queued commands when the operator disconnects, and still disconnects', async () => {
    const slow = await slowRateApply();
    await setRate(field('Flow rate'), '30'); await tick(300);     // queued
    await click(button('Disconnect'));
    await finish(slow);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(1);
    expect(bridge.pumpDisconnect).toHaveBeenCalledWith(0);
  });
  it('drops queued commands when the pump turns out to be disconnected', async () => {
    const slow = await slowRateApply();
    await click(button('Run configured pump'));
    statuses = [{...base, connected: false}, {...base}];
    await tick(500); await tick(500);                             // polls are not queued: the new status arrives
    await finish(slow);
    expect(bridge.pumpStart).not.toHaveBeenCalled();
    expect(alertText()).toContain('Start pump: cancelled');
  });
  it('does not send a queued command while the pump status is unknown', async () => {
    const slow = await slowRateApply();
    await click(button('Run configured pump'));
    vi.mocked(bridge.fetchPumpStatus).mockRejectedValue(new Error('Transport lost'));
    await tick(500); await tick(500);
    expect(button('Stop pump').disabled).toBe(false);
    await finish(slow);
    expect(bridge.pumpStart).not.toHaveBeenCalled();
    expect(alertText()).toContain('Start pump: cancelled: the pump is not connected');
  });
  it('drops queued commands when the panel goes away', async () => {
    const slow = await slowRateApply();
    await click(button('Run configured pump'));
    await act(async () => root.unmount());
    root = createRoot(host);
    await act(async () => slow.resolve(ok())); await tick(1000);
    expect(bridge.pumpStart).not.toHaveBeenCalled();
  });
});

describe('polling the devices (HW-015)', () => {
  it('reads each connected pump from the device, then takes the status, every 500 ms', async () => {
    statuses = [{...base}, {...base, connected: false}];
    await render();
    vi.mocked(bridge.pumpPollStatus).mockClear(); vi.mocked(bridge.fetchPumpStatus).mockClear();
    await tick(500);
    expect(bridge.pumpPollStatus).toHaveBeenCalledTimes(1);       // only the connected pump
    expect(bridge.pumpPollStatus).toHaveBeenCalledWith(0);
    expect(bridge.fetchPumpStatus).toHaveBeenCalledTimes(2);
    const order = [vi.mocked(bridge.pumpPollStatus).mock.invocationCallOrder[0], vi.mocked(bridge.fetchPumpStatus).mock.invocationCallOrder[0]];
    expect(order[0]).toBeLessThan(order[1]);
    await tick(500);
    expect(bridge.pumpPollStatus).toHaveBeenCalledTimes(2);
  });
  it('never overlaps polls', async () => {
    await render();
    const slow = deferred<CmdResult>();
    vi.mocked(bridge.pumpPollStatus).mockClear();
    vi.mocked(bridge.pumpPollStatus).mockReturnValue(slow.promise);
    await tick(500); await tick(500); await tick(500);
    expect(bridge.pumpPollStatus).toHaveBeenCalledTimes(2);       // one per connected pump of the single poll that is in flight
    slow.resolve(ok()); await tick(0);
    vi.mocked(bridge.pumpPollStatus).mockResolvedValue(ok());
    await tick(500);
    expect(bridge.pumpPollStatus).toHaveBeenCalledTimes(4);
  });
  it('shows the run state read from the device', async () => {
    await render();
    statuses = [{...base, run_status: 1, current_flow_rate: 20}, {...base}];
    await tick(500);
    expect(host.textContent).toContain('Connected · Forward');
    expect(host.textContent).toContain('Live rate 20');
  });
  it('stops on unmount and while the backend is not ready', async () => {
    await render({ready: false});
    await tick(2000);
    expect(bridge.pumpPollStatus).not.toHaveBeenCalled();
    expect(bridge.fetchPumpStatus).not.toHaveBeenCalled();
    await render({ready: true});
    await tick(500);
    expect(bridge.pumpPollStatus).toHaveBeenCalled();
    await act(async () => root.unmount());
    vi.mocked(bridge.pumpPollStatus).mockClear(); vi.mocked(bridge.fetchPumpStatus).mockClear();
    await tick(3000);
    expect(bridge.pumpPollStatus).not.toHaveBeenCalled();
    expect(bridge.fetchPumpStatus).not.toHaveBeenCalled();
    root = createRoot(host);
  });
});

describe('rate, unit and direction apply 300 ms after their trigger, as in Qt (HW-007, HW-008, HW-009)', () => {
  it('sends nothing while the operator types or pauses, however long: only Enter or leaving the field commits', async () => {
    statuses = [{...base, run_status: 1}, {...base}];
    await render();
    await type(field('Flow rate'), '1'); await tick(1000);       // a pause in the middle of "15" ...
    await type(field('Flow rate'), '15'); await tick(5000);
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled();       // ... never reaches the running pump
    expect(host.textContent).not.toContain('Set flow rate');
  });
  it('applies the committed rate once, 300 ms after Enter, also while the pump is running', async () => {
    statuses = [{...base, run_status: 1}, {...base}];
    await render();
    await type(field('Flow rate'), '15');
    await key(field('Flow rate'), 'keydown', 'Enter');
    await tick(200);
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled();
    await tick(100);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(1);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledWith(0, 15, 100);
    expect(bridge.pumpSetDirection).not.toHaveBeenCalled();
    expect(append).toHaveBeenCalledWith('Set flow rate: pumpSetFlowRate');
  });
  it('applies the rate when the field loses focus, but not when it lost focus without a change', async () => {
    await render();
    await key(field('Flow rate'), 'keydown', 'Tab');
    await fire(field('Flow rate'), 'focusout'); await tick(500);
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled();       // nothing was edited
    await type(field('Flow rate'), '12');
    await fire(field('Flow rate'), 'focusout'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(1);
    expect(bridge.pumpSetFlowRate).toHaveBeenLastCalledWith(0, 12, 100);
    await fire(field('Flow rate'), 'focusout'); await tick(500); // committed already
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(1);
  });
  it('re-applies the same rate when the operator presses Enter again (Qt: editingFinished)', async () => {
    await render();
    await setRate(field('Flow rate'), '12'); await tick(300);
    await key(field('Flow rate'), 'keydown', 'Enter'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(2);
  });
  it('applies a unit or direction change 300 ms after the change itself, with no commit', async () => {
    await render();
    await choose(field('Direction'), '1'); await tick(299);
    expect(bridge.pumpSetDirection).not.toHaveBeenCalled();
    await tick(1);
    expect(bridge.pumpSetDirection).toHaveBeenCalledWith(0, 1);
    await type(field('Flow rate'), '3');                          // typed, not committed
    await choose(field('Rate unit'), '103'); await tick(300);     // the unit change takes the typed value along, as Qt's spin box does
    expect(bridge.pumpSetFlowRate).toHaveBeenLastCalledWith(0, 3, 103);
  });
  it('applies a unit change with the typed rate, and a direction change alone', async () => {
    await render();
    await setRate(field('Flow rate'), '2'); await tick(300);
    await choose(field('Rate unit'), '103'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenLastCalledWith(0, 2, 103);
    await choose(field('Direction'), '1'); await tick(300);
    expect(bridge.pumpSetDirection).toHaveBeenCalledWith(0, 1);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(2);
  });
  it('sends nothing for a unit change before any rate was typed, and nothing for a blank rate', async () => {
    await render();
    await choose(field('Rate unit'), '103'); await tick(300);
    await setRate(field('Flow rate'), '4'); await tick(300);
    await setRate(field('Flow rate'), ''); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(1);
  });
  it('keeps the two pumps apart', async () => {
    await render();
    await setRate(field('Flow rate', 1), '30'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledWith(1, 30, 100);
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalledWith(0, expect.anything(), expect.anything());
  });
  it('does not apply what a pump connecting fills in', async () => {
    statuses = [{...base, connected: false}, {...base}];
    await render();
    statuses = [{...base, configured_flow_rate: 12.5, flow_rate_unit: 103, direction: 1}, {...base}];
    await tick(500); await tick(500);
    expect(field('Flow rate').value).toBe('12.5');
    expect(field('Rate unit').value).toBe('103');
    expect(field('Direction').value).toBe('1');
    await tick(1000);
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled();
    expect(bridge.pumpSetDirection).not.toHaveBeenCalled();
  });
  it('leaves the field empty for a pump that was never given a rate, and keeps an edit across a failed status read', async () => {
    await render();
    expect(field('Flow rate').value).toBe('');
    await type(field('Flow rate'), '7');
    vi.mocked(bridge.fetchPumpStatus).mockRejectedValueOnce(new Error('blip'));
    await tick(500); await tick(500);
    expect(field('Flow rate').value).toBe('7');
  });
  it('lets the Start button wait for the edit it follows', async () => {
    await render();
    await setRate(field('Flow rate'), '40');
    await click(button('Run configured pump'));
    await tick(0);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledWith(0, 40, 100);
    expect(vi.mocked(bridge.pumpSetFlowRate).mock.invocationCallOrder[0]).toBeLessThan(vi.mocked(bridge.pumpStart).mock.invocationCallOrder[0]);
  });
  it('refuses a rate outside 0-100000 or with more than 4 decimals and tells the operator', async () => {
    await render();
    for (const value of ['100001', '1.00001', '-1']) {
      await setRate(field('Flow rate'), value); await tick(300);
      expect(alertText(), value).toContain('Flow rate');
    }
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled();
    await type(field('Flow rate'), '1.00001');
    expect(host.querySelector('.hardware-hint-problem')?.textContent).toContain('at most 4 decimals');
  });
  it('sends a syringe rate as an exact whole number or refuses it (the backend would clamp in silence)', async () => {
    await render();
    await choose(field('Rate unit'), '103');
    await setRate(field('Flow rate'), '0.5'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenLastCalledWith(0, 500, 100);
    expect(host.textContent).toContain('Sent to the pump as 500 µL/min');
    await setRate(field('Flow rate'), '2.5'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenLastCalledWith(0, 2500, 100);     // 2.5 mL/min is a whole 2500 µL/min
    await choose(field('Rate unit'), '100'); await tick(300);                   // 2.5 µL/min is not a whole number
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(2);
    expect(alertText()).toContain('cannot be sent exactly');
    await setRate(field('Flow rate'), '0'); await tick(300);
    expect(alertText()).toContain('Stop');
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledTimes(2);
  });
  it('clears a refused value\'s message once the next value goes through', async () => {
    await render();
    await setRate(field('Flow rate'), '2.5'); await tick(300);
    expect(alertText()).toContain('cannot be sent exactly');
    await setRate(field('Flow rate'), '20'); await tick(300);
    expect(alertText()).toBe('');
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledWith(0, 20, 100);
  });
  it('shows a command failure of an automatic apply', async () => {
    vi.mocked(bridge.pumpSetFlowRate).mockResolvedValue({...ok(), ok: false, message: 'Set flow rate failed: timeout'});
    await render();
    await setRate(field('Flow rate'), '5'); await tick(300);
    expect(alertText()).toContain('Set flow rate failed: timeout');
  });
});

describe('peristaltic pump (Tushui, HW-070)', () => {
  const peristaltic = {...base, model: 1, speed_rpm: 0.4, microliters_per_rev: 25, configured_flow_rate: 10, flow_rate_unit: 100};
  it('takes a rate in head rpm and sends the equivalent flow in µL/min, with CCW/CW labels', async () => {
    statuses = [{...peristaltic}, {...peristaltic}];
    await render();
    expect(host.textContent).toContain('configured ≈ 0.40 rpm');
    const unit = field('Rate unit');
    expect(Array.from(unit.options).map(o => o.textContent)).toContain('rpm (head)');
    await choose(unit, '200');
    await setRate(field('Flow rate'), '0.8'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledWith(0, 20, 100); // 0.8 rpm x 25 µL/rev
    expect(host.textContent).toContain('Infuse (CCW)');
    expect(host.textContent).toContain('Withdraw (CW)');
  });
  it('falls back to the calibration field when the pump reports no µL/rev', async () => {
    statuses = [{...peristaltic, microliters_per_rev: 0}, {...peristaltic}];
    await render();
    await choose(field('Rate unit'), '200');
    await setRate(field('Flow rate'), '2'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledWith(0, 50, 100); // 2 rpm x the 25 µL/rev of the calibration field
    expect(alertText()).toBe('');
  });
  it('sends a peristaltic rate as typed, without the syringe whole-number rule', async () => {
    statuses = [{...peristaltic}, {...peristaltic}];
    await render();
    await setRate(field('Flow rate'), '2.5'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledWith(0, 2.5, 100);
  });
  it('a syringe pump never keeps the rpm unit', async () => {
    statuses = [{...peristaltic, connected: false}, {...base, connected: false}];
    await render();
    await choose(field('Pump model'), '1');
    await choose(field('Rate unit'), '200');
    expect(field('Rate unit').value).toBe('200');
    await choose(field('Pump model'), '0');
    expect(field('Rate unit').value).toBe('100');
    expect(Array.from(field('Rate unit').options).map(o => o.textContent)).not.toContain('rpm (head)');
    expect(host.textContent).not.toContain('(CCW)');
  });
  it('connects a slot with its calibration and the instrument endpoint', async () => {
    statuses = [{...base, connected: false}, {...base, connected: false}];
    await render();
    await choose(field('Pump model', 0), '1');
    expect(panel(0).textContent).toContain('Connect to /dev/ttyPS1 · 115200 baud · Modbus address 3');
    await choose(field('Pump model', 1), '1');
    expect(panel(1).textContent).toContain('Connect to /dev/ttyPS1 · 115200 baud · Modbus address 4');
    expect(field('Calibration (µL per revolution)', 0).value).toBe('25');
    await click(button('Connect', 0));
    expect(bridge.pumpConnectModel).toHaveBeenCalledWith(0, 1, '/dev/ttyPS1', 115200, 3, 25);
    expect(bridge.pumpConnectEndpoint).not.toHaveBeenCalled();
  });
  it('defaults both slots to the instrument pumps on PZ7035 (Sample 3, Sheath 4)', async () => {
    statuses = [{...base, connected: false}, {...base, connected: false}];
    const capabilities = {
      instrument: 'pz7035' as const, autofocus: false, trigger: false, host_background: false, frame_buffer: false,
      reanalysis: false, core_updates: false, egrabber_script: false, pl_identity: true, led_strobe: true, align_mode: false, run_mode: false,
      pump: {model: 'tushui_peristaltic', port: '/dev/ttyPS1', sample_address: 3, sheath_address: 4, microliters_per_rev: 25},
    };
    await render({capabilities});
    expect([0, 1].map(slot => panel(slot).textContent)).toEqual([expect.stringContaining('Connect to /dev/ttyPS1 · 115200 baud · Modbus address 3'), expect.stringContaining('Connect to /dev/ttyPS1 · 115200 baud · Modbus address 4')]);
    expect([0, 1].map(slot => field('Pump model', slot).value)).toEqual(['1', '1']);
    expect(host.textContent).toContain('Sample at Modbus address 3, Sheath at 4');
  });
});

describe('Connect uses the saved pump settings (HW-003, HW-005, HW-006)', () => {
  beforeEach(() => { statuses = [{...base, connected: false}, {...base, connected: false}]; });
  it('says where to set a port when none is saved, and calls nothing', async () => {
    await render();
    expect(panel(0).textContent).toContain('No COM port configured. Open Pump settings…');
    await click(button('Connect', 0));
    expect(alertText()).toContain('No COM port configured. Open Pump settings…');
    expect(bridge.pumpConnectEndpoint).not.toHaveBeenCalled();
    expect(bridge.pumpConnectModel).not.toHaveBeenCalled();
  });
  it('connects each pump with its saved port, baud rate and Modbus address, a syringe pump without a calibration', async () => {
    saved = {pump_sample_port: 'COM3', pump_sample_baud_rate: 9600, pump_sample_address: 5, pump_sheath_port: 'COM4'};
    await render();
    expect(panel(0).textContent).toContain('Connect to COM3 · 9600 baud · Modbus address 5');
    await click(button('Connect', 0));
    expect(bridge.pumpConnectEndpoint).toHaveBeenLastCalledWith(0, 'COM3', 9600, 5);
    await click(button('Connect', 1));
    expect(bridge.pumpConnectEndpoint).toHaveBeenLastCalledWith(1, 'COM4', 115200, 2);  // Qt's defaults: 115200 baud, Sheath on address 2
    expect(bridge.pumpConnectModel).not.toHaveBeenCalled();
  });
  it('takes a saved COM number as COMn and a saved system path as it is', async () => {
    saved = {pump_sample_com_port: 7, pump_sheath_port: '/dev/ttyUSB0'};
    await render();
    await click(button('Connect', 0)); await click(button('Connect', 1));
    expect(bridge.pumpConnectEndpoint).toHaveBeenNthCalledWith(1, 0, 'COM7', 115200, 1);
    expect(bridge.pumpConnectEndpoint).toHaveBeenNthCalledWith(2, 1, '/dev/ttyUSB0', 115200, 2);
  });
  it('follows a save made in the settings dialog at once', async () => {
    await render();
    expect(panel(0).textContent).toContain('No COM port configured');
    saved = {pump_sample_port: 'COM3'};
    await act(async () => { window.dispatchEvent(new CustomEvent('mib:host-settings', {detail: hostDocument()})); });
    expect(panel(0).textContent).toContain('Connect to COM3 · 115200 baud · Modbus address 1');
  });
  it('ignores a saved value that is no longer valid and refuses a baud rate outside 1200-921600', async () => {
    saved = {pump_sample_port: 'COM3', pump_sample_address: 248, pump_sample_baud_rate: 1000};
    await render();
    expect(panel(0).textContent).toContain('Modbus address 1');              // 248 is not an address: the default is used
    await click(button('Connect', 0));
    expect(alertText()).toContain('Baud rate');
    expect(bridge.pumpConnectEndpoint).not.toHaveBeenCalled();
  });
  it('shows the saved syringe size beside the connection, and offers Pump settings…', async () => {
    saved = {pump_sample_port: 'COM3', pump_sample_syringe_vol: 0.5, pump_sample_syringe_unit: 103};
    await render();
    expect(panel(0).textContent).toContain('Syringe 0.5 mL');
    const opened = vi.fn(); window.addEventListener(OPEN_PUMP_SETTINGS_EVENT, opened);
    await click(Array.from(host.querySelectorAll('button')).find(node => node.textContent === 'Pump settings…')!);
    window.removeEventListener(OPEN_PUMP_SETTINGS_EVENT, opened);
    expect(opened).toHaveBeenCalledOnce();
  });
});

describe('rate, unit and direction are kept on the host (HW-016)', () => {
  it('restores them into the fields at startup and pushes nothing to the pump', async () => {
    saved = {pump_sample_flow_rate: 12.5, pump_sample_flow_unit: 103, pump_sample_direction: 1, pump_sheath_flow_rate: 30};
    statuses = [{...base, connected: false}, {...base, connected: false}];
    await render();
    expect([field('Flow rate', 0).value, field('Rate unit', 0).value, field('Direction', 0).value]).toEqual(['12.5', '103', '1']);
    expect([field('Flow rate', 1).value, field('Rate unit', 1).value, field('Direction', 1).value]).toEqual(['30', '100', '0']);
    await tick(2000);
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled(); expect(bridge.pumpSetDirection).not.toHaveBeenCalled(); expect(bridge.pumpStart).not.toHaveBeenCalled();
    expect(savedPatches()).toEqual([]);                                      // restoring writes nothing
  });
  it('saves the fields once an applied rate or direction went through (Qt: onApplySample), per pump', async () => {
    await render();
    await setRate(field('Flow rate', 1), '20'); await tick(300);
    expect(bridge.pumpSetFlowRate).toHaveBeenCalledWith(1, 20, 100);
    expect(savedPatches()).toEqual([{pump_sheath_flow_unit: 100, pump_sheath_direction: 0, pump_sheath_flow_rate: 20}]);
    await choose(field('Direction', 1), '1'); await tick(300);
    expect(lastPatch()).toEqual({pump_sheath_flow_unit: 100, pump_sheath_direction: 1, pump_sheath_flow_rate: 20});
    await choose(field('Rate unit', 1), '103'); await tick(300);
    expect(lastPatch()).toEqual({pump_sheath_flow_unit: 103, pump_sheath_direction: 1, pump_sheath_flow_rate: 20});
    expect(savedPatches().every(patch => Object.keys(patch).every(key => key.startsWith('pump_sheath_')))).toBe(true);
  });
  it('saves nothing when the pump refused the edit, and leaves an unparsable rate out of the record', async () => {
    vi.mocked(bridge.pumpSetFlowRate).mockResolvedValue({...ok(), ok: false, message: 'Device stalled'});
    await render();
    await setRate(field('Flow rate'), '20'); await tick(300);
    expect(alertText()).toContain('Device stalled');
    expect(savedPatches()).toEqual([]);
    vi.mocked(bridge.pumpSetFlowRate).mockResolvedValue(ok());
    await choose(field('Direction'), '1'); await tick(300);
    expect(lastPatch()).toEqual({pump_sample_flow_unit: 100, pump_sample_direction: 1, pump_sample_flow_rate: 20});
    await type(field('Flow rate'), '1.23456');                              // five decimals: not a rate
    await choose(field('Direction'), '0'); await tick(300);
    expect(lastPatch()).toEqual({pump_sample_flow_unit: 100, pump_sample_direction: 0});
  });
  it('saves the edited fields on Disconnect and when the window goes away, and only if they were edited', async () => {
    await render();
    await click(button('Disconnect', 0));
    expect(savedPatches()).toEqual([]);                                      // untouched fields: nothing to save
    await render();
    await type(field('Flow rate', 1), '40');                                // typed, never applied
    await fire(window, 'pagehide');
    expect(savedPatches()).toEqual([{pump_sheath_flow_unit: 100, pump_sheath_direction: 0, pump_sheath_flow_rate: 40}]);
    await fire(window, 'pagehide');
    expect(savedPatches()).toHaveLength(1);                                  // saved: nothing more to save
    await type(field('Flow rate', 0), '15');
    await click(button('Disconnect', 0));
    expect(lastPatch()).toEqual({pump_sample_flow_unit: 100, pump_sample_direction: 0, pump_sample_flow_rate: 15});
    expect(bridge.pumpDisconnect).toHaveBeenCalledWith(0);
  });
  it('shows what the pump holds when it connects, and the saved rate for a pump that was never given one', async () => {
    saved = {pump_sample_flow_rate: 12.5, pump_sheath_flow_rate: 30};
    statuses = [{...base, connected: false}, {...base, connected: false}];
    await render();
    expect([field('Flow rate', 0).value, field('Flow rate', 1).value]).toEqual(['12.5', '30']);
    statuses = [{...base, configured_flow_rate: 18}, {...base, configured_flow_rate: 0}];
    await tick(500);
    expect(field('Flow rate', 0).value).toBe('18');                          // the pump's own value wins
    expect(field('Flow rate', 1).value).toBe('30');                          // never configured: the saved one stays
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled();
  });
  it('keeps the saved unit and direction with the saved rate for a pump that was never given one', async () => {
    // A restart found 1.5 mL/min, Withdraw saved; a fresh pump reports unit uL/min, Infuse (meaningless at rate 0).
    saved = {pump_sample_flow_rate: 1.5, pump_sample_flow_unit: 103, pump_sample_direction: 1};
    statuses = [{...base, connected: false}, {...base, connected: false}];
    await render();
    statuses = [{...base, configured_flow_rate: 0, flow_rate_unit: 100, direction: 0}, {...base, connected: false}];
    await tick(500);
    expect([field('Flow rate', 0).value, field('Rate unit', 0).value, field('Direction', 0).value]).toEqual(['1.5', '103', '1']);
    expect(bridge.pumpSetFlowRate).not.toHaveBeenCalled();
    expect(bridge.pumpSetDirection).not.toHaveBeenCalled();
  });
  it('does not overwrite the values of a pump that is already connected when the settings arrive', async () => {
    saved = {pump_sample_flow_rate: 12.5};
    statuses = [{...base, configured_flow_rate: 18}, {...base}];
    await render();
    expect(field('Flow rate', 0).value).toBe('18');
  });
  it('reports a save the host refused', async () => {
    vi.mocked(bridge.hostSettingsCommand).mockResolvedValue({...ok(), ok: false, message: 'There is no data directory to keep the settings in'});
    await render();
    await setRate(field('Flow rate'), '20'); await tick(300);
    expect(append).toHaveBeenCalledWith('Pump settings were not saved: There is no data directory to keep the settings in');
  });
});

describe('hold-to-purge (HW-012)', () => {
  const down = (slot = 0) => fire(purgeButton(slot), 'pointerdown', {button: 0});
  const up = (slot = 0) => fire(purgeButton(slot), 'pointerup', {button: 0});
  it('carries Qt\'s tooltip and purges only while held', async () => {
    await render();
    expect(purgeButton().title).toBe('Run pump at full speed (hold to purge)');
    expect(Array.from(host.querySelectorAll('button')).map(b => b.textContent)).not.toContain('Stop purge');
    await fire(purgeButton(), 'click');
    expect(bridge.pumpPurge).not.toHaveBeenCalled();             // a click is not a hold
    await down(); await tick(0);
    expect(bridge.pumpPurge).toHaveBeenCalledWith(0, 0);
    expect(bridge.pumpStopPurge).not.toHaveBeenCalled();
    await up();
    expect(bridge.pumpStopPurge).toHaveBeenCalledWith(0);
    expect(append).toHaveBeenCalledWith('Purge pump: pumpPurge');
    expect(append).toHaveBeenCalledWith('Stop purge: pumpStopPurge');
  });
  it('purges in the selected direction', async () => {
    await render();
    await choose(field('Direction'), '1'); await tick(300);
    expect(purgeButton().textContent).toContain('withdraw');
    await down(); await tick(0);
    expect(bridge.pumpPurge).toHaveBeenCalledWith(0, 1);
    await up();
  });
  it('stops when the pointer leaves, is cancelled, or is released anywhere in the page', async () => {
    await render();
    for (const release of [
      () => fire(purgeButton(), 'pointerout', {relatedTarget: null}),
      () => fire(purgeButton(), 'pointercancel'),
      () => fire(document, 'pointerup'),
      () => fire(window, 'blur'),
      () => fire(purgeButton(), 'focusout'),
    ]) {
      vi.mocked(bridge.pumpStopPurge).mockClear();
      await down(); await tick(0);
      await release();
      expect(bridge.pumpStopPurge, String(release)).toHaveBeenCalledTimes(1);
    }
  });
  it('stops when the page is hidden and when the panel unmounts', async () => {
    await render();
    await down(); await tick(0);
    Object.defineProperty(document, 'hidden', {configurable: true, get: () => true});
    await fire(document, 'visibilitychange');
    Object.defineProperty(document, 'hidden', {configurable: true, get: () => false});
    expect(bridge.pumpStopPurge).toHaveBeenCalledTimes(1);
    vi.mocked(bridge.pumpStopPurge).mockClear();
    await down(); await tick(0);
    await act(async () => root.unmount());
    expect(bridge.pumpStopPurge).toHaveBeenCalledWith(0);
    root = createRoot(host);
  });
  it('holds with the keyboard: Space or Enter down purges, up stops, a repeat does not purge again', async () => {
    await render();
    for (const k of [' ', 'Enter']) {
      vi.mocked(bridge.pumpPurge).mockClear(); vi.mocked(bridge.pumpStopPurge).mockClear();
      await key(purgeButton(), 'keydown', k); await tick(0);
      await key(purgeButton(), 'keydown', k, true); await tick(0);
      expect(bridge.pumpPurge, k).toHaveBeenCalledTimes(1);
      await key(purgeButton(), 'keyup', k);
      expect(bridge.pumpStopPurge, k).toHaveBeenCalledTimes(1);
    }
    await key(purgeButton(), 'keydown', 'a'); await tick(0);
    expect(bridge.pumpPurge).toHaveBeenCalledTimes(1);
  });
  it('never loses a release that comes while the start is still in flight', async () => {
    const start = deferred<CmdResult>();
    vi.mocked(bridge.pumpPurge).mockReturnValue(start.promise);
    await render();
    await down(); await tick(0);
    await up();
    expect(bridge.pumpStopPurge).not.toHaveBeenCalled();          // the stop would overtake the start
    await act(async () => start.resolve(ok('started')));
    expect(bridge.pumpStopPurge).toHaveBeenCalledTimes(1);
    expect(vi.mocked(bridge.pumpPurge).mock.invocationCallOrder[0]).toBeLessThan(vi.mocked(bridge.pumpStopPurge).mock.invocationCallOrder[0]);
  });
  it('sends nothing when the release comes before the purge left the command queue', async () => {
    const slow = deferred<CmdResult>();
    vi.mocked(bridge.pumpSetFlowRate).mockReturnValue(slow.promise);
    await render();
    await setRate(field('Flow rate'), '9'); await tick(300);          // an automatic apply holds the queue
    await down(); await tick(0); await up();
    await act(async () => slow.resolve(ok()));
    await tick(0);
    expect(bridge.pumpPurge).not.toHaveBeenCalled();
    expect(bridge.pumpStopPurge).not.toHaveBeenCalled();
  });
  it('purges after a pending rate edit was applied', async () => {
    await render();
    await setRate(field('Flow rate'), '9');
    await down(); await tick(0);
    expect(vi.mocked(bridge.pumpSetFlowRate).mock.invocationCallOrder[0]).toBeLessThan(vi.mocked(bridge.pumpPurge).mock.invocationCallOrder[0]);
    await up();
  });
  it('is stopped by a failed start report and shows it, and a failed release is shown too', async () => {
    vi.mocked(bridge.pumpPurge).mockResolvedValue({...ok(), ok: false, message: 'Pump purge failed: timeout'});
    await render();
    await down(); await tick(0);
    expect(alertText()).toContain('Pump purge failed: timeout');
    await up();
    expect(bridge.pumpStopPurge).toHaveBeenCalledTimes(1);       // the start reached the pump, so its release stops it although the reply failed (Qt)
    vi.mocked(bridge.pumpPurge).mockResolvedValue(ok());
    vi.mocked(bridge.pumpStopPurge).mockResolvedValue({...ok(), ok: false, message: 'Pump purge stop failed'});
    await down(); await tick(0); await up(); await tick(0);
    expect(alertText()).toContain('Pump purge stop failed');
  });
  it('lets go without a stop command when the pump disconnects during the hold', async () => {
    await render();
    await down(); await tick(0);
    statuses = [{...base, connected: false}, {...base}];
    await tick(500); await tick(500);
    await up();
    expect(bridge.pumpStopPurge).not.toHaveBeenCalled();
  });
  it('does not purge from a disabled button or a non-primary pointer button', async () => {
    statuses = [{...base, connected: false}, {...base}];
    await render();
    await down(0); await tick(0);
    expect(bridge.pumpPurge).not.toHaveBeenCalled();
    await fire(purgeButton(1), 'pointerdown', {button: 2});
    expect(bridge.pumpPurge).not.toHaveBeenCalled();
  });
});
