import {useEffect, useRef, useState, type ReactNode} from 'react';
import {bridge, type PulseGeneratorStatus} from '../bridge';
import type {OperatingMode} from '../commissioning';
import {EndpointDiscovery} from './EndpointDiscovery';
import {hardwareGate, HardwareCommandOwner, numericInput} from './hardwareControlModel';

/** The pulse generator's parts (#798): the Hardware setup page places them in a card; the default
 *  layout keeps today's fieldset. Every field and button is in exactly one part. */
export interface PulseGeneratorParts {
  connected: boolean;
  status: ReactNode;
  /** Endpoint discovery and the serial port, baud and Modbus address fields. */
  setup: ReactNode;
  /** Connect / Disconnect / Read status (a read). */
  connection: ReactNode;
  /** Channel, its readback, frequency and duty with their Set buttons (actuate). */
  timing: ReactNode;
  actuateReason: string;
  /** Enable pulse output (gated), Disable pulse output (always, unless owned). */
  actions: {enable: ReactNode; disable: ReactNode};
  error: string;
}
export type PulseGeneratorLayout = (parts: PulseGeneratorParts) => ReactNode;

type Props = {ready: boolean; experimentActive: boolean; mode: OperatingMode; armed: boolean; onDisarm: () => void; append: (message: string) => void; layout?: PulseGeneratorLayout};

function defaultLayout(p: PulseGeneratorParts): ReactNode {
  return <fieldset><legend>Acquisition pulse generator (RS485)</legend>
    {p.status}
    <p>This controls camera acquisition pulses, not sorter trigger pulses. Disconnect does not stop an existing output; disable each channel first.</p>
    {p.error && <p role="alert">{p.error}</p>}
    {p.setup}
    {p.connection}
    {p.timing}
    {p.actions.enable}
    {p.actions.disable}
    {p.actuateReason && <p>{p.actuateReason}</p>}
  </fieldset>;
}

export function PulseGeneratorControls({ready, experimentActive, mode, armed, onDisarm, append, layout}: Props) {
  const [status, setStatus] = useState<PulseGeneratorStatus | null>(null);
  const [port, setPort] = useState(''), [baud, setBaud] = useState('9600'), [address, setAddress] = useState('1');
  const [channel, setChannel] = useState(0), [frequency, setFrequency] = useState('5000'), [duty, setDuty] = useState('50');
  const [error, setError] = useState(''), [busy, setBusy] = useState(false);
  const owner = useRef(new HardwareCommandOwner());
  const alive = useRef(true);
  useEffect(() => {
    alive.current = true;
    let polling = false;
    setStatus(null);
    const poll = async () => {
      if (!ready || polling) return;
      polling = true;
      try { const next = await bridge.pulseGeneratorStatus(); if (alive.current) setStatus(next.valid ? next : null); }
      catch { if (alive.current) setStatus(null); }
      finally { polling = false; }
    };
    void poll(); const timer = window.setInterval(() => void poll(), 1000);
    return () => { alive.current = false; window.clearInterval(timer); };
  }, [ready]);
  const gate = (kind: 'configure' | 'actuate' | 'stop') => hardwareGate({ready, experimentActive, mode, armed, connected: !!status?.connected}, kind);
  const disabled = (kind: 'configure' | 'actuate' | 'stop') => busy || !!gate(kind) || (kind !== 'stop' && (!status || status.owned));
  async function run(kind: 'configure' | 'actuate' | 'stop', request: () => Record<string, unknown>) {
    if (disabled(kind)) return;
    setBusy(true); setError('');
    try {
      const body = request();
      const result = await owner.current.run(() => { if (kind === 'actuate') onDisarm(); return bridge.pulseGeneratorCommand(body); });
      append(result.message); setStatus(await bridge.pulseGeneratorStatus());
    } catch (failure) { setError(String(failure)); }
    finally { setBusy(false); }
  }
  /** Read the status now (a read: no command is sent). */
  async function readStatus() {
    if (!ready) return;
    try { const next = await bridge.pulseGeneratorStatus(); if (alive.current) setStatus(next.valid ? next : null); }
    catch (failure) { if (alive.current) setError(String(failure)); }
  }
  const parts: PulseGeneratorParts = {
    connected: !!status?.connected,
    error,
    actuateReason: gate('actuate'),
    status: <p>{status ? `${status.connected ? 'Connected' : 'Disconnected'} · ${status.port} · ${status.error}${status.owned ? ' · Owned by coordinated live view' : ''}` : 'Status unavailable'}</p>,
    setup: <>
      <EndpointDiscovery disabled={disabled('configure') || !!status?.connected} request={() => ({kinds: [3], hasSerialScope: true, serialPortName: port.trim(), baudRate: numericInput(baud, 'Baud', 1, 4000000, true), addressFrom: numericInput(address, 'Address', 1, 247, true), addressTo: numericInput(address, 'Address', 1, 247, true), origin: 'tauri-pulse-picker'})} onSelect={device => {setPort(device.system_path); setAddress(String(device.bus_address));}} />
      <label>System serial port<input value={port} disabled={disabled('configure')} onChange={e => setPort(e.target.value)} placeholder="COM3 or /dev/ttyUSB0" /></label>
      <label>Baud<input value={baud} disabled={disabled('configure')} onChange={e => setBaud(e.target.value)} /></label>
      <label>Modbus address<input value={address} disabled={disabled('configure')} onChange={e => setAddress(e.target.value)} /></label>
    </>,
    connection: <>
      <button disabled={disabled('configure') || !!status?.connected} onClick={() => void run('configure', () => ({action: 'connect', port: port.trim(), baud: numericInput(baud, 'Baud', 1, 4000000, true), address: numericInput(address, 'Address', 1, 247, true)}))}>Connect pulse generator</button>
      <button disabled={disabled('configure') || !status?.connected} onClick={() => void run('configure', () => ({action: 'disconnect'}))}>Disconnect pulse generator</button>
      {layout && <button disabled={!ready} onClick={() => void readStatus()}>Read status</button>}
    </>,
    timing: <>
      <label>Channel<select value={channel} disabled={busy} onChange={e => setChannel(Number(e.target.value))}>{[0,1,2,3].map(i => <option key={i} value={i}>{i + 1}</option>)}</select></label>
      <p>{status?.channels[channel] && `${status.channels[channel].frequency_hz} Hz · ${status.channels[channel].duty_percent}% · ${status.channels[channel].output_enabled ? 'Output ON' : 'Output OFF'}`}</p>
      <label>Frequency (Hz)<input value={frequency} onChange={e => setFrequency(e.target.value)} /></label>
      <button disabled={disabled('actuate')} onClick={() => void run('actuate', () => ({action: 'frequency', channel, value: numericInput(frequency, 'Frequency', 400, 40000)}))}>Set frequency</button>
      <label>Duty (%)<input value={duty} onChange={e => setDuty(e.target.value)} /></label>
      <button disabled={disabled('actuate')} onClick={() => void run('actuate', () => ({action: 'duty', channel, value: numericInput(duty, 'Duty', 0, 100)}))}>Set duty</button>
    </>,
    actions: {
      enable: <button disabled={disabled('actuate')} onClick={() => void run('actuate', () => ({action: 'enable', channel, enabled: true}))}>Enable pulse output</button>,
      disable: <button disabled={disabled('stop') || !!status?.owned} onClick={() => void run('stop', () => ({action: 'enable', channel, enabled: false}))}>Disable pulse output</button>,
    },
  };
  return <>{layout ? layout(parts) : defaultLayout(parts)}</>;
}
