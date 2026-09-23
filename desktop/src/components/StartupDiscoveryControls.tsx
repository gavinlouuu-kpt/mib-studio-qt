import {useEffect, useRef, useState} from 'react';
import {bridge, type StartupDiscoveryStatus} from '../bridge';
import {loadStartupPreference, saveStartupPreference, validateStartupPreference, type StartupPreference} from '../startupPreference';
const preferenceKey = 'mib.startup.auto-select-hardware';
export function StartupDiscoveryControls({ready, experimentActive, append, onSelectionChanged}: {ready: boolean; experimentActive: boolean; append: (message: string) => void; onSelectionChanged?: () => void}) {
  const [auto, setAuto] = useState(() => {try {return localStorage.getItem(preferenceKey) === 'true';} catch {return false;}});
  const [remembered, setRemembered] = useState(() => loadStartupPreference(localStorage));
  const [backend, setBackend] = useState(remembered.preference.backend), [endpoint, setEndpoint] = useState(remembered.preference.endpoint);
  const [port, setPort] = useState(String(remembered.preference.com_port)), [baud, setBaud] = useState(String(remembered.preference.baud)), [address, setAddress] = useState(String(remembered.preference.address));
  const [status, setStatus] = useState<StartupDiscoveryStatus | null>(null), [message, setMessage] = useState(remembered.error ?? '');
  const [pending, setPending] = useState(false), [preferenceReady, setPreferenceReady] = useState(false);
  const commandPending = useRef(false), started = useRef(false), previousSelection = useRef('');
  const selectionCallback = useRef(onSelectionChanged); selectionCallback.current = onSelectionChanged;
  const lifecycle = useRef({ready, experimentActive}); lifecycle.current = {ready, experimentActive};
  const active = !!status?.camera_running || !!status?.nanopositioner_running;
  async function run(action: string) {
    if (!ready || experimentActive || commandPending.current || !preferenceReady) return;
    commandPending.current = true; setPending(true);
    try {const result = await bridge.startupDiscoveryRun(action); setMessage(result.message); append(result.message); if (result.accepted) setStatus(await bridge.startupDiscoveryStatus());}
    catch (error) {setMessage(String(error));}
    finally {commandPending.current = false; setPending(false);}
  }
  async function save() {
    if (!ready || active || experimentActive || commandPending.current) return;
    commandPending.current = true; setPending(true);
    try {
      if (![port,baud,address].every(value => value.trim())) throw new Error('Serial settings must not be blank');
      const preference = validateStartupPreference({backend, endpoint, com_port: Number(port), baud: Number(baud), address: Number(address)});
      const result = await bridge.startupDiscoverySetPreference(preference);
      if (!result.accepted || !result.preference) throw new Error(result.message);
      setPreferenceReady(true); setRemembered({preference: result.preference}); setEndpoint(result.preference.endpoint);
      try {saveStartupPreference(localStorage, result.preference); setMessage('Startup preference saved and applied. No connection was made.');}
      catch (error) {setMessage(`Preference applied for this session but could not be saved: ${String(error)}`);}
    } catch (error) {setMessage(String(error));}
    finally {commandPending.current = false; setPending(false);}
  }
  useEffect(() => {
    if (!ready) {started.current = false; setPreferenceReady(false); return;}
    if (started.current) return;
    started.current = true;
    if (remembered.error) {setMessage(`Remembered preference is invalid; automatic selection skipped. ${remembered.error}`); return;}
    let alive = true;
    commandPending.current = true; setPending(true);
    void (async () => {
      try {
        const applied = await bridge.startupDiscoverySetPreference(remembered.preference);
        if (!alive || !lifecycle.current.ready) return;
        if (!applied.accepted) throw new Error(applied.message);
        setPreferenceReady(true);
        if (auto && !lifecycle.current.experimentActive) {const result = await bridge.startupDiscoveryRun('start'); setMessage(result.message); append(result.message);}
      } catch (error) {setMessage(String(error));}
      finally {commandPending.current = false; setPending(false);}
    })();
    return () => {alive = false;};
    // Preferences are sampled once per backend-ready session. Editing a draft
    // never retargets a running job; explicit Save installs the next preference.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready]);
  useEffect(() => {
    let alive = true, polling = false;
    const poll = async () => {if (!ready || polling) return; polling = true; try {const value = await bridge.startupDiscoveryStatus(); if (alive) {
      setStatus(value.valid ? value : null);
      const selection = `${value.camera_configured}/${value.nanopositioner_connected}`;
      if (previousSelection.current && selection !== previousSelection.current) selectionCallback.current?.();
      previousSelection.current = selection;
    }} catch {if (alive) setStatus(null);} finally {polling = false;}};
    void poll(); const timer = window.setInterval(() => void poll(), 500);
    return () => {alive = false; window.clearInterval(timer);};
  }, [ready]);
  const disabled = !ready || experimentActive || active || pending;
  return <fieldset><legend>Startup hardware selection</legend>
    <p>Uses the shared backend policy: select a unique camera, then identify and connect a unique nanopositioner. Ambiguous or incomplete discovery requires manual selection. No pump or pulse output is started.</p>
    <details><summary>Remembered nanopositioner selection</summary>
      <label>Preferred backend<select value={backend} disabled={pending} onChange={e => setBackend(e.target.value as StartupPreference['backend'])}><option value="auto">Automatic vendor identification</option><option value="oeabt">OEABT</option><option value="coremor">CoreMOR</option></select></label>
      <label>Preferred endpoint identity<input value={endpoint} disabled={pending} onChange={e => setEndpoint(e.target.value)} placeholder="Persistent identity or system path; blank = unique device" /></label>
      <label>Legacy COM number (-1 = none)<input value={port} disabled={pending} onChange={e => setPort(e.target.value)} /></label>
      <label>CoreMOR startup serial baud<input value={baud} disabled={pending} onChange={e => setBaud(e.target.value)} /></label>
      <label>CoreMOR startup device address<input value={address} disabled={pending} onChange={e => setAddress(e.target.value)} /></label>
      <button disabled={disabled} onClick={() => void save()}>Save startup preference</button>
      <p>Save before auto-selection. Editing fields alone never connects or changes the active scan.</p>
    </details>
    <label><input type="checkbox" checked={auto} onChange={e => {setAuto(e.target.checked); try {localStorage.setItem(preferenceKey, String(e.target.checked));} catch {setMessage('Startup preference could not be saved.');}}} />Auto-select hardware on next startup</label>
    <button disabled={disabled || !preferenceReady} onClick={() => void run('start')}>Auto-select hardware now</button>
    <button disabled={disabled || !preferenceReady} onClick={() => void run('camera')}>Retry camera selection</button>
    <button disabled={disabled || !preferenceReady} onClick={() => void run('nanopositioner')}>Retry nanopositioner selection</button>
    {message && <p role="status">{message}</p>}
    <p>{status ? `Camera: ${status.camera_running ? 'scanning' : status.camera_configured ? 'configured' : 'manual selection needed'} · Nanopositioner: ${status.nanopositioner_running ? 'scanning' : status.nanopositioner_connected ? 'connected' : 'manual selection needed'}` : 'Startup status unavailable'}</p>
    {status && [...status.camera.errors, ...status.nanopositioner.errors].map((error, index) => <p key={index}>{error}</p>)}
  </fieldset>;
}
