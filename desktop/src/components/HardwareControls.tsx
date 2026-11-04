import { metricNumber } from "../metricFormat";
import { useCallback, useEffect, useRef, useState, type ReactNode } from 'react';
import { bridge, type AutofocusConfig, type AutofocusStatus, type PumpStatus, type CmdResult, type PlatformCapabilities } from '../bridge';
import { DESKTOP_CAPABILITIES } from '../platformCapabilities';
import { DEFAULT_MODE, type OperatingMode } from '../commissioning';
import { AUTO_APPLY_DEBOUNCE_MS, HardwareCommandOwner, JOG_STEP_DEFAULT, JOG_STEP_MAX, JOG_STEP_MIN, PUMP_FLOW_DECIMALS, PUMP_FLOW_MAX, RING_WIDTH_DEFAULT, RING_WIDTH_MAX, RING_WIDTH_MIN, STATUS_POLL_MS, SYRINGE_VOLUME_MAX, SYRINGE_VOLUME_MIN, deviceGate, flowRateInput, numericInput, syringeVolumeForDevice, validateFocusConfig, type DeviceAction } from './hardwareControlModel';
import { loadHardwarePrefs, saveHardwarePrefs, withPump, type HardwarePrefs } from './hardwarePrefs';
import { PUMP_MODELS } from '../bridgeContract';
import './HardwareControls.css';
import {StartupDiscoveryControls} from './StartupDiscoveryControls';
import {MICROLITERS_PER_MIN_UNIT, RPM_UNIT, directionLabel, flowToRpm, rpmToMicrolitersPerMin} from '../pumpRate';
import {EndpointDiscovery} from './EndpointDiscovery';
import {PulseGeneratorControls} from './PulseGeneratorControls';

type Props = { ready: boolean; experimentActive: boolean; append: (message: string) => void; mode?: OperatingMode; armed?: boolean; onDisarm: () => void; onSelectionChanged?: () => void; capabilities?: PlatformCapabilities };
type Connection = { port: string; baud: string; address: string };
// HW-006: Qt defaults the Sample pump to Modbus address 1 and the Sheath pump to 2 (SyringePumpSettingsDialog.ui:59-63,152-156),
// so two pumps on one RS485 bus do not collide; slot 2 is the CoreMOR nanopositioner.
const initialConnection = (slot = 0): Connection => ({port: '', baud: '115200', address: slot === 1 ? '2' : '1'});
// 0.4 rpm delivers 10 µL/min on the instrument tubing until a measured calibration replaces it.
// The backend requires a calibration above 0 for every model; a syringe pump ignores it.
const DEFAULT_MICROLITERS_PER_REV = '25';
// Where the PZ7035 instrument's peristaltic pump answers (PS UART1 RS485).
const PERISTALTIC_DEFAULT_ENDPOINT = (slot: number) => ({port: '/dev/ttyPS1', address: slot === 1 ? '4' : '3'});
// The target ring width and the jog step have their own Qt controls above the configuration.
const focusFields: Array<[keyof AutofocusConfig, string]> = [
  ['focus_range', 'Focus range'], ['voltage_step', 'Voltage step (V)'],
  ['fine_voltage_step', 'Fine step (V)'], ['min_voltage', 'Minimum voltage (V)'], ['max_voltage', 'Maximum voltage (V)'],
  ['initial_voltage', 'Initial voltage (V)'],
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

/** Qt's purge button runs the pump only while it is held (SyringePumpTab.cpp:75-94): press starts, release
 *  (or the pointer leaving, focus loss, a disabled button, unmount) stops. */
export function HoldButton({disabled, title, onStart, onStop, children}: {disabled: boolean; title: string; onStart: () => void; onStop: () => void; children: ReactNode}) {
  const holding = useRef(false);
  const stopRef = useRef(onStop); stopRef.current = onStop;
  const start = () => { if (holding.current || disabled) return; holding.current = true; onStart(); };
  const stop = () => { if (!holding.current) return; holding.current = false; stopRef.current(); };
  useEffect(() => { if (disabled) stop(); }, [disabled]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => () => stop(), []); // eslint-disable-line react-hooks/exhaustive-deps
  const key = (k: string) => k === ' ' || k === 'Enter';
  return <button type="button" disabled={disabled} title={title}
    onPointerDown={start} onPointerUp={stop} onPointerCancel={stop} onPointerLeave={stop} onBlur={stop}
    onKeyDown={e => { if (key(e.key)) { e.preventDefault(); if (!e.repeat) start(); } }} onKeyUp={e => { if (key(e.key)) stop(); }}>{children}</button>;
}
type PumpEdit = {rate: string; unit: number; direction: number};
const blurOrEnter = (apply: () => void) => ({onBlur: apply, onKeyDown: (e: {key: string}) => { if (e.key === 'Enter') apply(); }});

export function HardwareControls({ready, experimentActive, append, mode = DEFAULT_MODE, armed = false, onDisarm, onSelectionChanged, capabilities = DESKTOP_CAPABILITIES}: Props) {
  // #501: on the PZ7035 the pump is the instrument's peristaltic pump and there is no
  // nanopositioner or host pulse generator.
  const instrumentPump = capabilities.pump;
  const slotAddress = (slot: number) => instrumentPump ? (slot === 0 ? instrumentPump.sample_address : instrumentPump.sheath_address) : 1;
  const pumpConnection = (slot: number): Connection => instrumentPump ? {port: instrumentPump.port, baud: '115200', address: String(slotAddress(slot))} : initialConnection(slot);
  const pumpModel = instrumentPump ? PUMP_MODELS.TushuiPeristaltic : PUMP_MODELS.DlspSyringe;
  const pumpCalibration = instrumentPump ? String(instrumentPump.microliters_per_rev) : DEFAULT_MICROLITERS_PER_REV;
  const [pumps, setPumps] = useState<Array<PumpStatus | null>>([null, null]);
  const pumpsRef = useRef<Array<PumpStatus | null>>([null, null]);
  const [focusBackend, setFocusBackend] = useState('coremor');
  const [focusEndpoint, setFocusEndpoint] = useState('');
  const [focus, setFocus] = useState<AutofocusStatus | null>(null);
  const [config, setConfig] = useState<AutofocusConfig | null>(null);
  const [connections, setConnections] = useState([pumpConnection(0), pumpConnection(1), initialConnection()]);
  const [rates, setRates] = useState(['', '']);
  const [units, setUnits] = useState([100, 100]);
  const [directions, setDirections] = useState([0, 0]);
  const [volumes, setVolumes] = useState(['', '']);
  const [volumeUnits, setVolumeUnits] = useState([100, 100]);
  const [models, setModels] = useState<number[]>([pumpModel, pumpModel]);
  const [calibrations, setCalibrations] = useState([pumpCalibration, pumpCalibration]);
  const [ringWidth, setRingWidth] = useState(String(RING_WIDTH_DEFAULT));
  const [jogStep, setJogStep] = useState(String(JOG_STEP_DEFAULT));
  // The capability report arrives after mount: adopt the instrument's pump once.
  useEffect(() => {
    if (!instrumentPump) return;
    setModels(old => old.map(() => PUMP_MODELS.TushuiPeristaltic));
    setCalibrations(old => old.map(() => String(instrumentPump.microliters_per_rev)));
    setConnections(old => old.map((c, i) => i < 2 ? pumpConnection(i) : c));
  }, [instrumentPump?.port, instrumentPump?.sample_address, instrumentPump?.sheath_address, instrumentPump?.microliters_per_rev]); // eslint-disable-line react-hooks/exhaustive-deps
  const [busy, setBusy] = useState(false);
  const busyRef = useRef(false);
  const [error, setError] = useState('');
  const [statusError, setStatusError] = useState('');
  const owner = useRef(new HardwareCommandOwner());
  const polling = useRef(false);
  const generation = useRef(0);
  // HW-016 / HW-030: restore the saved rate, unit, direction, volume, ring width and jog step.
  const prefs = useRef<Promise<HardwarePrefs> | null>(null);
  if (!prefs.current) prefs.current = loadHardwarePrefs();
  useEffect(() => {
    let alive = true;
    void prefs.current!.then(saved => {
      if (!alive) return;
      const pick = <T,>(old: T[], key: 'rate' | 'unit' | 'direction' | 'volume' | 'volumeUnit', blank: (v: T) => boolean) =>
        old.map((v, i) => { const value = saved.pumps?.[i]?.[key]; return value !== undefined && blank(v) ? value as unknown as T : v; });
      setRates(old => pick(old, 'rate', v => v === '')); setVolumes(old => pick(old, 'volume', v => v === ''));
      setUnits(old => pick(old, 'unit', () => true)); setDirections(old => pick(old, 'direction', () => true));
      setVolumeUnits(old => pick(old, 'volumeUnit', () => true));
      if (saved.focusSetpoint !== undefined) setRingWidth(String(saved.focusSetpoint));
      if (saved.jogStep !== undefined) setJogStep(String(saved.jogStep));
    });
    return () => { alive = false; };
  }, []);
  const save = (change: (old: HardwarePrefs) => HardwarePrefs) => void saveHardwarePrefs(change).catch(failure => append(`Hardware settings not saved: ${String(failure)}`));
  // #855 / HW-015: poll each connected pump on the device every 500 ms, as Qt does
  // (SyringePumpTab.cpp:276-288); start and purge do not update the cached run state.
  const refresh = useCallback(async (pollDevices = false) => {
    if (polling.current || !ready) return;
    polling.current = true;
    const current = generation.current;
    try {
      if (pollDevices) await Promise.all(pumpsRef.current.map((p, i) => p?.connected ? bridge.pumpPollStatus(i).catch(() => undefined) : undefined));
      const [sample, sheath, autofocus] = await Promise.all([bridge.fetchPumpStatus(0), bridge.fetchPumpStatus(1), bridge.fetchAutofocusStatus()]);
      if (current !== generation.current) return;
      if (!sample.valid || !sheath.valid || (capabilities.autofocus && !autofocus.valid)) throw new Error('Hardware status is unavailable.');
      pumpsRef.current = [sample, sheath];
      setPumps([sample, sheath]); setFocus(autofocus); setStatusError('');
      // Without a saved rate, show the rate the pump is configured with.
      setRates(old => old.map((rate, i) => { const p = [sample, sheath][i]; return rate === '' && p.connected ? String(p.configured_flow_rate) : rate; }));
    } catch (failure) {
      if (current === generation.current) { pumpsRef.current = [null, null]; setPumps([null, null]); setFocus(null); setStatusError(String(failure)); }
    } finally { polling.current = false; }
  }, [ready, capabilities.autofocus]);
  useEffect(() => {
    const current = ++generation.current;
    pumpsRef.current = [null, null];
    setPumps([null, null]); setFocus(null); setConfig(null);
    if (!ready) return;
    void refresh();
    void Promise.all([bridge.fetchAutofocusConfig(), prefs.current!]).then(async ([value, saved]) => {
      if (current !== generation.current) return;
      if (!value.valid) { setError('Autofocus configuration unavailable.'); return; }
      setConfig(value);
      if (saved.focusSetpoint === undefined) setRingWidth(String(value.focus_setpoint));
      // HW-030: the saved target ring width is applied at start, as Qt loads it from config.json.
      else if (capabilities.autofocus && saved.focusSetpoint !== value.focus_setpoint) {
        const result = await bridge.autofocusSetConfig({...value, focus_setpoint: saved.focusSetpoint});
        if (current === generation.current && result.ok) setConfig({...value, focus_setpoint: saved.focusSetpoint});
      }
    }).catch(failure => { if (current === generation.current) setError(String(failure)); });
    const timer = window.setInterval(() => void refresh(true), STATUS_POLL_MS);
    return () => { ++generation.current; window.clearInterval(timer); };
  }, [ready, refresh]); // eslint-disable-line react-hooks/exhaustive-deps
  const gate = (kind: DeviceAction, connected = false) => deviceGate({ready, experimentActive, connected}, kind);
  const connectDisabled = busy || !!gate('connect');
  /** Runs a command; `stop` commands skip the in-flight lock so Stop and Disable never wait (#855). */
  async function run(label: string, kind: DeviceAction, connected: boolean, command: () => Promise<CmdResult>): Promise<boolean> {
    const reason = gate(kind, connected);
    if (reason) { setError(reason); return false; }
    const locked = kind !== 'stop';
    if (locked && busyRef.current) return false;
    if (locked) { busyRef.current = true; setBusy(true); }
    setError('');
    try {
      const result = locked ? await owner.current.run(command) : await command();
      if (!result.ok) throw new Error(result.message || 'Hardware command failed.');
      append(`${label}: ${result.message || 'Command accepted'}`);
      await refresh();
      return true;
    } catch (failure) { const message = `${label}: ${String(failure)}`; setError(message); append(message); return false; }
    finally { if (locked) { busyRef.current = false; setBusy(false); } }
  }
  const update = <T,>(setter: (value: T[]) => void, values: T[], index: number, value: T) => setter(values.map((old, i) => i === index ? value : old));
  // HW-008: rate, unit and direction apply 300 ms after an edit, also while the pump runs.
  const applyTimers = useRef<Array<number | undefined>>([undefined, undefined]);
  const pendingApply = useRef<Array<PumpEdit | null>>([null, null]);
  useEffect(() => () => applyTimers.current.forEach(timer => window.clearTimeout(timer)), []);
  const applyPump = async (id: number) => {
    const edit = pendingApply.current[id];
    if (!edit) return;
    if (busyRef.current) { applyTimers.current[id] = window.setTimeout(() => void applyPump(id), AUTO_APPLY_DEBOUNCE_MS); return; }
    pendingApply.current[id] = null;
    const status = pumpsRef.current[id];
    if (!status?.connected) return;
    await run('Apply flow rate and direction', 'operate', true, async () => {
      const value = flowRateInput(edit.rate, edit.unit === RPM_UNIT ? 'Head speed' : 'Flow rate');
      // The backend takes a flow: rpm x the calibration the pump was connected with.
      const rate = edit.unit === RPM_UNIT
        ? await bridge.pumpSetFlowRate(id, rpmToMicrolitersPerMin(value, status.microliters_per_rev || Number(calibrations[id])), MICROLITERS_PER_MIN_UNIT)
        : await bridge.pumpSetFlowRate(id, value, edit.unit);
      return rate.ok ? bridge.pumpSetDirection(id, edit.direction) : rate;
    });
  };
  const editPump = (id: number, change: Partial<PumpEdit>) => {
    const edit = {rate: rates[id], unit: units[id], direction: directions[id], ...change};
    save(old => withPump(old, id, edit));
    if (!pumpsRef.current[id]?.connected) return;
    pendingApply.current[id] = edit;
    window.clearTimeout(applyTimers.current[id]);
    applyTimers.current[id] = window.setTimeout(() => void applyPump(id), AUTO_APPLY_DEBOUNCE_MS);
  };
  const applyVolume = (id: number, volume: string, unit: number) => {
    save(old => withPump(old, id, {volume, volumeUnit: unit}));
    if (!pumpsRef.current[id]?.connected || !volume.trim()) return;
    void run('Set syringe volume', 'operate', true, () => { const v = syringeVolumeForDevice(volume, unit); return bridge.pumpSetSyringeVolume(id, v.volume, v.unit); });
  };
  // A release can arrive before the purge start returns: stop only after it has.
  const purgeStarts = useRef<Array<Promise<unknown> | null>>([null, null]);
  const startPurge = (id: number) => { purgeStarts.current[id] = run('Purge pump', 'operate', !!pumpsRef.current[id]?.connected, () => bridge.pumpPurge(id, directions[id])); };
  const stopPurge = (id: number) => { const started = purgeStarts.current[id] ?? Promise.resolve(); purgeStarts.current[id] = null; void started.then(() => run('Stop purge', 'stop', true, () => bridge.pumpStopPurge(id))); };
  // HW-030 / HW-032: the target ring width applies live; a jog first sets its step (NanopositionerTab.cpp:367-392).
  const withBackendConfig = async (change: Partial<AutofocusConfig>) => {
    const current = await bridge.fetchAutofocusConfig();
    if (!current.valid) throw new Error('Autofocus configuration unavailable.');
    const next = {...current, ...change};
    const result = await bridge.autofocusSetConfig(next);
    if (result.ok) setConfig(old => old ? {...old, ...change} : next);
    return result;
  };
  const ringTimer = useRef<number | undefined>(undefined);
  useEffect(() => () => window.clearTimeout(ringTimer.current), []);
  const applyRingWidth = (value: string) => {
    window.clearTimeout(ringTimer.current);
    ringTimer.current = window.setTimeout(() => {
      let setpoint: number;
      try { setpoint = Math.round(numericInput(value, 'Target ring width', RING_WIDTH_MIN, RING_WIDTH_MAX) * 100) / 100; }
      catch (failure) { setError(String(failure)); return; }
      save(old => ({...old, focusSetpoint: setpoint}));
      void run('Set target ring width', 'stop', true, () => withBackendConfig({focus_setpoint: setpoint}));
    }, AUTO_APPLY_DEBOUNCE_MS);
  };
  const jog = (up: boolean) => void run(`Jog voltage ${up ? '+' : '−'}`, 'operate', !!focus?.connected, async () => {
    const step = numericInput(jogStep, 'Jog step', JOG_STEP_MIN, JOG_STEP_MAX);
    save(old => ({...old, jogStep: step}));
    const set = await withBackendConfig({manual_voltage_step: step});
    return set.ok ? bridge.autofocusJog(up) : set;
  });
  const unitSelect = (value: number, onChange: (value: number) => void, rate: boolean, disabled: boolean, rpm = false) => <select value={value} disabled={disabled} onChange={e => onChange(Number(e.target.value))}>{rpm && <option value={RPM_UNIT}>rpm (head)</option>}<option value={100}>{rate ? 'µL/min' : 'µL'}</option><option value={103}>{rate ? 'mL/min' : 'mL'}</option></select>;
  const autofocus = capabilities.autofocus;
  const focusConnected = !!focus?.connected;
  const focusReason = gate('operate', focusConnected);
  return <section className="hardware-controls" aria-label={autofocus ? 'Pump and autofocus controls' : 'Pump controls'}>
    <h2>{autofocus ? 'Pumps and autofocus' : 'Pumps'}</h2>
    {autofocus && <StartupDiscoveryControls ready={ready} experimentActive={experimentActive} append={append} onSelectionChanged={onSelectionChanged} />}
    <p>Pump{autofocus ? ' and autofocus' : ''} controls work whenever the device is connected, also during an experiment. Connecting and disconnecting are locked while an experiment runs. Stop pump, Stop purge{autofocus ? ' and Disable autofocus' : ''} are always available.</p>
    {autofocus
      ? <p>Pumps accept system serial endpoints; devices may share a bus at distinct Modbus addresses. Nanopositioners support CoreMOR and OEABT identities.</p>
      : instrumentPump && <p>This instrument's peristaltic pumps share {instrumentPump.port}: Sample at Modbus address {instrumentPump.sample_address}, Sheath at {instrumentPump.sheath_address}.</p>}
    {gate('connect') && <p role="status">{gate('connect')}</p>}
    {error && <p role="alert">{error}</p>}{statusError && <p role="alert">Status unavailable: {statusError}</p>}
    {pumps.map((status, id) => {
      const connected = !!status?.connected;
      const operateDisabled = busy || !!gate('operate', connected);
      const model = connected && status?.model !== undefined ? status.model : models[id];
      const peristaltic = model === PUMP_MODELS.TushuiPeristaltic;
      const unit = !peristaltic && units[id] === RPM_UNIT ? MICROLITERS_PER_MIN_UNIT : units[id];
      const selectModel = (value: number) => {
        update(setModels, models, id, value);
        if (value !== PUMP_MODELS.TushuiPeristaltic && units[id] === RPM_UNIT) update(setUnits, units, id, MICROLITERS_PER_MIN_UNIT);
        const current = connections[id];
        if (value === PUMP_MODELS.TushuiPeristaltic && !current.port.trim() && current.address === initialConnection(id).address)
          update(setConnections, connections, id, {...current, ...PERISTALTIC_DEFAULT_ENDPOINT(id)});
      };
      return <fieldset key={id}><legend>{id === 0 ? 'Sample pump' : 'Sheath pump'}</legend>
        <p>{status ? `${connected ? 'Connected' : 'Not connected'} · ${['Stopped', 'Forward', 'Backward', 'Paused'][status.run_status] ?? `Unknown state ${status.run_status}`}${status.stalled ? ' [STALL]' : ''}` : 'Status unknown'}</p>
        {connected && status && <p>{peristaltic ? 'Peristaltic' : 'Syringe'} · {status.port_name || `COM ${status.com_port}`} · Address {status.modbus_address} · Configured rate {status.configured_flow_rate} ({status.flow_rate_unit === 100 ? 'µL/min' : status.flow_rate_unit === 103 ? 'mL/min' : `unit ${status.flow_rate_unit}`}) · Direction {status.direction === 0 ? 'Infuse' : status.direction === 1 ? 'Withdraw' : 'Unknown'} · Live rate {status.current_flow_rate.toFixed(PUMP_FLOW_DECIMALS)}{peristaltic ? ' µL/min' : ''} · {peristaltic
          ? `Head ${(status.speed_rpm ?? 0).toFixed(2)} rpm at ${status.microliters_per_rev ?? '?'} µL/rev · Delivered ≈ ${status.accumulated_volume.toFixed(1)} µL (estimated from speed)`
          : `Accumulated volume ${status.accumulated_volume.toFixed(PUMP_FLOW_DECIMALS)} (device units)`}</p>}
        <div className="hardware-fields">
          <label>Pump model<select value={model} disabled={connectDisabled || connected} onChange={e => selectModel(Number(e.target.value))}>
            <option value={PUMP_MODELS.DlspSyringe}>Syringe (Longer dLSP)</option>
            <option value={PUMP_MODELS.TushuiPeristaltic}>Peristaltic (Tushui)</option>
          </select></label>
          {peristaltic && <label>Calibration (µL per revolution)<input type="number" min="0" step="any" value={calibrations[id]} disabled={connectDisabled || connected} onChange={e => update(setCalibrations, calibrations, id, e.target.value)} /></label>}
        </div>
        <ConnectionFields systemPort value={connections[id]} disabled={connectDisabled || connected} onChange={v => update(setConnections, connections, id, v)} />
        <div className="hardware-actions">
          <button disabled={connectDisabled || !status || connected} onClick={() => void run('Connect pump', 'connect', false, () => {
            const connection = connections[id]; const port = connection.port.trim(); if (!port) throw new Error('System serial port is required');
            const microlitersPerRev = peristaltic ? numericInput(calibrations[id], 'Calibration', 0.001, 100000) : Number(DEFAULT_MICROLITERS_PER_REV);
            return bridge.pumpConnectModel(id, model, /^\d+$/.test(port) ? `COM${port}` : port, numericInput(connection.baud, 'Baud rate', 1, 4000000, true), numericInput(connection.address, 'Address', 1, 247, true), microlitersPerRev);
          })}>Connect</button>
          <button disabled={connectDisabled || !connected} onClick={() => void run('Disconnect pump', 'connect', connected, () => bridge.pumpDisconnect(id))}>Disconnect</button>
        </div>
        <div className="hardware-fields">
          <label>Flow rate<input type="number" min="0" max={PUMP_FLOW_MAX} step={10 ** -PUMP_FLOW_DECIMALS} value={rates[id]} disabled={!ready} onChange={e => update(setRates, rates, id, e.target.value)} {...blurOrEnter(() => editPump(id, {}))} /></label>
          <label>Rate unit{unitSelect(unit, value => { update(setUnits, units, id, value); editPump(id, {unit: value}); }, true, !ready, peristaltic)}</label>
          {peristaltic && connected && status && flowToRpm(status.configured_flow_rate, status.flow_rate_unit, status.microliters_per_rev) !== null &&
            <span className="mono">configured ≈ {flowToRpm(status.configured_flow_rate, status.flow_rate_unit, status.microliters_per_rev)!.toFixed(2)} rpm</span>}
          <label>Direction<select value={directions[id]} disabled={!ready} onChange={e => { const value = Number(e.target.value); update(setDirections, directions, id, value); editPump(id, {direction: value}); }}><option value={0}>{directionLabel(peristaltic, 0)}</option><option value={1}>{directionLabel(peristaltic, 1)}</option></select></label>
          {!peristaltic && <>
          <label>Syringe volume ({SYRINGE_VOLUME_MIN}–{SYRINGE_VOLUME_MAX}, applied to the pump)<input type="number" min={SYRINGE_VOLUME_MIN} max={SYRINGE_VOLUME_MAX} step="0.1" value={volumes[id]} disabled={!ready} onChange={e => update(setVolumes, volumes, id, e.target.value)} {...blurOrEnter(() => applyVolume(id, volumes[id], volumeUnits[id]))} /></label>
          <label>Volume unit{unitSelect(volumeUnits[id], value => { update(setVolumeUnits, volumeUnits, id, value); applyVolume(id, volumes[id], value); }, false, !ready)}</label>
          </>}
        </div>
        {connected ? <p className="hint">Rate and direction apply to the pump {AUTO_APPLY_DEBOUNCE_MS} ms after an edit, also while it runs.</p> : <p className="hint">Rate and direction are saved and applied once the pump is connected.</p>}
        <div className="hardware-actions">
          <button disabled={operateDisabled} onClick={() => void run('Start pump', 'operate', connected, () => bridge.pumpStart(id))}>Start pump</button>
          <button disabled={!ready} onClick={() => void run('Stop pump', 'stop', connected, () => bridge.pumpStop(id))}>Stop pump</button>
          <HoldButton disabled={!!gate('operate', connected)} title="Run pump at full speed (hold to purge)" onStart={() => startPurge(id)} onStop={() => stopPurge(id)}>Purge {directions[id] === 0 ? 'infuse' : 'withdraw'}{peristaltic ? (directions[id] === 0 ? ' (CCW)' : ' (CW)') : ''} (hold)</HoldButton>
          <button disabled={!ready} onClick={() => void run('Stop purge', 'stop', connected, () => bridge.pumpStopPurge(id))}>Stop purge</button>
        </div>
      </fieldset>;
    })}
    {autofocus && <fieldset><legend>Autofocus / nanopositioner</legend>
      <p>{focus ? `${focus.connected ? 'Connected' : 'Disconnected'} · ${focus.enabled ? 'Enabled' : 'Disabled'} · ${focus.current_voltage} V · ${focus.backend_name ?? ""} ${focus.endpoint_id ?? ""}` : 'Status unknown'}</p>
      {focus && <p>Ring ratio: {metricNumber(focus.average_ring_ratio, 3)} average / {metricNumber(focus.median_ring_ratio, 3)} median · {focus.last_ring_ratio_update_us === 0 ? 'No focus sample received' : `Sample age ${metricNumber(focus.ring_ratio_age_us / 1000, 0)} ms${config && focus.ring_ratio_age_us > config.ring_ratio_stale_ms * 1000 ? ' (stale)' : ''}`}</p>}
      <EndpointDiscovery disabled={connectDisabled || focusConnected} request={() => ({kinds: [2], origin: 'tauri-nanopositioner-picker'})} onSelect={device => {
        const oeabt = device.claimed_by.some(vendor => /oeabt/i.test(vendor));
        setFocusBackend(oeabt ? 'oeabt' : 'coremor'); setFocusEndpoint(device.persistent_id || device.system_path);
        if (!oeabt) { const port = /(?:COM)([0-9]+)$/i.exec(device.system_path)?.[1]; if (port) update(setConnections, connections, 2, {...connections[2], port, address: String(device.bus_address)}); }
      }} />
      <label>Nanopositioner backend<select value={focusBackend} disabled={connectDisabled || focusConnected} onChange={e => setFocusBackend(e.target.value)}><option value="coremor">CoreMOR serial</option><option value="oeabt">OEABT USB</option></select></label>
      {focusBackend === 'oeabt' && <label>OEABT endpoint identity<input value={focusEndpoint} disabled={connectDisabled || focusConnected} onChange={e => setFocusEndpoint(e.target.value)} placeholder="Exact discovered endpoint ID" /></label>}
      {focusBackend === 'coremor' && <ConnectionFields value={connections[2]} disabled={connectDisabled || focusConnected} onChange={v => update(setConnections, connections, 2, v)} />}
      <div className="hardware-actions">
        <button disabled={connectDisabled || !focus || focus.connected} onClick={() => void run('Connect autofocus', 'connect', false, () => focusBackend === 'coremor' ? bridge.autofocusConnect(...connectionArgs(connections[2], 255, 0)) : bridge.autofocusConnectEndpoint(focusBackend, focusEndpoint.trim(), -1, 115200, 1))}>Connect autofocus</button>
        <button disabled={connectDisabled || !focusConnected} onClick={() => void run('Disconnect autofocus', 'connect', true, bridge.autofocusDisconnect)}>Disconnect autofocus</button>
      </div>
      <div className="hardware-fields">
        {/* HW-029: Qt's Enable Autofocus check box, live whenever connected; unchecking never waits for another command. */}
        <label><input type="checkbox" checked={!!focus?.enabled} disabled={focus?.enabled ? !ready : busy || !!focusReason}
          onChange={e => void (e.target.checked
            ? run('Enable autofocus', 'operate', focusConnected, () => bridge.autofocusSetEnabled(true))
            : run('Disable autofocus', 'stop', focusConnected, () => bridge.autofocusSetEnabled(false)))} />Enable Autofocus</label>
        <label>Target ring width ({RING_WIDTH_MIN}–{RING_WIDTH_MAX})<input type="number" min={RING_WIDTH_MIN} max={RING_WIDTH_MAX} step="0.01" value={ringWidth} disabled={!ready}
          onChange={e => { setRingWidth(e.target.value); applyRingWidth(e.target.value); }} /></label>
        <label>Jog step (V, {JOG_STEP_MIN}–{JOG_STEP_MAX})<input type="number" min={JOG_STEP_MIN} max={JOG_STEP_MAX} step="1" value={jogStep} disabled={!ready} onChange={e => setJogStep(e.target.value)} /></label>
        {[false, true].map(up => <button key={String(up)} disabled={busy || !!focusReason} onClick={() => jog(up)}>Jog voltage {up ? '+' : '−'}</button>)}
        <button disabled={!ready} onClick={() => void run('Disable autofocus', 'stop', focusConnected, () => bridge.autofocusSetEnabled(false))}>Disable autofocus</button>
      </div>
      {focusReason && <p>{focusReason}</p>}
      {config && <details><summary>Autofocus configuration</summary><div className="hardware-fields">
        {focusFields.map(([key, label]) => <label key={key}>{label}<input type="number" step="any" disabled={connectDisabled || !focus || focus.enabled} value={Number.isNaN(Number(config[key])) ? '' : Number(config[key])} onChange={e => setConfig({...config, [key]: e.target.value.trim() === '' ? NaN : Number(e.target.value)})} /></label>)}
        {(['require_new_sample_per_step', 'focus_direction'] as const).map(key => <label key={key}><input type="checkbox" disabled={connectDisabled || !focus || focus.enabled} checked={config[key]} onChange={e => setConfig({...config, [key]: e.target.checked})} />{key === 'focus_direction' ? 'Positive focus direction' : 'Require new sample per step'}</label>)}
      </div><button disabled={connectDisabled || !focus || focus.enabled} onClick={() => void run('Apply autofocus configuration', 'connect', focusConnected, () => { validateFocusConfig(config); return bridge.autofocusSetConfig(config); })}>Apply autofocus configuration</button><p>Applies to the running backend. The target ring width and jog step above are remembered for the next start; these values come from the applied config document.</p></details>}
    </fieldset>}
    {capabilities.trigger && <PulseGeneratorControls ready={ready} experimentActive={experimentActive} mode={mode} armed={armed} onDisarm={onDisarm} append={append} />}
  </section>;
}
