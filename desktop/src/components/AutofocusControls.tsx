import { metricNumber } from '../metricFormat';
import { useCallback, useEffect, useRef, useState } from 'react';
import { bridge, type AutofocusConfig, type AutofocusStatus, type CmdResult } from '../bridge';
import { DEFAULT_MODE } from '../commissioning';
import { HardwareCommandOwner, hardwareGate, operatorGate, validateFocusConfig } from './hardwareControlModel';
import { ConnectionFields, connectionArgs, initialConnection } from './HardwareConnectionFields';
import { EndpointDiscovery } from './EndpointDiscovery';
import './HardwareControls.css';

type Props = { ready: boolean; experimentActive: boolean; append: (message: string) => void };
const focusFields: Array<[keyof AutofocusConfig, string]> = [
  ['focus_setpoint', 'Focus setpoint'], ['focus_range', 'Focus range'], ['voltage_step', 'Voltage step (V)'],
  ['fine_voltage_step', 'Fine step (V)'], ['min_voltage', 'Minimum voltage (V)'], ['max_voltage', 'Maximum voltage (V)'],
  ['initial_voltage', 'Initial voltage (V)'], ['manual_voltage_step', 'Jog step (V)'],
  ['ring_ratio_stale_ms', 'Metric stale after (ms)'], ['min_samples_per_step', 'Minimum samples per step'], ['safe_shutdown_voltage', 'Shutdown voltage (V)'],
];

/** Nanopositioner / autofocus panel (NanopositionerTab in Qt): connection, enable, jog and tuning. Rendered only with
 *  `capabilities.autofocus`. Enable, Disable and Jog are operator controls as in Qt (D2): no Service mode, no arm, no experiment
 *  lock; Disable is available whenever the backend is ready (D3). Connection and the tuning editor stay locked during experiments. */
export function AutofocusControls({ready, experimentActive, append}: Props) {
  const [focusBackend, setFocusBackend] = useState('coremor');
  const [focusEndpoint, setFocusEndpoint] = useState('');
  const [focus, setFocus] = useState<AutofocusStatus | null>(null);
  const [config, setConfig] = useState<AutofocusConfig | null>(null);
  const [connection, setConnection] = useState(initialConnection());
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [statusError, setStatusError] = useState('');
  const owner = useRef(new HardwareCommandOwner());
  const polling = useRef(false);
  const generation = useRef(0);
  const refresh = useCallback(async () => {
    if (polling.current || !ready) return;
    polling.current = true;
    const current = generation.current;
    try {
      const autofocus = await bridge.fetchAutofocusStatus();
      if (current !== generation.current) return;
      if (!autofocus.valid) throw new Error('Hardware status is unavailable.');
      setFocus(autofocus); setStatusError('');
    } catch (failure) {
      if (current === generation.current) { setFocus(null); setStatusError(String(failure)); }
    } finally { polling.current = false; }
  }, [ready]);
  useEffect(() => {
    const current = ++generation.current;
    setFocus(null); setConfig(null);
    if (!ready) return;
    void refresh();
    void bridge.fetchAutofocusConfig().then(value => {
      if (current === generation.current) {
        if (value.valid) setConfig(value); else setError('Autofocus configuration unavailable.');
      }
    }).catch(failure => { if (current === generation.current) setError(String(failure)); });
    const timer = window.setInterval(() => void refresh(), 1000);
    return () => { ++generation.current; window.clearInterval(timer); };
  }, [ready, refresh]);
  const connected = !!focus?.connected;
  const configureReason = hardwareGate({ready, experimentActive, connected, mode: DEFAULT_MODE, armed: false}, 'configure');
  const configureDisabled = busy || !!configureReason;
  const commandReason = operatorGate({ready, connected}, 'command');
  async function run(label: string, reason: string, command: () => Promise<CmdResult>) {
    if (reason) { setError(reason); return; }
    if (busy) return;
    setBusy(true); setError('');
    try {
      const result = await owner.current.run(command);
      append(`${label}: ${result.message || 'Command accepted'}`);
      await refresh();
    } catch (failure) { const message = `${label}: ${String(failure)}`; setError(message); append(message); }
    finally { setBusy(false); }
  }
  /** Disable never waits for another command or for a connected device (#855, D3). */
  async function disable() {
    setError('');
    try {
      const result = await bridge.autofocusSetEnabled(false);
      if (!result.ok) throw new Error(result.message || 'Autofocus disable failed');
      append(`Disable autofocus: ${result.message || 'Command accepted'}`);
    } catch (failure) { const message = `Disable autofocus: ${String(failure)}`; setError(message); append(message); }
    await refresh();
  }
  return <>
    {error && <p role="alert">{error}</p>}{statusError && <p role="alert">Status unavailable: {statusError}</p>}
    <fieldset><legend>Autofocus / nanopositioner</legend>
      <p>Enable, Disable and Jog work as in the Qt app: no Service mode, available while an experiment runs. Disable is always available. Connection and tuning stay locked during an experiment.</p>
      <p>{focus ? `${focus.connected ? 'Connected' : 'Disconnected'} · ${focus.enabled ? 'Enabled' : 'Disabled'} · ${focus.current_voltage} V · ${focus.backend_name ?? ""} ${focus.endpoint_id ?? ""}` : 'Status unknown'}</p>
      {focus && <p>Ring ratio: {metricNumber(focus.average_ring_ratio, 3)} average / {metricNumber(focus.median_ring_ratio, 3)} median · {focus.last_ring_ratio_update_us === 0 ? 'No focus sample received' : `Sample age ${metricNumber(focus.ring_ratio_age_us / 1000, 0)} ms${config && focus.ring_ratio_age_us > config.ring_ratio_stale_ms * 1000 ? ' (stale)' : ''}`}</p>}
      <EndpointDiscovery disabled={configureDisabled || connected} request={() => ({kinds: [2], origin: 'tauri-nanopositioner-picker'})} onSelect={device => {
        const oeabt = device.claimed_by.some(vendor => /oeabt/i.test(vendor));
        setFocusBackend(oeabt ? 'oeabt' : 'coremor'); setFocusEndpoint(device.persistent_id || device.system_path);
        if (!oeabt) { const port = /(?:COM)([0-9]+)$/i.exec(device.system_path)?.[1]; if (port) setConnection({...connection, port, address: String(device.bus_address)}); }
      }} />
      <label>Nanopositioner backend<select value={focusBackend} disabled={configureDisabled || connected} onChange={e => setFocusBackend(e.target.value)}><option value="coremor">CoreMOR serial</option><option value="oeabt">OEABT USB</option></select></label>
      {focusBackend === 'oeabt' && <label>OEABT endpoint identity<input value={focusEndpoint} disabled={configureDisabled || connected} onChange={e => setFocusEndpoint(e.target.value)} placeholder="Exact discovered endpoint ID" /></label>}
      {focusBackend === 'coremor' && <ConnectionFields value={connection} disabled={configureDisabled || connected} onChange={setConnection} />}
      <div className="hardware-actions">
        <button disabled={configureDisabled || !focus || focus.connected} onClick={() => void run('Connect autofocus', configureReason, () => focusBackend === 'coremor' ? bridge.autofocusConnect(...connectionArgs(connection, 255, 0)) : bridge.autofocusConnectEndpoint(focusBackend, focusEndpoint.trim(), -1, 115200, 1))}>Connect autofocus</button>
        <button disabled={configureDisabled || !focus?.connected} onClick={() => void run('Disconnect autofocus', configureReason, bridge.autofocusDisconnect)}>Disconnect autofocus</button>
        <button data-tour="autofocus-enable" disabled={busy || !!commandReason || !!focus?.enabled} onClick={() => void run('Enable autofocus', commandReason, () => bridge.autofocusSetEnabled(true))}>Enable autofocus</button>
        <button data-tour="autofocus-disable" disabled={!ready} onClick={() => void disable()}>Disable autofocus</button>
        {[false, true].map(up => <button key={String(up)} data-tour={up ? 'autofocus-jog-up' : 'autofocus-jog-down'} disabled={busy || !!commandReason} onClick={() => void run('Jog voltage', commandReason, () => bridge.autofocusJog(up))}>Jog voltage {up ? '+' : '−'}</button>)}
      </div>
      {configureReason && <p role="status">{configureReason}</p>}
      {config && <details><summary>Autofocus configuration</summary><div className="hardware-fields">
        {focusFields.map(([key, label]) => <label key={key}>{label}<input type="number" step="any" disabled={configureDisabled || !focus || focus.enabled} value={Number.isNaN(Number(config[key])) ? '' : Number(config[key])} onChange={e => setConfig({...config, [key]: e.target.value.trim() === '' ? NaN : Number(e.target.value)})} /></label>)}
        {(['require_new_sample_per_step', 'focus_direction'] as const).map(key => <label key={key}><input type="checkbox" disabled={configureDisabled || !focus || focus.enabled} checked={config[key]} onChange={e => setConfig({...config, [key]: e.target.checked})} />{key === 'focus_direction' ? 'Positive focus direction' : 'Require new sample per step'}</label>)}
      </div><button disabled={configureDisabled || !focus || focus.enabled} onClick={() => void run('Apply autofocus configuration', configureReason, () => { validateFocusConfig(config); return bridge.autofocusSetConfig(config); })}>Apply autofocus configuration</button><p>Applies to the running backend; persistence across restarts is not provided by this command.</p></details>}
    </fieldset>
  </>;
}
