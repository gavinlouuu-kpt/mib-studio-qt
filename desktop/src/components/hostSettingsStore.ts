// The instrument host's saved settings, as the panels see them (epic #868; bridge ABI next (#890, set at merge) `fetch_host_settings` /
// `host_settings_command`). One read and one write path for the pump panel, the settings dialog and the boot toggles:
// every save goes through one chain (two quick saves cannot lose one another), and every change is announced to the
// other readers with a window event carrying the new document, so a dialog Apply updates the panel at once.
import { useEffect, useState } from 'react';
import { bridge, type HostSettingsDocument } from '../bridge';

export const HOST_SETTINGS_EVENT = 'mib:host-settings';

const failure = (error: unknown) => (error instanceof Error ? error.message : String(error));
export interface SaveOutcome { ok: boolean; message: string }

let chain: Promise<unknown> = Promise.resolve();
const announce = (doc: HostSettingsDocument) => window.dispatchEvent(new CustomEvent<HostSettingsDocument>(HOST_SETTINGS_EVENT, {detail: doc}));

/** Read the document and tell every listener. Resolves to null when the host cannot be read. */
export async function reloadHostSettings(): Promise<HostSettingsDocument | null> {
  try {
    const doc = await bridge.fetchHostSettings();
    if (!doc.valid) return null;
    announce(doc);
    return doc;
  } catch { return null; }
}

async function send(request: Record<string, unknown>): Promise<SaveOutcome> {
  try {
    const result = await bridge.hostSettingsCommand(request);
    if (!result.ok) return {ok: false, message: result.message || 'The host refused the settings.'};
    await reloadHostSettings();
    return {ok: true, message: result.message};
  } catch (error) { return {ok: false, message: failure(error)}; }
}

/** Merge `patch` into the saved pump settings (`null` removes a key). Saves are chained; never throws. */
export function savePumpSettings(patch: Record<string, unknown>): Promise<SaveOutcome> {
  const next = chain.then(() => send({action: 'set_pumps', settings: patch}));
  chain = next.catch(() => undefined);
  return next;
}
/** Save the boot service toggles (the services to leave off at the next start). */
export function saveBootServices(services: readonly string[]): Promise<SaveOutcome> {
  const next = chain.then(() => send({action: 'set_boot_services', services}));
  chain = next.catch(() => undefined);
  return next;
}

/** The saved settings once the backend is ready; follows every save. `null` until the first read (or when it fails). */
export function useHostSettings(ready: boolean): {doc: HostSettingsDocument | null; loaded: boolean} {
  const [doc, setDoc] = useState<HostSettingsDocument | null>(null);
  const [loaded, setLoaded] = useState(false);
  useEffect(() => {
    const onChange = (event: Event) => { const next = (event as CustomEvent<HostSettingsDocument>).detail; if (next) { setDoc(next); setLoaded(true); } };
    window.addEventListener(HOST_SETTINGS_EVENT, onChange);
    return () => window.removeEventListener(HOST_SETTINGS_EVENT, onChange);
  }, []);
  useEffect(() => {
    if (!ready) { setLoaded(false); return; }
    let live = true;
    void reloadHostSettings().then(next => { if (live) { if (next) setDoc(next); setLoaded(true); } });
    return () => { live = false; };
  }, [ready]);
  return {doc, loaded};
}
