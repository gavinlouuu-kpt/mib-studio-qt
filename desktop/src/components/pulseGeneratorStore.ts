// Where the pulse generator settings live between launches (HW-056).
//
// Qt keeps them in QSettings (group PulseGenerator) of the user running the app. The generator is wired to the
// instrument's host, not to the browser or the webview that happens to control it, so the durable copy is the
// shell preferences file of the instrument (bridge `get_preferences` / `set_preferences`, BE-9: the Tauri app's
// app-config dir, or the server's data dir), the same store that keeps the sidebar state. A localStorage copy is
// the fallback when the preferences cannot be read or written, like the remembered nanopositioner selection
// (startupPreference.ts). Saving never throws: a failure is returned so the panel can say it.
import { PULSE_SETTINGS_KEY, PULSE_SETTINGS_STORAGE_KEY, parsePulseSettings, type PulseLimits, type PulseSettings, DEFAULT_PULSE_LIMITS } from './pulseGeneratorModel';

export interface PreferenceApi {
  get: () => Promise<Record<string, unknown>>;
  set: (preferences: Record<string, unknown>) => Promise<void>;
}
type StorageRead = Pick<Storage, 'getItem'>;
type StorageWrite = Pick<Storage, 'setItem'>;

export async function loadPulseSettings(api: PreferenceApi, storage: StorageRead | null, limits: PulseLimits = DEFAULT_PULSE_LIMITS): Promise<Partial<PulseSettings>> {
  try {
    const parsed = parsePulseSettings((await api.get())[PULSE_SETTINGS_KEY], limits);
    if (Object.keys(parsed).length) return parsed;
  } catch { /* fall back to the local copy */ }
  try {
    const text = storage?.getItem(PULSE_SETTINGS_STORAGE_KEY);
    if (text) return parsePulseSettings(JSON.parse(text), limits);
  } catch { /* nothing usable stored */ }
  return {};
}

// Saves are chained: each reads the file, merges and writes, so two quick saves cannot lose one another.
let chain: Promise<unknown> = Promise.resolve();

/** Merge `patch` into the saved record and write it. Resolves to an error text, or '' when the durable copy was written. */
export function savePulseSettings(api: PreferenceApi, storage: (StorageRead & StorageWrite) | null, patch: Partial<PulseSettings>, limits: PulseLimits = DEFAULT_PULSE_LIMITS): Promise<string> {
  const run = async (): Promise<string> => {
    let preferences: Record<string, unknown> = {};
    let previous: Partial<PulseSettings> = {};
    let failure = '';
    try {
      preferences = await api.get();
      previous = parsePulseSettings(preferences[PULSE_SETTINGS_KEY], limits);
    } catch (error) { failure = String(error); }
    if (!Object.keys(previous).length) {
      try { const text = storage?.getItem(PULSE_SETTINGS_STORAGE_KEY); if (text) previous = parsePulseSettings(JSON.parse(text), limits); } catch { /* none */ }
    }
    const merged = { ...previous, ...patch };
    try { storage?.setItem(PULSE_SETTINGS_STORAGE_KEY, JSON.stringify(merged)); } catch { /* private mode, quota */ }
    if (failure) return failure;
    try { await api.set({ ...preferences, [PULSE_SETTINGS_KEY]: merged }); } catch (error) { return String(error); }
    return '';
  };
  const next = chain.then(run, run);
  chain = next.catch(() => undefined);
  return next;
}
