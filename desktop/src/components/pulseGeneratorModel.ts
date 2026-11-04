// Pulse generator panel model (epic #868: HW-049..HW-055). Pure helpers, no React or Tauri imports, so the
// Qt-parity wording, ranges and command sequences are unit tested. The Qt reference is
// src/frontend/tabs/ConfigTabs.cpp (port/serial rows :490-535, status label :1501-1541, scan :1674-1776,
// Start/Stop :1808-1839).
import type {CmdResult, DiscoveredDevice, DiscoveryRequest, DiscoverySnapshot, PulseGeneratorStatus} from '../bridge';
import {DISCOVERY_DEVICE_KINDS, DISCOVERY_ERROR_KINDS, DISCOVERY_IDENTIFICATION_STATUSES, DISCOVERY_JOB_STATES} from '../bridgeContract';
import {numericInput} from './hardwareControlModel';

/**
 * The one place the generator's hard limits live in the frontend. The backend is the only real enforcement
 * (a value outside it is refused), so these must follow it:
 *  - channelCount, frequencyHz: include/backend/services/PulseGeneratorService.h (CHANNEL_COUNT, MIN/MAX_FREQUENCY_HZ)
 *    and the 400-40000 / 0-100 checks in BackendFacade::pulseGeneratorCommandJson.
 *  - address: the Modbus RTU slave range, enforced by the same function (Qt's spin allows 1-255; 248-255 are refused
 *    at connect, so Tauri stops them in the field).
 * The status payload carries one entry per channel, so the channel list comes from `channelCount(status)`; the constant
 * is only the answer before the first status. pulseGeneratorModel.test.ts compares these values with the C++ sources.
 */
export const PULSE_LIMITS = {
  channelCount: 4,
  frequencyHz: {min: 400, max: 40000},
  dutyPercent: {min: 0, max: 100},
  address: {min: 1, max: 247},
} as const;
/** Qt "Scan": probe addresses 1-16 with a read-only register read, 250 ms each (ConfigTabs.cpp:1698-1704). */
export const SCAN_RANGE = {from: 1, to: 16, perAddressTimeoutMs: 250} as const;
/** PulseGeneratorService::toString(LinkError::None): the status payload's `error` when nothing went wrong. */
const LINK_OK = 'ok';

export const BAUD_RATES = [4800, 9600, 14400, 19200, 38400, 56000, 57600, 115200] as const;
export const DATA_BITS = [8, 7] as const;
export const PARITIES = ['N', 'E', 'O'] as const;
export const STOP_BITS = [1, 2] as const;
export type Parity = (typeof PARITIES)[number];

/** Typed values stay strings until a command validates them; channel is the 0-based index (Qt shows channel + 1). */
export interface PulseForm {
  port: string; baud: number; dataBits: number; parity: Parity; stopBits: number;
  address: string; channel: number; frequency: string; duty: string;
}
/** Qt defaults: 9600 8N1 (module factory setting), address 1, channel 1, the 5000 Hz bench rate at 50 % duty. */
export const DEFAULT_PULSE_FORM: PulseForm = {
  port: '', baud: 9600, dataBits: 8, parity: 'N', stopBits: 1, address: '1', channel: 0, frequency: '5000', duty: '50',
};

export const channelCount = (status: PulseGeneratorStatus | null): number => status?.channels.length || PULSE_LIMITS.channelCount;

/** Qt's status label: 'Scanning…', 'Disconnected', 'Disconnected — <error>', 'Verified addr N — ChX: f Hz, d% duty, output ON/off'. */
export function statusLabel(status: PulseGeneratorStatus | null, scanning: boolean, channel: number): string {
  if (!status) return 'Status unavailable';
  const owned = status.owned ? ' · Owned by coordinated live view' : '';
  if (!status.connected) {
    if (scanning) return 'Scanning…';
    return (!status.error || status.error === LINK_OK ? 'Disconnected' : `Disconnected — ${status.error}`) + owned;
  }
  const state = status.channels[channel];
  const output = state ? `Ch${channel + 1}: ${state.frequency_hz} Hz, ${state.duty_percent}% duty, output ${state.output_enabled ? 'ON' : 'off'}` : `Ch${channel + 1}: no data`;
  return `Verified addr ${status.address} — ${output}${owned}`;
}

export type PulseStepName = 'connect' | 'disconnect' | 'frequency' | 'duty' | 'enable' | 'disable';
export interface PulseStep {name: PulseStepName; request: Record<string, unknown>}
const STEP_LABEL: Record<PulseStepName, string> = {connect: 'connect', disconnect: 'disconnect', frequency: 'frequency', duty: 'duty', enable: 'enable output', disable: 'disable output'};

export function connectStep(form: PulseForm): PulseStep {
  const port = form.port.trim();
  if (!port) throw new Error('Enter or select a serial port first.');
  const {min, max} = PULSE_LIMITS.address;
  return {name: 'connect', request: {action: 'connect', port, baud: form.baud, data_bits: form.dataBits, parity: form.parity, stop_bits: form.stopBits, address: numericInput(form.address, 'Address', min, max, true)}};
}
export const disconnectStep = (): PulseStep => ({name: 'disconnect', request: {action: 'disconnect'}});
export const stopStep = (channel: number): PulseStep => ({name: 'disable', request: {action: 'enable', channel, enabled: false}});

/** Qt "Set": frequency then duty, nothing enabled. Both values are checked before the first write. */
export function setSteps(form: PulseForm): PulseStep[] {
  const {frequencyHz, dutyPercent} = PULSE_LIMITS;
  const frequency = numericInput(form.frequency, 'Frequency', frequencyHz.min, frequencyHz.max);
  const duty = numericInput(form.duty, 'Duty', dutyPercent.min, dutyPercent.max);
  return [
    {name: 'frequency', request: {action: 'frequency', channel: form.channel, value: frequency}},
    {name: 'duty', request: {action: 'duty', channel: form.channel, value: duty}},
  ];
}
/** Qt "Start": frequency, duty, then enable, so one Start is enough after connect. */
export const startSteps = (form: PulseForm): PulseStep[] => [...setSteps(form), {name: 'enable', request: {action: 'enable', channel: form.channel, enabled: true}}];

export type SequenceOutcome =
  | {ok: true; message: string}
  | {ok: false; message: string};
/** Send the steps in order and stop at the first failure, naming the step and what was already written. */
export async function runSteps(verb: string, steps: PulseStep[], send: (request: Record<string, unknown>) => Promise<CmdResult>): Promise<SequenceOutcome> {
  const done: string[] = [];
  for (const [index, step] of steps.entries()) {
    let failure = '';
    try {
      const result = await send(step.request);
      if (result.ok) { done.push(STEP_LABEL[step.name]); continue; }
      failure = result.message || 'Hardware command failed.';
    } catch (error) { failure = error instanceof Error ? error.message : String(error); }
    const reason = failure.replace(/[.\s]+$/, '');
    const written = done.length ? ` Already written: ${done.join(', ')}.` : ' Nothing was written.';
    return {ok: false, message: steps.length === 1 ? `${verb} failed: ${reason}.` : `${verb} failed at step ${index + 1} of ${steps.length} (${STEP_LABEL[step.name]}): ${reason}.${written}`};
  }
  return {ok: true, message: `${verb} done (${done.join(', ')}).`};
}

/** The read-only address scan (Qt Scan): the selected port and serial settings, addresses 1-16. */
export function scanRequest(form: PulseForm): DiscoveryRequest {
  const port = form.port.trim();
  if (!port) throw new Error('Enter or select a serial port before scanning.');
  return {
    kinds: [DISCOVERY_DEVICE_KINDS.PulseGenerator], hasSerialScope: true, serialPortName: port, baudRate: form.baud, dataBits: form.dataBits,
    parity: form.parity, stopBits: form.stopBits, addressFrom: SCAN_RANGE.from, addressTo: SCAN_RANGE.to,
    perAddressTimeoutMs: SCAN_RANGE.perAddressTimeoutMs, origin: 'tauri-pulse-scan',
  };
}

export interface ScanSummary {
  outcome: 'cancelled' | 'error' | 'empty' | 'found';
  /** One line per responding address (the backend wording: 'Address N — pulse generator' / 'Modbus device …'). */
  lines: string[];
  /** What to tell the operator; empty when cancelled. */
  message: string;
  /** The first identified generator's address: Qt fills the address field with it. */
  firstGenerator: number | null;
}
// Errors that mean the port itself could not be scanned (Qt onPulseGenScanFinished): different advice than a silent bus.
const PORT_ERRORS: number[] = [
  DISCOVERY_ERROR_KINDS.Busy, DISCOVERY_ERROR_KINDS.OpenFailed, DISCOVERY_ERROR_KINDS.PermissionDenied, DISCOVERY_ERROR_KINDS.Timeout,
  DISCOVERY_ERROR_KINDS.Unsupported, DISCOVERY_ERROR_KINDS.InvalidRequest, DISCOVERY_ERROR_KINDS.ProviderException,
];
const isGenerator = (device: DiscoveredDevice) => device.kind === DISCOVERY_DEVICE_KINDS.PulseGenerator && device.identification === DISCOVERY_IDENTIFICATION_STATUSES.Identified;
export function summariseScan(snapshot: DiscoverySnapshot, port: string): ScanSummary {
  if (snapshot.state === DISCOVERY_JOB_STATES.Cancelled) return {outcome: 'cancelled', lines: [], message: '', firstGenerator: null};
  const portError = snapshot.errors.find(e => PORT_ERRORS.includes(e.kind));
  if (portError) return {outcome: 'error', lines: [], message: `Could not open ${port} for scanning: ${portError.message}. The port may be held by another program, or by MIB with different serial settings.`, firstGenerator: null};
  if (snapshot.state === DISCOVERY_JOB_STATES.Failed) return {outcome: 'error', lines: [], message: snapshot.errors.map(e => e.message).join('; ') || 'The scan failed.', firstGenerator: null};
  if (!snapshot.candidates.length) return {outcome: 'empty', lines: [], message: `No Modbus devices responded on ${port} (addresses ${SCAN_RANGE.from}–${SCAN_RANGE.to}).`, firstGenerator: null};
  const first = snapshot.candidates.find(isGenerator);
  return {outcome: 'found', lines: snapshot.candidates.map(d => d.display_name), message: `Scan of ${port} (addresses ${SCAN_RANGE.from}–${SCAN_RANGE.to}) found ${snapshot.candidates.length} device${snapshot.candidates.length === 1 ? '' : 's'}.`, firstGenerator: first ? first.bus_address : null};
}
