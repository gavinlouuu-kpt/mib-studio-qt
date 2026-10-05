import {useEffect, useRef, useState} from 'react';
import {bridge, type BackgroundCalibrationStatus} from '../bridge';
import {numericInput, HardwareCommandOwner} from './hardwareControlModel';
export function BackgroundCalibrationControls({ready, experimentActive, onPublished}: {ready: boolean; experimentActive: boolean; onPublished?: () => void}) {
  const [status, setStatus] = useState<BackgroundCalibrationStatus | null>(null), [error, setError] = useState('');
  const [required, setRequired] = useState('10'), [attempts, setAttempts] = useState('200'), [timeout, setTimeoutValue] = useState('5000');
  const [busy, setBusy] = useState(false);
  const owner = useRef(new HardwareCommandOwner()), generation = useRef('');
  const onPublishedRef = useRef(onPublished); onPublishedRef.current = onPublished;
  useEffect(() => {
    let alive = true, pending = false;
    setStatus(null);
    const poll = async () => {if (!ready || pending) return; pending = true; try {
      const next = await bridge.backgroundCalibrationStatus(); if (!alive) return;
      setStatus(next.valid ? next : null);
      if (next.state === 'succeeded' && generation.current !== next.operation_generation) {generation.current = next.operation_generation; onPublishedRef.current?.();}
    } catch {if (alive) setStatus(null);} finally {pending = false;}};
    void poll(); const timer = window.setInterval(() => void poll(), 500);
    return () => {alive = false; window.clearInterval(timer);};
  }, [ready]);
  async function run(cancel = false) {
    if (!ready || busy || (!cancel && experimentActive)) return;
    setBusy(true); setError('');
    try {
      const request = cancel ? {action: 'cancel'} : {action: 'start', required_accepted: numericInput(required, 'Accepted frames', 1, 100000, true), max_attempts: numericInput(attempts, 'Maximum attempts', 1, 1000000, true), timeout_ms: numericInput(timeout, 'Timeout', 1, 600000, true)};
      if (!cancel && request.max_attempts! < request.required_accepted!) throw new Error('Maximum attempts must be at least the required accepted frames.');
      await owner.current.run(() => bridge.backgroundCalibrationCommand(request)); setStatus(await bridge.backgroundCalibrationStatus());
    } catch (failure) {setError(String(failure));} finally {setBusy(false);}
  }
  const running = status?.state === 'running';
  return <fieldset><legend>Background calibration</legend>
    <p>With camera and realtime processing running, average empty frames using a frozen processing recipe. The current background stays active unless calibration succeeds.</p>
    <label>Accepted frames<input value={required} disabled={running || busy} onChange={e => setRequired(e.target.value)} /></label>
    <label>Maximum attempts<input value={attempts} disabled={running || busy} onChange={e => setAttempts(e.target.value)} /></label>
    <label>Timeout (ms)<input value={timeout} disabled={running || busy} onChange={e => setTimeoutValue(e.target.value)} /></label>
    <button disabled={!ready || !status || busy || running || experimentActive} onClick={() => void run()}>Calibrate background</button>
    <button disabled={!ready || busy || !running} onClick={() => void run(true)}>Cancel calibration</button>
    {error && <p role="alert">{error}</p>}
    <p>{status ? `${status.state}: ${status.accepted} accepted / ${status.attempted} examined · ${status.rejected_non_empty} non-empty · ${status.rejected_processing_failed} processing failures · ${status.message}` : 'Calibration status unavailable'}</p>
    {status?.state === 'succeeded' && <p>Published generation {status.published_background_generation} · SHA-256 {status.published_sha256}</p>}
  </fieldset>;
}
