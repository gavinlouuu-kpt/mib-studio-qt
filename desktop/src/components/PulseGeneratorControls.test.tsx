// @vitest-environment jsdom
import {act} from 'react';
import {createRoot, type Root} from 'react-dom/client';
import {beforeEach, afterEach, it, expect, vi} from 'vitest';
import {PulseGeneratorControls} from './PulseGeneratorControls';
import {bridge} from '../bridge';
import {CAMERA_SELECTION_MODES, DISCOVERY_IDENTIFICATION_STATUSES, DISCOVERY_JOB_STATES} from '../bridgeContract';
import {invoke} from '../transport';
import {confirm} from '../transport/dialogs';
vi.mock('../bridge', () => ({bridge: {
  pulseGeneratorStatus: vi.fn(), pulseGeneratorCommand: vi.fn(), getPreferences: vi.fn(), setPreferences: vi.fn(),
  startDeviceDiscovery: vi.fn(), fetchDeviceDiscovery: vi.fn(), cancelDeviceDiscovery: vi.fn(),
  fetchCameraSelection: vi.fn(), selectMindVisionCamera: vi.fn(),
}}));
vi.mock('../transport', () => ({invoke: vi.fn()}));
vi.mock('../transport/dialogs', () => ({confirm: vi.fn()}));
let host: HTMLDivElement, root: Root;
const disarm = vi.fn(), append = vi.fn();
const button = (name: string) => [...host.querySelectorAll('button')].find(x => x.textContent === name)!;
const field = <T extends HTMLElement>(label: string) => [...host.querySelectorAll('label')].find(x => x.textContent?.startsWith(label))!.querySelector('input,select') as unknown as T;
async function type(label: string, value: string) {
  const element = field<HTMLInputElement | HTMLSelectElement>(label);
  const prototype = element instanceof HTMLSelectElement ? HTMLSelectElement.prototype : HTMLInputElement.prototype;
  await act(async () => { Object.getOwnPropertyDescriptor(prototype, 'value')!.set!.call(element, value); element.dispatchEvent(new Event(element instanceof HTMLSelectElement ? 'change' : 'input', {bubbles: true})); });
}
const ok = (message = 'Done') => ({ok: true, command: 12, operation_id: '0', message});
const channelState = (output_enabled: boolean) => ({frequency_hz: 5000, duty_percent: 50, output_enabled});
const status = {valid: true, connected: true, owned: false, error: 'ok', port: '/dev/test', baud: 9600, address: 3, channels: [channelState(false), channelState(true)]};
const written = () => vi.mocked(bridge.pulseGeneratorCommand).mock.calls.map(call => call[0]);
beforeEach(() => {
  Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
  vi.clearAllMocks();
  vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue(status);
  vi.mocked(bridge.pulseGeneratorCommand).mockResolvedValue(ok());
  vi.mocked(bridge.getPreferences).mockResolvedValue({});
  vi.mocked(bridge.setPreferences).mockResolvedValue(undefined);
  vi.mocked(bridge.cancelDeviceDiscovery).mockResolvedValue(true);
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
async function render(props = {}) {await act(async () => root.render(<PulseGeneratorControls ready experimentActive={false} mode="service" armed onDisarm={disarm} append={append} {...props} />));}

it('does not write on mount, and one arm is enough for a cold Start (frequency, duty, enable)', async () => {
  await render();
  expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled();
  await type('Frequency', '1000'); await type('Duty', '2');
  await act(async () => button('Start').click());
  expect(disarm).toHaveBeenCalledOnce();
  expect(written()).toEqual([{action: 'frequency', channel: 0, value: 1000}, {action: 'duty', channel: 0, value: 2}, {action: 'enable', channel: 0, enabled: true}]);
  expect(append).toHaveBeenCalledWith('Start done (frequency, duty, enable output).');
});
it('Set writes frequency and duty only', async () => {
  await render(); await act(async () => button('Set').click());
  expect(written().map(x => x.action)).toEqual(['frequency', 'duty']);
  expect(disarm).toHaveBeenCalledOnce();
});
it('stops a failed Start at the failing step and says which one', async () => {
  vi.mocked(bridge.pulseGeneratorCommand).mockImplementation(async request => request.action === 'duty' ? {...ok('bus timeout'), ok: false} : ok());
  await render(); await act(async () => button('Start').click());
  expect(written().map(x => x.action)).toEqual(['frequency', 'duty']);
  expect(host.querySelector('[role=alert]')!.textContent).toBe('Start failed at step 2 of 3 (duty): bus timeout. Already written: frequency.');
  expect(append).not.toHaveBeenCalled();
});
it('rejects a bad value before writing anything', async () => {
  await render(); await type('Duty', '150');
  await act(async () => button('Start').click());
  expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled();
  expect(disarm).not.toHaveBeenCalled(); // a refused value does not spend the arm
  expect(host.querySelector('[role=alert]')!.textContent).toContain('Duty must be');
});
it('Start needs Service mode, an arm and an idle experiment; Stop needs none of them', async () => {
  await render({armed: false});
  expect(button('Start').disabled).toBe(true); expect(button('Set').disabled).toBe(true); expect(button('Stop').disabled).toBe(false);
  await render({experimentActive: true});
  expect(button('Start').disabled).toBe(true);
  await act(async () => button('Stop').click());
  expect(written()).toEqual([{action: 'enable', channel: 0, enabled: false}]);
  expect(disarm).not.toHaveBeenCalled();
  expect(append).toHaveBeenCalledWith('Stop done (disable output).');
});
it('Stop disables the selected channel', async () => {
  await render({mode: 'operator', armed: false}); await type('Channel', '1');
  await act(async () => button('Stop').click());
  expect(written()).toEqual([{action: 'enable', channel: 1, enabled: false}]);
});
it('does not override coordinated live-view ownership', async () => {
  vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue({...status, owned: true}); await render();
  for (const name of ['Start', 'Set', 'Stop', 'Disconnect pulse generator']) expect(button(name).disabled, name).toBe(true);
});
it('reports backend failures without success and blocks duplicate requests', async () => {
  let resolve!: (x: ReturnType<typeof ok>) => void; vi.mocked(bridge.pulseGeneratorCommand).mockReturnValue(new Promise(r => {resolve = r;}));
  await render(); await act(async () => {button('Start').click(); button('Start').click();});
  expect(bridge.pulseGeneratorCommand).toHaveBeenCalledOnce();
  await act(async () => resolve({...ok('Port lost'), ok: false}));
  expect(host.textContent).toContain('Port lost');
  expect(bridge.pulseGeneratorCommand).toHaveBeenCalledOnce();
});
it('shows the status the way Qt words it', async () => {
  await render();
  expect(host.querySelector('p')!.textContent).toBe('Verified addr 3 — Ch1: 5000 Hz, 50% duty, output off');
  await type('Channel', '1');
  expect(host.querySelector('p')!.textContent).toBe('Verified addr 3 — Ch2: 5000 Hz, 50% duty, output ON');
  vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue({...status, connected: false, error: 'port busy'});
  await act(async () => { await new Promise(resolve => setTimeout(resolve, 1100)); });
  expect(host.querySelector('p')!.textContent).toBe('Disconnected — port busy');
});

// ---- Connect, serial settings and persistence (HW-050, HW-056)
it('connects with every serial setting and saves the settings', async () => {
  vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue({...status, connected: false});
  await render();
  await type('System serial port', '/dev/ttyUSB7'); await type('Baud', '19200'); await type('Data bits', '7'); await type('Parity', 'E'); await type('Stop bits', '2'); await type('Modbus address', '5');
  await act(async () => button('Connect pulse generator').click());
  expect(written()).toEqual([{action: 'connect', port: '/dev/ttyUSB7', baud: 19200, data_bits: 7, parity: 'E', stop_bits: 2, address: 5}]);
  expect(disarm).not.toHaveBeenCalled();
  expect(bridge.setPreferences).toHaveBeenCalledWith({pulseGenerator: {port: '/dev/ttyUSB7', baud: 19200, dataBits: 7, parity: 'E', stopBits: 2, channel: 0, address: 5, frequencyHz: 5000, dutyPercent: 50}});
});
it('a failed connect saves nothing', async () => {
  vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue({...status, connected: false});
  vi.mocked(bridge.pulseGeneratorCommand).mockResolvedValue({...ok('port unavailable'), ok: false});
  await render(); await type('System serial port', '/dev/ttyUSB7');
  await act(async () => button('Connect pulse generator').click());
  expect(bridge.setPreferences).not.toHaveBeenCalled();
});
it('Start saves the settings too', async () => {
  await render(); await type('Frequency', '2000');
  await act(async () => button('Start').click());
  expect(bridge.setPreferences).toHaveBeenCalledWith({pulseGenerator: expect.objectContaining({frequencyHz: 2000, dutyPercent: 50})});
});
it('restores the saved settings on mount', async () => {
  vi.mocked(bridge.getPreferences).mockResolvedValue({pulseGenerator: {port: '/dev/ttyUSB3', baud: 38400, dataBits: 7, parity: 'O', stopBits: 2, address: 9, channel: 1, frequencyHz: 1500, dutyPercent: 7}});
  await render();
  expect(field<HTMLInputElement>('System serial port').value).toBe('/dev/ttyUSB3');
  expect(field<HTMLSelectElement>('Baud').value).toBe('38400');
  expect(field<HTMLSelectElement>('Parity').value).toBe('O');
  expect(field<HTMLInputElement>('Modbus address').value).toBe('9');
  expect(field<HTMLSelectElement>('Channel').value).toBe('1');
  expect(field<HTMLInputElement>('Frequency').value).toBe('1500');
  expect(field<HTMLInputElement>('Duty').value).toBe('7');
});
it('offers the Qt serial choices', async () => {
  await render();
  const options = (label: string) => [...field<HTMLSelectElement>(label).options].map(option => option.value);
  expect(options('Baud')).toEqual(['4800', '9600', '14400', '19200', '38400', '56000', '57600', '115200']);
  expect(options('Data bits')).toEqual(['8', '7']); expect(options('Parity')).toEqual(['N', 'E', 'O']); expect(options('Stop bits')).toEqual(['1', '2']);
  expect(field<HTMLSelectElement>('Baud').value).toBe('9600');
});

// ---- Scan (HW-051)
const generator = {kind: 3, display_name: 'Address 4 — pulse generator', bus_address: 4, identification: DISCOVERY_IDENTIFICATION_STATUSES.Identified};
const scanSnapshot = (patch = {}) => ({valid: true, job_id: '7', state: DISCOVERY_JOB_STATES.Completed, complete: true, candidates: [], errors: [], ...patch});
it('scans addresses 1-16 of the selected port, lists the responders and fills the first generator address', async () => {
  vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue({...status, connected: false});
  vi.mocked(bridge.startDeviceDiscovery).mockResolvedValue({accepted: true, coalesced: false, job_id: '7', rejection: 0, reason: ''});
  vi.mocked(bridge.fetchDeviceDiscovery).mockResolvedValue(scanSnapshot({candidates: [{...generator, bus_address: 2, display_name: 'Address 2 — Modbus device (not a pulse generator, left untouched)', identification: DISCOVERY_IDENTIFICATION_STATUSES.Unidentified}, generator]}) as never);
  await render(); await type('System serial port', '/dev/ttyUSB0'); await type('Modbus address', '9');
  await act(async () => button('Scan').click());
  expect(bridge.startDeviceDiscovery).toHaveBeenCalledWith(expect.objectContaining({kinds: [3], hasSerialScope: true, serialPortName: '/dev/ttyUSB0', baudRate: 9600, dataBits: 8, parity: 'N', stopBits: 1, addressFrom: 1, addressTo: 16}));
  expect(field<HTMLInputElement>('Modbus address').value).toBe('4');
  expect(host.textContent).toContain('Address 2 — Modbus device (not a pulse generator, left untouched)');
  expect(host.textContent).toContain('Address 4 — pulse generator');
  expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled(); // a scan never writes
});
it('shows Scanning… while the scan runs and can cancel it', async () => {
  vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue({...status, connected: false});
  vi.mocked(bridge.startDeviceDiscovery).mockResolvedValue({accepted: true, coalesced: false, job_id: '7', rejection: 0, reason: ''});
  let cancelled = false;
  vi.mocked(bridge.fetchDeviceDiscovery).mockImplementation(async () => scanSnapshot({state: cancelled ? DISCOVERY_JOB_STATES.Cancelled : DISCOVERY_JOB_STATES.Running}) as never);
  vi.mocked(bridge.cancelDeviceDiscovery).mockImplementation(async () => { cancelled = true; return true; });
  await render(); await type('System serial port', '/dev/ttyUSB0');
  await act(async () => { void button('Scan').click(); });
  expect(host.querySelector('p')!.textContent).toBe('Scanning…');
  expect(button('Connect pulse generator').disabled).toBe(true);
  await act(async () => { button('Cancel scan').click(); await new Promise(resolve => setTimeout(resolve, 250)); });
  expect(bridge.cancelDeviceDiscovery).toHaveBeenCalledWith('7');
  expect(host.querySelector('p')!.textContent).toBe('Disconnected');
  expect(button('Scan').disabled).toBe(false);
  expect(host.querySelector('[role=alert]')).toBeNull();
});
it('needs a port to scan', async () => {
  vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue({...status, connected: false});
  await render(); await act(async () => button('Scan').click());
  expect(bridge.startDeviceDiscovery).not.toHaveBeenCalled();
  expect(host.querySelector('[role=alert]')!.textContent).toBe('Enter or select a serial port before scanning.');
});
it('cannot scan while connected', async () => {
  await render(); expect(button('Scan').disabled).toBe(true);
});

// ---- Rig preset (HW-057)
it('Use XGC + R5D preset saves the rig preset and generator link into the camera file, then resets the bench fields', async () => {
  vi.mocked(bridge.fetchCameraSelection).mockResolvedValue({valid: true, configured: true, running: false, mode: CAMERA_SELECTION_MODES.MindVision, mindvision_index: 0, label: 'MV', mindvision_config_path: '/data/camera.json'} as never);
  vi.mocked(bridge.selectMindVisionCamera).mockResolvedValue(ok('applied'));
  vi.mocked(confirm).mockResolvedValue(true);
  vi.mocked(invoke).mockImplementation((async (_command: string, args?: Record<string, unknown>) => args?.action === 'read' ? {path: '/data/camera.json', text: '{"gamma":100}', revision: 'r1'} : {path: args?.path, text: args?.text, revision: 'r2'}) as never);
  await render(); await type('System serial port', '/dev/ttyUSB0');
  await act(async () => button('Use XGC + R5D preset for Live View').click());
  expect(bridge.selectMindVisionCamera).toHaveBeenCalledWith(0, 'MV', '/data/camera.json');
  expect(append).toHaveBeenCalledWith(expect.stringContaining('rig preset (XGC + R5D) saved'));
  expect(field<HTMLInputElement>('Frequency').value).toBe('5000'); expect(field<HTMLInputElement>('Duty').value).toBe('10');
  expect(bridge.pulseGeneratorCommand).not.toHaveBeenCalled(); // does not start hardware
  expect(disarm).not.toHaveBeenCalled(); // not an actuation: no arm needed
});
it('the rig preset says why it cannot proceed', async () => {
  vi.mocked(bridge.fetchCameraSelection).mockResolvedValue({valid: true, configured: false, mode: CAMERA_SELECTION_MODES.None} as never);
  await render(); await type('System serial port', '/dev/ttyUSB0');
  await act(async () => button('Use XGC + R5D preset for Live View').click());
  expect(host.querySelector('[role=alert]')!.textContent).toBe('Select a MindVision camera on the Connect stage first.');
});
