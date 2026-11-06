// Acquisition pulse generator (RS485 external trigger source): the rules of the panel, as pure functions.
// Qt reference: src/frontend/tabs/ConfigTabs.cpp:483-654 (panel and XGC + R5D preset) and :1501-1839 (status
// label, port list, persistence, scan, Connect / Set / Start / Stop). Backend limits:
// include/backend/services/PulseGeneratorService.h and BackendFacade.cpp pulseGeneratorCommandJson.
// Epic #868 (HW-043..HW-057), decisions D5 (Start / Stop as Qt, no Service mode or arm; locked while an
// experiment is active, Stop always) and D6 (the panel stays in the hardware panel).
import type { DiscoveredDevice, DiscoveryRequest, DiscoverySnapshot, PulseGeneratorStatus, SerialPortEntry } from '../bridge';
import { DISCOVERY_DEVICE_KINDS, DISCOVERY_ERROR_KINDS, DISCOVERY_JOB_STATES } from '../bridgeContract';
import { isTerminalDiscoveryState } from '../discovery';

export type Parity = 'N' | 'E' | 'O';

/** The module's limits. The backend reports them in `pulse_generator_status`; these are the fallback
 *  for a server that does not, and a test pins them to PulseGeneratorService.h (MIN/MAX_FREQUENCY_HZ,
 *  CHANNEL_COUNT) so they cannot drift. */
export interface PulseLimits { channelCount: number; minFrequencyHz: number; maxFrequencyHz: number }
export const DEFAULT_PULSE_LIMITS: PulseLimits = { channelCount: 4, minFrequencyHz: 400, maxFrequencyHz: 40000 };
export function pulseLimits(status: Pick<PulseGeneratorStatus, 'channel_count' | 'min_frequency_hz' | 'max_frequency_hz'> | null): PulseLimits {
  const finite = (value: unknown, fallback: number) => (typeof value === 'number' && Number.isFinite(value) && value > 0 ? value : fallback);
  return {
    channelCount: Math.floor(finite(status?.channel_count, DEFAULT_PULSE_LIMITS.channelCount)),
    minFrequencyHz: finite(status?.min_frequency_hz, DEFAULT_PULSE_LIMITS.minFrequencyHz),
    maxFrequencyHz: finite(status?.max_frequency_hz, DEFAULT_PULSE_LIMITS.maxFrequencyHz),
  };
}

// Qt's combos (ConfigTabs.cpp:499-522). The module's factory default is 9600 8N1.
export const PULSE_BAUD_RATES = [4800, 9600, 14400, 19200, 38400, 56000, 57600, 115200] as const;
export const PULSE_DATA_BITS = [8, 7] as const;
export const PULSE_PARITIES: readonly Parity[] = ['N', 'E', 'O'];
export const PULSE_STOP_BITS = [1, 2] as const;
/** Modbus slave address. Qt's spin box allows 1-255, but the backend (and the camera's live_view block) refuses
 *  anything above 247, the last valid Modbus slave address; offering 248-255 would only fail at Connect. */
export const PULSE_ADDRESS_MIN = 1;
export const PULSE_ADDRESS_MAX = 247;
/** Read-only scan range and per-address timeout (ConfigTabs.cpp:1702-1706). */
export const PULSE_SCAN = { addressFrom: 1, addressTo: 16, perAddressTimeoutMs: 250 } as const;
/** Bench default trigger rate: 5000 fps at 512x96, 1 us exposure (ConfigTabs.cpp:552-556). */
export const PULSE_DEFAULTS = { baud: 9600, dataBits: 8, parity: 'N' as Parity, stopBits: 1, address: 1, channel: 1, frequencyHz: 5000, dutyPercent: 50 };

/** The form: text for the numbers the operator types, so a half-typed value is not rewritten under them. */
export interface PulseDraft {
  portName: string; baud: number; dataBits: number; parity: Parity; stopBits: number;
  address: string; channel: number; frequency: string; duty: string;
}
export const defaultPulseDraft = (): PulseDraft => ({
  portName: '', baud: PULSE_DEFAULTS.baud, dataBits: PULSE_DEFAULTS.dataBits, parity: PULSE_DEFAULTS.parity,
  stopBits: PULSE_DEFAULTS.stopBits, address: String(PULSE_DEFAULTS.address), channel: PULSE_DEFAULTS.channel,
  frequency: String(PULSE_DEFAULTS.frequencyHz), duty: String(PULSE_DEFAULTS.dutyPercent),
});

// ---- Validation: out-of-range input is refused with a message, never replaced (HW-050, HW-051) --------------

function plainNumber(text: string): number {
  return text.trim() === '' ? NaN : Number(text);
}
/** At most `places` decimals (the module's resolution is 0.01 Hz and 0.01 %): a value with more would be rounded silently. */
function withinResolution(value: number, places: number): boolean {
  const scaled = value * 10 ** places;
  return Math.abs(scaled - Math.round(scaled)) < 1e-6 * Math.max(1, Math.abs(scaled));
}
export function parseAddress(text: string): number {
  const value = plainNumber(text);
  if (!Number.isInteger(value) || value < PULSE_ADDRESS_MIN || value > PULSE_ADDRESS_MAX) {
    throw new Error(`Modbus address must be a whole number between ${PULSE_ADDRESS_MIN} and ${PULSE_ADDRESS_MAX}.`);
  }
  return value;
}
export function parseFrequency(text: string, limits: PulseLimits = DEFAULT_PULSE_LIMITS): number {
  const value = plainNumber(text);
  if (!Number.isFinite(value) || value < limits.minFrequencyHz || value > limits.maxFrequencyHz || !withinResolution(value, 2)) {
    throw new Error(`Frequency must be between ${limits.minFrequencyHz} and ${limits.maxFrequencyHz} Hz (0.01 Hz steps).`);
  }
  return value;
}
export function parseDuty(text: string): number {
  const value = plainNumber(text);
  if (!Number.isFinite(value) || value < 0 || value > 100 || !withinResolution(value, 2)) {
    throw new Error('Duty must be between 0 and 100 % (0.01 % steps).');
  }
  return value;
}
export function parseChannel(channel: number, limits: PulseLimits = DEFAULT_PULSE_LIMITS): number {
  if (!Number.isInteger(channel) || channel < 1 || channel > limits.channelCount) throw new Error(`Channel must be between 1 and ${limits.channelCount}.`);
  return channel;
}
function requirePort(portName: string): string {
  const port = portName.trim();
  if (!port) throw new Error('No serial port selected. Plug in the RS485 adapter and press Refresh.');
  return port;
}

// ---- Commands (pulse_generator_command) -----------------------------------------------------------------------

export function connectCommand(draft: PulseDraft): Record<string, unknown> {
  return {
    action: 'connect', port: requirePort(draft.portName), baud: draft.baud, data_bits: draft.dataBits,
    parity: draft.parity, stop_bits: draft.stopBits, address: parseAddress(draft.address),
  };
}
/** Qt Set: write the frequency and the duty (the duty reaches the wire while the output is on). */
export function setCommand(draft: PulseDraft, limits: PulseLimits = DEFAULT_PULSE_LIMITS): Record<string, unknown> {
  return { action: 'set', channel: parseChannel(draft.channel, limits) - 1, frequency_hz: parseFrequency(draft.frequency, limits), duty_percent: parseDuty(draft.duty) };
}
/** Qt Start: frequency + duty + output on, one command. */
export function startCommand(draft: PulseDraft, limits: PulseLimits = DEFAULT_PULSE_LIMITS): Record<string, unknown> {
  return { ...setCommand(draft, limits), action: 'start' };
}
/** Qt Stop: gate the output off (duty 0 on the wire; the line idles low). */
export function stopCommand(draft: PulseDraft, limits: PulseLimits = DEFAULT_PULSE_LIMITS): Record<string, unknown> {
  return { action: 'enable', channel: parseChannel(draft.channel, limits) - 1, enabled: false };
}

// ---- Which controls are usable (D5) ---------------------------------------------------------------------------

export type PulseAction = 'refresh' | 'scan' | 'connect' | 'disconnect' | 'set' | 'start' | 'stop';
export interface PulseGateInput { ready: boolean; experimentActive: boolean; connected: boolean; owned: boolean; busy: boolean; scanning: boolean }
/** Empty when the action is allowed, else why not. No Service mode and no arm (Qt has none; this is rig setup).
 *  While an experiment is active the generator is the acquisition clock, so Connect, Disconnect, Set and Start are
 *  locked (the backend refuses them too); Stop is always allowed, also while another command is in flight (it is
 *  queued behind it by the panel). The coordinated live view owns the generator: every write is refused. */
export function pulseGate(input: PulseGateInput, action: PulseAction): string {
  if (!input.ready) return 'Backend is not ready.';
  if (action === 'stop') {
    if (input.owned) return 'The generator is owned by the coordinated live view.';
    return input.connected ? '' : 'Connect the pulse generator first.';
  }
  if (input.scanning) return action === 'scan' ? '' : 'A scan is running.';
  if (input.busy) return 'A pulse-generator command is in progress.';
  switch (action) {
    case 'refresh': return input.connected ? 'Disconnect to change the port.' : '';
    case 'scan': return input.connected ? 'Disconnect to scan the bus.' : '';
    case 'connect':
      if (input.owned) return 'The generator is owned by the coordinated live view.';
      return input.experimentActive ? 'Locked while an experiment is active: the generator is the acquisition clock.' : '';
    case 'disconnect':
      if (input.owned) return 'The generator is owned by the coordinated live view.';
      return input.experimentActive ? 'Locked while an experiment is active: the generator is the acquisition clock.' : '';
    case 'set':
    case 'start':
      if (!input.connected) return 'Connect the pulse generator first.';
      if (input.owned) return 'The generator is owned by the coordinated live view.';
      return input.experimentActive ? 'Locked while an experiment is active: the generator is the acquisition clock.' : '';
  }
}

// ---- Status label (HW-055) ------------------------------------------------------------------------------------

const quiet = (error: string) => !error || error === 'ok' || error === 'None';
function plain(value: number): string {
  return String(Number(value.toFixed(2)));
}
/** Qt's wording (ConfigTabs.cpp:1501-1541): 'Verified addr N — ChX: f Hz, d% duty, output ON/off',
 *  'Disconnected — <error>', 'Scanning…'. The coordinated live view's ownership is added, as Tauri showed it. */
export function pulseStatusLabel(input: { status: PulseGeneratorStatus | null; scanning: boolean; channel: number }): string {
  const { status, scanning, channel } = input;
  if (scanning && !status?.connected) return 'Scanning…';
  if (!status) return 'Status unavailable';
  if (!status.connected) return quiet(status.error) ? 'Disconnected' : `Disconnected — ${status.error}`;
  const state = status.channels[channel - 1];
  const owned = status.owned ? ' (owned by the coordinated live view)' : '';
  if (!state) return `Verified addr ${status.address}${owned}`;
  return `Verified addr ${status.address} — Ch${channel}: ${plain(state.frequency_hz)} Hz, ${plain(state.duty_percent)}% duty, output ${state.output_enabled ? 'ON' : 'off'}${owned}`;
}

// ---- Serial ports (HW-049) ------------------------------------------------------------------------------------

const hex4 = (value: number) => value.toString(16).padStart(4, '0');
/** Qt's port label: 'ttyUSB0 — FT232R USB UART, S/N A12345, 0403:6001'. */
export function portLabel(port: SerialPortEntry): string {
  const extra: string[] = [];
  if (port.description) extra.push(port.description);
  if (port.serial_number) extra.push(`S/N ${port.serial_number}`);
  if (port.vendor_id !== 0) extra.push(`${hex4(port.vendor_id)}:${hex4(port.product_id)}`);
  return extra.length ? `${port.system_name} — ${extra.join(', ')}` : port.system_name;
}
export function validPorts(value: unknown): SerialPortEntry[] {
  const ports = (value as { ports?: unknown } | null)?.ports;
  if (!Array.isArray(ports)) return [];
  return ports.filter((p): p is SerialPortEntry => !!p && typeof p === 'object' && typeof (p as SerialPortEntry).system_name === 'string' && (p as SerialPortEntry).system_name !== '')
    .map(p => ({ system_name: p.system_name, system_location: String(p.system_location ?? ''), description: String(p.description ?? ''), manufacturer: String(p.manufacturer ?? ''),
      serial_number: String(p.serial_number ?? ''), vendor_id: Number(p.vendor_id) || 0, product_id: Number(p.product_id) || 0 }));
}

// ---- Persistence (HW-056) -------------------------------------------------------------------------------------

/** What survives a restart (Qt QSettings group PulseGenerator): the port with its USB identity, the serial
 *  framing, the address, the channel, the frequency and the duty. */
export interface PulseSettings {
  portName: string; portSerialNumber: string; portVid: number; portPid: number;
  baud: number; dataBits: number; parity: Parity; stopBits: number; address: number; channel: number; frequencyHz: number; dutyPercent: number;
}
export const PULSE_SETTINGS_KEY = 'pulseGenerator';
export const PULSE_SETTINGS_STORAGE_KEY = 'mib.pulsegenerator.v1';
/** Tolerant like Qt's restore: a field that is missing or no longer valid keeps its default, the rest is restored. */
export function parsePulseSettings(raw: unknown, limits: PulseLimits = DEFAULT_PULSE_LIMITS): Partial<PulseSettings> {
  if (!raw || typeof raw !== 'object') return {};
  const r = raw as Record<string, unknown>;
  const out: Partial<PulseSettings> = {};
  const text = (v: unknown, max: number) => (typeof v === 'string' && v.length <= max && !v.includes('\0') ? v : undefined);
  const int = (v: unknown, min: number, max: number) => (typeof v === 'number' && Number.isInteger(v) && v >= min && v <= max ? v : undefined);
  const one = <T,>(v: unknown, allowed: readonly T[]) => (allowed.includes(v as T) ? (v as T) : undefined);
  const port = text(r.portName, 512); if (port !== undefined) out.portName = port;
  const serial = text(r.portSerialNumber, 256); if (serial !== undefined) out.portSerialNumber = serial;
  const vid = int(r.portVid, 0, 0xffff); if (vid !== undefined) out.portVid = vid;
  const pid = int(r.portPid, 0, 0xffff); if (pid !== undefined) out.portPid = pid;
  const baud = one(r.baud, PULSE_BAUD_RATES); if (baud !== undefined) out.baud = baud;
  const bits = one(r.dataBits, PULSE_DATA_BITS); if (bits !== undefined) out.dataBits = bits;
  const parity = one(r.parity, PULSE_PARITIES); if (parity !== undefined) out.parity = parity;
  const stop = one(r.stopBits, PULSE_STOP_BITS); if (stop !== undefined) out.stopBits = stop;
  const address = int(r.address, PULSE_ADDRESS_MIN, PULSE_ADDRESS_MAX); if (address !== undefined) out.address = address;
  const channel = int(r.channel, 1, limits.channelCount); if (channel !== undefined) out.channel = channel;
  if (typeof r.frequencyHz === 'number' && Number.isFinite(r.frequencyHz) && r.frequencyHz >= limits.minFrequencyHz && r.frequencyHz <= limits.maxFrequencyHz) out.frequencyHz = r.frequencyHz;
  if (typeof r.dutyPercent === 'number' && Number.isFinite(r.dutyPercent) && r.dutyPercent >= 0 && r.dutyPercent <= 100) out.dutyPercent = r.dutyPercent;
  return out;
}
export function draftFromSettings(settings: Partial<PulseSettings>): PulseDraft {
  const base = defaultPulseDraft();
  return {
    portName: settings.portName ?? base.portName, baud: settings.baud ?? base.baud, dataBits: settings.dataBits ?? base.dataBits,
    parity: settings.parity ?? base.parity, stopBits: settings.stopBits ?? base.stopBits,
    address: settings.address !== undefined ? String(settings.address) : base.address, channel: settings.channel ?? base.channel,
    frequency: settings.frequencyHz !== undefined ? String(settings.frequencyHz) : base.frequency,
    duty: settings.dutyPercent !== undefined ? String(settings.dutyPercent) : base.duty,
  };
}
/** The record to save: what the operator has, with the identity of the chosen port. A field that does not
 *  parse right now is left out, so a half-typed value never overwrites a good saved one. */
export function settingsFromDraft(draft: PulseDraft, ports: readonly SerialPortEntry[], limits: PulseLimits = DEFAULT_PULSE_LIMITS): Partial<PulseSettings> {
  const port = ports.find(p => p.system_name === draft.portName);
  const out: Partial<PulseSettings> = { baud: draft.baud, dataBits: draft.dataBits, parity: draft.parity, stopBits: draft.stopBits, channel: draft.channel };
  if (draft.portName) {
    out.portName = draft.portName; out.portSerialNumber = port?.serial_number ?? ''; out.portVid = port?.vendor_id ?? 0; out.portPid = port?.product_id ?? 0;
  }
  try { out.address = parseAddress(draft.address); } catch { /* keep the saved one */ }
  try { out.frequencyHz = parseFrequency(draft.frequency, limits); } catch { /* keep the saved one */ }
  try { out.dutyPercent = parseDuty(draft.duty); } catch { /* keep the saved one */ }
  return out;
}

export type PortResolution = { portName: string; via: 'name' | 'usb' } | { portName: ''; via: 'ambiguous' | 'none' };
/** Qt's re-resolution (ConfigTabs.cpp:1625-1664): the saved system name first; when the node was renamed
 *  (ttyUSB0 to ttyUSB1), the saved USB identity (serial number and VID:PID), but only when it matches exactly
 *  one port, otherwise the operator chooses. */
export function resolvePort(ports: readonly SerialPortEntry[], saved: Partial<PulseSettings>): PortResolution {
  if (saved.portName && ports.some(p => p.system_name === saved.portName)) return { portName: saved.portName, via: 'name' };
  const serial = saved.portSerialNumber ?? '', vid = saved.portVid ?? 0, pid = saved.portPid ?? 0;
  if (serial === '' && vid === 0) return { portName: '', via: 'none' };
  const matches = ports.filter(p => p.serial_number === serial && p.vendor_id === vid && p.product_id === pid);
  if (matches.length === 1) return { portName: matches[0].system_name, via: 'usb' };
  return { portName: '', via: matches.length > 1 ? 'ambiguous' : 'none' };
}

// ---- Scan (HW-051) --------------------------------------------------------------------------------------------

/** Qt's read-only probe: addresses 1-16 on the chosen port with the chosen framing, never a write. */
export function scanRequest(draft: PulseDraft): DiscoveryRequest {
  return {
    kinds: [DISCOVERY_DEVICE_KINDS.PulseGenerator], hasSerialScope: true, serialPortName: requirePort(draft.portName),
    baudRate: draft.baud, dataBits: draft.dataBits, parity: draft.parity, stopBits: draft.stopBits,
    addressFrom: PULSE_SCAN.addressFrom, addressTo: PULSE_SCAN.addressTo, perAddressTimeoutMs: PULSE_SCAN.perAddressTimeoutMs,
    origin: 'tauri-pulse-scan',
  };
}
export interface PulseScanReport { lines: string[]; firstGenerator: number | null; summary: string; failed: boolean }
const PORT_LEVEL_ERRORS: number[] = [
  DISCOVERY_ERROR_KINDS.Busy, DISCOVERY_ERROR_KINDS.OpenFailed, DISCOVERY_ERROR_KINDS.PermissionDenied, DISCOVERY_ERROR_KINDS.Timeout,
  DISCOVERY_ERROR_KINDS.Unsupported, DISCOVERY_ERROR_KINDS.InvalidRequest, DISCOVERY_ERROR_KINDS.ProviderException,
];
/** Qt's result dialog as text (ConfigTabs.cpp:1741-1776): a port-level failure is different advice than a silent
 *  bus; each answering address is a pulse generator, another Modbus device (left untouched) or a corrupt reply. */
export function describeScan(snapshot: Pick<DiscoverySnapshot, 'state' | 'candidates' | 'errors'>, portName: string): PulseScanReport {
  for (const error of snapshot.errors) {
    if (PORT_LEVEL_ERRORS.includes(error.kind)) {
      return { lines: [], firstGenerator: null, failed: true,
        summary: `Could not open ${portName} for scanning: ${error.message}. The port may be held by another program, or by MIB with different serial settings.` };
    }
  }
  if (!snapshot.candidates.length) {
    return { lines: [], firstGenerator: null, failed: false, summary: `No Modbus devices responded on ${portName} (addresses ${PULSE_SCAN.addressFrom}–${PULSE_SCAN.addressTo}).` };
  }
  const lines = snapshot.candidates.map((device: DiscoveredDevice) => device.display_name);
  const first = snapshot.candidates.find(device => device.identification === 0 && device.claimed_by.length > 0);
  return { lines, firstGenerator: first ? first.bus_address : null, failed: false,
    summary: `Pulse generator scan — ${portName}` };
}

export interface PulseScanApi {
  start: (request: DiscoveryRequest) => Promise<{ accepted: boolean; job_id: string; reason: string; rejection: number }>;
  fetch: (jobId: string) => Promise<DiscoverySnapshot>;
  cancel: (jobId: string) => Promise<boolean>;
  sleep?: (ms: number) => Promise<void>;
}
/** Run a scan and poll it to the end. `cancelled()` is checked between polls (the Cancel scan button): the job is
 *  cancelled on the backend and null is returned. Rejects when the backend refuses to start it. */
export async function runPulseScan(api: PulseScanApi, request: DiscoveryRequest, cancelled: () => boolean = () => false,
  options: { intervalMs?: number; timeoutMs?: number; now?: () => number } = {}): Promise<DiscoverySnapshot | null> {
  const interval = options.intervalMs ?? 100, now = options.now ?? (() => Date.now()), sleep = api.sleep ?? ((ms: number) => new Promise<void>(r => setTimeout(r, ms)));
  const start = await api.start(request);
  if (!start.accepted) throw new Error(`Could not start the scan: ${start.reason || `rejection ${start.rejection}`}`);
  // The backend job deadline is 60 s; the same margin the camera discovery client uses.
  const deadline = now() + (options.timeoutMs ?? 65_000);
  for (;;) {
    if (cancelled()) { await api.cancel(start.job_id).catch(() => false); return null; }
    const snapshot = await api.fetch(start.job_id);
    if (snapshot.valid && isTerminalDiscoveryState(snapshot.state)) return snapshot.state === DISCOVERY_JOB_STATES.Cancelled ? null : snapshot;
    if (now() >= deadline) { await api.cancel(start.job_id).catch(() => false); throw new Error('The scan timed out.'); }
    await sleep(interval);
  }
}

// ---- XGC + R5D preset for Live View (HW-057) ------------------------------------------------------------------

/** The MindVision rig preset of Qt's 'Use XGC + R5D preset for Live View' (ConfigTabs.cpp:571-633): a 512x96
 *  window, a rising-edge external trigger (one frame per generator pulse), a semi-automatic strobe. The generator
 *  rig values go in `live_view`. */
export const XGC_R5D_CAMERA_PRESET = {
  width: 512, height: 96, offset_x: 0, offset_y: 0, exposure_time_us: 100.0, auto_exposure_enabled: false,
  trigger_mode: 2, ext_trig_signal_type: 0, trigger_count: 1, ext_trig_jitter_us: 0, acq_trigger_delay_us: 0,
  strobe_mode: 1, strobe_delay_us: 0, strobe_pulse_width_us: 100,
  // Polarity 0 pulses OUT1 with the strobe on the XGC/R5D rig; 1 left the line idling high (LED on): measured 2026-09-15.
  strobe_polarity: 0,
} as const;
/** The trigger the camera is given: 1000 Hz rising edge with a 20 us pulse (2 % duty). */
export const XGC_R5D_LIVE_VIEW = { frequency_hz: 1000.0, duty_percent: 2.0 } as const;
/** The manual Set / Start values Qt leaves in the panel after the preset (the Align / Run rate). */
export const XGC_R5D_PANEL_VALUES = { frequency: '5000', duty: '10' } as const;

export function buildLiveViewDocument(documentText: string, draft: PulseDraft): string {
  const port = requirePort(draft.portName);
  let parsed: unknown;
  try { parsed = JSON.parse(documentText); } catch { throw new Error('Fix the camera JSON before saving the preset.'); }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) throw new Error('Fix the camera JSON before saving the preset.');
  const live = {
    enabled: true, port, address: parseAddress(draft.address), channel: parseChannel(draft.channel),
    frequency_hz: XGC_R5D_LIVE_VIEW.frequency_hz, duty_percent: XGC_R5D_LIVE_VIEW.duty_percent,
    baud: draft.baud, data_bits: draft.dataBits, parity: draft.parity, stop_bits: draft.stopBits,
  };
  return `${JSON.stringify({ ...(parsed as Record<string, unknown>), ...XGC_R5D_CAMERA_PRESET, live_view: live }, null, 2)}\n`;
}
