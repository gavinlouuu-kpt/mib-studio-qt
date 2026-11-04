// Persisted pulse generator settings (epic #868, HW-056). Qt keeps them in QSettings group "PulseGenerator"
// (ConfigTabs.cpp:1579-1664); the desktop keeps them under the `pulseGenerator` key of the preferences file
// (bridge.getPreferences / setPreferences, one JSON document shared with the shell). Preferences exist only in the
// desktop shell, so the browser transport falls back to localStorage.
//
// Qt also stores the port's USB identity (serial number, VID:PID) to re-find a renamed port. The bridge has no serial
// port enumeration yet (see the vault note), so only the port name is kept.
import {bridge} from '../bridge';
import {BAUD_RATES, DATA_BITS, PARITIES, PULSE_LIMITS, STOP_BITS, type PulseForm} from './pulseGeneratorModel';

export const PULSE_PREFERENCES_KEY = 'pulseGenerator';

const finite = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value);
const oneOf = <T,>(list: readonly T[], value: unknown): value is T => list.includes(value as T);

/** Keep only the fields that are valid today; a hand-edited or older file never reaches the form as a bad value. */
export function sanitizeSettings(raw: unknown): Partial<PulseForm> {
  if (!raw || typeof raw !== 'object') return {};
  const saved = raw as Record<string, unknown>, out: Partial<PulseForm> = {};
  if (typeof saved.port === 'string' && saved.port.length <= 512) out.port = saved.port;
  if (oneOf(BAUD_RATES, saved.baud)) out.baud = saved.baud;
  if (oneOf(DATA_BITS, saved.dataBits)) out.dataBits = saved.dataBits;
  if (oneOf(PARITIES, saved.parity)) out.parity = saved.parity;
  if (oneOf(STOP_BITS, saved.stopBits)) out.stopBits = saved.stopBits;
  if (finite(saved.address) && Number.isInteger(saved.address) && saved.address >= PULSE_LIMITS.address.min && saved.address <= PULSE_LIMITS.address.max) out.address = String(saved.address);
  if (finite(saved.channel) && Number.isInteger(saved.channel) && saved.channel >= 0 && saved.channel < PULSE_LIMITS.channelCount) out.channel = saved.channel;
  if (finite(saved.frequencyHz) && saved.frequencyHz >= PULSE_LIMITS.frequencyHz.min && saved.frequencyHz <= PULSE_LIMITS.frequencyHz.max) out.frequency = String(saved.frequencyHz);
  if (finite(saved.dutyPercent) && saved.dutyPercent >= PULSE_LIMITS.dutyPercent.min && saved.dutyPercent <= PULSE_LIMITS.dutyPercent.max) out.duty = String(saved.dutyPercent);
  return out;
}

/** The form as stored. Typed text that is not a valid number is left out rather than stored. */
export function serializeSettings(form: PulseForm): Record<string, unknown> {
  const out: Record<string, unknown> = {port: form.port.trim(), baud: form.baud, dataBits: form.dataBits, parity: form.parity, stopBits: form.stopBits, channel: form.channel};
  const number = (text: string) => (text.trim() ? Number(text) : NaN);
  if (Number.isFinite(number(form.address))) out.address = number(form.address);
  if (Number.isFinite(number(form.frequency))) out.frequencyHz = number(form.frequency);
  if (Number.isFinite(number(form.duty))) out.dutyPercent = number(form.duty);
  return out;
}

const LOCAL_KEY = 'mib.pulseGenerator';

export async function loadPulseGeneratorSettings(): Promise<Partial<PulseForm>> {
  try {
    const preferences = await bridge.getPreferences();
    return sanitizeSettings(preferences[PULSE_PREFERENCES_KEY]);
  } catch {
    try { return sanitizeSettings(JSON.parse(localStorage.getItem(LOCAL_KEY) ?? 'null')); } catch { return {}; }
  }
}

// Read-modify-write of the whole preferences document: one write at a time, so two saves never interleave.
let queue: Promise<unknown> = Promise.resolve();
/** Best effort: a store that is unavailable (browser, read-only config dir) never blocks or fails the hardware command. */
export function savePulseGeneratorSettings(form: PulseForm): Promise<void> {
  const settings = serializeSettings(form);
  const write = queue.then(async () => {
    try {
      const preferences = await bridge.getPreferences();
      await bridge.setPreferences({...preferences, [PULSE_PREFERENCES_KEY]: settings});
    } catch {
      try { localStorage.setItem(LOCAL_KEY, JSON.stringify(settings)); } catch { /* storage unavailable: settings reset next launch */ }
    }
  });
  queue = write;
  return write;
}
