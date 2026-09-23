import { useCallback, useEffect, useRef, useState } from 'react';
import { bridge, type AutofocusConfig, type AutofocusStatus, type PumpStatus, type CmdResult } from '../bridge';
import { DEFAULT_MODE, type OperatingMode } from '../commissioning';
import { HardwareCommandOwner, hardwareGate, numericInput, validateFocusConfig } from './hardwareControlModel';
import './HardwareControls.css';
import {EndpointDiscovery} from './EndpointDiscovery';
import {PulseGeneratorControls} from './PulseGeneratorControls';

type Props = { ready: boolean; experimentActive: boolean; append: (message: string) => void; mode?: OperatingMode; armed?: boolean; onDisarm: () => void };
type Connection = { port: string; baud: string; address: string };
const initialConnection = (): Connection => ({port: '', baud: '115200', address: '1'});
const focusFields: Array<[keyof AutofocusConfig, string]> = [
  ['focus_setpoint', 'Focus setpoint'], ['focus_range', 'Focus range'], ['voltage_step', 'Voltage step (V)'],
  ['fine_voltage_step', 'Fine step (V)'], ['min_voltage', 'Minimum voltage (V)'], ['max_voltage', 'Maximum voltage (V)'],
  ['initial_voltage', 'Initial voltage (V)'], ['manual_voltage_step', 'Jog step (V)'],
  ['ring_ratio_stale_ms', 'Metric stale after (ms)'], ['min_samples_per_step', 'Minimum samples per step'], ['safe_shutdown_voltage', 'Shutdown voltage (V)'],
];
function ConnectionFields({value, onChange, disabled}: {value: Connection; onChange: (v: Connection) => void; disabled: boolean}) {
  return <div className="hardware-fields">{(['port', 'baud', 'address'] as const).map(key => <label key={key}>{ {port: 'COM port number', baud: 'Baud rate', address: 'Device address'}[key] }
    <input type="number" value={value[key]} disabled={disabled} onChange={event => onChange({...value, [key]: event.target.value})} />
  </label>)}</div>;
}
function connectionArgs(value: Connection, addressMax: number, addressMin = 1): [number, number, number] {
  return [numericInput(value.port, 'COM port', 1, 65535, true), numericInput(value.baud, 'Baud rate', 1, 4000000, true), numericInput(value.address, 'Address', addressMin, addressMax, true)];
}

export function HardwareControls({ready, experimentActive, append, mode = DEFAULT_MODE, armed = false, onDisarm}: Props) {
  const [pumps, setPumps] = useState<Array<PumpStatus | null>>([null, null]);
  const [focusBackend, setFocusBackend] = useState('coremor');
  const [focusEndpoint, setFocusEndpoint] = useState('');
  const [focus, setFocus] = useState<AutofocusStatus | null>(null);
  const [config, setConfig] = useState<AutofocusConfig | null>(null);
  const [connections, setConnections] = useState([initialConnection(), initialConnection(), initialConnection()]);
  const [rates, setRates] = useState(['', '']);
  const [units, setUnits] = useState([100, 100]);
  const [directions, setDirections] = useState([0, 0]);
  const [volumes, setVolumes] = useState(['', '']);
  const [volumeUnits, setVolumeUnits] = useState([100, 100]);
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
      const [sample, sheath, autofocus] = await Promise.all([bridge.fetchPumpStatus(0), bridge.fetchPumpStatus(1), bridge.fetchAutofocusStatus()]);
      if (current !== generation.current) return;
      if (!sample.valid || !sheath.valid || !autofocus.valid) throw new Error('Hardware status is unavailable.');
      setPumps([sample, sheath]); setFocus(autofocus); setStatusError('');
    } catch (failure) {
      if (current === generation.current) { setPumps([null, null]); setFocus(null); setStatusError(String(failure)); }
    } finally { polling.current = false; }
  }, [ready]);
  useEffect(() => {
    const current = ++generation.current;
    setPumps([null, null]); setFocus(null); setConfig(null);
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
  const gate = (kind: 'configure' | 'actuate' | 'stop', connected = false) => hardwareGate({ready, experimentActive, connected, mode, armed}, kind);
  const configureDisabled = busy || !!gate('configure');
  async function run(label: string, kind: 'configure' | 'actuate' | 'stop', connected: boolean, command: () => Promise<CmdResult>) {
    const reason = gate(kind, connected);
    if (reason) { setError(reason); return; }
    if (busy) return;
    setBusy(true); setError('');
    try {
      const result = await owner.current.run(() => { if (kind === 'actuate') onDisarm(); return command(); });
      append(`${label}: ${result.message || 'Command accepted'}`);
      await refresh();
    } catch (failure) { const message = `${label}: ${String(failure)}`; setError(message); append(message); }
    finally { setBusy(false); }
  }
  const update = <T,>(setter: (value: T[]) => void, values: T[], index: number, value: T) => setter(values.map((old, i) => i === index ? value : old));
  const unitSelect = (value: number, onChange: (value: number) => void, rate: boolean) => <select value={value} disabled={configureDisabled} onChange={e => onChange(Number(e.target.value))}><option value={100}>{rate ? 'µL/min' : 'µL'}</option><option value={103}>{rate ? 'mL/min' : 'mL'}</option></select>;
  return <section className="hardware-controls" aria-label="Pump and autofocus controls">
    <h2>Pumps and autofocus</h2>
    <p>Manual run, purge, enable and jog require Service / Commissioning mode and arming. Stop and disable remain available during experiments.</p>
    <p>Pump connections use numeric COM ports. Nanopositioners support CoreMOR and OEABT endpoint identities.</p>
    {gate('configure') && <p role="status">{gate('configure')}</p>}
    {error && <p role="alert">{error}</p>}{statusError && <p role="alert">Status unavailable: {statusError}</p>}
    {pumps.map((status, id) => {
      const connected = !!status?.connected;
      const actuateReason = gate('actuate', connected);
      const stopped = status?.run_status === 0;
      return <fieldset key={id}><legend>{id === 0 ? 'Sample pump' : 'Sheath pump'}</legend>
        <p>{status ? `${connected ? 'Connected' : 'Disconnected'} · ${['Stopped', 'Forward', 'Backward', 'Paused'][status.run_status] ?? `Unknown state ${status.run_status}`} · ${status.stalled ? 'STALLED' : 'No stall reported'}` : 'Status unknown'}</p>
        {connected && status && <p>COM {status.com_port} · Address {status.modbus_address} · Configured rate {status.configured_flow_rate} ({status.flow_rate_unit === 100 ? 'µL/min' : status.flow_rate_unit === 103 ? 'mL/min' : `unit ${status.flow_rate_unit}`}) · Direction {status.direction === 0 ? 'Infuse' : status.direction === 1 ? 'Withdraw' : 'Unknown'} · Live rate {status.current_flow_rate} · Accumulated volume {status.accumulated_volume} (device units)</p>}
        <ConnectionFields value={connections[id]} disabled={configureDisabled || connected} onChange={v => update(setConnections, connections, id, v)} />
        <div className="hardware-actions">
          <button disabled={configureDisabled || !status || connected} onClick={() => void run('Connect pump', 'configure', false, () => bridge.pumpConnect(id, ...connectionArgs(connections[id], 247)))}>Connect</button>
          <button disabled={configureDisabled || !connected} onClick={() => void run('Disconnect pump', 'configure', connected, () => bridge.pumpDisconnect(id))}>Disconnect</button>
          <button disabled={busy || !ready || !connected} onClick={() => void run('Poll pump', 'stop', connected, () => bridge.pumpPollStatus(id))}>Read device status</button>
        </div>
        <div className="hardware-fields">
          <label>Flow rate<input type="number" min="0" value={rates[id]} disabled={configureDisabled || !connected || !stopped} onChange={e => update(setRates, rates, id, e.target.value)} /></label>
          <label>Rate unit{unitSelect(units[id], value => update(setUnits, units, id, value), true)}</label>
          <button disabled={configureDisabled || !connected || !stopped} onClick={() => void run('Set flow rate', 'configure', connected, () => bridge.pumpSetFlowRate(id, numericInput(rates[id], 'Flow rate', 0), units[id]))}>Apply rate</button>
          <label>Direction<select value={directions[id]} disabled={configureDisabled || !connected || !stopped} onChange={e => update(setDirections, directions, id, Number(e.target.value))}><option value={0}>Infuse</option><option value={1}>Withdraw</option></select></label>
          <button disabled={configureDisabled || !connected || !stopped} onClick={() => void run('Set direction', 'configure', connected, () => bridge.pumpSetDirection(id, directions[id]))}>Apply direction</button>
          <label>Syringe volume (1–9999)<input type="number" min="1" max="9999" step="1" value={volumes[id]} disabled={configureDisabled || !connected || !stopped} onChange={e => update(setVolumes, volumes, id, e.target.value)} /></label>
          <label>Volume unit{unitSelect(volumeUnits[id], value => update(setVolumeUnits, volumeUnits, id, value), false)}</label>
          <button disabled={configureDisabled || !connected || !stopped} onClick={() => void run('Set syringe volume', 'configure', connected, () => bridge.pumpSetSyringeVolume(id, numericInput(volumes[id], 'Syringe volume', 1, 9999, true), volumeUnits[id]))}>Apply volume</button>
        </div>
        {actuateReason && <p>{actuateReason}</p>}
        <div className="hardware-actions">
          <button disabled={busy || !!actuateReason || !stopped} onClick={() => void run('Start pump', 'actuate', connected, () => bridge.pumpStart(id))}>Run configured pump</button>
          <button disabled={busy || !!actuateReason || !stopped} onClick={() => void run('Purge pump', 'actuate', connected, () => bridge.pumpPurge(id, directions[id]))}>Purge {directions[id] === 0 ? 'infuse' : 'withdraw'}</button>
          <button disabled={busy || !ready} onClick={() => void run('Stop pump', 'stop', connected, () => bridge.pumpStop(id))}>Stop pump</button>
          <button disabled={busy || !ready} onClick={() => void run('Stop purge', 'stop', connected, () => bridge.pumpStopPurge(id))}>Stop purge</button>
        </div>
      </fieldset>;
    })}
    <fieldset><legend>Autofocus / nanopositioner</legend>
      <p>{focus ? `${focus.connected ? 'Connected' : 'Disconnected'} · ${focus.enabled ? 'Enabled' : 'Disabled'} · ${focus.current_voltage} V · ${focus.backend_name ?? ""} ${focus.endpoint_id ?? ""}` : 'Status unknown'}</p>
      {focus && <p>Ring ratio: {focus.average_ring_ratio} average / {focus.median_ring_ratio} median · {focus.last_ring_ratio_update_us === 0 ? 'No focus sample received' : `Sample age ${(focus.ring_ratio_age_us / 1000).toFixed(0)} ms${config && focus.ring_ratio_age_us > config.ring_ratio_stale_ms * 1000 ? ' (stale)' : ''}`}</p>}
      <EndpointDiscovery disabled={configureDisabled || !!focus?.connected} request={() => ({kinds: [2], origin: 'tauri-nanopositioner-picker'})} onSelect={device => {
        const oeabt = device.claimed_by.some(vendor => /oeabt/i.test(vendor));
        setFocusBackend(oeabt ? 'oeabt' : 'coremor'); setFocusEndpoint(device.persistent_id || device.system_path);
        if (!oeabt) { const port = /(?:COM)([0-9]+)$/i.exec(device.system_path)?.[1]; if (port) update(setConnections, connections, 2, {...connections[2], port, address: String(device.bus_address)}); }
      }} />
      <label>Nanopositioner backend<select value={focusBackend} disabled={configureDisabled || !!focus?.connected} onChange={e => setFocusBackend(e.target.value)}><option value="coremor">CoreMOR serial</option><option value="oeabt">OEABT USB</option></select></label>
      {focusBackend === 'oeabt' && <label>OEABT endpoint identity<input value={focusEndpoint} disabled={configureDisabled || !!focus?.connected} onChange={e => setFocusEndpoint(e.target.value)} placeholder="Exact discovered endpoint ID" /></label>}
      {focusBackend === 'coremor' && <ConnectionFields value={connections[2]} disabled={configureDisabled || !!focus?.connected} onChange={v => update(setConnections, connections, 2, v)} />}
      <div className="hardware-actions">
        <button disabled={configureDisabled || !focus || focus.connected} onClick={() => void run('Connect autofocus', 'configure', false, () => focusBackend === 'coremor' ? bridge.autofocusConnect(...connectionArgs(connections[2], 255, 0)) : bridge.autofocusConnectEndpoint(focusBackend, focusEndpoint.trim(), -1, 115200, 1))}>Connect autofocus</button>
        <button disabled={configureDisabled || !focus?.connected} onClick={() => void run('Disconnect autofocus', 'configure', true, bridge.autofocusDisconnect)}>Disconnect autofocus</button>
        <button disabled={busy || !!gate('actuate', !!focus?.connected) || !!focus?.enabled} onClick={() => void run('Enable autofocus', 'actuate', !!focus?.connected, () => bridge.autofocusSetEnabled(true))}>Enable autofocus</button>
        <button disabled={busy || !ready} onClick={() => void run('Disable autofocus', 'stop', !!focus?.connected, () => bridge.autofocusSetEnabled(false))}>Disable autofocus</button>
        {[false, true].map(up => <button key={String(up)} disabled={busy || !!gate('actuate', !!focus?.connected) || !!focus?.enabled} onClick={() => void run('Jog voltage', 'actuate', !!focus?.connected, () => bridge.autofocusJog(up))}>Jog voltage {up ? '+' : '−'}</button>)}
      </div>
      {gate('actuate', !!focus?.connected) && <p>{gate('actuate', !!focus?.connected)}</p>}
      {config && <details><summary>Autofocus configuration</summary><div className="hardware-fields">
        {focusFields.map(([key, label]) => <label key={key}>{label}<input type="number" step="any" disabled={configureDisabled || !focus || focus.enabled} value={Number.isNaN(Number(config[key])) ? '' : Number(config[key])} onChange={e => setConfig({...config, [key]: e.target.value.trim() === '' ? NaN : Number(e.target.value)})} /></label>)}
        {(['require_new_sample_per_step', 'focus_direction'] as const).map(key => <label key={key}><input type="checkbox" disabled={configureDisabled || !focus || focus.enabled} checked={config[key]} onChange={e => setConfig({...config, [key]: e.target.checked})} />{key === 'focus_direction' ? 'Positive focus direction' : 'Require new sample per step'}</label>)}
      </div><button disabled={configureDisabled || !focus || focus.enabled} onClick={() => void run('Apply autofocus configuration', 'configure', !!focus?.connected, () => { validateFocusConfig(config); return bridge.autofocusSetConfig(config); })}>Apply autofocus configuration</button><p>Applies to the running backend; persistence across restarts is not provided by this command.</p></details>}
    </fieldset>
    <PulseGeneratorControls ready={ready} experimentActive={experimentActive} mode={mode} armed={armed} onDisarm={onDisarm} append={append} />
  </section>;
}
