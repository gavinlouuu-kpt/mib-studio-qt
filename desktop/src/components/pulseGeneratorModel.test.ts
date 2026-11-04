import {describe, expect, it, vi} from 'vitest';
import type {DiscoveredDevice, DiscoverySnapshot, PulseGeneratorStatus} from '../bridge';
import {DISCOVERY_ERROR_KINDS, DISCOVERY_IDENTIFICATION_STATUSES, DISCOVERY_JOB_STATES} from '../bridgeContract';
import {
  BAUD_RATES, channelCount, connectStep, DEFAULT_PULSE_FORM, PULSE_LIMITS, runSteps, scanRequest, setSteps, startSteps, statusLabel, stopStep,
  summariseScan, type PulseForm,
} from './pulseGeneratorModel';
// The backend sources the limits mirror (the C++ is the enforcement; this fails when one side moves alone).
import serviceHeader from '../../../include/backend/services/PulseGeneratorService.h?raw';
import facadeSource from '../../../src/backend/app/BackendFacade.cpp?raw';

const form = (patch: Partial<PulseForm> = {}): PulseForm => ({...DEFAULT_PULSE_FORM, port: '/dev/ttyUSB0', ...patch});
const channel = {frequency_hz: 5000, duty_percent: 50, output_enabled: true};
const status = (patch: Partial<PulseGeneratorStatus> = {}): PulseGeneratorStatus => ({valid: true, connected: true, owned: false, error: 'ok', port: '/dev/ttyUSB0', baud: 9600, address: 3, channels: [channel, channel, channel, channel], ...patch});
const device = (patch: Partial<DiscoveredDevice>): DiscoveredDevice => ({kind: 3, provider_id: 'pulse-generator', display_name: 'Address 1 — pulse generator', system_path: '/dev/ttyUSB0', persistent_id: '', sdk_index: -1, interface_index: -1, device_index: -1, stream_index: -1, bus_address: 1, stable_identity: '', identity_strength: 1, identification: DISCOVERY_IDENTIFICATION_STATUSES.Identified, claimed_by: [], capabilities: [], synthetic: false, camera_type: -1, interface_id: '', device_id: '', stream_id: '', model_name: '', firmware_version: '', label: '', ...patch});
const snapshot = (patch: Partial<DiscoverySnapshot>): DiscoverySnapshot => ({valid: true, job_id: '1', generation: '1', state: DISCOVERY_JOB_STATES.Completed, complete: true, overflow: false, attempt: 1, max_attempts: 1, kinds: [3], candidates: [], errors: [], providers_run: [], origin: '', ...patch});

describe('limits mirror the backend (HW-051 suspicious item)', () => {
  it('channel count and frequency range equal PulseGeneratorService.h', () => {
    const constant = (name: string) => Number(new RegExp(`${name}\\s*=\\s*([0-9.]+)`).exec(serviceHeader)?.[1]);
    expect(constant('CHANNEL_COUNT')).toBe(PULSE_LIMITS.channelCount);
    expect(constant('MIN_FREQUENCY_HZ')).toBe(PULSE_LIMITS.frequencyHz.min);
    expect(constant('MAX_FREQUENCY_HZ')).toBe(PULSE_LIMITS.frequencyHz.max);
  });
  it('the command ranges and address limits equal BackendFacade::pulseGeneratorCommandJson', () => {
    expect(facadeSource).toContain(`action == "frequency" ? ${PULSE_LIMITS.frequencyHz.min}.0 : ${PULSE_LIMITS.dutyPercent.min}.0`);
    expect(facadeSource).toContain(`action == "frequency" ? ${PULSE_LIMITS.frequencyHz.max}.0 : ${PULSE_LIMITS.dutyPercent.max}.0`);
    expect(facadeSource).toContain(`address < ${PULSE_LIMITS.address.min} || address > ${PULSE_LIMITS.address.max}`);
  });
  it('takes the channel list from the status when it has one', () => {
    expect(channelCount(null)).toBe(4);
    expect(channelCount(status({channels: [channel, channel]}))).toBe(2);
  });
});

describe('Qt defaults and ranges (HW-050)', () => {
  it('9600 8N1, address 1, channel 1, 5000 Hz at 50 %', () => {
    expect(DEFAULT_PULSE_FORM).toMatchObject({baud: 9600, dataBits: 8, parity: 'N', stopBits: 1, address: '1', channel: 0, frequency: '5000', duty: '50'});
    expect(BAUD_RATES).toEqual([4800, 9600, 14400, 19200, 38400, 56000, 57600, 115200]);
  });
  it('connect sends every serial setting', () => {
    expect(connectStep(form({baud: 19200, dataBits: 7, parity: 'E', stopBits: 2, address: '5'})).request).toEqual({action: 'connect', port: '/dev/ttyUSB0', baud: 19200, data_bits: 7, parity: 'E', stop_bits: 2, address: 5});
  });
  it('refuses a missing port and an address outside the Modbus range before any command', () => {
    expect(() => connectStep(form({port: '  '}))).toThrow('serial port');
    expect(() => connectStep(form({address: '0'}))).toThrow('Address');
    expect(() => connectStep(form({address: '248'}))).toThrow('Address');
    expect(() => connectStep(form({address: '1.5'}))).toThrow('Address');
  });
});

describe('status label wording (HW-055)', () => {
  it('uses the Qt words', () => {
    expect(statusLabel(status(), false, 0)).toBe('Verified addr 3 — Ch1: 5000 Hz, 50% duty, output ON');
    expect(statusLabel(status({channels: [channel, {frequency_hz: 1000, duty_percent: 2.5, output_enabled: false}]}), false, 1)).toBe('Verified addr 3 — Ch2: 1000 Hz, 2.5% duty, output off');
    expect(statusLabel(status({connected: false}), false, 0)).toBe('Disconnected');
    expect(statusLabel(status({connected: false, error: 'port busy'}), false, 0)).toBe('Disconnected — port busy');
    expect(statusLabel(status({connected: false}), true, 0)).toBe('Scanning…');
  });
  it('keeps what Tauri adds', () => {
    expect(statusLabel(null, false, 0)).toBe('Status unavailable');
    expect(statusLabel(status({owned: true}), false, 0)).toContain('Owned by coordinated live view');
  });
});

describe('Start and Stop sequences (HW-054)', () => {
  it('Start writes frequency, duty, then enable for the chosen channel', () => {
    expect(startSteps(form({channel: 2, frequency: '1000', duty: '2'})).map(step => step.request)).toEqual([
      {action: 'frequency', channel: 2, value: 1000}, {action: 'duty', channel: 2, value: 2}, {action: 'enable', channel: 2, enabled: true},
    ]);
  });
  it('Set writes frequency and duty only; Stop only disables', () => {
    expect(setSteps(form()).map(step => step.name)).toEqual(['frequency', 'duty']);
    expect(stopStep(1).request).toEqual({action: 'enable', channel: 1, enabled: false});
  });
  it('checks both values before the first write', () => {
    expect(() => startSteps(form({frequency: '399'}))).toThrow('Frequency');
    expect(() => startSteps(form({frequency: '40001'}))).toThrow('Frequency');
    expect(() => startSteps(form({duty: '101'}))).toThrow('Duty');
    expect(() => startSteps(form({duty: ''}))).toThrow('Duty');
  });
  it('runs the steps in order and reports success', async () => {
    const send = vi.fn(async () => ({ok: true, command: 12, operation_id: '0', message: 'done'}));
    const outcome = await runSteps('Start', startSteps(form()), send);
    expect(send).toHaveBeenCalledTimes(3);
    expect(outcome).toEqual({ok: true, message: 'Start done (frequency, duty, enable output).'});
  });
  it('stops at the first failure and names the step and what was written', async () => {
    const send = vi.fn(async (request: Record<string, unknown>) => request.action === 'duty' ? {ok: false, command: 12, operation_id: '0', message: 'bus timeout'} : {ok: true, command: 12, operation_id: '0', message: 'ok'});
    const outcome = await runSteps('Start', startSteps(form()), send);
    expect(send).toHaveBeenCalledTimes(2);
    expect(outcome).toEqual({ok: false, message: 'Start failed at step 2 of 3 (duty): bus timeout. Already written: frequency.'});
  });
  it('reports a thrown transport error the same way, and a lone failing command plainly', async () => {
    const lost = await runSteps('Start', startSteps(form()), async () => { throw new Error('IPC lost'); });
    expect(lost).toEqual({ok: false, message: 'Start failed at step 1 of 3 (frequency): IPC lost. Nothing was written.'});
    const single = await runSteps('Stop', [stopStep(0)], async () => ({ok: false, command: 12, operation_id: '0', message: ''}));
    expect(single).toEqual({ok: false, message: 'Stop failed: Hardware command failed.'});
  });
});

describe('address scan (HW-051)', () => {
  it('is a bounded read-only probe of addresses 1-16 with the selected serial settings', () => {
    expect(scanRequest(form({baud: 19200, dataBits: 7, parity: 'O', stopBits: 2, address: '9'}))).toEqual({
      kinds: [3], hasSerialScope: true, serialPortName: '/dev/ttyUSB0', baudRate: 19200, dataBits: 7, parity: 'O', stopBits: 2,
      addressFrom: 1, addressTo: 16, perAddressTimeoutMs: 250, origin: 'tauri-pulse-scan',
    });
    expect(() => scanRequest(form({port: ''}))).toThrow('serial port');
  });
  it('lists every responder and picks the first identified generator', () => {
    const summary = summariseScan(snapshot({candidates: [
      device({bus_address: 2, display_name: 'Address 2 — Modbus device (not a pulse generator, left untouched)', identification: DISCOVERY_IDENTIFICATION_STATUSES.Unidentified}),
      device({bus_address: 5, display_name: 'Address 5 — pulse generator'}),
      device({bus_address: 7, display_name: 'Address 7 — pulse generator'}),
    ]}), '/dev/ttyUSB0');
    expect(summary).toMatchObject({outcome: 'found', firstGenerator: 5, lines: ['Address 2 — Modbus device (not a pulse generator, left untouched)', 'Address 5 — pulse generator', 'Address 7 — pulse generator']});
  });
  it('does not fill the address when no generator answered', () => {
    const summary = summariseScan(snapshot({candidates: [device({bus_address: 2, identification: DISCOVERY_IDENTIFICATION_STATUSES.Unidentified})]}), 'COM3');
    expect(summary).toMatchObject({outcome: 'found', firstGenerator: null});
  });
  it('says so when nothing responded, and when the port could not be opened', () => {
    expect(summariseScan(snapshot({}), 'COM3')).toMatchObject({outcome: 'empty', message: 'No Modbus devices responded on COM3 (addresses 1–16).'});
    const busy = summariseScan(snapshot({errors: [{provider_id: 'pulse-generator', kind: DISCOVERY_ERROR_KINDS.Busy, message: 'port busy', endpoint: 'COM3'}]}), 'COM3');
    expect(busy.outcome).toBe('error');
    expect(busy.message).toContain('Could not open COM3 for scanning: port busy');
  });
  it('is silent when cancelled and reports a failed job', () => {
    expect(summariseScan(snapshot({state: DISCOVERY_JOB_STATES.Cancelled}), 'COM3')).toMatchObject({outcome: 'cancelled', message: ''});
    expect(summariseScan(snapshot({state: DISCOVERY_JOB_STATES.Failed}), 'COM3')).toMatchObject({outcome: 'error', message: 'The scan failed.'});
  });
});
