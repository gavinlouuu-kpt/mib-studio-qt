// Operator nanopositioner / autofocus rules (Tauri parity wave 1, hardware stream; epic #868, PR3): input ranges of the
// Qt NanopositionerTab, the device list built from the discovery job, the connect target the backend accepts, the command
// queue that keeps a stale Enable or Jog from going out after the operator pressed Disable, and the remembered selection
// and setpoint. Pure logic, no React, so it is unit-tested without a DOM.
import type { AutofocusConfig, DiscoveredDevice } from '../bridge';
import { DISCOVERY_IDENTIFICATION_STATUSES } from '../bridgeContract';
import { defaultStartupPreference, loadStartupPreference, saveStartupPreference, startupPreferenceKey, validateStartupPreference, type StartupPreference } from '../startupPreference';

/** NanopositionerTab.ui:63-78: the backend combo. Auto-detect keeps every device in the list. */
export type BackendChoice = 'auto' | 'oeabt' | 'coremor';
export type ResolvedBackend = 'oeabt' | 'coremor';
export const BACKEND_CHOICES: ReadonlyArray<{value: BackendChoice; label: string}> = [
  {value: 'auto', label: 'Auto-detect'}, {value: 'oeabt', label: 'OEABT serial'}, {value: 'coremor', label: 'CoreMOR'},
];
/** NanopositionerTab.cpp:101-107: the CoreMOR baud combo. */
export const COREMOR_BAUD_RATES = [9600, 19200, 38400, 57600, 115200];
export const DEFAULT_BAUD = 115200;
export const ADDRESS_MIN = 0;
export const ADDRESS_MAX = 255;
export const DEFAULT_ADDRESS = 1;
/** NanopositionerTab.ui:212-245: target ring width spin box, 1-100, 2 decimals, default 20. */
export const SETPOINT_MIN = 1;
export const SETPOINT_MAX = 100;
export const SETPOINT_DECIMALS = 2;
export const DEFAULT_SETPOINT = 20;
/** NanopositionerTab.ui:308-361: the manual voltage step spin box, whole volts 1-100, default 1. */
export const JOG_STEP_MIN = 1;
export const JOG_STEP_MAX = 100;
export const DEFAULT_JOG_STEP = 1;
/** Qt applies the setpoint on every keystroke; the Tauri field waits for the operator to stop typing so that "25" never
 *  sends 2 to a running autofocus. Enter and leaving the field apply at once. */
export const SETPOINT_APPLY_MS = 500;
/** Qt refreshes the voltage label and the checkbox every 500 ms (NanopositionerTab.cpp:150-155). */
export const STATUS_POLL_MS = 500;
/** The configuration (stale limit, setpoint set by a profile) is read every Nth status poll. */
export const CONFIG_POLL_EVERY = 4;
export const NO_DEVICE_HINT = 'Click Refresh to search for nanopositioners.';

export const errorText = (failure: unknown) => failure instanceof Error ? failure.message : String(failure);

/** The setpoint as the spin box would hold it: 1-100, rounded to 2 decimals. */
export function parseSetpoint(text: string): number {
  const trimmed = text.trim();
  if (!/^(\d+\.?\d*|\.\d+)$/.test(trimmed)) throw new Error(`Target ring width must be a number from ${SETPOINT_MIN} to ${SETPOINT_MAX}.`);
  const value = Number(Number(trimmed).toFixed(SETPOINT_DECIMALS));
  if (!(value >= SETPOINT_MIN && value <= SETPOINT_MAX)) throw new Error(`Target ring width must be from ${SETPOINT_MIN} to ${SETPOINT_MAX}.`);
  return value;
}
export const formatSetpoint = (value: number) => String(Number(value.toFixed(SETPOINT_DECIMALS)));

/** The jog step as the spin box would hold it: a whole number of volts, 1-100. */
export function parseJogStep(text: string): number {
  const trimmed = text.trim();
  if (!/^\d+$/.test(trimmed)) throw new Error(`Step must be a whole number of volts from ${JOG_STEP_MIN} to ${JOG_STEP_MAX}.`);
  const value = Number(trimmed);
  if (!(value >= JOG_STEP_MIN && value <= JOG_STEP_MAX)) throw new Error(`Step must be from ${JOG_STEP_MIN} to ${JOG_STEP_MAX} V.`);
  return value;
}
/** The step field's value for a configured step (Qt: static_cast<int>(manualVoltageStep) into a 1-100 spin box). */
export const stepFromConfig = (value: number) => Number.isFinite(value) ? String(Math.min(JOG_STEP_MAX, Math.max(JOG_STEP_MIN, Math.trunc(value)))) : String(DEFAULT_JOG_STEP);

/** The device address as the spin box would hold it. */
export function parseAddress(text: string): number {
  const trimmed = text.trim();
  if (!/^\d+$/.test(trimmed) || Number(trimmed) < ADDRESS_MIN || Number(trimmed) > ADDRESS_MAX) throw new Error(`Device address must be a whole number from ${ADDRESS_MIN} to ${ADDRESS_MAX}.`);
  return Number(trimmed);
}

/** One row of the device list: a discovered endpoint, or the remembered selection when the scan did not return it. */
export interface Candidate {
  /** Persistent identity the backend connects by (a `/dev/serial/by-id` path, a COM name). */
  id: string;
  systemPath: string;
  /** Qt's combo text: the display name, with 'OEABT candidate — ' in front of OEABT endpoints. */
  label: string;
  backend: ResolvedBackend;
  /** CoreMOR COM number; -1 for OEABT. */
  comPort: number;
  identification: 'identified' | 'unidentified' | 'ambiguous';
  /** True when the row is the remembered selection and the scan did not list it. */
  saved: boolean;
}

const VENDOR_PREFIX = 'vendor:';
/** The backend that enumerated the endpoint. The discovery candidate carries it as `vendor:<name>` in its capabilities
 *  ("CoreMorrow / XMT" or "OEABT", NanopositionerProvider.cpp). */
function candidateBackend(device: DiscoveredDevice): ResolvedBackend {
  const vendor = device.capabilities.find(capability => capability.startsWith(VENDOR_PREFIX))?.slice(VENDOR_PREFIX.length) ?? '';
  if (/core\s*morrow|coremor|xmt/i.test(vendor)) return 'coremor';
  if (/oeabt/i.test(vendor)) return 'oeabt';
  // No vendor capability: a bare COM name is the CoreMorrow DLL's addressing, anything else is a serial path.
  return /^COM\d+$/i.test(device.system_path) && !device.claimed_by.some(name => /oeabt/i.test(name)) ? 'coremor' : 'oeabt';
}
const comNumber = (path: string) => { const match = /^COM(\d+)$/i.exec(path.trim()); return match ? Number(match[1]) : -1; };

export function candidateFromDevice(device: DiscoveredDevice): Candidate {
  const backend = candidateBackend(device);
  const name = device.display_name || device.system_path;
  const identification = device.identification === DISCOVERY_IDENTIFICATION_STATUSES.Identified ? 'identified'
    : device.identification === DISCOVERY_IDENTIFICATION_STATUSES.Ambiguous ? 'ambiguous' : 'unidentified';
  return {
    id: device.persistent_id || device.system_path, systemPath: device.system_path,
    label: backend === 'oeabt' ? `OEABT candidate — ${name}` : name,
    backend, comPort: backend === 'coremor' ? comNumber(device.persistent_id || device.system_path) : -1, identification, saved: false,
  };
}

/** The remembered selection as a list row, for the case that the scan has not run or did not list it. */
export function candidateFromPreference(preference: StartupPreference): Candidate | null {
  const id = preference.endpoint.trim();
  if (!id) return null;
  const backend: ResolvedBackend = preference.backend === 'coremor' || (preference.backend === 'auto' && preference.com_port > 0) ? 'coremor' : 'oeabt';
  return {
    id, systemPath: id, label: `${backend === 'oeabt' ? 'OEABT candidate — ' : ''}${id} (saved selection, not scanned)`, backend,
    comPort: backend === 'coremor' ? (preference.com_port > 0 ? preference.com_port : comNumber(id)) : -1, identification: 'unidentified', saved: true,
  };
}

/** Qt's populateComPortList: the backend choice filters the list; the saved row only joins when the scan did not list it. */
export function listCandidates(scanned: Candidate[], choice: BackendChoice, saved: Candidate | null): Candidate[] {
  const rows = [...scanned];
  if (saved && !rows.some(row => row.id === saved.id)) rows.push(saved);
  return choice === 'auto' ? rows : rows.filter(row => row.backend === choice);
}

/** CoreMOR baud and address matter only for CoreMOR (updateNanopositionerUI: `coremorSettings`). */
export const usesCoremorSettings = (choice: BackendChoice, candidate: Candidate | null) =>
  choice === 'coremor' || (choice === 'auto' && candidate?.backend === 'coremor');

export interface ConnectTarget { backend: ResolvedBackend; endpoint: string; comPort: number; baud: number; address: number }
/** What `autofocus_connect_endpoint` takes. The backend refuses an Auto connect (BackendFacade.cpp:2330-2332), so Auto
 *  resolves to the row's own backend here, as AutofocusService::connect resolves it for Qt. The baud and address fields
 *  always go along (Qt applies them to every endpoint; OEABT ignores them). */
export function connectTarget(input: {choice: BackendChoice; candidate: Candidate | null; baud: string; address: string}): ConnectTarget {
  const {candidate} = input;
  if (!candidate) throw new Error('Select a serial device first (Refresh searches for nanopositioners).');
  const backend: ResolvedBackend = input.choice === 'auto' ? candidate.backend : input.choice;
  const baud = Number(input.baud);
  if (!Number.isInteger(baud) || baud <= 0) throw new Error('Baud rate must be a positive whole number.');
  const address = parseAddress(input.address);
  if (backend === 'coremor' && !(candidate.comPort > 0)) throw new Error(`${candidate.id} is not a CoreMOR COM port (COMn).`);
  return {backend, endpoint: candidate.id, comPort: backend === 'coremor' ? candidate.comPort : -1, baud, address};
}

/** What Qt writes to config.json on connect, disconnect and exit (NanopositionerTab::saveConfig), in the shape of the
 *  startup preference the backend's startup discovery already reads. */
export function selectionPreference(input: {choice: BackendChoice; candidate: Candidate | null; baud: string; address: string; previous: StartupPreference}): StartupPreference | null {
  const baud = Number(input.baud), address = Number(input.address);
  if (!Number.isInteger(baud) || baud < 1 || !Number.isInteger(address) || address < ADDRESS_MIN || address > ADDRESS_MAX) return null;
  const {candidate, choice, previous} = input;
  // No device in the list: Qt still saves baud and address and keeps the earlier selection.
  if (!candidate) return {...previous, baud, address};
  const comPort = candidate.backend === 'coremor' && choice !== 'oeabt' && candidate.comPort > 0 ? candidate.comPort : -1;
  // A CoreMOR preference needs its COM number; without one the choice is kept as Auto rather than saving something the backend refuses.
  const backend: BackendChoice = choice === 'coremor' && comPort < 1 ? 'auto' : choice;
  return {backend, endpoint: candidate.id, com_port: comPort, baud, address};
}

export type CommandKind = 'connection' | 'actuate' | 'config';
export const SKIPPED = Symbol('skipped');
export type CommandContext = {
  /** True once the operator pressed Disable or Disconnect after this command was queued: an actuating command must not be sent. */
  cancelled: () => boolean;
};
/** Operator commands run one at a time, each waiting for its turn (a setpoint edit never overlaps a Jog). Disable never
 *  enters the queue (D3). `cancelActuation()` is called when the operator disables or disconnects: an Enable or Jog still
 *  waiting, or waiting inside its own steps, is not sent afterwards (the pump Stop fix of PR1 uses the same pattern). */
export class AutofocusCommandQueue {
  private tail: Promise<unknown> = Promise.resolve();
  private epoch = 0;
  cancelActuation(): void { this.epoch += 1; }
  run<T>(kind: CommandKind, command: (context: CommandContext) => Promise<T>): Promise<T | typeof SKIPPED> {
    const queuedAt = this.epoch;
    const context: CommandContext = {cancelled: () => kind === 'actuate' && this.epoch !== queuedAt};
    const result = this.tail.then((): Promise<T | typeof SKIPPED> | typeof SKIPPED => context.cancelled() ? SKIPPED : command(context));
    this.tail = result.catch(() => undefined);
    return result;
  }
}

// ---- Remembered values (Qt: config.json autofocus_backend / _endpoint / _com_port / _baud_rate / _device_address / _focus_setpoint) ----
export const SELECTION_SAVED_EVENT = 'mib:nanopositioner-selection-saved';
export const setpointKey = 'mib.autofocus.setpoint.v1';
/** The Home value (MIBQT-45): the target ring width the applied profile or method set, else the backend's default. Kept so
 *  that a reloaded page (the backend kept running) shows the same Home as before. */
export const homeKey = 'mib.autofocus.home.v1';

export function rememberedSelection(storage: Pick<Storage, 'getItem'> | undefined = safeStorage()): StartupPreference {
  if (!storage) return {...defaultStartupPreference};
  return loadStartupPreference(storage).preference;
}
/** Writes the startup preference (the same record the startup discovery uses) and tells the startup panel. Never throws. */
export function rememberSelection(preference: StartupPreference, storage: Pick<Storage, 'setItem'> | undefined = safeStorage()): {saved: boolean; error?: string; preference?: StartupPreference} {
  try {
    if (!storage) throw new Error('Browser storage is unavailable');
    const saved = saveStartupPreference(storage, validateStartupPreference(preference));
    if (typeof window !== 'undefined') window.dispatchEvent(new CustomEvent(SELECTION_SAVED_EVENT, {detail: saved}));
    return {saved: true, preference: saved};
  } catch (failure) { return {saved: false, error: errorText(failure)}; }
}
export function rememberedSetpoint(storage: Pick<Storage, 'getItem'> | undefined = safeStorage(), key = setpointKey): number | null {
  try {
    const text = storage?.getItem(key);
    if (!text) return null;
    const value = Number(JSON.parse(text));
    return Number.isFinite(value) && value >= SETPOINT_MIN && value <= SETPOINT_MAX ? Number(value.toFixed(SETPOINT_DECIMALS)) : null;
  } catch { return null; }
}
export function rememberSetpoint(value: number, storage: Pick<Storage, 'setItem'> | undefined = safeStorage(), key = setpointKey): boolean {
  try { if (!storage) return false; storage.setItem(key, JSON.stringify(Number(value.toFixed(SETPOINT_DECIMALS)))); return true; } catch { return false; }
}
/** The stored Home value, or the backend's default target ring width (20) when none is stored or it is out of range. */
export const rememberedHome = (storage: Pick<Storage, 'getItem'> | undefined = safeStorage()) => rememberedSetpoint(storage, homeKey) ?? DEFAULT_SETPOINT;
export const rememberHome = (value: number, storage: Pick<Storage, 'setItem'> | undefined = safeStorage()) => rememberSetpoint(value, storage, homeKey);
/** Two setpoints are the same when they agree to the spin box's two decimals. */
export const sameSetpoint = (a: number, b: number) => Math.abs(a - b) < 0.005;
function safeStorage(): Storage | undefined { try { return typeof localStorage === 'undefined' ? undefined : localStorage; } catch { return undefined; } }
export { startupPreferenceKey };

/** Fields of the configuration the operator controls own; the tuning editor leaves them alone so that applying it never
 *  overwrites a live setpoint or step with a stale copy. */
export const OPERATOR_CONFIG_KEYS = ['focus_setpoint', 'manual_voltage_step'] as const satisfies ReadonlyArray<keyof AutofocusConfig>;
