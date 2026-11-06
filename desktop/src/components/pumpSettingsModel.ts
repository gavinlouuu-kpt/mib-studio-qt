// Syringe pump settings (Qt's SyringePumpSettingsDialog and the config.json keys it and SyringePumpTab write; epic #868:
// HW-003, HW-004, HW-016, HW-020, HW-021, HW-023, HW-038). Pure rules, no React, so they are unit-tested without a DOM.
//
// The keys are Qt's (`pump_sample_com_port`, `_baud_rate`, `_address`, `_syringe_vol`, `_syringe_unit`, `_flow_rate`,
// `_flow_unit`, `_direction`, and the Sheath's likewise) plus `pump_*_port`, the exact endpoint text, because this app
// also takes system paths (`/dev/ttyUSB0`) that an integer COM number cannot name. The instrument host keeps them
// (host_settings.json, ABI next (#890, set at merge)) and reserves the saved ports against discovery probes.
import type { PlatformCapabilities, SerialPortEntry } from '../bridge';
import { numericInput } from './hardwareControlModel';
import { BAUD_MAX, BAUD_MIN, DEFAULT_BAUD, UNIT_ML, UNIT_UL, VOLUME_DECIMALS, VOLUME_MAX, VOLUME_MIN, parseDecimal, syringeVolumeForDevice } from './pumpControlModel';

export const PUMP_LABELS = ['Sample', 'Sheath'] as const;
export const ADDRESS_MIN = 1;
export const ADDRESS_MAX = 247;
/** Qt's Scan: addresses 1-8, 300 ms each (SyringePumpSettingsDialog::scanAddresses). */
export const PUMP_SCAN = {start: 1, end: 8, timeoutMs: 300} as const;
/** The settings are read once the backend is ready; the scan answer is polled until it ends. */
export const SCAN_POLL_MS = 150;
export const SCAN_GIVE_UP_MS = 20000;

export const pumpKey = (slot: number, suffix: string) => `pump_${slot === 0 ? 'sample' : 'sheath'}_${suffix}`;
/** Qt's defaults when nothing is saved: Sample answers at Modbus address 1, Sheath at 2. */
export const defaultAddress = (slot: number) => slot + 1;

/** What the host has saved for one pump; null = never saved. */
export interface SavedPump {
  port: string; baud: number | null; address: number | null;
  volume: number | null; volumeUnit: number | null; rate: number | null; rateUnit: number | null; direction: number | null;
}
const whole = (value: unknown, min: number, max: number) => typeof value === 'number' && Number.isInteger(value) && value >= min && value <= max ? value : null;
const finite = (value: unknown, min: number, max: number) => typeof value === 'number' && Number.isFinite(value) && value >= min && value <= max ? value : null;

/** Tolerant like Qt's loadConfig: a value that is missing or no longer valid is null, the rest is used. */
export function savedPump(settings: Record<string, unknown> | undefined, slot: number): SavedPump {
  const s = settings ?? {};
  const text = s[pumpKey(slot, 'port')];
  const comPort = whole(s[pumpKey(slot, 'com_port')], 1, 65535);
  return {
    port: typeof text === 'string' && text.trim() ? text : comPort !== null ? `COM${comPort}` : '',
    baud: whole(s[pumpKey(slot, 'baud_rate')], 1, 4000000), address: whole(s[pumpKey(slot, 'address')], ADDRESS_MIN, ADDRESS_MAX),
    volume: finite(s[pumpKey(slot, 'syringe_vol')], VOLUME_MIN, VOLUME_MAX), volumeUnit: whole(s[pumpKey(slot, 'syringe_unit')], 0, 65535),
    rate: finite(s[pumpKey(slot, 'flow_rate')], 0, 100000), rateUnit: whole(s[pumpKey(slot, 'flow_unit')], 0, 65535), direction: whole(s[pumpKey(slot, 'direction')], 0, 1),
  };
}

/** The COM number of a Windows port name ("COM3", "com3", "3"), else null. */
export function comNumber(port: string): number | null {
  const match = /^(?:COM)?(\d{1,5})$/i.exec(port.trim());
  const n = match ? Number(match[1]) : NaN;
  return Number.isInteger(n) && n >= 1 && n <= 65535 ? n : null;
}
/** Digits alone are a COM number, as the old free-text field read them. */
export const portName = (port: string) => { const n = /^\d+$/.test(port.trim()) ? comNumber(port) : null; return n === null ? port.trim() : `COM${n}`; };

/** The connection a Connect uses: the saved one, else the defaults the caller has for this pump. */
export interface Connection { port: string; baud: number; address: number }
export function connectionOf(saved: SavedPump, slot: number, fallback?: {port: string; address: number}): Connection {
  return {port: saved.port || fallback?.port || '', baud: saved.baud ?? DEFAULT_BAUD, address: saved.address ?? fallback?.address ?? defaultAddress(slot)};
}
/** Where the instrument's own pumps answer when nothing is saved (PZ7035: PS UART1 RS485, Sample slave 3, Sheath slave 4). */
export function instrumentFallback(capabilities: PlatformCapabilities, slot: number): {port: string; address: number} | undefined {
  const pump = capabilities.pump;
  return pump ? {port: pump.port, address: slot === 0 ? pump.sample_address : pump.sheath_address} : undefined;
}
export const numberOrNull = (text: string): number | null => { const n = Number(text); return text.trim() && Number.isFinite(n) ? n : null; };
/** Qt's wording when nothing is saved (SyringePumpTab::onConnectSample). */
export const NO_PORT_MESSAGE = 'No COM port configured. Open Pump settings…';

// ---- The dialog's form --------------------------------------------------------------------------------------------

export interface PumpDraft { port: string; baud: string; address: string; volume: string; volumeUnit: number }
const trimNumber = (value: number) => String(Number(value.toFixed(VOLUME_DECIMALS)));
export function draftOf(saved: SavedPump, slot: number, fallback?: {port: string; address: number}): PumpDraft {
  const connection = connectionOf(saved, slot, fallback);
  return {
    port: connection.port, baud: String(connection.baud), address: String(connection.address),
    volume: saved.volume === null ? '' : trimNumber(saved.volume), volumeUnit: saved.volumeUnit === UNIT_ML ? UNIT_ML : UNIT_UL,
  };
}

/** What Apply saves and sends, from both pumps' forms. Everything is checked before anything is written (atomic Apply), and
 *  the first problem is named with its pump. `volumes` is false for pumps that have no syringe (a peristaltic instrument). */
export interface Planned { patch: Record<string, unknown>; volumes: Array<{slot: number; value: number; unit: number} | null> }
export function planApply(drafts: readonly PumpDraft[], volumes: boolean): Planned {
  const patch: Record<string, unknown> = {};
  const live: Planned['volumes'] = [];
  drafts.forEach((draft, slot) => {
    const label = PUMP_LABELS[slot];
    const named = <T,>(step: () => T): T => { try { return step(); } catch (failure) { throw new Error(`${label} pump: ${failure instanceof Error ? failure.message : String(failure)}`); } };
    const port = portName(draft.port);
    // Qt writes the port only when one is selected; a blank selection leaves the saved one.
    if (port) {
      if (port.length > 512 || port.includes('\0')) throw new Error(`${label} pump: the serial port name is not valid.`);
      patch[pumpKey(slot, 'port')] = port;
      patch[pumpKey(slot, 'com_port')] = comNumber(port);
    }
    patch[pumpKey(slot, 'baud_rate')] = named(() => numericInput(draft.baud, 'Baud rate', BAUD_MIN, BAUD_MAX, true));
    patch[pumpKey(slot, 'address')] = named(() => numericInput(draft.address, 'Modbus address', ADDRESS_MIN, ADDRESS_MAX, true));
    live.push(null);
    if (volumes && draft.volume.trim()) {
      const value = named(() => parseDecimal(draft.volume, 'Syringe volume', VOLUME_MIN, VOLUME_MAX, VOLUME_DECIMALS));
      // D9: a volume the syringe pump cannot take exactly is refused here, not saved and silently truncated later.
      const exact = named(() => syringeVolumeForDevice(value, draft.volumeUnit));
      patch[pumpKey(slot, 'syringe_vol')] = value;
      patch[pumpKey(slot, 'syringe_unit')] = draft.volumeUnit;
      live[slot] = {slot, value: exact.value, unit: exact.unit};
    }
  });
  return {patch, volumes: live};
}

// ---- Serial ports -------------------------------------------------------------------------------------------------

/** The names the port list offers: enumerated ports, plus a saved port that is not (or no longer) listed so that it is never
 *  replaced by the first row without the operator choosing (Qt silently shows the first port instead). */
export function portChoices(ports: readonly SerialPortEntry[], selected: string): Array<{value: string; label: string; missing: boolean}> {
  const rows = ports.map(p => ({value: p.system_name, label: portOption(p), missing: false}));
  const wanted = selected.trim();
  if (wanted && !ports.some(p => samePort(p.system_name, wanted) || samePort(p.system_location, wanted))) rows.push({value: wanted, label: `${wanted} (saved, not found)`, missing: true});
  return rows;
}
/** The listed name that is the port `selected` names ("ttyUSB0" for "/dev/ttyUSB0"; "COM3" for "com3"). */
export function listedName(ports: readonly SerialPortEntry[], selected: string): string {
  const hit = ports.find(p => samePort(p.system_name, selected) || samePort(p.system_location, selected));
  return hit ? hit.system_name : selected;
}
const key = (port: string) => port.trim().replace(/^\\\\\.\\/, '').replace(/^\/dev\//, '').toLowerCase();
const samePort = (a: string, b: string) => !!a && !!b && key(a) === key(b);
const hex4 = (value: number) => value.toString(16).padStart(4, '0');
/** Qt's port label: 'ttyUSB0 — FT232R USB UART, S/N A12345, 0403:6001'. */
export function portOption(port: SerialPortEntry): string {
  const extra: string[] = [];
  if (port.description) extra.push(port.description);
  if (port.serial_number) extra.push(`S/N ${port.serial_number}`);
  if (port.vendor_id !== 0) extra.push(`${hex4(port.vendor_id)}:${hex4(port.product_id)}`);
  return extra.length ? `${port.system_name} — ${extra.join(', ')}` : port.system_name;
}

// ---- Scan messages (SyringePumpSettingsDialog::scanAddresses) -----------------------------------------------------------

export const SCAN_NO_PORT = 'Select a COM port first.';
export function scanMessage(result: {state: string; found: number[]; error: string}, port: string, baud: number): {text: string; problem: boolean} {
  if (result.state === 'failed') return {text: `Scan failed: ${result.error || 'the port could not be read'}.`, problem: true};
  if (!result.found.length) return {text: `No pump address responded on ${port} at ${baud} baud.`, problem: false};
  return {text: `Found ${result.found.length} responsive address(es) on ${port}.`, problem: false};
}

// ---- Boot service toggles (MainWindow.cpp:246-302) ----------------------------------------------------------------------

/** Qt's list, in its order. The eighth, "Auto update checker", is left out: this app never checks for updates by itself, so
 *  the switch would do nothing. A token saved but not listed here (auto_update, dot_grid) is kept as it is. */
export const BOOT_SERVICE_OPTIONS = [
  {token: 'sqlite', label: 'SQLite service'}, {token: 'hdf5', label: 'HDF5 service'}, {token: 'processing', label: 'Processing service'},
  {token: 'autofocus', label: 'Autofocus wiring'}, {token: 'trigger', label: 'Trigger wiring'}, {token: 'capture', label: 'Capture service'},
  {token: 'playback', label: 'Playback service'},
] as const;
export const BOOT_SAVED_MESSAGE = 'Saved. Restart the application for changes to take effect.';

/** The tokens saved on the host; "all" (Qt: every box ticked) reads as every listed service. */
export function savedBootServices(settings: Record<string, unknown> | undefined): string[] {
  const raw = settings?.boot_disabled_services;
  return Array.isArray(raw) ? raw.filter((t): t is string => typeof t === 'string') : [];
}
export function bootChecks(saved: readonly string[]): Record<string, boolean> {
  const all = saved.includes('all');
  return Object.fromEntries(BOOT_SERVICE_OPTIONS.map(o => [o.token, all || saved.includes(o.token)]));
}
/** The list to save: the ticked services, plus every saved token the dialog does not list (never "all": it is expanded). */
export function bootServicesToSave(checks: Record<string, boolean>, saved: readonly string[]): string[] {
  const listed = new Set<string>(BOOT_SERVICE_OPTIONS.map(o => o.token));
  const kept = saved.filter(t => !listed.has(t) && t !== 'all');
  return [...BOOT_SERVICE_OPTIONS.filter(o => checks[o.token]).map(o => o.token), ...kept];
}
