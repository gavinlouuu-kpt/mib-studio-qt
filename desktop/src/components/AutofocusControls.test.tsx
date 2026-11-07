// @vitest-environment jsdom
// The nanopositioner panel works as Qt's NanopositionerTab (epic #868, PR3): operator controls, live target ring width, jog step,
// runtime messages, device list with a backend filter, and the remembered selection.
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { AutofocusControls } from './AutofocusControls';
import { publishAutofocusMessage, resetAutofocusMessages } from './autofocusMessages';
import { homeKey, setpointKey } from './autofocusControlModel';
import { resetStartupRestore, settleStartupRestore } from '../startupRestoreGate';
import { startupPreferenceKey } from '../startupPreference';
import { bridge } from '../bridge';
import type { AutofocusConfig, AutofocusStatus, CmdResult, DiscoveredDevice, DiscoverySnapshot } from '../bridge';

vi.mock('../bridge', () => ({bridge: {
  fetchAutofocusStatus: vi.fn(), fetchAutofocusConfig: vi.fn(), autofocusSetEnabled: vi.fn(), autofocusJog: vi.fn(), autofocusSetConfig: vi.fn(),
  autofocusConnectEndpoint: vi.fn(), autofocusDisconnect: vi.fn(), startDeviceDiscovery: vi.fn(), fetchDeviceDiscovery: vi.fn(), cancelDeviceDiscovery: vi.fn(),
  startupDiscoverySetPreference: vi.fn(),
}}));
const ok = (message = 'ok'): CmdResult => ({ok: true, command: 11, message, operation_id: '0'});
const deferred = <T,>() => { let resolve!: (value: T) => void; const promise = new Promise<T>(r => { resolve = r; }); return {promise, resolve}; };
const baseStatus: AutofocusStatus = {valid: true, connected: false, enabled: false, current_voltage: 0, com_port: 0, backend_name: 'oeabt', endpoint_id: '', average_ring_ratio: 0, median_ring_ratio: 0, last_ring_ratio_update_us: 0, ring_ratio_age_us: 0};
const baseConfig: AutofocusConfig = {valid: true, focus_setpoint: 20, focus_range: .5, voltage_step: 1, fine_voltage_step: .2, min_voltage: 0, max_voltage: 100, initial_voltage: 50, manual_voltage_step: 1, ring_ratio_stale_ms: 1500, min_samples_per_step: 100, safe_shutdown_voltage: 0, require_new_sample_per_step: true, focus_direction: true};
const device = (over: Partial<DiscoveredDevice>): DiscoveredDevice => ({
  kind: 2, provider_id: 'nanopositioner', display_name: 'USB Serial', system_path: '/dev/ttyUSB0', persistent_id: '/dev/serial/by-id/usb-1', sdk_index: 0,
  interface_index: 0, device_index: 0, stream_index: 0, bus_address: 0, stable_identity: '', identity_strength: 2, identification: 0, claimed_by: ['OEABT'],
  capabilities: ['vendor:OEABT'], synthetic: false, camera_type: -1, interface_id: '', device_id: '', stream_id: '', model_name: '', firmware_version: '', label: '', ...over,
});
const usb = device({}), com3 = device({display_name: 'COM3', system_path: 'COM3', persistent_id: 'COM3', capabilities: ['vendor:CoreMorrow / XMT'], claimed_by: []});
const snapshot = (candidates: DiscoveredDevice[]): DiscoverySnapshot => ({valid: true, job_id: '7', generation: '1', state: 2, complete: true, overflow: false, attempt: 1, max_attempts: 1, kinds: [2], candidates, errors: [], providers_run: [], origin: ''});

let host: HTMLDivElement, root: Root;
let status: AutofocusStatus, config: AutofocusConfig, stored: Map<string, string>, found: DiscoveredDevice[];
const append = vi.fn();
const panel = () => host.querySelector('.autofocus-controls')!;
const label = (text: string) => Array.from(panel().querySelectorAll('label')).find(node => node.textContent?.startsWith(text))!;
const field = (text: string) => label(text).querySelector('input, select') as HTMLInputElement & HTMLSelectElement;
const button = (name: string | RegExp) => Array.from(panel().querySelectorAll('button')).find(node => typeof name === 'string' ? node.textContent === name : name.test(node.textContent ?? ''))! as HTMLButtonElement;
const type = async (input: HTMLInputElement, value: string) => act(async () => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value); input.dispatchEvent(new Event('input', {bubbles: true})); });
const choose = async (select: HTMLSelectElement, value: string) => act(async () => { Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value')!.set!.call(select, value); select.dispatchEvent(new Event('change', {bubbles: true})); });
const click = async (node: HTMLElement) => act(async () => node.click());
const tick = async (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const press = async (input: HTMLElement, key: string) => act(async () => { input.dispatchEvent(new KeyboardEvent('keydown', {key, bubbles: true})); });
const alerts = () => Array.from(host.querySelectorAll('[role="alert"]')).map(node => node.textContent ?? '');
async function render(props: {ready?: boolean; experimentActive?: boolean; resumedSession?: boolean} = {}) {
  await act(async () => root.render(<AutofocusControls ready={props.ready ?? true} experimentActive={props.experimentActive ?? false} append={append} resumedSession={props.resumedSession} />));
  await tick(0);
}
const connectDevice = async () => { status = {...status, connected: true, current_voltage: 50.126, endpoint_id: usb.persistent_id}; };
beforeEach(() => {
  Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
  vi.useFakeTimers();
  vi.clearAllMocks();
  resetAutofocusMessages();
  resetStartupRestore(); settleStartupRestore();      // no profiles panel here: the startup restore has settled
  stored = new Map();
  vi.stubGlobal('localStorage', {getItem: (key: string) => stored.get(key) ?? null, setItem: (key: string, value: string) => { stored.set(key, value); }});
  status = {...baseStatus}; config = {...baseConfig}; found = [usb, com3];
  vi.mocked(bridge.fetchAutofocusStatus).mockImplementation(async () => status);
  vi.mocked(bridge.fetchAutofocusConfig).mockImplementation(async () => config);
  vi.mocked(bridge.autofocusSetConfig).mockImplementation(async next => { config = {...next}; return ok('Autofocus config applied'); });
  vi.mocked(bridge.autofocusSetEnabled).mockImplementation(async enabled => { status = {...status, enabled}; return ok(enabled ? 'Autofocus enabled' : 'Autofocus disabled'); });
  vi.mocked(bridge.autofocusJog).mockResolvedValue(ok('Voltage jog requested'));
  vi.mocked(bridge.autofocusConnectEndpoint).mockImplementation(async () => { await connectDevice(); return ok('Autofocus controller connected'); });
  vi.mocked(bridge.autofocusDisconnect).mockImplementation(async () => { status = {...status, connected: false, enabled: false, current_voltage: 0}; return ok('Autofocus controller disconnected'); });
  vi.mocked(bridge.startDeviceDiscovery).mockResolvedValue({accepted: true, job_id: '7', rejection: 0, reason: ''} as never);
  vi.mocked(bridge.fetchDeviceDiscovery).mockImplementation(async () => snapshot(found));
  vi.mocked(bridge.startupDiscoverySetPreference).mockImplementation(async preference => ({accepted: true, message: 'Applied', preference}));
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); vi.unstubAllGlobals(); });

describe('operator controls (D2): enable, target ring width and jog need nothing but a connected device', () => {
  it('Enable autofocus is a check box, enabled only when connected, and works during an experiment without Service mode or the arm', async () => {
    await render({experimentActive: true});
    const box = field('Enable autofocus');
    expect(box.type).toBe('checkbox'); expect(box.disabled).toBe(true);
    await connectDevice(); await tick(600);
    expect(field('Enable autofocus').disabled).toBe(false);
    await click(field('Enable autofocus'));
    expect(bridge.autofocusSetEnabled).toHaveBeenCalledWith(true);
    await tick(600);
    expect(field('Enable autofocus').checked).toBe(true);
    await click(field('Enable autofocus'));          // unchecking disables, through the always-available path
    expect(bridge.autofocusSetEnabled).toHaveBeenLastCalledWith(false);
  });
  it('the check box follows the click at once and then the backend\'s answer', async () => {
    status = {...status, connected: true, endpoint_id: usb.persistent_id};
    const slow = deferred<CmdResult>();
    vi.mocked(bridge.autofocusSetEnabled).mockImplementationOnce(() => slow.promise);
    await render();
    await click(field('Enable autofocus'));
    expect(field('Enable autofocus').checked).toBe(true);                 // before the backend answered
    status = {...status, enabled: true};
    await act(async () => slow.resolve(ok('Autofocus enabled'))); await tick(600);
    expect(field('Enable autofocus').checked).toBe(true);
    vi.mocked(bridge.autofocusSetEnabled).mockResolvedValueOnce({ok: false, command: 11, message: 'Autofocus controller is not connected', operation_id: '0'});
    status = {...status, enabled: false};
    await tick(600);
    await click(field('Enable autofocus'));                               // refused: the box returns to the backend's state
    await tick(600);
    expect(field('Enable autofocus').checked).toBe(false);
    expect(alerts().some(text => text.includes('not connected'))).toBe(true);
  });
  it('shows the voltage as Qt does: two decimals when connected, -- V otherwise', async () => {
    await render();
    expect(host.textContent).toContain('Voltage: -- V');
    await connectDevice(); await tick(600);
    expect(host.textContent).toContain('Voltage: 50.13 V');
  });
});

describe('target ring width (HW-030)', () => {
  it('shows the live setting, applied 500 ms after the last edit as one write, also while autofocus runs and an experiment is active', async () => {
    status = {...status, connected: true, enabled: true};
    await render({experimentActive: true});
    expect(field('Target ring width').value).toBe('20');
    expect(field('Target ring width').disabled).toBe(false);
    await type(field('Target ring width'), '2'); await tick(200);
    await type(field('Target ring width'), '25'); await tick(400);
    expect(bridge.autofocusSetConfig).not.toHaveBeenCalled();
    await tick(200);
    expect(bridge.autofocusSetConfig).toHaveBeenCalledTimes(1);
    expect(bridge.autofocusSetConfig).toHaveBeenCalledWith({...baseConfig, focus_setpoint: 25});
    expect(config.focus_setpoint).toBe(25);
    expect(append).toHaveBeenCalledWith('Target ring width set to 25');
  });
  it('applies at once on Enter and on leaving the field, rounding to two decimals', async () => {
    await render();
    await type(field('Target ring width'), '21.456');
    await press(field('Target ring width'), 'Enter');
    await tick(0);
    expect(bridge.autofocusSetConfig).toHaveBeenCalledWith(expect.objectContaining({focus_setpoint: 21.46}));
    expect(field('Target ring width').value).toBe('21.46');
    await type(field('Target ring width'), '30');
    await act(async () => { field('Target ring width').dispatchEvent(new FocusEvent('focusout', {bubbles: true})); });
    await tick(0);
    expect(config.focus_setpoint).toBe(30);
  });
  it('refuses values outside 1-100 with a message and sends nothing', async () => {
    await render();
    for (const bad of ['0', '100.5', '', 'x']) {
      await type(field('Target ring width'), bad); await press(field('Target ring width'), 'Enter');
      expect(alerts().some(text => /Target ring width must/.test(text)), bad).toBe(true);
    }
    await tick(1000);
    expect(bridge.autofocusSetConfig).not.toHaveBeenCalled();
    expect(stored.has(setpointKey)).toBe(false);
  });
  it('remembers the value, writing nothing else of the configuration', async () => {
    config = {...config, focus_range: 0.75};
    await render();
    await type(field('Target ring width'), '18'); await press(field('Target ring width'), 'Enter'); await tick(0);
    expect(JSON.parse(stored.get(setpointKey)!)).toBe(18);
    expect(bridge.autofocusSetConfig).toHaveBeenCalledWith({...baseConfig, focus_range: 0.75, focus_setpoint: 18});
  });
  it('follows the backend when a profile changed it, but not while the operator is typing', async () => {
    await render();
    config = {...config, focus_setpoint: 17}; await tick(2600);
    expect(field('Target ring width').value).toBe('17');
    await type(field('Target ring width'), '19');
    config = {...config, focus_setpoint: 16}; await tick(300);
    expect(field('Target ring width').value).toBe('19');
  });
  it('restores the remembered value into a fresh backend once, with a log line; never into a resumed session or during an experiment', async () => {
    stored.set(setpointKey, '22.5');
    await render();
    expect(config.focus_setpoint).toBe(22.5);
    expect(field('Target ring width').value).toBe('22.5');
    expect(append).toHaveBeenCalledWith('Autofocus target ring width restored to 22.5 (remembered from the last session)');
    await act(async () => root.unmount()); root = createRoot(host);
    config = {...baseConfig}; vi.mocked(bridge.autofocusSetConfig).mockClear();
    await render({resumedSession: true});
    expect(config.focus_setpoint).toBe(20); expect(bridge.autofocusSetConfig).not.toHaveBeenCalled();
    await act(async () => root.unmount()); root = createRoot(host);
    await render({experimentActive: true});
    expect(bridge.autofocusSetConfig).not.toHaveBeenCalled();
  });
});

describe('latest wins over the profile, and Home (MIBQT-45)', () => {
  /** A backend start: the startup restore re-applies a profile whose target ring width is `profile`, then settles. */
  const startBackend = async (profile: number, props: {experimentActive?: boolean} = {}) => {
    resetStartupRestore();
    config = {...baseConfig};
    await render(props);
    config = {...config, focus_setpoint: profile};                    // the profile restore lands while the panel waits
    await act(async () => settleStartupRestore()); await tick(0);
  };
  const restart = async () => { await act(async () => root.unmount()); root = createRoot(host); vi.mocked(bridge.autofocusSetConfig).mockClear(); append.mockClear(); };
  const home = () => button(/^Home \(/);

  it('the operator\'s latest value wins across a restart over a profile with another value, and the restore never overwrites it', async () => {
    await startBackend(31);
    await type(field('Target ring width'), '24'); await press(field('Target ring width'), 'Enter'); await tick(0);
    expect(JSON.parse(stored.get(setpointKey)!)).toBe(24);
    await restart();
    resetStartupRestore();
    config = {...baseConfig};
    await render();
    expect(bridge.autofocusSetConfig).not.toHaveBeenCalled();         // waits for the startup restore
    config = {...config, focus_setpoint: 31};
    await act(async () => settleStartupRestore()); await tick(0);
    expect(config.focus_setpoint).toBe(24);
    expect(field('Target ring width').value).toBe('24');
    expect(append).toHaveBeenCalledWith('Autofocus target ring width restored to 24 (remembered from the last session)');
    await tick(5000);                                                  // later polls do not bring the profile's value back
    expect(config.focus_setpoint).toBe(24); expect(bridge.autofocusSetConfig).toHaveBeenCalledTimes(1);
    expect(home().textContent).toBe('Home (31)');                      // the profile's value is the Home value
    expect(JSON.parse(stored.get(homeKey)!)).toBe(31);
  });
  it('a profile or method applied later in the session is the newest value: remembered for the next start and taken as Home', async () => {
    stored.set(setpointKey, '22.5');
    await startBackend(31);
    expect(config.focus_setpoint).toBe(22.5);
    config = {...config, focus_setpoint: 27};                          // Apply Saved Profile Settings, a method, a config document
    await tick(2600);
    expect(field('Target ring width').value).toBe('27');
    expect(JSON.parse(stored.get(setpointKey)!)).toBe(27);
    expect(home().textContent).toBe('Home (27)');
  });
  it('Home moves to the value it shows, through the queue, and is remembered as the latest; a pending typed value is dropped', async () => {
    stored.set(setpointKey, '22.5');
    await startBackend(31);
    expect(home().textContent).toBe('Home (31)'); expect(home().disabled).toBe(false);
    await type(field('Target ring width'), '40');                      // not applied yet (500 ms)
    await click(home()); await tick(1000);
    expect(bridge.autofocusSetConfig).toHaveBeenLastCalledWith({...baseConfig, focus_setpoint: 31});
    expect(config.focus_setpoint).toBe(31);
    expect(field('Target ring width').value).toBe('31');
    expect(JSON.parse(stored.get(setpointKey)!)).toBe(31);
    expect(append).toHaveBeenCalledWith('Target ring width set to the Home value 31');
    expect(bridge.autofocusSetConfig).not.toHaveBeenCalledWith(expect.objectContaining({focus_setpoint: 40}));
  });
  it('Home is refused while a run is active, also when the run started while Home waited in the queue', async () => {
    await startBackend(31, {experimentActive: true});
    expect(home().disabled).toBe(true); expect(home().title).toBe('Not available during an experiment');
    expect(field('Target ring width').disabled).toBe(false);           // the typed setpoint stays an operator control (D2)
    await restart();
    await startBackend(31);
    const slow = deferred<CmdResult>();
    vi.mocked(bridge.autofocusSetConfig).mockImplementationOnce(async next => { await slow.promise; config = {...next}; return ok(); });
    await type(field('Target ring width'), '25'); await press(field('Target ring width'), 'Enter');
    await click(home());                                               // queued behind the typed value
    await render({experimentActive: true});
    await act(async () => slow.resolve(ok())); await tick(0);
    expect(bridge.autofocusSetConfig).toHaveBeenCalledTimes(1);
    expect(config.focus_setpoint).toBe(25);
    expect(alerts().some(text => text.includes('Home: an experiment started; nothing was changed.'))).toBe(true);
  });
  it('a Home value outside 1-100 is refused like a typed one', async () => {
    await startBackend(150);
    await click(home()); await tick(0);
    expect(bridge.autofocusSetConfig).not.toHaveBeenCalled();
    expect(alerts().some(text => text.startsWith('Home: Target ring width must be from 1 to 100'))).toBe(true);
  });
  it('a panel mounted again in the same backend session keeps the session\'s Home, not the live value', async () => {
    await startBackend(31);
    await type(field('Target ring width'), '24'); await press(field('Target ring width'), 'Enter'); await tick(0);
    await restart();
    await render();                                                    // same backend: the startup restore settled long ago
    expect(home().textContent).toBe('Home (31)');
    expect(config.focus_setpoint).toBe(24);
  });
});

describe('manual jog and its step (HW-032)', () => {
  beforeEach(() => { status = {...status, connected: true, current_voltage: 50, endpoint_id: usb.persistent_id}; });
  it('step 1-100 V, default 1 or the configured whole number; enabled only when connected', async () => {
    config = {...config, manual_voltage_step: 5};
    await render();
    expect(field('Step').value).toBe('5'); expect(field('Step').disabled).toBe(false);
    status = {...status, connected: false}; await tick(600);
    expect(field('Step').disabled).toBe(true);
    expect(button('Jog voltage +').disabled).toBe(true);
  });
  it('writes the step into the configuration before every jog, as Qt does, and jogs in the pressed direction', async () => {
    await render();
    await type(field('Step'), '7');
    await click(button('Jog voltage +'));
    expect(bridge.autofocusSetConfig).toHaveBeenCalledWith({...baseConfig, manual_voltage_step: 7});
    expect(bridge.autofocusJog).toHaveBeenCalledWith(true);
    expect(vi.mocked(bridge.autofocusSetConfig).mock.invocationCallOrder[0]).toBeLessThan(vi.mocked(bridge.autofocusJog).mock.invocationCallOrder[0]);
    await click(button('Jog voltage −'));
    expect(bridge.autofocusSetConfig).toHaveBeenCalledTimes(1);   // unchanged step: nothing to write
    expect(bridge.autofocusJog).toHaveBeenLastCalledWith(false);
  });
  it('refuses a step outside 1-100 or not a whole number, without moving anything', async () => {
    await render();
    for (const bad of ['0', '101', '2.5', '']) {
      await type(field('Step'), bad); await click(button('Jog voltage +'));
      expect(alerts().some(text => /Step must/.test(text)), bad).toBe(true);
    }
    expect(bridge.autofocusJog).not.toHaveBeenCalled();
  });
  it('works while autofocus is enabled and during an experiment', async () => {
    status = {...status, enabled: true};
    await render({experimentActive: true});
    await click(button('Jog voltage +'));
    expect(bridge.autofocusJog).toHaveBeenCalledWith(true);
  });
  it('reports a refused configuration write and does not jog', async () => {
    vi.mocked(bridge.autofocusSetConfig).mockResolvedValue({ok: false, command: 11, message: 'Invalid voltage range (min must be < max)', operation_id: '0'});
    await render();
    await type(field('Step'), '3'); await click(button('Jog voltage +'));
    expect(alerts().some(text => text.includes('Invalid voltage range'))).toBe(true);
    expect(bridge.autofocusJog).not.toHaveBeenCalled();
  });
});

describe('Disable stops everything queued behind it (D3)', () => {
  it('a queued Enable is never sent after Disable, and Disable does not wait for the queue', async () => {
    status = {...status, connected: true, endpoint_id: usb.persistent_id};
    await render();
    const slowRead = deferred<AutofocusConfig>();
    vi.mocked(bridge.fetchAutofocusConfig).mockImplementationOnce(() => slowRead.promise);
    await type(field('Target ring width'), '24'); await press(field('Target ring width'), 'Enter');   // a configuration write now holds the queue
    await click(field('Enable autofocus'));                                                          // Enable waits behind it
    expect(bridge.autofocusSetEnabled).not.toHaveBeenCalled();
    await click(button('Disable autofocus'));
    expect(bridge.autofocusSetEnabled).toHaveBeenCalledTimes(1);
    expect(bridge.autofocusSetEnabled).toHaveBeenLastCalledWith(false);
    await act(async () => slowRead.resolve({...config}));
    await tick(600);
    expect(bridge.autofocusSetEnabled).toHaveBeenCalledTimes(1);   // the queued Enable was dropped
    expect(bridge.autofocusSetConfig).toHaveBeenCalledWith(expect.objectContaining({focus_setpoint: 24}));   // the setpoint still applied
    expect(append).toHaveBeenCalledWith('Enable autofocus: not sent, autofocus was disabled or disconnected first');
    expect(status.enabled).toBe(false);
  });
  it('a Jog that is still reading the configuration is not sent after Disable', async () => {
    status = {...status, connected: true, endpoint_id: usb.persistent_id};
    await render();
    const slowRead = deferred<AutofocusConfig>();
    vi.mocked(bridge.fetchAutofocusConfig).mockImplementationOnce(() => slowRead.promise);
    await click(button('Jog voltage +'));
    expect(button('Disconnect autofocus').disabled).toBe(true);       // a command is pending: connection waits for it
    await click(button('Disable autofocus'));
    await act(async () => slowRead.resolve({...config}));
    await tick(600);
    expect(bridge.autofocusJog).not.toHaveBeenCalled();
    expect(append).toHaveBeenCalledWith('Jog voltage: not sent, autofocus was disabled or disconnected first');
  });
  it('Disable is refused only when the backend is not ready', async () => {
    await render({ready: false});
    expect(button('Disable autofocus').disabled).toBe(true);
    await render({ready: true});
    expect(button('Disable autofocus').disabled).toBe(false);
  });
});

describe('device list, backend filter and connect (HW-024, HW-025, HW-026)', () => {
  const rows = () => Array.from(field('Serial device').options).map(option => option.textContent);
  it('Refresh runs the nanopositioner discovery and lists OEABT endpoints as candidates', async () => {
    await render();
    expect(host.textContent).toContain('Click Refresh to search for nanopositioners.');
    expect(button('Connect autofocus').disabled).toBe(true);
    await click(button('Refresh'));
    expect(bridge.startDeviceDiscovery).toHaveBeenCalledWith({kinds: [2], origin: 'tauri-nanopositioner-picker'});
    expect(rows()).toEqual(['OEABT candidate — USB Serial', 'COM3']);
    expect(host.textContent).toContain('Found 2 candidate endpoint(s). Identity is verified only when connecting.');
    expect(button('Connect autofocus').disabled).toBe(false);
  });
  it('selects the first device the scan identified, not necessarily the first row (it may be the pumps\' adapter)', async () => {
    found = [device({display_name: 'COM3', system_path: '/dev/COM3', persistent_id: '/dev/COM3', identification: 1, claimed_by: []}), device({display_name: 'COM5', system_path: '/dev/COM5', persistent_id: '/dev/COM5'})];
    await render(); await click(button('Refresh'));
    expect(field('Serial device').value).toBe('/dev/COM5');
    await click(button('Connect autofocus'));
    expect(bridge.autofocusConnectEndpoint).toHaveBeenCalledWith('oeabt', '/dev/COM5', -1, 115200, 1);
  });
  it('says so when nothing was found', async () => {
    found = [];
    await render(); await click(button('Refresh'));
    expect(host.textContent).toContain('No matching serial devices found. Check USB/power, then click Refresh.');
    expect(button('Connect autofocus').disabled).toBe(true);
  });
  it('the backend selector filters the list; CoreMOR baud and address are enabled only for CoreMOR', async () => {
    await render(); await click(button('Refresh'));
    expect(Array.from(field('Backend').options).map(option => option.textContent)).toEqual(['Auto-detect', 'OEABT serial', 'CoreMOR']);
    expect(field('Backend').value).toBe('auto');
    expect(field('Baud rate').disabled).toBe(true); expect(field('Device address').disabled).toBe(true);   // the first row is OEABT
    await choose(field('Backend'), 'coremor');
    expect(rows()).toEqual(['COM3']);
    expect(field('Baud rate').disabled).toBe(false); expect(field('Device address').disabled).toBe(false);
    expect(Array.from(field('Baud rate').options).map(option => option.value)).toEqual(['9600', '19200', '38400', '57600', '115200']);
    expect(field('Baud rate').value).toBe('115200'); expect(field('Device address').value).toBe('1');
    expect(field('Device address').min).toBe('0'); expect(field('Device address').max).toBe('255');
    await choose(field('Backend'), 'oeabt');
    expect(rows()).toEqual(['OEABT candidate — USB Serial']);
    expect(field('Baud rate').disabled).toBe(true);
  });
  it('connects an OEABT candidate by its identity with the baud and address fields, not literals', async () => {
    await render(); await click(button('Refresh'));
    await choose(field('Serial device'), usb.persistent_id);
    await click(button('Connect autofocus'));
    expect(bridge.autofocusConnectEndpoint).toHaveBeenCalledWith('oeabt', usb.persistent_id, -1, 115200, 1);
  });
  it('Auto-detect connects a CoreMOR row as CoreMOR with the COM number, baud and address', async () => {
    await render(); await click(button('Refresh'));
    await choose(field('Serial device'), 'COM3');
    await choose(field('Baud rate'), '57600'); await type(field('Device address'), '7');
    await click(button('Connect autofocus'));
    expect(bridge.autofocusConnectEndpoint).toHaveBeenCalledWith('coremor', 'COM3', 3, 57600, 7);
  });
  it('refuses an address outside 0-255 and reports it', async () => {
    await render(); await click(button('Refresh'));
    await choose(field('Serial device'), 'COM3'); await type(field('Device address'), '256');
    await click(button('Connect autofocus'));
    expect(bridge.autofocusConnectEndpoint).not.toHaveBeenCalled();
    expect(alerts().some(text => /address/.test(text))).toBe(true);
  });
  it('locks Refresh, backend and device while connected, and during an experiment', async () => {
    await render(); await click(button('Refresh'));
    await click(button('Connect autofocus')); await tick(600);
    for (const control of [button('Refresh'), field('Backend'), field('Serial device'), button('Connect autofocus')]) expect(control.disabled).toBe(true);
    expect(button('Disconnect autofocus').disabled).toBe(false);
    await click(button('Disconnect autofocus')); await tick(600);
    expect(button('Refresh').disabled).toBe(false);
    await render({experimentActive: true});
    expect(button('Refresh').disabled).toBe(true);
    expect(host.textContent).toContain('Hardware changes are locked while an experiment is active.');
  });
  // D10: a saved device is offered without a scan, so Connect would be clickable with Refresh locked. The button is the lock
  // (React drops clicks on a disabled button; the command's own guard behind it is defence in depth).
  it('locks Connect during an experiment, also for the saved device offered without a scan (D10)', async () => {
    stored.set(startupPreferenceKey, JSON.stringify({backend: 'oeabt', endpoint: usb.persistent_id, com_port: -1, baud: 115200, address: 1}));
    await render({experimentActive: true});
    const connect = button('Connect autofocus');
    expect(field('Serial device').value).toBe(usb.persistent_id);
    expect(connect.disabled).toBe(true);
    await click(connect);
    expect(bridge.autofocusConnectEndpoint).not.toHaveBeenCalled();
    expect(host.textContent).toContain('Hardware changes are locked while an experiment is active.');
  });
  it('locks Disconnect during an experiment (D10) while Disable stays available', async () => {
    await render(); await click(button('Refresh'));
    await click(button('Connect autofocus')); await tick(600);
    await render({experimentActive: true});
    const disconnect = button('Disconnect autofocus');
    expect(disconnect.disabled).toBe(true);
    expect(button('Disable autofocus').disabled).toBe(false);
    await click(disconnect);
    expect(bridge.autofocusDisconnect).not.toHaveBeenCalled();
    expect(host.textContent).toContain('Hardware changes are locked while an experiment is active.');
  });
  it('does not rescan while a scan runs and reports a failed one', async () => {
    const slow = deferred<DiscoverySnapshot>();
    vi.mocked(bridge.fetchDeviceDiscovery).mockImplementationOnce(() => slow.promise);
    await render();
    await click(button('Refresh'));
    expect(button('Scanning…').disabled).toBe(true);
    expect(button('Connect autofocus').disabled).toBe(true);
    await act(async () => slow.resolve(snapshot([])));
    vi.mocked(bridge.startDeviceDiscovery).mockRejectedValue(new Error('discovery refused: busy'));
    await click(button('Refresh'));
    expect(host.textContent).toContain('Refresh failed: Error: discovery refused: busy'.replace('Error: ', ''));
  });
});

describe('remembered selection (HW-036)', () => {
  const saved = () => JSON.parse(stored.get(startupPreferenceKey)!);
  it('is saved on connect with the backend choice, the endpoint identity, baud and address, and pushed to the backend', async () => {
    await render(); await click(button('Refresh'));
    await choose(field('Serial device'), usb.persistent_id);
    await click(button('Connect autofocus'));
    expect(saved()).toEqual({backend: 'auto', endpoint: usb.persistent_id, com_port: -1, baud: 115200, address: 1});
    expect(bridge.startupDiscoverySetPreference).toHaveBeenCalledWith(saved());
  });
  it('keeps the CoreMOR COM number, baud and address', async () => {
    await render(); await click(button('Refresh'));
    await choose(field('Backend'), 'coremor'); await choose(field('Baud rate'), '38400'); await type(field('Device address'), '4');
    await click(button('Connect autofocus'));
    expect(saved()).toEqual({backend: 'coremor', endpoint: 'COM3', com_port: 3, baud: 38400, address: 4});
  });
  it('is saved again on disconnect and when the page goes away', async () => {
    await render(); await click(button('Refresh'));
    await click(button('Connect autofocus')); await tick(600);
    stored.delete(startupPreferenceKey);
    await click(button('Disconnect autofocus'));
    expect(saved().endpoint).toBe(usb.persistent_id);
    stored.delete(startupPreferenceKey);
    await act(async () => { window.dispatchEvent(new Event('pagehide')); });
    expect(saved().endpoint).toBe(usb.persistent_id);
  });
  it('pre-fills backend, device, baud and address at startup, and Connect works without a scan', async () => {
    stored.set(startupPreferenceKey, JSON.stringify({backend: 'coremor', endpoint: 'COM3', com_port: 3, baud: 19200, address: 9}));
    await render();
    expect(field('Backend').value).toBe('coremor');
    expect(Array.from(field('Serial device').options).map(option => option.textContent)).toEqual(['COM3 (saved selection, not scanned)']);
    expect(field('Baud rate').value).toBe('19200'); expect(field('Device address').value).toBe('9');
    expect(host.textContent).toContain('Showing the saved selection.');
    await click(button('Connect autofocus'));
    expect(bridge.autofocusConnectEndpoint).toHaveBeenCalledWith('coremor', 'COM3', 3, 19200, 9);
  });
  it('a corrupt record falls back to the defaults', async () => {
    stored.set(startupPreferenceKey, '{broken');
    await render();
    expect(field('Backend').value).toBe('auto'); expect(field('Baud rate').value).toBe('115200');
    expect(host.textContent).toContain('Click Refresh to search for nanopositioners.');
  });
  it('a failure to store is reported, and the connection stays', async () => {
    vi.stubGlobal('localStorage', {getItem: () => null, setItem: () => { throw new Error('quota'); }});
    await render(); await click(button('Refresh'));
    await click(button('Connect autofocus'));
    expect(alerts().some(text => text.includes('could not be remembered') && text.includes('quota'))).toBe(true);
  });
});

describe('runtime messages (HW-033)', () => {
  it('shows autofocus failures the backend reported, newest last, at most three', async () => {
    publishAutofocusMessage('Voltage change failed: write refused by controller');
    await render();
    expect(host.querySelector('[aria-label="Autofocus messages"]')!.textContent).toContain('Voltage change failed: write refused by controller');
    for (const text of ['b', 'c', 'd']) await act(async () => publishAutofocusMessage(text));
    const shown = Array.from(host.querySelectorAll('[aria-label="Autofocus messages"] p')).map(node => node.textContent);
    expect(shown).toEqual(['b', 'c', 'd']);
  });
  it('a successful connect clears the failures of the connection before', async () => {
    publishAutofocusMessage('Voltage change failed: write refused by controller');
    await render(); await click(button('Refresh'));
    expect(host.querySelector('[aria-label="Autofocus messages"]')!.textContent).toContain('write refused');
    await click(button('Connect autofocus'));
    expect(host.querySelector('[aria-label="Autofocus messages"]')!.textContent).toBe('');
  });
  it('keeps the connect failure and its reason together', async () => {
    vi.mocked(bridge.autofocusConnectEndpoint).mockResolvedValue({ok: false, command: 11, message: 'Autofocus controller connect failed', operation_id: '0'});
    await render(); await click(button('Refresh'));
    await click(button('Connect autofocus'));
    await act(async () => publishAutofocusMessage('OEABT: no identity answer on /dev/ttyUSB0'));
    expect(alerts().some(text => text.includes('Autofocus controller connect failed'))).toBe(true);
    expect(host.textContent).toContain('OEABT: no identity answer on /dev/ttyUSB0');
    expect(append).toHaveBeenCalledWith('Connect autofocus: Error: Autofocus controller connect failed'.replace('Error: ', ''));
  });
});

describe('tuning editor (HW-035) leaves the operator controls alone', () => {
  it('applies its own fields on top of the live configuration, so a live setpoint and step survive', async () => {
    status = {...status, connected: true};
    await render();
    config = {...config, focus_setpoint: 31, manual_voltage_step: 9};     // changed by the operator controls meanwhile
    const details = host.querySelector('details')!;
    expect(details.textContent).not.toContain('Focus setpoint'); expect(details.textContent).not.toContain('Jog step');
    const range = Array.from(details.querySelectorAll('label')).find(node => node.textContent?.startsWith('Focus range'))!.querySelector('input')!;
    await type(range as HTMLInputElement, '0.8');
    await click(button('Apply autofocus configuration'));
    expect(bridge.autofocusSetConfig).toHaveBeenLastCalledWith({...baseConfig, focus_range: 0.8, focus_setpoint: 31, manual_voltage_step: 9});
  });
  it('stays locked while autofocus runs and refuses an invalid range', async () => {
    status = {...status, connected: true, enabled: true};
    await render();
    expect(button('Apply autofocus configuration').disabled).toBe(true);
    status = {...status, enabled: false}; await tick(600);
    const min = Array.from(host.querySelectorAll('details label')).find(node => node.textContent?.startsWith('Minimum voltage'))!.querySelector('input')!;
    await type(min as HTMLInputElement, '200');
    await click(button('Apply autofocus configuration'));
    expect(alerts().some(text => text.includes('Minimum voltage must be below maximum voltage'))).toBe(true);
    expect(bridge.autofocusSetConfig).not.toHaveBeenCalled();
  });
});
