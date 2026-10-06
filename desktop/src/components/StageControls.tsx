import { useCallback, useEffect, useRef, useState } from 'react';
import { bridge, type CmdResult, type StageStatus } from '../bridge';
import { DISCOVERY_DEVICE_KINDS } from '../bridgeContract';
import { DEFAULT_MODE, type OperatingMode } from '../commissioning';
import { EndpointDiscovery } from './EndpointDiscovery';
import { HardwareCommandOwner, numericInput } from './hardwareControlModel';
import {
  JOG_STEPS_UM, MID_TRAVEL_LABEL, SET_ZERO_WARNING, connectionText, envelopeProblem, limitWiringText, moveStateText,
  parseMicrons, pollIntervalMs, positionText, stageGate, stageIndicators, zeroText, type StageGateKind,
} from './stageControlModel';
import './HardwareControls.css';
import './StageControls.css';

type Props = {
  ready: boolean;
  experimentActive: boolean;
  append: (message: string) => void;
  mode?: OperatingMode;
  armed?: boolean;
  onDisarm: () => void;
};

/** Z stage (ZC300 + TBZF6-60, #464, ADR 0013 Amendment 1). Positions are micrometres.
 *  The stage is never homed: the operator sets zero and the backend bounds travel around it. The backend
 *  enforces the safety rules; this panel mirrors them so the operator sees why a control is disabled. Nothing
 *  here moves or writes on mount, on connect, or on reconnect. */
export function StageControls({ ready, experimentActive, append, mode = DEFAULT_MODE, armed = false, onDisarm }: Props) {
  const [status, setStatus] = useState<StageStatus | null>(null);
  const [statusError, setStatusError] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const [port, setPort] = useState('');
  const [usbSerial, setUsbSerial] = useState('');
  const [address, setAddress] = useState('1');
  const [step, setStep] = useState<number>(10);
  const [target, setTarget] = useState('');
  const [zeroConfirming, setZeroConfirming] = useState(false);
  const [midTravel, setMidTravel] = useState(false);
  const owner = useRef(new HardwareCommandOwner());
  const polling = useRef(false);
  const generation = useRef(0);

  const refresh = useCallback(async () => {
    if (polling.current || !ready) return;
    polling.current = true;
    const current = generation.current;
    try {
      const next = await bridge.fetchStageStatus();
      if (current !== generation.current) return;
      if (!next.valid) throw new Error('Stage status is unavailable.');
      setStatus(next); setStatusError('');
    } catch (failure) {
      if (current === generation.current) { setStatus(null); setStatusError(String(failure)); }
    } finally { polling.current = false; }
  }, [ready]);

  useEffect(() => {
    const current = ++generation.current;
    setStatus(null); setStatusError('');
    if (!ready) return;
    void refresh();
    return () => { if (generation.current === current) ++generation.current; };
  }, [ready, refresh]);
  const interval = pollIntervalMs(status);
  useEffect(() => {
    if (!ready) return;
    const timer = window.setInterval(() => void refresh(), interval);
    return () => window.clearInterval(timer);
  }, [ready, refresh, interval]);

  const gate = (kind: StageGateKind) => stageGate({ ready, experimentActive, mode, armed, status }, kind);

  /** `prepare` validates the inputs and returns the bridge call. It runs before the arming is consumed, so a
   *  mistyped target costs the operator nothing: only a command that is really sent disarms. */
  async function run(label: string, kind: StageGateKind, prepare: () => () => Promise<CmdResult>) {
    const reason = gate(kind);
    if (reason) { setError(reason); return; }
    if (busy) return;
    setBusy(true); setError('');
    try {
      const command = prepare();
      const result = await owner.current.run(() => {
        if (kind === 'move' || kind === 'set-zero') onDisarm(); // one-shot arming, like the pumps
        return command();
      });
      append(`${label}: ${result.message || 'Command accepted'}`);
      await refresh();
    } catch (failure) {
      const message = `${label}: ${String(failure)}`;
      setError(message); append(message);
    } finally { setBusy(false); }
  }

  /** Stop never waits for the panel's own command lock: a pending command must not keep it from firing. */
  async function stop() {
    if (!ready) return;
    setError('');
    try {
      const result = await bridge.stageStop();
      if (!result.ok) throw new Error(result.message || 'Stop failed.');
      append(`Stop stage: ${result.message || 'Stopped'}`);
    } catch (failure) {
      const message = `Stop stage: ${String(failure)}`;
      setError(message); append(message);
    }
    await refresh();
  }

  const connected = !!status?.connected;
  const moveReason = gate('move');
  const zeroReason = gate('set-zero');
  const connectReason = gate('connect');
  const disconnectReason = gate('disconnect');
  const applyReason = gate('apply-profile');
  const moveDisabled = busy || !!moveReason;

  const jog = (sign: 1 | -1) => run('Move stage', 'move', () => {
    const delta = sign * step;
    const problem = status ? envelopeProblem(status, delta, true) : '';
    if (problem) throw new Error(problem);
    return () => bridge.stageMoveBy(delta);
  });
  const goTo = () => run('Move stage', 'move', () => {
    const value = parseMicrons(target, 'Target position');
    const problem = status ? envelopeProblem(status, value, false) : '';
    if (problem) throw new Error(problem);
    return () => bridge.stageMoveTo(value);
  });
  const setZero = () => {
    const declared = midTravel;
    setZeroConfirming(false); setMidTravel(false);
    return run('Set zero', 'set-zero', () => () => bridge.stageSetZero(declared));
  };

  return <section className="hardware-controls stage-controls" aria-label="Z stage controls">
    <h2>Z stage</h2>
    <p>Setting zero and moves need Service / Commissioning mode and arming, and moves need the zero to be set first. The stage is never homed. Stop is always available, also during an experiment. Positions are in micrometres.</p>
    {error && <p role="alert">{error}</p>}
    {statusError && <p role="alert">Status unavailable: {statusError}</p>}
    {status?.last_error && <p role="status">Last stage error: {status.last_error}</p>}

    <fieldset><legend>Connection</legend>
      <p>{connectionText(status)}</p>
      {status && !status.enabled && !connected && <p>No stage is enabled in the active profile; you can still connect manually.</p>}
      {connected && !status?.configured && <p role="status">The controller settings do not match the stage profile, so motion is refused until you apply the stage settings.</p>}
      <EndpointDiscovery disabled={busy || !!connectReason} request={() => {
        const scanPort = port.trim();
        if (!scanPort) throw new Error('Enter the serial port to scan first (for example /dev/ttyUSB1 or COM7).');
        const addr = numericInput(address, 'Address', 1, 247, true);
        return { kinds: [DISCOVERY_DEVICE_KINDS.MotionStage], hasSerialScope: true, serialPortName: scanPort, baudRate: 115200, addressFrom: addr, addressTo: addr, origin: 'tauri-stage-picker' };
      }} onSelect={device => { setPort(device.system_path); setUsbSerial(device.persistent_id); setAddress(String(device.bus_address)); }} />
      <div className="hardware-fields">
        <label>System serial port<input value={port} disabled={busy || !!connectReason} onChange={e => setPort(e.target.value)} placeholder="/dev/ttyUSB1 or COM7" /></label>
        <label>USB serial number<input value={usbSerial} disabled={busy || !!connectReason} onChange={e => setUsbSerial(e.target.value)} placeholder="e.g. A10RB8XC" /></label>
        <label>Modbus address<input type="number" min="1" max="247" value={address} disabled={busy || !!connectReason} onChange={e => setAddress(e.target.value)} /></label>
      </div>
      <p>Leave the port and USB serial number blank to use the endpoint from the profile's stage settings. Connecting only reads the controller; it never moves the stage.</p>
      <div className="hardware-actions">
        <button disabled={busy || !!connectReason} onClick={() => void run('Connect stage', 'connect', () => { const addr = numericInput(address, 'Address', 1, 247, true); return () => bridge.stageConnect(port.trim(), usbSerial.trim(), addr); })}>Connect stage</button>
        <button disabled={busy || !!disconnectReason} onClick={() => void run('Disconnect stage', 'disconnect', () => () => bridge.stageDisconnect())}>Disconnect stage</button>
        {connected && !status?.configured && <button disabled={busy || !!applyReason} onClick={() => void run('Apply stage settings', 'apply-profile', () => () => bridge.stageApplyProfile())}>Apply stage settings</button>}
      </div>
      {connectReason && !connected && <p>{connectReason}</p>}
    </fieldset>

    <fieldset><legend>Position</legend>
      {status && connected ? <>
        <p className="stage-position" aria-label="Stage position">{positionText(status)}</p>
        <p>{zeroText(status)} · {moveStateText(status)}</p>
        <p>Limit switches: {limitWiringText(status)}</p>
        <ul className="stage-indicators" aria-label="Stage indicators">
          {stageIndicators(status).map(item => <li key={item.key} data-active={item.active} data-severity={item.alarm ? 'alarm' : 'info'}>{item.label}: {item.active ? 'ACTIVE' : 'clear'}</li>)}
        </ul>
      </> : <p>Connect the stage to see its position.</p>}
      <div className="hardware-actions">
        <button className="stage-stop" disabled={!ready} onClick={() => void stop()}>Stop stage</button>
      </div>
    </fieldset>

    <fieldset><legend>Zero</legend>
      <p>The stage is never homed. Set zero here tells the software that the stage is at 0 µm now; it moves nothing. It is needed once after the controller powers up, and after an emergency stop or driver alarm.</p>
      {!zeroConfirming
        ? <div className="hardware-actions"><button disabled={busy || !!zeroReason} onClick={() => setZeroConfirming(true)}>Set zero here…</button></div>
        : <div className="stage-zero-confirm" role="group" aria-label="Confirm Set zero">
            <p role="alert">{SET_ZERO_WARNING}</p>
            <label><input type="checkbox" checked={midTravel} onChange={e => setMidTravel(e.target.checked)} />{MID_TRAVEL_LABEL}</label>
            <div className="hardware-actions">
              <button disabled={busy || !!zeroReason} onClick={() => void setZero()}>Set zero here</button>
              <button onClick={() => { setZeroConfirming(false); setMidTravel(false); }}>Cancel</button>
            </div>
          </div>}
      {zeroReason && connected && <p>{zeroReason}</p>}
    </fieldset>

    <fieldset><legend>Move</legend>
      <div className="hardware-fields">
        <label>Step<select value={step} disabled={busy} onChange={e => setStep(Number(e.target.value))}>{JOG_STEPS_UM.map(v => <option key={v} value={v}>{v} µm</option>)}</select></label>
        <button disabled={moveDisabled} onClick={() => void jog(-1)}>Move −{step} µm</button>
        <button disabled={moveDisabled} onClick={() => void jog(1)}>Move +{step} µm</button>
      </div>
      <div className="hardware-fields">
        <label>Go to position (µm)<input type="number" step="1" value={target} disabled={busy} onChange={e => setTarget(e.target.value)} /></label>
        <button disabled={moveDisabled} onClick={() => void goTo()}>Go to position</button>
      </div>
      {moveReason && connected && <p>{moveReason}</p>}
    </fieldset>
  </section>;
}
