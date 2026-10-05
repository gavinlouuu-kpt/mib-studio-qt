import {useEffect, useRef, useState} from 'react';
import {bridge, type CaptureLifecycle} from '../bridge';
export function CaptureRecovery({ready, blocked, onRetry, onConfigure}: {ready: boolean; blocked: boolean; onRetry: () => Promise<void>; onConfigure: () => void}) {
  const [status, setStatus] = useState<CaptureLifecycle | null>(null), [busy, setBusy] = useState(false), [error, setError] = useState('');
  const pending = useRef(false);
  useEffect(() => {
    let alive = true, polling = false;
    const poll = async () => {if (!ready || polling) return; polling = true; try {const value = await bridge.fetchCaptureLifecycle(); if (alive) {setStatus(value.valid ? value : null); setError('');}} catch (e) {if (alive) setError(String(e));} finally {polling = false;}};
    void poll(); const timer = window.setInterval(() => void poll(), 500);
    return () => {alive = false; window.clearInterval(timer);};
  }, [ready]);
  if (!status || status.failure === 'none' || status.camera_ready) return error ? <p role="alert">Camera lifecycle status unavailable: {error}</p> : null;
  const disabled = !ready || blocked || busy || !!error || !['idle','faulted'].includes(status.state);
  async function retry() {if (disabled || pending.current) return; pending.current = true; setBusy(true); try {await onRetry(); setStatus(await bridge.fetchCaptureLifecycle());} catch (e) {setError(String(e));} finally {pending.current = false; setBusy(false);}}
  return <fieldset><legend>Camera recovery</legend>
    <p role="alert">Capture {status.failure_generation}: {status.failure} — {status.message}</p>
    <p>Check the device, cabling and settings before retrying. Retry schedules a new session; only a confirmed camera-ready state means recovery succeeded.</p>
    <button disabled={disabled} onClick={() => void retry()}>Retry configured camera</button>
    <button disabled={disabled} onClick={onConfigure}>Review camera configuration</button>
    {blocked && <p>Finalize the experiment and recording before reconnecting.</p>}
    {error && <p role="alert">{error}</p>}
  </fieldset>;
}
