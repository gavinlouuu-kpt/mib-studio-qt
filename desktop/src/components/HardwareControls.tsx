import { metricNumber } from "../metricFormat";
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { bridge, type AutofocusConfig, type CmdResult, type PlatformCapabilities } from '../bridge';
import { usePumpStatus } from './usePumpStatus';
import { DESKTOP_CAPABILITIES } from '../platformCapabilities';
import { DEFAULT_MODE, type OperatingMode } from '../commissioning';
import { HardwareCommandOwner, hardwareGate, numericInput, validateFocusConfig } from './hardwareControlModel';
import { PUMP_MODELS } from '../bridgeContract';
import './HardwareControls.css';
import {StartupDiscoveryControls} from './StartupDiscoveryControls';
import {MICROLITERS_PER_MIN_UNIT, RPM_UNIT, directionLabel, flowToRpm, rpmToMicrolitersPerMin} from '../pumpRate';
import {EndpointDiscovery} from './EndpointDiscovery';
import {PulseGeneratorControls, type PulseGeneratorLayout} from './PulseGeneratorControls';

/** One pump slot's parts (#798): the Hardware setup page places them in a card; the default layout
 *  keeps today's fieldset. Every field and button of the slot is in exactly one part. */
export interface PumpParts {
  id: number;
  title: string;
  connected: boolean;
  peristaltic: boolean;
  /** The status line(s): connection, run state, stall; the connected device's facts. */
  status: ReactNode;
  /** Pump model (and the peristaltic calibration), then the serial endpoint fields. */
  setup: ReactNode;
  /** Connect / Disconnect / Read device status. */
  connection: ReactNode;
  /** Flow rate + unit, direction and (syringe) volume + unit, each with its Apply. */
  rate: ReactNode;
  /** Why Run and Purge are unavailable ("" when they are): the actuate gate's reason. */
  actuateReason: string;
  /** The gated actions; Stop pump and Stop purge are always available while the backend is ready. */
  actions: { run: ReactNode; purge: ReactNode; stopPump: ReactNode; stopPurge: ReactNode };
}

export interface AutofocusParts {
  connected: boolean;
  status: ReactNode;
  /** Endpoint discovery, the backend select, the endpoint or serial fields. */
  setup: ReactNode;
  /** Connect / Disconnect autofocus, Read status (a read of the shared poll). */
  connection: ReactNode;
  actuateReason: string;
  /** Enable (gated), Disable (always), Jog − / Jog + (gated). */
  actions: { enable: ReactNode; disable: ReactNode; jogDown: ReactNode; jogUp: ReactNode };
  /** The autofocus configuration disclosure, as today. */
  config: ReactNode;
}

export interface HardwareParts {
  pumps: [PumpParts, PumpParts];
  /** null without the autofocus capability (the PZ7035). */
  autofocus: AutofocusParts | null;
  /** The startup hardware selection (StartupDiscoveryControls); null without autofocus. */
  startup: ReactNode | null;
  /** The pulse generator, with an optional layout of its own; null without the trigger capability. */
  pulseGenerator: ((layout?: PulseGeneratorLayout) => ReactNode) | null;
  /** The panel's notices: why configuration is locked ("" when it is not), the last command's
   *  error, and the status poll's failure. */
  notices: { configureReason: string; error: string; statusError: string };
  /** The instrument's pumps share one port (PZ7035): the sentence that says so; "" on the desktop. */
  instrumentPumpText: string;
}

type Props = { ready: boolean; experimentActive: boolean; append: (message: string) => void; mode?: OperatingMode; armed?: boolean; onDisarm: () => void; onSelectionChanged?: () => void; capabilities?: PlatformCapabilities;
  /** Place the parts yourself (the Hardware setup page, #798). One instance still owns the poll,
   *  the busy state and the command owner; the parts are only placed, never duplicated. */
  layout?: (parts: HardwareParts) => ReactNode };
type Connection = { port: string; baud: string; address: string };
const initialConnection = (): Connection => ({port: '', baud: '115200', address: '1'});
// 0.4 rpm delivers 10 µL/min on the instrument tubing until a measured calibration replaces it.
const DEFAULT_MICROLITERS_PER_REV = '25';
// Where the PZ7035 instrument's peristaltic pump answers (PS UART1 RS485).
const PERISTALTIC_DEFAULT_ENDPOINT = {port: '/dev/ttyPS1', address: '3'};
const focusFields: Array<[keyof AutofocusConfig, string]> = [
  ['focus_setpoint', 'Focus setpoint'], ['focus_range', 'Focus range'], ['voltage_step', 'Voltage step (V)'],
  ['fine_voltage_step', 'Fine step (V)'], ['min_voltage', 'Minimum voltage (V)'], ['max_voltage', 'Maximum voltage (V)'],
  ['initial_voltage', 'Initial voltage (V)'], ['manual_voltage_step', 'Jog step (V)'],
  ['ring_ratio_stale_ms', 'Metric stale after (ms)'], ['min_samples_per_step', 'Minimum samples per step'], ['safe_shutdown_voltage', 'Shutdown voltage (V)'],
];
function ConnectionFields({value, onChange, disabled, systemPort = false}: {value: Connection; onChange: (v: Connection) => void; disabled: boolean; systemPort?: boolean}) {
  return <div className="hardware-fields">{(['port', 'baud', 'address'] as const).map(key => <label key={key}>{ {port: systemPort ? 'System serial port (COM3 or /dev/ttyUSB0)' : 'COM port number', baud: 'Baud rate', address: 'Device address'}[key] }
    <input type={systemPort && key === 'port' ? 'text' : 'number'} value={value[key]} disabled={disabled} onChange={event => onChange({...value, [key]: event.target.value})} />
  </label>)}</div>;
}
function connectionArgs(value: Connection, addressMax: number, addressMin = 1): [number, number, number] {
  return [numericInput(value.port, 'COM port', 1, 65535, true), numericInput(value.baud, 'Baud rate', 1, 4000000, true), numericInput(value.address, 'Address', addressMin, addressMax, true)];
}

/** Today's panel: the parts in their fieldsets, unchanged in words and order. */
function defaultLayout(p: HardwareParts, autofocus: boolean): ReactNode {
  return <section className="hardware-controls" aria-label={autofocus ? 'Pump and autofocus controls' : 'Pump controls'}>
    <h2>{autofocus ? 'Pumps and autofocus' : 'Pumps'}</h2>
    {p.startup}
    <p>Manual run and purge{autofocus ? ', enable and jog' : ''} require Service / Commissioning mode and arming. Stop{autofocus ? ' and disable' : ''} remain{autofocus ? '' : 's'} available during experiments.</p>
    {autofocus
      ? <p>Pumps accept system serial endpoints; devices may share a bus at distinct Modbus addresses. Nanopositioners support CoreMOR and OEABT identities.</p>
      : p.instrumentPumpText && <p>{p.instrumentPumpText}</p>}
    {p.notices.configureReason && <p role="status">{p.notices.configureReason}</p>}
    {p.notices.error && <p role="alert">{p.notices.error}</p>}{p.notices.statusError && <p role="alert">Status unavailable: {p.notices.statusError}</p>}
    {p.pumps.map(pump => <fieldset key={pump.id}><legend>{pump.title}</legend>
      {pump.status}
      {pump.setup}
      <div className="hardware-actions">{pump.connection}</div>
      {pump.rate}
      {pump.actuateReason && <p>{pump.actuateReason}</p>}
      <div className="hardware-actions">{pump.actions.run}{pump.actions.purge}{pump.actions.stopPump}{pump.actions.stopPurge}</div>
    </fieldset>)}
    {p.autofocus && <fieldset><legend>Autofocus / nanopositioner</legend>
      {p.autofocus.status}
      {p.autofocus.setup}
      <div className="hardware-actions">{p.autofocus.connection}{p.autofocus.actions.enable}{p.autofocus.actions.disable}{p.autofocus.actions.jogDown}{p.autofocus.actions.jogUp}</div>
      {p.autofocus.actuateReason && <p>{p.autofocus.actuateReason}</p>}
      {p.autofocus.config}
    </fieldset>}
    {p.pulseGenerator?.()}
  </section>;
}

export function HardwareControls({ready, experimentActive, append, mode = DEFAULT_MODE, armed = false, onDisarm, onSelectionChanged, capabilities = DESKTOP_CAPABILITIES, layout}: Props) {
  // #501: on the PZ7035 the pump is the instrument's peristaltic pump and there is no
  // nanopositioner or host pulse generator.
  const instrumentPump = capabilities.pump;
  const slotAddress = (slot: number) => instrumentPump ? (slot === 0 ? instrumentPump.sample_address : instrumentPump.sheath_address) : 1;
  const pumpConnection = (slot: number): Connection => instrumentPump ? {port: instrumentPump.port, baud: '115200', address: String(slotAddress(slot))} : initialConnection();
  const pumpModel = instrumentPump ? PUMP_MODELS.TushuiPeristaltic : PUMP_MODELS.DlspSyringe;
  const pumpCalibration = instrumentPump ? String(instrumentPump.microliters_per_rev) : DEFAULT_MICROLITERS_PER_REV;
  // The pump and autofocus status come from the one shared poll (usePumpStatus, #797): this panel
  // and the telemetry sidebar read the same snapshot, whichever of them is mounted.
  const hw = usePumpStatus(ready);
  // A slot (or, with autofocus, the controller) the backend did not report makes the whole status
  // unknown here: no configure or actuate button acts on a half-read state.
  const unavailable = !hw.error && hw.read && (!hw.pumps[0] || !hw.pumps[1] || (capabilities.autofocus && !hw.focus));
  const pumps = unavailable ? [null, null] : hw.pumps;
  const focus = unavailable ? null : hw.focus;
  const statusError = hw.error || (unavailable ? 'Error: Hardware status is unavailable.' : '');
  const refresh = hw.refresh;
  const [focusBackend, setFocusBackend] = useState('coremor');
  const [focusEndpoint, setFocusEndpoint] = useState('');
  const [config, setConfig] = useState<AutofocusConfig | null>(null);
  const [connections, setConnections] = useState([pumpConnection(0), pumpConnection(1), initialConnection()]);
  const [rates, setRates] = useState(['', '']);
  const [units, setUnits] = useState([100, 100]);
  const [directions, setDirections] = useState([0, 0]);
  const [volumes, setVolumes] = useState(['', '']);
  const [volumeUnits, setVolumeUnits] = useState([100, 100]);
  const [models, setModels] = useState<number[]>([pumpModel, pumpModel]);
  const [calibrations, setCalibrations] = useState([pumpCalibration, pumpCalibration]);
  // The capability report arrives after mount: adopt the instrument's pump once.
  useEffect(() => {
    if (!instrumentPump) return;
    setModels(old => old.map(() => PUMP_MODELS.TushuiPeristaltic));
    setCalibrations(old => old.map(() => String(instrumentPump.microliters_per_rev)));
    setConnections(old => old.map((c, i) => i < 2 ? pumpConnection(i) : c));
  }, [instrumentPump?.port, instrumentPump?.sample_address, instrumentPump?.sheath_address, instrumentPump?.microliters_per_rev]); // eslint-disable-line react-hooks/exhaustive-deps
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const owner = useRef(new HardwareCommandOwner());
  const generation = useRef(0);
  useEffect(() => {
    const current = ++generation.current;
    setConfig(null);
    if (!ready) return;
    void bridge.fetchAutofocusConfig().then(value => {
      if (current === generation.current) {
        if (value.valid) setConfig(value); else setError('Autofocus configuration unavailable.');
      }
    }).catch(failure => { if (current === generation.current) setError(String(failure)); });
    return () => { ++generation.current; };
  }, [ready]);
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
  const unitSelect = (value: number, onChange: (value: number) => void, rate: boolean, rpm = false) => <select value={value} disabled={configureDisabled} onChange={e => onChange(Number(e.target.value))}>{rpm && <option value={RPM_UNIT}>rpm (head)</option>}<option value={100}>{rate ? 'µL/min' : 'µL'}</option><option value={103}>{rate ? 'mL/min' : 'mL'}</option></select>;
  const autofocus = capabilities.autofocus;

  const pumpParts = pumps.map((status, id): PumpParts => {
    const connected = !!status?.connected;
    const actuateReason = gate('actuate', connected);
    const stopped = status?.run_status === 0;
    const model = connected && status?.model !== undefined ? status.model : models[id];
    const peristaltic = model === PUMP_MODELS.TushuiPeristaltic;
    const selectModel = (value: number) => {
      update(setModels, models, id, value);
      const current = connections[id];
      if (value === PUMP_MODELS.TushuiPeristaltic && !current.port.trim() && current.address === '1')
        update(setConnections, connections, id, {...current, ...PERISTALTIC_DEFAULT_ENDPOINT});
    };
    return {
      id, title: id === 0 ? 'Sample pump' : 'Sheath pump', connected, peristaltic, actuateReason,
      status: <>
        <p>{status ? `${connected ? 'Connected' : 'Disconnected'} · ${['Stopped', 'Forward', 'Backward', 'Paused'][status.run_status] ?? `Unknown state ${status.run_status}`} · ${status.stalled ? 'STALLED' : 'No stall reported'}` : 'Status unknown'}</p>
        {connected && status && <p>{peristaltic ? 'Peristaltic' : 'Syringe'} · {status.port_name || `COM ${status.com_port}`} · Address {status.modbus_address} · Configured rate {status.configured_flow_rate} ({status.flow_rate_unit === 100 ? 'µL/min' : status.flow_rate_unit === 103 ? 'mL/min' : `unit ${status.flow_rate_unit}`}) · Direction {status.direction === 0 ? 'Infuse' : status.direction === 1 ? 'Withdraw' : 'Unknown'} · Live rate {status.current_flow_rate}{peristaltic ? ' µL/min' : ''} · {peristaltic
          ? `Head ${(status.speed_rpm ?? 0).toFixed(2)} rpm at ${status.microliters_per_rev ?? '?'} µL/rev · Delivered ≈ ${status.accumulated_volume.toFixed(1)} µL (estimated from speed)`
          : `Accumulated volume ${status.accumulated_volume} (device units)`}</p>}
      </>,
      setup: <>
        <div className="hardware-fields">
          <label>Pump model<select value={model} disabled={configureDisabled || connected} onChange={e => selectModel(Number(e.target.value))}>
            <option value={PUMP_MODELS.DlspSyringe}>Syringe (Longer dLSP)</option>
            <option value={PUMP_MODELS.TushuiPeristaltic}>Peristaltic (Tushui)</option>
          </select></label>
          {peristaltic && <label>Calibration (µL per revolution)<input type="number" min="0" step="any" value={calibrations[id]} disabled={configureDisabled || connected} onChange={e => update(setCalibrations, calibrations, id, e.target.value)} /></label>}
        </div>
        <ConnectionFields systemPort value={connections[id]} disabled={configureDisabled || connected} onChange={v => update(setConnections, connections, id, v)} />
      </>,
      connection: <>
        <button disabled={configureDisabled || !status || connected} onClick={() => void run('Connect pump', 'configure', false, () => {
          const connection = connections[id]; const port = connection.port.trim(); if (!port) throw new Error('System serial port is required');
          const microlitersPerRev = peristaltic ? numericInput(calibrations[id], 'Calibration', 0.001, 100000) : Number(DEFAULT_MICROLITERS_PER_REV);
          return bridge.pumpConnectModel(id, model, /^\d+$/.test(port) ? `COM${port}` : port, numericInput(connection.baud, 'Baud rate', 1, 4000000, true), numericInput(connection.address, 'Address', 1, 247, true), microlitersPerRev);
        })}>Connect</button>
        <button disabled={configureDisabled || !connected} onClick={() => void run('Disconnect pump', 'configure', connected, () => bridge.pumpDisconnect(id))}>Disconnect</button>
        <button disabled={busy || !ready || !connected} onClick={() => void run('Poll pump', 'stop', connected, () => bridge.pumpPollStatus(id))}>Read device status</button>
      </>,
      rate: <div className="hardware-fields">
        <label>Flow rate<input type="number" min="0" value={rates[id]} disabled={configureDisabled || !connected || !stopped} onChange={e => update(setRates, rates, id, e.target.value)} /></label>
        <label>Rate unit{unitSelect(units[id], value => update(setUnits, units, id, value), true, peristaltic)}</label>
        <button disabled={configureDisabled || !connected || !stopped} onClick={() => void run('Set flow rate', 'configure', connected, () => {
          const value = numericInput(rates[id], units[id] === RPM_UNIT ? 'Head speed' : 'Flow rate', 0);
          // The backend takes a flow: rpm x the calibration the pump was connected with.
          return units[id] === RPM_UNIT
            ? bridge.pumpSetFlowRate(id, rpmToMicrolitersPerMin(value, status?.microliters_per_rev ?? Number(calibrations[id])), MICROLITERS_PER_MIN_UNIT)
            : bridge.pumpSetFlowRate(id, value, units[id]);
        })}>Apply rate</button>
        {peristaltic && connected && status && flowToRpm(status.configured_flow_rate, status.flow_rate_unit, status.microliters_per_rev) !== null &&
          <span className="mono">configured ≈ {flowToRpm(status.configured_flow_rate, status.flow_rate_unit, status.microliters_per_rev)!.toFixed(2)} rpm</span>}
        <label>Direction<select value={directions[id]} disabled={configureDisabled || !connected || !stopped} onChange={e => update(setDirections, directions, id, Number(e.target.value))}><option value={0}>{directionLabel(peristaltic, 0)}</option><option value={1}>{directionLabel(peristaltic, 1)}</option></select></label>
        <button disabled={configureDisabled || !connected || !stopped} onClick={() => void run('Set direction', 'configure', connected, () => bridge.pumpSetDirection(id, directions[id]))}>Apply direction</button>
        {!peristaltic && <>
        <label>Syringe volume (1–9999)<input type="number" min="1" max="9999" step="1" value={volumes[id]} disabled={configureDisabled || !connected || !stopped} onChange={e => update(setVolumes, volumes, id, e.target.value)} /></label>
        <label>Volume unit{unitSelect(volumeUnits[id], value => update(setVolumeUnits, volumeUnits, id, value), false)}</label>
        <button disabled={configureDisabled || !connected || !stopped} onClick={() => void run('Set syringe volume', 'configure', connected, () => bridge.pumpSetSyringeVolume(id, numericInput(volumes[id], 'Syringe volume', 1, 9999, true), volumeUnits[id]))}>Apply volume</button>
        </>}
      </div>,
      actions: {
        run: <button disabled={busy || !!actuateReason || !stopped} onClick={() => void run('Start pump', 'actuate', connected, () => bridge.pumpStart(id))}>Run configured pump</button>,
        purge: <button disabled={busy || !!actuateReason || !stopped} onClick={() => void run('Purge pump', 'actuate', connected, () => bridge.pumpPurge(id, directions[id]))}>Purge {directions[id] === 0 ? 'infuse' : 'withdraw'}{peristaltic ? (directions[id] === 0 ? ' (CCW)' : ' (CW)') : ''}</button>,
        stopPump: <button disabled={busy || !ready} onClick={() => void run('Stop pump', 'stop', connected, () => bridge.pumpStop(id))}>Stop pump</button>,
        stopPurge: <button disabled={busy || !ready} onClick={() => void run('Stop purge', 'stop', connected, () => bridge.pumpStopPurge(id))}>Stop purge</button>,
      },
    };
  }) as [PumpParts, PumpParts];

  const focusConnected = !!focus?.connected;
  const focusActuate = gate('actuate', focusConnected);
  const autofocusParts: AutofocusParts | null = autofocus ? {
    connected: focusConnected,
    actuateReason: focusActuate,
    status: <>
      <p>{focus ? `${focus.connected ? 'Connected' : 'Disconnected'} · ${focus.enabled ? 'Enabled' : 'Disabled'} · ${focus.current_voltage} V · ${focus.backend_name ?? ""} ${focus.endpoint_id ?? ""}` : 'Status unknown'}</p>
      {focus && <p>Ring ratio: {metricNumber(focus.average_ring_ratio, 3)} average / {metricNumber(focus.median_ring_ratio, 3)} median · {focus.last_ring_ratio_update_us === 0 ? 'No focus sample received' : `Sample age ${metricNumber(focus.ring_ratio_age_us / 1000, 0)} ms${config && focus.ring_ratio_age_us > config.ring_ratio_stale_ms * 1000 ? ' (stale)' : ''}`}</p>}
    </>,
    setup: <>
      <EndpointDiscovery disabled={configureDisabled || focusConnected} request={() => ({kinds: [2], origin: 'tauri-nanopositioner-picker'})} onSelect={device => {
        const oeabt = device.claimed_by.some(vendor => /oeabt/i.test(vendor));
        setFocusBackend(oeabt ? 'oeabt' : 'coremor'); setFocusEndpoint(device.persistent_id || device.system_path);
        if (!oeabt) { const port = /(?:COM)([0-9]+)$/i.exec(device.system_path)?.[1]; if (port) update(setConnections, connections, 2, {...connections[2], port, address: String(device.bus_address)}); }
      }} />
      <label>Nanopositioner backend<select value={focusBackend} disabled={configureDisabled || focusConnected} onChange={e => setFocusBackend(e.target.value)}><option value="coremor">CoreMOR serial</option><option value="oeabt">OEABT USB</option></select></label>
      {focusBackend === 'oeabt' && <label>OEABT endpoint identity<input value={focusEndpoint} disabled={configureDisabled || focusConnected} onChange={e => setFocusEndpoint(e.target.value)} placeholder="Exact discovered endpoint ID" /></label>}
      {focusBackend === 'coremor' && <ConnectionFields value={connections[2]} disabled={configureDisabled || focusConnected} onChange={v => update(setConnections, connections, 2, v)} />}
    </>,
    connection: <>
      <button disabled={configureDisabled || !focus || focus.connected} onClick={() => void run('Connect autofocus', 'configure', false, () => focusBackend === 'coremor' ? bridge.autofocusConnect(...connectionArgs(connections[2], 255, 0)) : bridge.autofocusConnectEndpoint(focusBackend, focusEndpoint.trim(), -1, 115200, 1))}>Connect autofocus</button>
      <button disabled={configureDisabled || !focusConnected} onClick={() => void run('Disconnect autofocus', 'configure', true, bridge.autofocusDisconnect)}>Disconnect autofocus</button>
      {layout && <button disabled={!ready} onClick={() => void refresh()}>Read status</button>}
    </>,
    actions: {
      enable: <button disabled={busy || !!focusActuate || !!focus?.enabled} onClick={() => void run('Enable autofocus', 'actuate', focusConnected, () => bridge.autofocusSetEnabled(true))}>Enable autofocus</button>,
      disable: <button disabled={busy || !ready} onClick={() => void run('Disable autofocus', 'stop', focusConnected, () => bridge.autofocusSetEnabled(false))}>Disable autofocus</button>,
      jogDown: <button disabled={busy || !!focusActuate || !!focus?.enabled} onClick={() => void run('Jog voltage', 'actuate', focusConnected, () => bridge.autofocusJog(false))}>Jog voltage −</button>,
      jogUp: <button disabled={busy || !!focusActuate || !!focus?.enabled} onClick={() => void run('Jog voltage', 'actuate', focusConnected, () => bridge.autofocusJog(true))}>Jog voltage +</button>,
    },
    config: config && <details><summary>Autofocus configuration</summary><div className="hardware-fields">
      {focusFields.map(([key, label]) => <label key={key}>{label}<input type="number" step="any" disabled={configureDisabled || !focus || focus.enabled} value={Number.isNaN(Number(config[key])) ? '' : Number(config[key])} onChange={e => setConfig({...config, [key]: e.target.value.trim() === '' ? NaN : Number(e.target.value)})} /></label>)}
      {(['require_new_sample_per_step', 'focus_direction'] as const).map(key => <label key={key}><input type="checkbox" disabled={configureDisabled || !focus || focus.enabled} checked={config[key]} onChange={e => setConfig({...config, [key]: e.target.checked})} />{key === 'focus_direction' ? 'Positive focus direction' : 'Require new sample per step'}</label>)}
    </div><button disabled={configureDisabled || !focus || focus.enabled} onClick={() => void run('Apply autofocus configuration', 'configure', focusConnected, () => { validateFocusConfig(config); return bridge.autofocusSetConfig(config); })}>Apply autofocus configuration</button><p>Applies to the running backend; persistence across restarts is not provided by this command.</p></details>,
  } : null;

  const parts: HardwareParts = {
    pumps: pumpParts,
    autofocus: autofocusParts,
    startup: autofocus ? <StartupDiscoveryControls ready={ready} experimentActive={experimentActive} append={append} onSelectionChanged={onSelectionChanged} /> : null,
    pulseGenerator: capabilities.trigger
      ? (pulseLayout?: PulseGeneratorLayout) => <PulseGeneratorControls ready={ready} experimentActive={experimentActive} mode={mode} armed={armed} onDisarm={onDisarm} append={append} layout={pulseLayout} />
      : null,
    notices: { configureReason: gate('configure'), error, statusError },
    instrumentPumpText: instrumentPump ? `This instrument's peristaltic pumps share ${instrumentPump.port}: Sample at Modbus address ${instrumentPump.sample_address}, Sheath at ${instrumentPump.sheath_address}.` : '',
  };
  return <>{layout ? layout(parts) : defaultLayout(parts, autofocus)}</>;
}
