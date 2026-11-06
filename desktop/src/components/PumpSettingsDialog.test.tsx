// @vitest-environment jsdom
// Settings > Syringe Pump Settings… (epic #868: HW-003, HW-004, HW-020, HW-021, HW-023): the port list with Refresh, the address
// scan with Qt's messages, the saved connection and volume, and an atomic OK / Cancel / Apply.
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { PumpSettingsDialog } from './PumpSettingsDialog';
import { bridge } from '../bridge';
import type { CmdResult, HostPumpScan, HostSettingsDocument, PlatformCapabilities, PumpStatus, SerialPortEntry } from '../bridge';

vi.mock('../bridge', () => ({bridge: {
  listSerialPorts: vi.fn(), fetchHostSettings: vi.fn(), hostSettingsCommand: vi.fn(), fetchPumpStatus: vi.fn(), pumpSetSyringeVolume: vi.fn(),
}}));
const ok = (message = 'ok', operation_id = '0'): CmdResult => ({ok: true, command: 10, message, operation_id});
const entry = (system_name: string, extra: Partial<SerialPortEntry> = {}): SerialPortEntry => ({system_name, system_location: `/dev/${system_name}`, description: '', manufacturer: '', serial_number: '', vendor_id: 0, product_id: 0, ...extra});
const idle: HostPumpScan = {state: 'idle', operation_id: '0', port: '', found: [], error: ''};
const pump: PumpStatus = {valid: true, connected: false, run_status: 0, current_flow_rate: 0, accumulated_volume: 0, min_flow_rate: 0, max_flow_rate: 100, stalled: false, com_port: 3, baud_rate: 115200, modbus_address: 1, configured_flow_rate: 0, flow_rate_unit: 100, direction: 0};
let host: HTMLDivElement, root: Root;
let saved: Record<string, unknown>, scan: HostPumpScan, scanPolls: number, scanAnswer: HostPumpScan | null, ports: SerialPortEntry[];
const append = vi.fn(), onClose = vi.fn();
const patches = () => vi.mocked(bridge.hostSettingsCommand).mock.calls.map(([request]) => request as Record<string, unknown>).filter(request => request.action === 'set_pumps').map(request => request.settings as Record<string, unknown>);
const scans = () => vi.mocked(bridge.hostSettingsCommand).mock.calls.map(([request]) => request as Record<string, unknown>).filter(request => request.action === 'scan_pump_addresses');

const control = (label: string) => host.querySelector(`[aria-label="${label}"]`) as HTMLSelectElement & HTMLInputElement;
const button = (name: string) => Array.from(host.querySelectorAll('button')).find(node => node.textContent === name)! as HTMLButtonElement;
const click = async (node: HTMLElement) => act(async () => node.click());
const type = async (input: HTMLInputElement, value: string) => act(async () => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value); input.dispatchEvent(new Event('input', {bubbles: true})); });
const choose = async (select: HTMLSelectElement, value: string) => act(async () => { Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value')!.set!.call(select, value); select.dispatchEvent(new Event('change', {bubbles: true})); });
const tick = async (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const status = () => host.querySelector('.pump-settings')!.textContent ?? '';
async function render(capabilities?: PlatformCapabilities) {
  await act(async () => root.render(<PumpSettingsDialog ready append={append} onClose={onClose} {...(capabilities ? {capabilities} : {})} />));
  await tick(0);
}
beforeEach(() => {
  Object.assign(globalThis, {IS_REACT_ACT_ENVIRONMENT: true});
  vi.useFakeTimers();
  vi.clearAllMocks();
  saved = {}; scan = {...idle}; scanPolls = 0; scanAnswer = null; ports = [entry('COM3', {description: 'USB-SERIAL CH340', vendor_id: 0x1a86, product_id: 0x7523}), entry('COM4')];
  vi.mocked(bridge.listSerialPorts).mockImplementation(async () => ({ports}));
  vi.mocked(bridge.fetchHostSettings).mockImplementation(async (): Promise<HostSettingsDocument> => {
    // A scan answers on the third read after it was started.
    if (scan.state === 'running' && ++scanPolls >= 3 && scanAnswer) scan = scanAnswer;
    return {valid: true, path: '/data/host_settings.json', settings: structuredClone(saved), load_error: '', boot_env: '', pump_scan: {...scan}};
  });
  vi.mocked(bridge.hostSettingsCommand).mockImplementation(async request => {
    const body = request as Record<string, unknown>;
    if (body.action === 'set_pumps') { for (const [key, value] of Object.entries(body.settings as Record<string, unknown>)) { if (value === null) delete saved[key]; else saved[key] = value; } return ok('Pump settings saved'); }
    if (body.action === 'scan_pump_addresses') { scan = {state: 'running', operation_id: '7', port: String(body.port), found: [], error: ''}; scanPolls = 0; return ok('Pump address scan started', '7'); }
    return {...ok(), ok: false, message: 'unknown'};
  });
  vi.mocked(bridge.fetchPumpStatus).mockResolvedValue(pump);
  vi.mocked(bridge.pumpSetSyringeVolume).mockResolvedValue(ok('Syringe volume set'));
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

describe('the form', () => {
  it('lists the enumerated ports with their identity and offers Refresh, keeping a selection that is still there', async () => {
    saved = {pump_sample_port: 'COM3'};
    await render();
    const options = Array.from(control('Sample pump serial port').options).map(o => o.textContent);
    expect(options).toEqual(['— select a port —', 'COM3 — USB-SERIAL CH340, 1a86:7523', 'COM4']);
    expect(control('Sample pump serial port').value).toBe('COM3');
    expect(control('Sheath pump serial port').value).toBe('');                // nothing saved: nothing preselected (Qt would save the first row)
    ports = [entry('COM3'), entry('COM4'), entry('COM5')];
    await click(button('Refresh'));
    expect(vi.mocked(bridge.listSerialPorts)).toHaveBeenCalledTimes(2);
    expect(Array.from(control('Sample pump serial port').options).map(o => o.value)).toContain('COM5');
    expect(control('Sample pump serial port').value).toBe('COM3');
  });
  it('keeps a saved port that is not listed (unplugged), and a saved system path, instead of showing the first row', async () => {
    saved = {pump_sample_port: 'COM9', pump_sheath_port: '/dev/ttyUSB0'};
    ports = [entry('ttyUSB0'), entry('COM3')];
    await render();
    expect(control('Sample pump serial port').value).toBe('COM9');
    expect(Array.from(control('Sample pump serial port').options).map(o => o.textContent)).toContain('COM9 (saved, not found)');
    expect(control('Sheath pump serial port').value).toBe('ttyUSB0');
  });
  it('starts from the saved connection with Qt\'s defaults (115200 baud, Sample address 1, Sheath 2) and offers the standard baud rates', async () => {
    saved = {pump_sample_baud_rate: 9600, pump_sample_address: 4, pump_sample_syringe_vol: 2.5, pump_sample_syringe_unit: 103};
    await render();
    expect([control('Sample pump baud rate').value, control('Sample pump Modbus address').value, control('Sample pump syringe volume').value, control('Sample pump syringe unit').value]).toEqual(['9600', '4', '2.5', '103']);
    expect([control('Sheath pump baud rate').value, control('Sheath pump Modbus address').value, control('Sheath pump syringe volume').value, control('Sheath pump syringe unit').value]).toEqual(['115200', '2', '', '100']);
    expect([control('Sample pump baud rate').getAttribute('min'), control('Sample pump baud rate').getAttribute('max')]).toEqual(['1200', '921600']);
    const list = host.querySelector('#pump-settings-baud')!;
    expect(Array.from(list.querySelectorAll('option')).map(o => o.getAttribute('value'))).toEqual(['1200', '2400', '4800', '9600', '19200', '38400', '57600', '115200', '230400', '460800', '921600']);
  });
  it('waits for the backend, and has no syringe volume for the instrument\'s peristaltic pumps', async () => {
    await act(async () => root.render(<PumpSettingsDialog ready={false} append={append} onClose={onClose} />));
    expect(status()).toContain('Waiting for the backend');
    expect(button('OK').disabled).toBe(true);
    const instrument = {pump: {model: 'tushui_peristaltic', port: '/dev/ttyPS1', sample_address: 3, sheath_address: 4, microliters_per_rev: 25}} as PlatformCapabilities;
    await act(async () => root.unmount()); root = createRoot(host);
    await render(instrument);
    expect(control('Sample pump syringe volume')).toBeNull();
    expect([control('Sample pump serial port').value, control('Sample pump Modbus address').value, control('Sheath pump Modbus address').value]).toEqual(['/dev/ttyPS1', '3', '4']);
  });
  it('closes on Cancel and Escape without saving, and on a click outside', async () => {
    await render();
    await type(control('Sample pump baud rate'), '9600');
    await click(button('Cancel'));
    await act(async () => { window.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape'})); });
    await click(host.querySelector('.modal-backdrop') as HTMLElement);
    expect(onClose).toHaveBeenCalledTimes(3);
    expect(patches()).toEqual([]);
  });
});

describe('Scan (HW-021)', () => {
  beforeEach(() => { saved = {pump_sample_port: 'COM3'}; });
  it('asks for a port first, with Qt\'s message, and scans nothing', async () => {
    await render();
    await click(control('Scan Sheath pump addresses'));
    expect(status()).toContain('Select a COM port first.');
    expect(scans()).toEqual([]);
  });
  it('scans 1-8 at 300 ms on the dialog\'s port and baud rate, lists the answers and fills the address with Use', async () => {
    scanAnswer = {state: 'completed', operation_id: '7', port: 'COM3', found: [1, 2], error: ''};
    await render();
    await type(control('Sample pump baud rate'), '57600');
    await click(control('Scan Sample pump addresses'));
    expect(scans()).toEqual([{action: 'scan_pump_addresses', port: 'COM3', baud: 57600, start: 1, end: 8, timeout_ms: 300}]);
    expect(status()).toContain('Scanning…'); expect(control('Scan Sample pump addresses').disabled).toBe(true);
    await tick(600);
    expect(status()).toContain('Found 2 responsive address(es) on COM3.');
    expect(Array.from(control('Sample pump scan result').options).map(o => o.value)).toEqual(['1', '2']);
    expect(append).toHaveBeenCalledWith('Sample pump scan: Found 2 responsive address(es) on COM3.');
    await choose(control('Sample pump scan result'), '2');
    await click(control('Use scanned Sample pump address'));
    expect(control('Sample pump Modbus address').value).toBe('2');
    expect(patches()).toEqual([]);                                               // Use fills the field; only OK / Apply saves
  });
  it('says no pump answered, with the port and baud rate', async () => {
    scanAnswer = {state: 'completed', operation_id: '7', port: 'COM3', found: [], error: ''};
    await render();
    await click(control('Scan Sample pump addresses')); await tick(600);
    expect(status()).toContain('No pump address responded on COM3 at 115200 baud.');
    expect(control('Use scanned Sample pump address').disabled).toBe(true);
  });
  it('says why a port could not be scanned, instead of "no pump answered"', async () => {
    scanAnswer = {state: 'failed', operation_id: '7', port: 'COM3', found: [], error: 'could not open the port: port busy'};
    await render();
    await click(control('Scan Sample pump addresses')); await tick(600);
    expect(host.querySelector('[role="alert"]')!.textContent).toBe('Scan failed: could not open the port: port busy.');
  });
  it('shows a scan the host refused, and refuses a baud rate that is not a number', async () => {
    vi.mocked(bridge.hostSettingsCommand).mockResolvedValueOnce({...ok(), ok: false, message: 'A pump address scan is already running'});
    await render();
    await click(control('Scan Sample pump addresses'));
    expect(status()).toContain('A pump address scan is already running');
    expect(control('Scan Sample pump addresses').disabled).toBe(false);
    await type(control('Sample pump baud rate'), 'fast');
    await click(control('Scan Sample pump addresses'));
    expect(status()).toContain('Baud rate must be a whole number.');
    expect(scans()).toHaveLength(1);
  });
});

describe('OK / Cancel / Apply (HW-020)', () => {
  it('saves both pumps in one write: Qt\'s keys, the exact port and the volume in the unit typed; OK then closes', async () => {
    saved = {pump_sample_flow_rate: 20};
    await render();
    await choose(control('Sample pump serial port'), 'COM3'); await choose(control('Sheath pump serial port'), 'COM4');
    await type(control('Sample pump baud rate'), '9600'); await type(control('Sheath pump Modbus address'), '6');
    await type(control('Sample pump syringe volume'), '2500'); await choose(control('Sheath pump syringe unit'), '103'); await type(control('Sheath pump syringe volume'), '5');
    expect(patches()).toEqual([]);
    await click(button('OK'));
    expect(patches()).toEqual([{
      pump_sample_port: 'COM3', pump_sample_com_port: 3, pump_sample_baud_rate: 9600, pump_sample_address: 1, pump_sample_syringe_vol: 2500, pump_sample_syringe_unit: 100,
      pump_sheath_port: 'COM4', pump_sheath_com_port: 4, pump_sheath_baud_rate: 115200, pump_sheath_address: 6, pump_sheath_syringe_vol: 5, pump_sheath_syringe_unit: 103,
    }]);
    expect(saved.pump_sample_flow_rate).toBe(20);                                  // the rate record is not touched
    expect(append).toHaveBeenCalledWith('Pump settings saved');
    expect(onClose).toHaveBeenCalledOnce();
  });
  it('Apply saves and stays open; Apply again saves again', async () => {
    await render();
    await choose(control('Sample pump serial port'), 'COM3');
    await click(button('Apply'));
    expect(patches()).toHaveLength(1); expect(onClose).not.toHaveBeenCalled(); expect(status()).toContain('Saved.');
    await type(control('Sample pump baud rate'), '19200'); await click(button('Apply'));
    expect(patches()).toHaveLength(2); expect(patches()[1].pump_sample_baud_rate).toBe(19200);
  });
  it('is atomic: one bad value on either pump saves nothing, names the pump, and keeps the dialog open', async () => {
    await render();
    await choose(control('Sample pump serial port'), 'COM3');
    await type(control('Sheath pump Modbus address'), '0');
    await click(button('OK'));
    expect(host.querySelector('[role="alert"]')!.textContent).toMatch(/^Sheath pump: Modbus address/);
    expect(patches()).toEqual([]); expect(onClose).not.toHaveBeenCalled();
    await type(control('Sheath pump Modbus address'), '2'); await type(control('Sample pump syringe volume'), '0.5');
    await click(button('Apply'));
    expect(host.querySelector('[role="alert"]')!.textContent).toMatch(/^Sample pump: .*cannot be sent exactly/);        // D9
    expect(patches()).toEqual([]);
  });
  it('applies the volume live to a connected pump as a whole number (0.5 mL goes out as 500 µL) and not to one that is not', async () => {
    vi.mocked(bridge.fetchPumpStatus).mockImplementation(async slot => ({...pump, connected: slot === 0}));
    await render();
    await type(control('Sample pump syringe volume'), '0.5'); await choose(control('Sample pump syringe unit'), '103');
    await type(control('Sheath pump syringe volume'), '100');
    await click(button('Apply'));
    expect(bridge.pumpSetSyringeVolume).toHaveBeenCalledOnce();
    expect(bridge.pumpSetSyringeVolume).toHaveBeenCalledWith(0, 500, 100);
    expect(append).toHaveBeenCalledWith('Sample pump: syringe volume set to 500 µL');
    expect(saved.pump_sample_syringe_vol).toBe(0.5); expect(saved.pump_sample_syringe_unit).toBe(103);   // what was typed is what is kept
  });
  it('keeps what it saved when a connected pump refuses the volume, says so, and does not close on OK', async () => {
    vi.mocked(bridge.fetchPumpStatus).mockResolvedValue({...pump, connected: true});
    vi.mocked(bridge.pumpSetSyringeVolume).mockResolvedValue({...ok(), ok: false, message: 'Device stalled'});
    await render();
    await type(control('Sample pump syringe volume'), '100');
    await click(button('OK'));
    expect(host.querySelector('[role="alert"]')!.textContent).toBe('Saved, but Sample pump did not take the syringe volume: Device stalled');
    expect(saved.pump_sample_syringe_vol).toBe(100); expect(onClose).not.toHaveBeenCalled();
  });
  it('says when the host refused the save, and does not close', async () => {
    vi.mocked(bridge.hostSettingsCommand).mockResolvedValue({...ok(), ok: false, message: 'There is no data directory to keep the settings in'});
    await render();
    await click(button('OK'));
    expect(host.querySelector('[role="alert"]')!.textContent).toBe('The pump settings were not saved: There is no data directory to keep the settings in');
    expect(onClose).not.toHaveBeenCalled();
  });
  it('does not start a second save while one is in flight', async () => {
    let release!: (value: CmdResult) => void;
    vi.mocked(bridge.hostSettingsCommand).mockReturnValueOnce(new Promise<CmdResult>(resolve => { release = resolve; }));
    await render();
    await click(button('Apply')); await click(button('Apply')); await click(button('OK'));
    expect(vi.mocked(bridge.hostSettingsCommand)).toHaveBeenCalledOnce();
    await act(async () => release(ok()));
  });
});
