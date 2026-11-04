import { useCallback, useEffect, useRef, useState } from 'react';
import { bridge, type PumpStatus, type CmdResult, type PlatformCapabilities } from '../bridge';
import { DESKTOP_CAPABILITIES } from '../platformCapabilities';
import { DEFAULT_MODE, type OperatingMode } from '../commissioning';
import { HardwareCommandOwner, hardwareGate, numericInput } from './hardwareControlModel';
import { PUMP_MODELS } from '../bridgeContract';
import { ConnectionFields, initialConnection, type Connection } from './HardwareConnectionFields';
import { MICROLITERS_PER_MIN_UNIT, RPM_UNIT, directionLabel, flowToRpm, rpmToMicrolitersPerMin } from '../pumpRate';
import './HardwareControls.css';

type Props = { ready: boolean; experimentActive: boolean; append: (message: string) => void; mode?: OperatingMode; armed?: boolean; onDisarm: () => void; capabilities?: PlatformCapabilities };
// 0.4 rpm delivers 10 µL/min on the instrument tubing until a measured calibration replaces it.
const DEFAULT_MICROLITERS_PER_REV = '25';
// Where the PZ7035 instrument's peristaltic pump answers (PS UART1 RS485).
const PERISTALTIC_DEFAULT_ENDPOINT = {port: '/dev/ttyPS1', address: '3'};

/** Sample and Sheath pump panels (SyringePumpTab in Qt): connection, rate, direction, volume, run, purge. */
export function PumpControls({ready, experimentActive, append, mode = DEFAULT_MODE, armed = false, onDisarm, capabilities = DESKTOP_CAPABILITIES}: Props) {
  // #501: on the PZ7035 the pump is the instrument's peristaltic pump.
  const instrumentPump = capabilities.pump;
  const slotAddress = (slot: number) => instrumentPump ? (slot === 0 ? instrumentPump.sample_address : instrumentPump.sheath_address) : 1;
  const pumpConnection = (slot: number): Connection => instrumentPump ? {port: instrumentPump.port, baud: '115200', address: String(slotAddress(slot))} : initialConnection();
  const pumpModel = instrumentPump ? PUMP_MODELS.TushuiPeristaltic : PUMP_MODELS.DlspSyringe;
  const pumpCalibration = instrumentPump ? String(instrumentPump.microliters_per_rev) : DEFAULT_MICROLITERS_PER_REV;
  const [pumps, setPumps] = useState<Array<PumpStatus | null>>([null, null]);
  const [connections, setConnections] = useState([pumpConnection(0), pumpConnection(1)]);
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
  const [statusError, setStatusError] = useState('');
  const owner = useRef(new HardwareCommandOwner());
  const polling = useRef(false);
  const generation = useRef(0);
  const refresh = useCallback(async () => {
    if (polling.current || !ready) return;
    polling.current = true;
    const current = generation.current;
    try {
      const [sample, sheath] = await Promise.all([bridge.fetchPumpStatus(0), bridge.fetchPumpStatus(1)]);
      if (current !== generation.current) return;
      if (!sample.valid || !sheath.valid) throw new Error('Hardware status is unavailable.');
      setPumps([sample, sheath]); setStatusError('');
    } catch (failure) {
      if (current === generation.current) { setPumps([null, null]); setStatusError(String(failure)); }
    } finally { polling.current = false; }
  }, [ready]);
  useEffect(() => {
    ++generation.current;
    setPumps([null, null]);
    if (!ready) return;
    void refresh();
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
  const unitSelect = (value: number, onChange: (value: number) => void, rate: boolean, rpm = false) => <select value={value} disabled={configureDisabled} onChange={e => onChange(Number(e.target.value))}>{rpm && <option value={RPM_UNIT}>rpm (head)</option>}<option value={100}>{rate ? 'µL/min' : 'µL'}</option><option value={103}>{rate ? 'mL/min' : 'mL'}</option></select>;
  const autofocus = capabilities.autofocus;
  return <div className="hardware-pumps">
    <p>Manual run and purge{autofocus ? ', enable and jog' : ''} require Service / Commissioning mode and arming. Stop{autofocus ? ' and disable' : ''} remain{autofocus ? '' : 's'} available during experiments.</p>
    {autofocus
      ? <p>Pumps accept system serial endpoints; devices may share a bus at distinct Modbus addresses. Nanopositioners support CoreMOR and OEABT identities.</p>
      : instrumentPump && <p>This instrument's peristaltic pumps share {instrumentPump.port}: Sample at Modbus address {instrumentPump.sample_address}, Sheath at {instrumentPump.sheath_address}.</p>}
    {gate('configure') && <p role="status">{gate('configure')}</p>}
    {error && <p role="alert">{error}</p>}{statusError && <p role="alert">Status unavailable: {statusError}</p>}
    {pumps.map((status, id) => {
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
      return <fieldset key={id}><legend>{id === 0 ? 'Sample pump' : 'Sheath pump'}</legend>
        <p>{status ? `${connected ? 'Connected' : 'Disconnected'} · ${['Stopped', 'Forward', 'Backward', 'Paused'][status.run_status] ?? `Unknown state ${status.run_status}`} · ${status.stalled ? 'STALLED' : 'No stall reported'}` : 'Status unknown'}</p>
        {connected && status && <p>{peristaltic ? 'Peristaltic' : 'Syringe'} · {status.port_name || `COM ${status.com_port}`} · Address {status.modbus_address} · Configured rate {status.configured_flow_rate} ({status.flow_rate_unit === 100 ? 'µL/min' : status.flow_rate_unit === 103 ? 'mL/min' : `unit ${status.flow_rate_unit}`}) · Direction {status.direction === 0 ? 'Infuse' : status.direction === 1 ? 'Withdraw' : 'Unknown'} · Live rate {status.current_flow_rate}{peristaltic ? ' µL/min' : ''} · {peristaltic
          ? `Head ${(status.speed_rpm ?? 0).toFixed(2)} rpm at ${status.microliters_per_rev ?? '?'} µL/rev · Delivered ≈ ${status.accumulated_volume.toFixed(1)} µL (estimated from speed)`
          : `Accumulated volume ${status.accumulated_volume} (device units)`}</p>}
        <div className="hardware-fields">
          <label>Pump model<select value={model} disabled={configureDisabled || connected} onChange={e => selectModel(Number(e.target.value))}>
            <option value={PUMP_MODELS.DlspSyringe}>Syringe (Longer dLSP)</option>
            <option value={PUMP_MODELS.TushuiPeristaltic}>Peristaltic (Tushui)</option>
          </select></label>
          {peristaltic && <label>Calibration (µL per revolution)<input type="number" min="0" step="any" value={calibrations[id]} disabled={configureDisabled || connected} onChange={e => update(setCalibrations, calibrations, id, e.target.value)} /></label>}
        </div>
        <ConnectionFields systemPort value={connections[id]} disabled={configureDisabled || connected} onChange={v => update(setConnections, connections, id, v)} />
        <div className="hardware-actions">
          <button disabled={configureDisabled || !status || connected} onClick={() => void run('Connect pump', 'configure', false, () => {
            const connection = connections[id]; const port = connection.port.trim(); if (!port) throw new Error('System serial port is required');
            const microlitersPerRev = peristaltic ? numericInput(calibrations[id], 'Calibration', 0.001, 100000) : Number(DEFAULT_MICROLITERS_PER_REV);
            return bridge.pumpConnectModel(id, model, /^\d+$/.test(port) ? `COM${port}` : port, numericInput(connection.baud, 'Baud rate', 1, 4000000, true), numericInput(connection.address, 'Address', 1, 247, true), microlitersPerRev);
          })}>Connect</button>
          <button disabled={configureDisabled || !connected} onClick={() => void run('Disconnect pump', 'configure', connected, () => bridge.pumpDisconnect(id))}>Disconnect</button>
          <button disabled={busy || !ready || !connected} onClick={() => void run('Poll pump', 'stop', connected, () => bridge.pumpPollStatus(id))}>Read device status</button>
        </div>
        <div className="hardware-fields">
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
        </div>
        {actuateReason && <p>{actuateReason}</p>}
        <div className="hardware-actions">
          <button disabled={busy || !!actuateReason || !stopped} onClick={() => void run('Start pump', 'actuate', connected, () => bridge.pumpStart(id))}>Run configured pump</button>
          <button disabled={busy || !!actuateReason || !stopped} onClick={() => void run('Purge pump', 'actuate', connected, () => bridge.pumpPurge(id, directions[id]))}>Purge {directions[id] === 0 ? 'infuse' : 'withdraw'}{peristaltic ? (directions[id] === 0 ? ' (CCW)' : ' (CW)') : ''}</button>
          <button disabled={busy || !ready} onClick={() => void run('Stop pump', 'stop', connected, () => bridge.pumpStop(id))}>Stop pump</button>
          <button disabled={busy || !ready} onClick={() => void run('Stop purge', 'stop', connected, () => bridge.pumpStopPurge(id))}>Stop purge</button>
        </div>
      </fieldset>;
    })}
  </div>;
}
