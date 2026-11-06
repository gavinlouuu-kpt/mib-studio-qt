// @vitest-environment node
// The pulse generator panel's rules (epic #868, HW-043..HW-057): ranges, commands, gates (D5), Qt's status wording,
// port labels and re-resolution, tolerant settings, the scan report and the XGC + R5D preset.
// The backend header, as text: the drift test below compares the fallback limits against it.
import backendHeader from '../../../include/backend/services/PulseGeneratorService.h?raw';
import { describe, expect, it, vi } from 'vitest';
import type { DiscoveredDevice, DiscoverySnapshot, PulseGeneratorStatus, SerialPortEntry } from '../bridge';
import {
  DEFAULT_PULSE_LIMITS, PULSE_BAUD_RATES, PULSE_SCAN, buildLiveViewDocument, connectCommand, defaultPulseDraft, describeScan, draftFromSettings,
  parseAddress, parseChannel, parseDuty, parseFrequency, parsePulseSettings, portLabel, pulseGate, pulseLimits, pulseStatusLabel, resolvePort,
  runPulseScan, scanRequest, setCommand, settingsFromDraft, startCommand, stopCommand, validPorts, type PulseDraft, type PulseGateInput,
} from './pulseGeneratorModel';

const port = (name: string, extra: Partial<SerialPortEntry> = {}): SerialPortEntry => ({ system_name: name, system_location: `/dev/${name}`, description: '', manufacturer: '', serial_number: '', vendor_id: 0, product_id: 0, ...extra });
const draft = (change: Partial<PulseDraft> = {}): PulseDraft => ({ ...defaultPulseDraft(), portName: 'COM4', ...change });

describe('limits come from the backend constants', () => {
  it('matches PulseGeneratorService.h (the fallback cannot drift from the backend)', () => {
    const header = backendHeader;
    const number = (name: string) => Number(new RegExp(`${name}\\s*=\\s*([0-9.]+)`).exec(header)?.[1]);
    expect(number('CHANNEL_COUNT')).toBe(DEFAULT_PULSE_LIMITS.channelCount);
    expect(number('MIN_FREQUENCY_HZ')).toBe(DEFAULT_PULSE_LIMITS.minFrequencyHz);
    expect(number('MAX_FREQUENCY_HZ')).toBe(DEFAULT_PULSE_LIMITS.maxFrequencyHz);
  });
  it('prefers what the status reports and ignores junk', () => {
    expect(pulseLimits({ channel_count: 2, min_frequency_hz: 100, max_frequency_hz: 900 })).toEqual({ channelCount: 2, minFrequencyHz: 100, maxFrequencyHz: 900 });
    expect(pulseLimits(null)).toEqual(DEFAULT_PULSE_LIMITS);
    expect(pulseLimits({ channel_count: -1, min_frequency_hz: Number.NaN, max_frequency_hz: 0 })).toEqual(DEFAULT_PULSE_LIMITS);
  });
  it("offers Qt's baud list with 9600 the default, 8N1, address 1", () => {
    expect([...PULSE_BAUD_RATES]).toEqual([4800, 9600, 14400, 19200, 38400, 56000, 57600, 115200]);
    expect(defaultPulseDraft()).toMatchObject({ baud: 9600, dataBits: 8, parity: 'N', stopBits: 1, address: '1', channel: 1, frequency: '5000', duty: '50' });
  });
});

describe('input is refused with a message, never replaced', () => {
  it('address 1-247 whole numbers', () => {
    expect(parseAddress('1')).toBe(1); expect(parseAddress('247')).toBe(247);
    for (const bad of ['', '0', '248', '255', '1.5', 'x', '-3']) expect(() => parseAddress(bad), bad).toThrow(/between 1 and 247/);
  });
  it('frequency 400-40000 Hz in 0.01 Hz steps', () => {
    expect(parseFrequency('400')).toBe(400); expect(parseFrequency('40000')).toBe(40000); expect(parseFrequency('1000.25')).toBe(1000.25);
    for (const bad of ['399.99', '40000.01', '', 'abc', '1000.123', 'Infinity']) expect(() => parseFrequency(bad), bad).toThrow(/between 400 and 40000/);
    expect(() => parseFrequency('50', { channelCount: 4, minFrequencyHz: 10, maxFrequencyHz: 100 })).not.toThrow();
  });
  it('duty 0-100 % in 0.01 % steps', () => {
    expect(parseDuty('0')).toBe(0); expect(parseDuty('100')).toBe(100); expect(parseDuty('33.33')).toBe(33.33);
    for (const bad of ['-0.01', '100.01', '', 'x', '5.555']) expect(() => parseDuty(bad), bad).toThrow(/between 0 and 100/);
  });
  it('channel within the module', () => {
    expect(parseChannel(4)).toBe(4);
    for (const bad of [0, 5, 1.5]) expect(() => parseChannel(bad)).toThrow(/between 1 and 4/);
  });
});

describe('commands', () => {
  it('Connect sends the port, the framing and the address', () => {
    expect(connectCommand(draft({ baud: 19200, dataBits: 7, parity: 'E', stopBits: 2, address: '5' }))).toEqual({ action: 'connect', port: 'COM4', baud: 19200, data_bits: 7, parity: 'E', stop_bits: 2, address: 5 });
    expect(() => connectCommand(draft({ portName: '' }))).toThrow(/No serial port selected/);
    expect(() => connectCommand(draft({ address: '300' }))).toThrow(/Modbus address/);
  });
  it('Start is one command: frequency + duty + output on; Set the same without the output; Stop gates the output off', () => {
    const d = draft({ channel: 2, frequency: '1000', duty: '2' });
    expect(startCommand(d)).toEqual({ action: 'start', channel: 1, frequency_hz: 1000, duty_percent: 2 });
    expect(setCommand(d)).toEqual({ action: 'set', channel: 1, frequency_hz: 1000, duty_percent: 2 });
    expect(stopCommand(d)).toEqual({ action: 'enable', channel: 1, enabled: false });
  });
  it('a refused value sends nothing', () => {
    expect(() => startCommand(draft({ frequency: '5' }))).toThrow();
    expect(() => setCommand(draft({ duty: '101' }))).toThrow();
  });
});

describe('gates (D5)', () => {
  const base: PulseGateInput = { ready: true, experimentActive: false, connected: true, owned: false, busy: false, scanning: false };
  it('needs neither Service mode nor an arm: a connected, idle generator can Set, Start and Stop', () => {
    for (const action of ['set', 'start', 'stop'] as const) expect(pulseGate(base, action), action).toBe('');
    expect(pulseGate({ ...base, connected: false }, 'connect')).toBe('');
    expect(pulseGate({ ...base, connected: false }, 'scan')).toBe('');
  });
  it('locks Connect, Disconnect, Set and Start while an experiment is active, but never Stop', () => {
    const running = { ...base, experimentActive: true };
    for (const action of ['connect', 'disconnect', 'set', 'start'] as const) expect(pulseGate(running, action), action).toMatch(/Locked while an experiment is active/);
    expect(pulseGate(running, 'stop')).toBe('');
  });
  it('Stop stays available while a command is in flight (the panel queues it) and is refused only when not ready, owned or not connected', () => {
    expect(pulseGate({ ...base, busy: true }, 'stop')).toBe('');
    expect(pulseGate({ ...base, busy: true }, 'start')).toMatch(/in progress/);
    expect(pulseGate({ ...base, ready: false }, 'stop')).toMatch(/not ready/);
    expect(pulseGate({ ...base, owned: true }, 'stop')).toMatch(/coordinated live view/);
    expect(pulseGate({ ...base, connected: false }, 'stop')).toMatch(/Connect/);
  });
  it('the coordinated live view owns the generator: every write is refused', () => {
    for (const action of ['connect', 'disconnect', 'set', 'start', 'stop'] as const) expect(pulseGate({ ...base, owned: true }, action), action).toMatch(/coordinated live view/);
  });
  it('port settings and Scan need a disconnected generator; a scan blocks everything else', () => {
    expect(pulseGate(base, 'refresh')).toMatch(/Disconnect/); expect(pulseGate(base, 'scan')).toMatch(/Disconnect/);
    expect(pulseGate({ ...base, connected: false, scanning: true }, 'scan')).toBe('');
    expect(pulseGate({ ...base, connected: false, scanning: true }, 'connect')).toMatch(/scan/);
  });
});

describe("status label (Qt's wording)", () => {
  const status = (change: Partial<PulseGeneratorStatus> = {}): PulseGeneratorStatus => ({ valid: true, connected: true, owned: false, error: 'ok', port: 'COM4', baud: 9600, address: 3,
    channels: [{ frequency_hz: 5000, duty_percent: 50, output_enabled: true }, { frequency_hz: 1000.456, duty_percent: 2, output_enabled: false }], ...change });
  it('Verified addr N — ChX: f Hz, d% duty, output ON/off', () => {
    expect(pulseStatusLabel({ status: status(), scanning: false, channel: 1 })).toBe('Verified addr 3 — Ch1: 5000 Hz, 50% duty, output ON');
    expect(pulseStatusLabel({ status: status(), scanning: false, channel: 2 })).toBe('Verified addr 3 — Ch2: 1000.46 Hz, 2% duty, output off');
  });
  it('Disconnected, Disconnected — <error>, Scanning…', () => {
    expect(pulseStatusLabel({ status: status({ connected: false }), scanning: false, channel: 1 })).toBe('Disconnected');
    expect(pulseStatusLabel({ status: status({ connected: false, error: 'bus timeout' }), scanning: false, channel: 1 })).toBe('Disconnected — bus timeout');
    expect(pulseStatusLabel({ status: status({ connected: false, error: 'bus timeout' }), scanning: true, channel: 1 })).toBe('Scanning…');
    expect(pulseStatusLabel({ status: null, scanning: false, channel: 1 })).toBe('Status unavailable');
  });
  it('says when the coordinated live view owns it', () => {
    expect(pulseStatusLabel({ status: status({ owned: true }), scanning: false, channel: 1 })).toContain('owned by the coordinated live view');
  });
});

describe('serial ports', () => {
  it('labels a port with description, S/N and VID:PID as Qt does', () => {
    expect(portLabel(port('ttyUSB0', { description: 'FT232R USB UART', serial_number: 'A123', vendor_id: 0x0403, product_id: 0x6001 }))).toBe('ttyUSB0 — FT232R USB UART, S/N A123, 0403:6001');
    expect(portLabel(port('ttyS0'))).toBe('ttyS0');
    expect(portLabel(port('COM3', { vendor_id: 0x1a86, product_id: 0x7523 }))).toBe('COM3 — 1a86:7523');
  });
  it('accepts only well-formed entries', () => {
    expect(validPorts({ ports: [{ system_name: 'COM4', vendor_id: 1027, product_id: 24577 }, { system_name: '' }, null, { nope: 1 }] })).toHaveLength(1);
    expect(validPorts(null)).toEqual([]); expect(validPorts({ ports: 'x' })).toEqual([]);
  });
  describe('re-resolution of the saved port', () => {
    const ports = [port('ttyUSB1', { serial_number: 'A1', vendor_id: 1, product_id: 2 }), port('ttyUSB2', { serial_number: 'B2', vendor_id: 1, product_id: 2 }), port('ttyS0')];
    it('the saved system name wins', () => {
      expect(resolvePort(ports, { portName: 'ttyS0', portSerialNumber: 'A1', portVid: 1, portPid: 2 })).toEqual({ portName: 'ttyS0', via: 'name' });
    });
    it('a renamed node is found by its USB identity when it matches exactly one port', () => {
      expect(resolvePort(ports, { portName: 'ttyUSB0', portSerialNumber: 'A1', portVid: 1, portPid: 2 })).toEqual({ portName: 'ttyUSB1', via: 'usb' });
    });
    it('an ambiguous or unknown identity leaves the choice to the operator', () => {
      const twins = [port('ttyUSB1', { vendor_id: 1, product_id: 2 }), port('ttyUSB2', { vendor_id: 1, product_id: 2 })];
      expect(resolvePort(twins, { portName: 'gone', portSerialNumber: '', portVid: 1, portPid: 2 })).toEqual({ portName: '', via: 'ambiguous' });
      expect(resolvePort(ports, { portName: 'gone', portSerialNumber: 'Z', portVid: 9, portPid: 9 })).toEqual({ portName: '', via: 'none' });
      expect(resolvePort(ports, {})).toEqual({ portName: '', via: 'none' });
    });
  });
});

describe('persistence record', () => {
  it('restores field by field: an invalid value keeps its default, the rest is restored', () => {
    const parsed = parsePulseSettings({ portName: 'COM4', baud: 12345, dataBits: 7, parity: 'Q', stopBits: 2, address: 300, channel: 3, frequencyHz: 1000, dutyPercent: 120, portVid: 1027 });
    expect(parsed).toEqual({ portName: 'COM4', dataBits: 7, stopBits: 2, channel: 3, frequencyHz: 1000, portVid: 1027 });
    expect(draftFromSettings(parsed)).toMatchObject({ portName: 'COM4', baud: 9600, dataBits: 7, parity: 'N', stopBits: 2, address: '1', channel: 3, frequency: '1000', duty: '50' });
    expect(parsePulseSettings(null)).toEqual({}); expect(parsePulseSettings('x')).toEqual({});
  });
  it('saves the port with its USB identity and leaves out a half-typed number', () => {
    const ports = [port('COM4', { serial_number: 'SIMPG001', vendor_id: 0x0403, product_id: 0x6001 })];
    const saved = settingsFromDraft(draft({ frequency: '12', duty: '25', address: '7' }), ports);
    expect(saved).toMatchObject({ portName: 'COM4', portSerialNumber: 'SIMPG001', portVid: 0x0403, portPid: 0x6001, baud: 9600, address: 7, dutyPercent: 25, channel: 1 });
    expect(saved).not.toHaveProperty('frequencyHz');
  });
});

describe('scan (HW-051)', () => {
  it('is bounded, read-only and covers addresses 1-16 on the chosen port with the chosen framing', () => {
    expect(PULSE_SCAN).toEqual({ addressFrom: 1, addressTo: 16, perAddressTimeoutMs: 250 });
    expect(scanRequest(draft({ baud: 19200, dataBits: 7, parity: 'O', stopBits: 2 }))).toMatchObject({ kinds: [3], hasSerialScope: true, serialPortName: 'COM4', baudRate: 19200, dataBits: 7, parity: 'O', stopBits: 2, addressFrom: 1, addressTo: 16, perAddressTimeoutMs: 250 });
    expect(() => scanRequest(draft({ portName: '' }))).toThrow();
  });
  const device = (address: number, identification: number, name: string, claimed: string[] = []): DiscoveredDevice => ({ kind: 3, provider_id: 'p', display_name: name, system_path: 'COM4', persistent_id: '', sdk_index: -1, interface_index: -1, device_index: -1, stream_index: -1,
    bus_address: address, stable_identity: '', identity_strength: 1, identification, claimed_by: claimed, capabilities: [], synthetic: false, camera_type: -1, interface_id: '', device_id: '', stream_id: '', model_name: '', firmware_version: '', label: '' });
  it('reports what answered and fills the first pulse generator, not another Modbus device', () => {
    const report = describeScan({ state: 2, errors: [], candidates: [device(2, 1, 'Address 2 — Modbus device (not a pulse generator, left untouched)'), device(5, 0, 'Address 5 — pulse generator', ['pulse-generator']), device(9, 0, 'Address 9 — pulse generator', ['pulse-generator'])] }, 'COM4');
    expect(report.firstGenerator).toBe(5); expect(report.lines).toHaveLength(3); expect(report.failed).toBe(false);
  });
  it('says so when nothing answered, and a port-level failure is different advice', () => {
    expect(describeScan({ state: 2, errors: [], candidates: [] }, 'COM4').summary).toBe('No Modbus devices responded on COM4 (addresses 1–16).');
    const failed = describeScan({ state: 2, candidates: [], errors: [{ provider_id: 'p', kind: 2, message: 'port busy', endpoint: 'COM4' }] }, 'COM4');
    expect(failed.failed).toBe(true); expect(failed.summary).toContain('Could not open COM4 for scanning: port busy');
  });
  describe('runPulseScan', () => {
    const snapshot = (state: number, valid = true): DiscoverySnapshot => ({ valid, job_id: '7', generation: '1', state, complete: true, overflow: false, attempt: 1, max_attempts: 1, kinds: [3], candidates: [], errors: [], providers_run: [], origin: '' });
    it('polls to the end', async () => {
      const fetch = vi.fn().mockResolvedValueOnce(snapshot(1)).mockResolvedValueOnce(snapshot(2));
      const result = await runPulseScan({ start: async () => ({ accepted: true, job_id: '7', reason: '', rejection: 0 }), fetch, cancel: vi.fn(), sleep: async () => {} }, scanRequest(draft()));
      expect(result?.state).toBe(2); expect(fetch).toHaveBeenCalledTimes(2);
    });
    it('rejects a refused start with the reason', async () => {
      await expect(runPulseScan({ start: async () => ({ accepted: false, job_id: '0', reason: 'too many jobs', rejection: 13 }), fetch: vi.fn(), cancel: vi.fn() }, scanRequest(draft()))).rejects.toThrow('Could not start the scan: too many jobs');
    });
    it('cancels the backend job when the operator cancels, and returns nothing', async () => {
      let cancelled = false; const cancel = vi.fn().mockResolvedValue(true);
      const fetch = vi.fn().mockImplementation(async () => { cancelled = true; return snapshot(1); });
      const result = await runPulseScan({ start: async () => ({ accepted: true, job_id: '7', reason: '', rejection: 0 }), fetch, cancel, sleep: async () => {} }, scanRequest(draft()), () => cancelled);
      expect(result).toBeNull(); expect(cancel).toHaveBeenCalledWith('7');
    });
    it('gives up after the deadline and cancels', async () => {
      let t = 0; const cancel = vi.fn().mockResolvedValue(true);
      await expect(runPulseScan({ start: async () => ({ accepted: true, job_id: '7', reason: '', rejection: 0 }), fetch: async () => snapshot(1), cancel, sleep: async () => { t += 40_000; } }, scanRequest(draft()), () => false, { now: () => t })).rejects.toThrow('timed out');
      expect(cancel).toHaveBeenCalled();
    });
  });
});

describe('XGC + R5D preset (HW-057)', () => {
  const doc = JSON.stringify({ camera_name: 'XGC', exposure_time_us: 5, width: 1920, custom_key: 'kept' });
  it("writes Qt's rig preset and the generator connection into live_view, keeping other keys", () => {
    const out = JSON.parse(buildLiveViewDocument(doc, draft({ portName: 'ttyUSB0', baud: 19200, dataBits: 7, parity: 'E', stopBits: 2, address: '4', channel: 2 })));
    expect(out).toMatchObject({ camera_name: 'XGC', custom_key: 'kept', width: 512, height: 96, offset_x: 0, offset_y: 0, exposure_time_us: 100, auto_exposure_enabled: false, trigger_mode: 2,
      ext_trig_signal_type: 0, trigger_count: 1, ext_trig_jitter_us: 0, acq_trigger_delay_us: 0, strobe_mode: 1, strobe_delay_us: 0, strobe_pulse_width_us: 100, strobe_polarity: 0 });
    expect(out.live_view).toEqual({ enabled: true, port: 'ttyUSB0', address: 4, channel: 2, frequency_hz: 1000, duty_percent: 2, baud: 19200, data_bits: 7, parity: 'E', stop_bits: 2 });
  });
  it("refuses like Qt: no port, broken JSON, JSON that is not an object", () => {
    expect(() => buildLiveViewDocument(doc, draft({ portName: '' }))).toThrow(/No serial port selected/);
    expect(() => buildLiveViewDocument('{nope', draft())).toThrow('Fix the camera JSON before saving the preset.');
    expect(() => buildLiveViewDocument('[1]', draft())).toThrow('Fix the camera JSON before saving the preset.');
  });
});
