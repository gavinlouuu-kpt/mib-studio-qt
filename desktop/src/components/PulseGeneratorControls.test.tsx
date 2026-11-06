// @vitest-environment jsdom
// The pulse generator panel (epic #868, HW-048..HW-057, decisions D5/D6): Qt's port list, serial settings, Scan,
// Connect, Set / Start / Stop without Service mode or an arm, Qt's status wording, persistence and the XGC + R5D preset.
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { PulseGeneratorControls } from './PulseGeneratorControls';
import { StudioContext } from '../studio/StudioContext';
import type { Studio } from '../studio/useStudioController';
import { bridge, type PulseGeneratorStatus, type SerialPortEntry } from '../bridge';

vi.mock('../bridge', () => ({ bridge: {
  pulseGeneratorStatus: vi.fn(), pulseGeneratorCommand: vi.fn(), listSerialPorts: vi.fn(), getPreferences: vi.fn(), setPreferences: vi.fn(),
  startDeviceDiscovery: vi.fn(), fetchDeviceDiscovery: vi.fn(), cancelDeviceDiscovery: vi.fn(),
} }));

let host: HTMLDivElement, root: Root;
const append = vi.fn();
const memory = new Map<string, string>();
let prefs: Record<string, unknown>;
const port = (name: string, extra: Partial<SerialPortEntry> = {}): SerialPortEntry => ({ system_name: name, system_location: `/dev/${name}`, description: '', manufacturer: '', serial_number: '', vendor_id: 0, product_id: 0, ...extra });
const ports = [port('COM4', { description: 'FT232R USB UART', serial_number: 'SIMPG001', vendor_id: 0x0403, product_id: 0x6001 }), port('COM6', { vendor_id: 0x0403, product_id: 0x6015 })];
const channel = (frequency_hz: number, duty_percent: number, output_enabled: boolean) => ({ frequency_hz, duty_percent, output_enabled });
const connectedStatus = (change: Partial<PulseGeneratorStatus> = {}): PulseGeneratorStatus => ({ valid: true, connected: true, owned: false, error: 'ok', port: 'COM4', baud: 9600, address: 1, data_bits: 8, parity: 'N', stop_bits: 1,
  channels: [channel(5000, 50, false), channel(0, 0, false), channel(0, 0, false), channel(0, 0, false)], ...change });
const idleStatus = (change: Partial<PulseGeneratorStatus> = {}): PulseGeneratorStatus => connectedStatus({ connected: false, port: '', ...change });
const ok = (message = 'Pulse-generator command completed') => ({ ok: true, command: 12, operation_id: '0', message });

const q = (selector: string) => host.querySelector(selector) as HTMLElement;
const button = (name: string) => [...host.querySelectorAll('button')].find(x => x.textContent === name || x.getAttribute('aria-label') === name)! as HTMLButtonElement;
const field = (label: string) => host.querySelector(`[aria-label="${label}"]`) as HTMLInputElement | HTMLSelectElement;
const setValue = (el: HTMLInputElement | HTMLSelectElement, value: string) => {
  const proto = el instanceof HTMLSelectElement ? HTMLSelectElement.prototype : HTMLInputElement.prototype;
  Object.getOwnPropertyDescriptor(proto, 'value')!.set!.call(el, value);
  el.dispatchEvent(new Event(el instanceof HTMLSelectElement ? 'change' : 'input', { bubbles: true }));
};
const edit = async (label: string, value: string) => act(async () => setValue(field(label), value));
const click = async (name: string) => act(async () => button(name).click());
const flush = async () => act(async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); });
const commands = () => vi.mocked(bridge.pulseGeneratorCommand).mock.calls.map(c => c[0]);
async function render(props: Record<string, unknown> = {}, studio?: Partial<Studio>) {
  const panel = <PulseGeneratorControls ready experimentActive={false} append={append} {...props} />;
  await act(async () => root.render(studio ? <StudioContext.Provider value={studio as Studio}>{panel}</StudioContext.Provider> : panel));
  await flush();
}

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.clearAllMocks(); memory.clear(); prefs = {};
  vi.stubGlobal('localStorage', { getItem: (key: string) => memory.get(key) ?? null, setItem: (key: string, value: string) => { memory.set(key, value); } });
  vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue(idleStatus());
  vi.mocked(bridge.pulseGeneratorCommand).mockResolvedValue(ok());
  vi.mocked(bridge.listSerialPorts).mockResolvedValue({ ports });
  vi.mocked(bridge.getPreferences).mockImplementation(async () => structuredClone(prefs));
  vi.mocked(bridge.setPreferences).mockImplementation(async next => { prefs = structuredClone(next); });
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.unstubAllGlobals(); });

describe('the form', () => {
  it("lists the serial ports with description, S/N and VID:PID and defaults to Qt's settings; mounting writes nothing", async () => {
    await render();
    const options = [...field('Pulse generator port').querySelectorAll('option')].map(o => o.textContent);
    expect(options).toEqual(['COM4 — FT232R USB UART, S/N SIMPG001, 0403:6001', 'COM6 — 0403:6015']);
    expect(field('Pulse generator port').value).toBe('COM4');
    expect(field('Pulse generator baud').value).toBe('9600'); expect(field('Pulse generator data bits').value).toBe('8');
    expect(field('Pulse generator parity').value).toBe('N'); expect(field('Pulse generator stop bits').value).toBe('1');
    expect(field('Pulse generator Modbus address').value).toBe('1');
    expect(field('Pulse generator frequency (Hz)').value).toBe('5000'); expect(field('Pulse generator duty (%)').value).toBe('50');
    expect([...field('Pulse generator baud').querySelectorAll('option')].map(o => o.textContent)).toEqual(['4800', '9600', '14400', '19200', '38400', '56000', '57600', '115200']);
    expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled();
    expect(q('[aria-label="Pulse generator status"]').textContent).toBe('Disconnected');
  });
  it('says so when there are no ports', async () => {
    vi.mocked(bridge.listSerialPorts).mockResolvedValue({ ports: [] });
    await render();
    expect(field('Pulse generator port').textContent).toBe('No serial ports found');
  });
  it('Refresh re-enumerates and keeps the selected port while it exists', async () => {
    await render();
    await edit('Pulse generator port', 'COM6');
    vi.mocked(bridge.listSerialPorts).mockResolvedValue({ ports: [...ports, port('COM7')] });
    await click('Refresh'); await flush();
    expect(bridge.listSerialPorts).toHaveBeenCalledTimes(2);
    expect(field('Pulse generator port').value).toBe('COM6');
    expect(field('Pulse generator port').querySelectorAll('option')).toHaveLength(3);
    vi.mocked(bridge.listSerialPorts).mockResolvedValue({ ports: [port('COM7')] });
    await click('Refresh'); await flush();
    expect(field('Pulse generator port').value).toBe('COM7');
  });
});

describe('persistence (HW-056)', () => {
  it('restores the saved settings after a restart and re-resolves a renamed port by its USB identity', async () => {
    prefs = { pulseGenerator: { portName: 'ttyUSB0', portSerialNumber: 'SIMPG001', portVid: 0x0403, portPid: 0x6001, baud: 19200, dataBits: 7, parity: 'E', stopBits: 2, address: 9, channel: 3, frequencyHz: 1234.5, dutyPercent: 12.5 } };
    await render();
    expect(field('Pulse generator port').value).toBe('COM4');
    expect(host.textContent).toContain('Port ttyUSB0 is gone; COM4 has the saved USB identity.');
    expect(field('Pulse generator baud').value).toBe('19200'); expect(field('Pulse generator data bits').value).toBe('7');
    expect(field('Pulse generator parity').value).toBe('E'); expect(field('Pulse generator stop bits').value).toBe('2');
    expect(field('Pulse generator Modbus address').value).toBe('9'); expect(field('Pulse generator channel').value).toBe('3');
    expect(field('Pulse generator frequency (Hz)').value).toBe('1234.5'); expect(field('Pulse generator duty (%)').value).toBe('12.5');
    expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled();
  });
  it('a successful Connect saves the port with its identity, the framing and the address', async () => {
    await render();
    await edit('Pulse generator baud', '19200'); await edit('Pulse generator Modbus address', '4');
    await click('Connect pulse generator'); await flush();
    expect(prefs.pulseGenerator).toMatchObject({ portName: 'COM4', portSerialNumber: 'SIMPG001', portVid: 0x0403, portPid: 0x6001, baud: 19200, dataBits: 8, parity: 'N', stopBits: 1, address: 4, channel: 1, frequencyHz: 5000, dutyPercent: 50 });
  });
  it('a failed Connect saves nothing; a save failure is shown, not silent', async () => {
    vi.mocked(bridge.pulseGeneratorCommand).mockResolvedValue({ ...ok(), ok: false, message: 'bus timeout' });
    await render();
    await click('Connect pulse generator'); await flush();
    expect(prefs.pulseGenerator).toBeUndefined();
    vi.mocked(bridge.pulseGeneratorCommand).mockResolvedValue(ok());
    vi.mocked(bridge.setPreferences).mockRejectedValue(new Error('read-only'));
    await click('Connect pulse generator'); await flush();
    expect(q('[role="alert"]').textContent).toContain('were not saved');
  });
});

describe('Connect', () => {
  it('sends the port, the framing and the address, and logs it', async () => {
    await render();
    await edit('Pulse generator data bits', '7'); await edit('Pulse generator parity', 'O'); await edit('Pulse generator stop bits', '2');
    await click('Connect pulse generator'); await flush();
    expect(commands()).toEqual([{ action: 'connect', port: 'COM4', baud: 9600, data_bits: 7, parity: 'O', stop_bits: 2, address: 1 }]);
    expect(append).toHaveBeenCalledWith('pulse generator connected on COM4 (addr 1, 9600 baud)');
  });
  it("a failure says what Qt says: port, baud, address and what to check", async () => {
    vi.mocked(bridge.pulseGeneratorCommand).mockResolvedValue({ ...ok(), ok: false, message: 'bus timeout' });
    await render();
    await click('Connect pulse generator'); await flush();
    expect(q('[role="alert"]').textContent).toBe('Failed to connect on COM4 (baud 9600, addr 1): bus timeout. Check wiring, port and Modbus address.');
  });
  it('refuses an address outside 1-247 with a message and sends nothing', async () => {
    await render();
    await edit('Pulse generator Modbus address', '255');
    await click('Connect pulse generator');
    expect(q('[role="alert"]').textContent).toContain('between 1 and 247'); expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled();
  });
  it('is locked while an experiment is active, with the reason', async () => {
    await render({ experimentActive: true });
    expect(button('Connect pulse generator').disabled).toBe(true);
    expect(button('Connect pulse generator').title).toContain('Locked while an experiment is active');
  });
  it('while connected the form shows the open connection and cannot be edited; Disconnect keeps an output running', async () => {
    vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue(connectedStatus({ baud: 19200, address: 6, port: 'COM6' }));
    await render();
    expect(field('Pulse generator port').value).toBe('COM6'); expect(field('Pulse generator baud').value).toBe('19200'); expect(field('Pulse generator Modbus address').value).toBe('6');
    expect(field('Pulse generator port').disabled).toBe(true); expect(button('Refresh').disabled).toBe(true); expect(button('Scan').disabled).toBe(true);
    await click('Disconnect pulse generator'); await flush();
    expect(commands()).toEqual([{ action: 'disconnect' }]);
  });
});

describe('Set / Start / Stop (D5): no Service mode, no arm', () => {
  beforeEach(() => { vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue(connectedStatus()); });
  it('Start writes frequency, duty and the output in ONE click', async () => {
    await render();
    await edit('Pulse generator frequency (Hz)', '1000'); await edit('Pulse generator duty (%)', '2'); await edit('Pulse generator channel', '2');
    await click('Start'); await flush();
    expect(commands()).toEqual([{ action: 'start', channel: 1, frequency_hz: 1000, duty_percent: 2 }]);
    expect(append).toHaveBeenCalledWith('pulse generator Ch2: started 1000 Hz, 2% duty');
  });
  it('Set applies frequency and duty without enabling the output', async () => {
    await render();
    await edit('Pulse generator frequency (Hz)', '2000'); await click('Set'); await flush();
    expect(commands()).toEqual([{ action: 'set', channel: 0, frequency_hz: 2000, duty_percent: 50 }]);
  });
  it('Stop gates the output off, with no arm', async () => {
    await render();
    await click('Stop'); await flush();
    expect(commands()).toEqual([{ action: 'enable', channel: 0, enabled: false }]);
    expect(append).toHaveBeenCalledWith('pulse generator Ch1: stopped (output off)');
  });
  it('are disabled until the generator is connected', async () => {
    vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue(idleStatus());
    await render();
    for (const name of ['Set', 'Start', 'Stop']) expect(button(name).disabled, name).toBe(true);
  });
  it('a refused value says why and sends nothing (frequency 400-40000, duty 0-100)', async () => {
    await render();
    await edit('Pulse generator frequency (Hz)', '399'); await click('Start');
    expect(q('[role="alert"]').textContent).toContain('between 400 and 40000');
    await edit('Pulse generator frequency (Hz)', '1000'); await edit('Pulse generator duty (%)', '101'); await click('Set');
    expect(q('[role="alert"]').textContent).toContain('between 0 and 100');
    expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled();
  });
  it('a backend failure is reported, never shown as success', async () => {
    vi.mocked(bridge.pulseGeneratorCommand).mockResolvedValue({ ...ok(), ok: false, message: 'write failed' });
    await render();
    await click('Start'); await flush();
    expect(q('[role="alert"]').textContent).toBe('Failed to start the pulse train: write failed');
    expect(append).not.toHaveBeenCalledWith(expect.stringContaining('started'));
  });
  it('while an experiment is active Set and Start are locked, Stop still works', async () => {
    await render({ experimentActive: true });
    expect(button('Set').disabled).toBe(true); expect(button('Start').disabled).toBe(true); expect(button('Start').title).toContain('acquisition clock');
    expect(button('Stop').disabled).toBe(false);
    await click('Stop'); await flush();
    expect(commands()).toEqual([{ action: 'enable', channel: 0, enabled: false }]);
  });
  it('blocks a double click on Start but queues a Stop pressed while Start is in flight (never lost, and last)', async () => {
    let finish!: (value: ReturnType<typeof ok>) => void;
    vi.mocked(bridge.pulseGeneratorCommand).mockImplementationOnce(() => new Promise(resolve => { finish = resolve; }));
    await render();
    await act(async () => { button('Start').click(); button('Start').click(); });
    expect(bridge.pulseGeneratorCommand).toHaveBeenCalledOnce();
    await click('Stop');
    expect(bridge.pulseGeneratorCommand).toHaveBeenCalledOnce();
    expect(host.textContent).toContain('Stop is queued behind the running command.');
    await act(async () => finish(ok())); await flush();
    expect(commands().map(c => c.action)).toEqual(['start', 'enable']);
    expect(commands()[1]).toEqual({ action: 'enable', channel: 0, enabled: false });
  });
  // Review of #881, blocking 1: a Stop pressed during a Stop must not survive and switch off the next Start's output.
  it('a Stop pressed while a Stop is in flight is sent right after it, never after the next Start', async () => {
    let finish!: (value: ReturnType<typeof ok>) => void;
    vi.mocked(bridge.pulseGeneratorCommand).mockImplementationOnce(() => new Promise(resolve => { finish = resolve; }));
    await render();
    await click('Stop'); await click('Stop');
    await act(async () => finish(ok())); await flush();
    expect(commands().map(c => c.action)).toEqual(['enable', 'enable']);
    await click('Start'); await flush();
    expect(commands().map(c => c.action)).toEqual(['enable', 'enable', 'start']);
  });
  // Review of #881, blocking 2: a queued Stop waits for the generator's answer only, not for the settings save or status read.
  it('a Stop queued behind Start is sent when the generator answers, while the settings save is still pending', async () => {
    let finish!: (value: ReturnType<typeof ok>) => void;
    vi.mocked(bridge.pulseGeneratorCommand).mockImplementationOnce(() => new Promise(resolve => { finish = resolve; }));
    await render();
    let readPrefs!: () => void;
    vi.mocked(bridge.getPreferences).mockImplementation(() => new Promise(resolve => { readPrefs = () => resolve(structuredClone(prefs)); }));
    await click('Start'); await click('Stop');
    await act(async () => finish(ok())); await flush();
    expect(commands().map(c => c.action)).toEqual(['start', 'enable']);
    expect(commands()[1]).toEqual({ action: 'enable', channel: 0, enabled: false });
    await act(async () => readPrefs()); await flush();   // the save completes afterwards (saves are serialised)
  });
  it('the coordinated live view owns the generator: no write is offered or sent', async () => {
    vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue(connectedStatus({ owned: true }));
    await render();
    for (const name of ['Set', 'Start', 'Stop', 'Disconnect pulse generator']) expect(button(name).disabled, name).toBe(true);
    expect(q('[aria-label="Pulse generator status"]').textContent).toContain('owned by the coordinated live view');
  });
  it("shows Qt's status wording for the selected channel", async () => {
    vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue(connectedStatus({ address: 3, channels: [channel(5000, 50, true), channel(1000.5, 2, false), channel(0, 0, false), channel(0, 0, false)] }));
    await render();
    expect(q('[aria-label="Pulse generator status"]').textContent).toBe('Verified addr 3 — Ch1: 5000 Hz, 50% duty, output ON');
    await edit('Pulse generator channel', '2');
    expect(q('[aria-label="Pulse generator status"]').textContent).toBe('Verified addr 3 — Ch2: 1000.5 Hz, 2% duty, output off');
  });
  it('Set / Start remember the values (frequency, duty, channel) for the next launch', async () => {
    await render();
    await edit('Pulse generator frequency (Hz)', '2500'); await edit('Pulse generator duty (%)', '25'); await click('Start'); await flush();
    expect(prefs.pulseGenerator).toMatchObject({ frequencyHz: 2500, dutyPercent: 25, channel: 1 });
  });
});

describe('Scan (HW-051)', () => {
  const device = (address: number, identification: number, display_name: string, claimed_by: string[]) => ({ kind: 3, provider_id: 'pulse-generator', display_name, system_path: 'COM4', persistent_id: '', sdk_index: -1, interface_index: -1, device_index: -1, stream_index: -1, bus_address: address,
    stable_identity: `COM4@${address}`, identity_strength: 1, identification, claimed_by, capabilities: [], synthetic: false, camera_type: -1, interface_id: '', device_id: '', stream_id: '', model_name: '', firmware_version: '', label: '' });
  const snapshot = (state: number, candidates: unknown[] = [], errors: unknown[] = []) => ({ valid: true, job_id: '5', generation: '1', state, complete: true, overflow: false, attempt: 1, max_attempts: 1, kinds: [3], candidates, errors, providers_run: [], origin: '' });
  it('probes addresses 1-16 read-only, lists what answered and fills the first generator into the address', async () => {
    vi.mocked(bridge.startDeviceDiscovery).mockResolvedValue({ accepted: true, coalesced: false, job_id: '5', rejection: 0, reason: '' });
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot(2, [device(2, 1, 'Address 2 — Modbus device (not a pulse generator, left untouched)', []), device(7, 0, 'Address 7 — pulse generator', ['pulse-generator'])]) as never);
    await render();
    await click('Scan'); await flush();
    expect(bridge.startDeviceDiscovery).toHaveBeenCalledWith(expect.objectContaining({ kinds: [3], hasSerialScope: true, serialPortName: 'COM4', baudRate: 9600, addressFrom: 1, addressTo: 16, perAddressTimeoutMs: 250 }));
    expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled();
    expect(field('Pulse generator Modbus address').value).toBe('7');
    const result = q('[aria-label="Pulse generator scan result"]').textContent!;
    expect(result).toContain('Address 7 — pulse generator'); expect(result).toContain('Modbus device (not a pulse generator, left untouched)');
  });
  it('reports a silent bus and a refused start', async () => {
    vi.mocked(bridge.startDeviceDiscovery).mockResolvedValue({ accepted: true, coalesced: false, job_id: '5', rejection: 0, reason: '' });
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot(2) as never);
    await render();
    await click('Scan'); await flush();
    expect(q('[aria-label="Pulse generator scan result"]').textContent).toContain('No Modbus devices responded on COM4 (addresses 1–16).');
    vi.mocked(bridge.startDeviceDiscovery).mockResolvedValue({ accepted: false, coalesced: false, job_id: '0', rejection: 13, reason: 'too many jobs' });
    await click('Scan'); await flush();
    expect(q('[role="alert"]').textContent).toBe('Could not start the scan: too many jobs');
  });
  it('a port-level failure is shown as advice, not as a silent bus', async () => {
    vi.mocked(bridge.startDeviceDiscovery).mockResolvedValue({ accepted: true, coalesced: false, job_id: '5', rejection: 0, reason: '' });
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot(2, [], [{ provider_id: 'p', kind: 2, message: 'port busy', endpoint: 'COM4' }]) as never);
    await render();
    await click('Scan'); await flush();
    expect(q('[role="alert"]').textContent).toContain('Could not open COM4 for scanning: port busy');
  });
  it('shows Scanning… and Cancel scan while it runs, and cancels the backend job', async () => {
    vi.mocked(bridge.startDeviceDiscovery).mockResolvedValue({ accepted: true, coalesced: false, job_id: '5', rejection: 0, reason: '' });
    vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(snapshot(1) as never);
    vi.mocked(bridge.cancelDeviceDiscovery).mockResolvedValue(true);
    await render();
    await click('Scan');
    expect(q('[aria-label="Pulse generator status"]').textContent).toBe('Scanning…');
    expect(button('Cancel scan')).toBeTruthy(); expect(button('Connect pulse generator').disabled).toBe(true);
    await click('Cancel scan');
    await act(async () => { await new Promise(r => setTimeout(r, 250)); });
    expect(bridge.cancelDeviceDiscovery).toHaveBeenCalledWith('5');
    expect(button('Scan')).toBeTruthy();
  });
});

describe('XGC + R5D preset for Live View (HW-057)', () => {
  const camera = (change: Record<string, unknown> = {}) => ({ doc: { path: '/cfg/mindvisionConfig.json', text: '', revision: 'r1' }, text: JSON.stringify({ camera_name: 'XGC', exposure_time_us: 5 }), dirty: false, busy: false, message: '', kind: 'json', run: vi.fn(async () => true), ...change });
  const studio = (cam: ReturnType<typeof camera>, change: Record<string, unknown> = {}) => ({ mindvisionDocument: cam, running: false, caps: { egrabber_script: true }, ...change }) as unknown as Partial<Studio>;
  it('is not offered outside the classic view or without the MindVision editor', async () => {
    await render();
    expect(button('Use XGC + R5D preset for Live View')).toBeUndefined();
    await render({}, studio(camera(), { caps: { egrabber_script: false } }));
    expect(button('Use XGC + R5D preset for Live View')).toBeUndefined();
  });
  it("saves Qt's rig preset and the selected generator connection into the camera document's live_view, and leaves the hardware alone", async () => {
    const cam = camera();
    await render({}, studio(cam));
    await edit('Pulse generator Modbus address', '4'); await edit('Pulse generator channel', '2'); await edit('Pulse generator baud', '19200');
    await click('Use XGC + R5D preset for Live View'); await flush();
    expect(cam.run).toHaveBeenCalledOnce();
    const [action, text] = cam.run.mock.calls[0] as unknown as [string, string];
    expect(action).toBe('save');
    const saved = JSON.parse(text);
    expect(saved).toMatchObject({ camera_name: 'XGC', width: 512, height: 96, trigger_mode: 2, ext_trig_signal_type: 0, strobe_mode: 1, strobe_polarity: 0 });
    expect(saved.live_view).toEqual({ enabled: true, port: 'COM4', address: 4, channel: 2, frequency_hz: 1000, duty_percent: 2, baud: 19200, data_bits: 8, parity: 'N', stop_bits: 1 });
    expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled();
    // Qt leaves the manual Set / Start values at the Align / Run rate afterwards.
    expect(field('Pulse generator frequency (Hz)').value).toBe('5000'); expect(field('Pulse generator duty (%)').value).toBe('10');
    expect(host.textContent).toContain('Apply the saved file to the camera before Play; nothing was started.');
  });
  it('refuses as Qt does: Live View running, no camera file, no port, broken JSON', async () => {
    const cam = camera();
    await render({}, studio(cam, { running: true }));
    expect(button('Use XGC + R5D preset for Live View').disabled).toBe(true);
    await render({}, studio(camera({ doc: null })));
    await click('Use XGC + R5D preset for Live View');
    expect(q('[role="alert"]').textContent).toContain('Load the MindVision camera JSON first');
    await render({}, studio(camera({ doc: { path: '', text: '', revision: '' } })));
    await click('Use XGC + R5D preset for Live View');
    expect(q('[role="alert"]').textContent).toContain('Save the MindVision camera JSON to a file first');
    const broken = camera({ text: '{nope' });
    await render({}, studio(broken));
    await click('Use XGC + R5D preset for Live View');
    expect(q('[role="alert"]').textContent).toBe('Fix the camera JSON before saving the preset.');
    expect(broken.run).not.toHaveBeenCalled();
    vi.mocked(bridge.listSerialPorts).mockResolvedValue({ ports: [] });
    const noPort = camera();
    await render({}, studio(noPort));
    await act(async () => root.unmount()); root = createRoot(host);
    await render({}, studio(noPort)); await click('Use XGC + R5D preset for Live View');
    expect(q('[role="alert"]').textContent).toContain('No serial port selected'); expect(noPort.run).not.toHaveBeenCalled();
  });
});
