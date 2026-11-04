// Boot Service Toggles (Qt-parity HW-038): Qt's Settings > Boot Service Toggles dialog.
//
// The chosen services are persisted in the desktop preferences under `bootDisabledServices` and the
// Tauri shell turns them into MIB_DISABLED_SERVICES when it initializes the backend at the next
// launch, unless the environment already names services (the environment wins). Nothing changes in
// the running backend, so the only feedback is the saved note. The write is read-modify-write: every
// other preference key is kept. Mount it where the other settings live; it owns no layout.
import { useCallback, useEffect, useState } from "react";
import { bridge } from "../../bridge";

/** The preferences key (bridge-contract.json hardware_parity.boot_disabled_services). */
export const BOOT_DISABLED_SERVICES_KEY = "bootDisabledServices";

/** Qt's list, with its labels and order (MainWindow.cpp). The tokens are what the backend reads from
 *  MIB_DISABLED_SERVICES; the Rust side validates against the same list. */
export const BOOT_SERVICE_OPTIONS = [
  { token: "sqlite", label: "SQLite service" },
  { token: "hdf5", label: "HDF5 service" },
  { token: "processing", label: "Processing service" },
  { token: "autofocus", label: "Autofocus wiring" },
  { token: "trigger", label: "Trigger wiring" },
  { token: "capture", label: "Capture service" },
  { token: "playback", label: "Playback service" },
  { token: "auto_update", label: "Auto update checker" },
] as const;

type Token = (typeof BOOT_SERVICE_OPTIONS)[number]["token"];
type Preferences = Record<string, unknown>;

/** Where the preferences live; the Tauri/WebSocket bridge by default, a fake in tests. */
export interface PreferencesStore {
  get: () => Promise<Preferences>;
  set: (preferences: Preferences) => Promise<void>;
}

const bridgeStore: PreferencesStore = {
  get: () => bridge.getPreferences(),
  set: (preferences) => bridge.setPreferences(preferences),
};

/** The known tokens of a stored value, as Qt normalizes them (trimmed, lower case, `-` as `_`);
 *  anything else is dropped, as the shell drops it at startup. */
export function disabledServicesFrom(value: unknown): Set<Token> {
  const known = new Set<string>(BOOT_SERVICE_OPTIONS.map((o) => o.token));
  const out = new Set<Token>();
  if (Array.isArray(value)) {
    for (const entry of value) {
      const token = typeof entry === "string" ? entry.trim().toLowerCase().replace(/-/g, "_") : "";
      if (known.has(token)) out.add(token as Token);
    }
  }
  return out;
}

const message = (error: unknown) => (error instanceof Error ? error.message : String(error));

export function BootServiceToggles({ store = bridgeStore }: { store?: PreferencesStore }) {
  const [disabled, setDisabled] = useState<Set<Token>>(new Set());
  const [loaded, setLoaded] = useState(false);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    store.get().then(
      (preferences) => { if (alive) { setDisabled(disabledServicesFrom(preferences?.[BOOT_DISABLED_SERVICES_KEY])); setLoaded(true); } },
      (e) => { if (alive) setError(`Could not read the saved preferences: ${message(e)}`); },
    );
    return () => { alive = false; };
  }, [store]);

  const toggle = useCallback((token: Token, on: boolean) => {
    setSaved(false);
    setDisabled((current) => {
      const next = new Set(current);
      if (on) next.add(token); else next.delete(token);
      return next;
    });
  }, []);

  const save = useCallback(async () => {
    setSaving(true); setSaved(false); setError(null);
    try {
      // Read again: another part of the app may have written other keys since this form loaded.
      const current = await store.get();
      const tokens = BOOT_SERVICE_OPTIONS.map((o) => o.token).filter((t) => disabled.has(t));
      await store.set({ ...current, [BOOT_DISABLED_SERVICES_KEY]: tokens });
      setSaved(true);
    } catch (e) {
      setError(`Could not save: ${message(e)}`);
    } finally {
      setSaving(false);
    }
  }, [store, disabled]);

  return (
    <section className="boot-service-toggles" aria-label="Boot Service Toggles">
      <h3>Boot Service Toggles</h3>
      <p>Disable selected services at next application startup. Changes are persisted and applied on next launch.</p>
      <div className="hardware-fields">
        {BOOT_SERVICE_OPTIONS.map(({ token, label }) => (
          <label key={token}>
            <input type="checkbox" checked={disabled.has(token)} disabled={!loaded || saving}
                   onChange={(e) => toggle(token, e.target.checked)} />
            {label}
          </label>
        ))}
      </div>
      <div className="hardware-actions">
        <button disabled={!loaded || saving} onClick={() => void save()}>Save</button>
      </div>
      {saved && <p role="status">Saved. Restart the application for changes to take effect.</p>}
      {error && <p role="alert">{error}</p>}
    </section>
  );
}
