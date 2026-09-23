import {useEffect, useRef, useState} from 'react';
import {bridge, type StartupDiscoveryStatus} from '../bridge';
const preferenceKey = 'mib.startup.auto-select-hardware';
export function StartupDiscoveryControls({ready, experimentActive, append, onSelectionChanged}: {ready: boolean; experimentActive: boolean; append: (message: string) => void; onSelectionChanged?: () => void}) {
  const [auto, setAuto] = useState(() => {try {return localStorage.getItem(preferenceKey) === 'true';} catch {return false;}});
  const [status, setStatus] = useState<StartupDiscoveryStatus | null>(null), [message, setMessage] = useState('');
  const [pending, setPending] = useState(false);
  const commandPending = useRef(false), started = useRef(false);
  const previousSelection = useRef('');
  const selectionCallback = useRef(onSelectionChanged); selectionCallback.current = onSelectionChanged;
  const active = !!status?.camera_running || !!status?.nanopositioner_running;
  async function run(action: string) {
    if (!ready || experimentActive || commandPending.current) return;
    commandPending.current = true; setPending(true);
    try {const result = await bridge.startupDiscoveryRun(action); setMessage(result.message); append(result.message); if (!result.accepted) return; setStatus(await bridge.startupDiscoveryStatus());}
    catch (error) {setMessage(String(error));}
    finally {commandPending.current = false; setPending(false);}
  }
  useEffect(() => {
    if (!ready) {started.current = false; return;}
    if (started.current) return;
    started.current = true;
    if (auto && !experimentActive) void run('start');
    // Startup preference is sampled once per backend-ready session, not toggling.
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
  return <fieldset><legend>Startup hardware selection</legend>
    <p>Uses the shared backend policy: select a unique camera, then identify and connect a unique nanopositioner. Ambiguous or incomplete discovery requires manual selection. No pump or pulse output is started.</p>
    <label><input type="checkbox" checked={auto} onChange={e => {setAuto(e.target.checked); try {localStorage.setItem(preferenceKey, String(e.target.checked));} catch {setMessage('Startup preference could not be saved.');}}} />Auto-select hardware on next startup</label>
    <button disabled={!ready || experimentActive || active || pending} onClick={() => void run('start')}>Auto-select hardware now</button>
    <button disabled={!ready || experimentActive || active || pending} onClick={() => void run('camera')}>Retry camera selection</button>
    <button disabled={!ready || experimentActive || active || pending} onClick={() => void run('nanopositioner')}>Retry nanopositioner selection</button>
    {message && <p role="status">{message}</p>}
    <p>{status ? `Camera: ${status.camera_running ? 'scanning' : status.camera_configured ? 'configured' : 'manual selection needed'} · Nanopositioner: ${status.nanopositioner_running ? 'scanning' : status.nanopositioner_connected ? 'connected' : 'manual selection needed'}` : 'Startup status unavailable'}</p>
    {status && [...status.camera.errors, ...status.nanopositioner.errors].map((error, index) => <p key={index}>{error}</p>)}
  </fieldset>;
}
