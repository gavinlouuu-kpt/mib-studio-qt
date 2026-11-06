// Settings > Boot Service Toggles… (Qt's MainWindow.cpp:246-302; epic #868: HW-038): services to leave off at the next
// start. Saved on the instrument host (host_settings.json) and read before the backend starts, where Qt applies
// Startup/DisabledServices as MIB_DISABLED_SERVICES in main.cpp. A MIB_DISABLED_SERVICES set in the environment still wins.
import { useEffect, useRef, useState } from 'react';
import { useHostSettings, saveBootServices } from './hostSettingsStore';
import { BOOT_SAVED_MESSAGE, BOOT_SERVICE_OPTIONS, bootChecks, bootServicesToSave, savedBootServices } from './pumpSettingsModel';
import './PumpSettingsDialog.css';

type Props = { ready: boolean; append: (message: string) => void; onClose: () => void };

export function BootServicesDialog({ready, append, onClose}: Props) {
  const {doc, loaded} = useHostSettings(ready);
  const [checks, setChecks] = useState<Record<string, boolean> | null>(null);
  const [message, setMessage] = useState<{text: string; problem: boolean}>({text: '', problem: false});
  const [busy, setBusy] = useState(false);
  const alive = useRef(true);
  const saved = savedBootServices(doc?.settings);
  const extra = saved.filter(token => token !== 'all' && !BOOT_SERVICE_OPTIONS.some(option => option.token === token));
  useEffect(() => { alive.current = true; return () => { alive.current = false; }; }, []);
  useEffect(() => { if (loaded && doc && !checks) setChecks(bootChecks(savedBootServices(doc.settings))); }, [loaded, doc, checks]);
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => { if (event.key === 'Escape') onClose(); };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [onClose]);

  async function save() {
    if (!checks || busy) return;
    setBusy(true);
    const outcome = await saveBootServices(bootServicesToSave(checks, saved));
    if (!alive.current) return;
    setBusy(false);
    if (outcome.ok) { setMessage({text: BOOT_SAVED_MESSAGE, problem: false}); append(`Boot service toggles: ${BOOT_SAVED_MESSAGE}`); }
    else setMessage({text: `Not saved: ${outcome.message}`, problem: true});
  }
  return <div className="modal-backdrop" onClick={onClose}>
    <div className="modal boot-services" role="dialog" aria-modal="true" aria-label="Boot Service Toggles" onClick={event => event.stopPropagation()}>
      <h3>Boot Service Toggles</h3>
      <p>Disable selected services at next application startup. Changes are persisted and applied on next launch.</p>
      {doc?.boot_env && <p role="status" className="pump-settings-problem">MIB_DISABLED_SERVICES is set in this application's environment ({doc.boot_env}); it overrides what is saved here.</p>}
      {!checks ? <p role="status">{loaded ? 'The saved settings could not be read.' : 'Waiting for the backend…'}</p>
        : <div>{BOOT_SERVICE_OPTIONS.map(option => <label key={option.token} className="boot-service">
          <input type="checkbox" checked={!!checks[option.token]} onChange={event => setChecks({...checks, [option.token]: event.target.checked})} />{option.label}</label>)}
          {extra.length > 0 && <p>Also left off, kept as saved: {extra.join(', ')}.</p>}
        </div>}
      {message.text && <p role={message.problem ? 'alert' : 'status'} className={message.problem ? 'pump-settings-problem' : undefined}>{message.text}</p>}
      <div className="actions">
        <button type="button" className="btn" disabled={!checks || busy || !ready} onClick={() => void save().then(() => undefined)}>OK</button>
        <button type="button" className="btn" onClick={onClose}>Cancel</button>
      </div>
    </div>
  </div>;
}
