import { useCallback, useEffect, useRef, useState } from 'react';
import { bridge, type CmdResult, type PlatformCapabilities, type PumpStatus } from '../bridge';
import { DESKTOP_CAPABILITIES } from '../platformCapabilities';
import { PUMP_MODELS } from '../bridgeContract';
import { numericInput, operatorGate } from './hardwareControlModel';
import { ConnectionFields, initialConnection, type Connection } from './HardwareConnectionFields';
import {
  AUTO_APPLY_MS, BAUD_MAX, BAUD_MIN, BAUD_RATES, POLL_MS, PURGE_TOOLTIP, RATE_DECIMALS, RATE_MAX, UNIT_ML, UNIT_UL, VOLUME_DECIMALS, VOLUME_MAX,
  VOLUME_MIN, CommandCancelled, PumpCommandQueue, PurgeHold, errorText, formatRate, parseDecimal, syringeRateForDevice, syringeVolumeForDevice,
} from './pumpControlModel';
import { MICROLITERS_PER_MIN_UNIT, RPM_UNIT, directionLabel, flowToRpm, rpmToMicrolitersPerMin } from '../pumpRate';
import './HardwareControls.css';

type Props = { ready: boolean; append: (message: string) => void; capabilities?: PlatformCapabilities };
// 0.4 rpm delivers 10 µL/min on the instrument tubing until a measured calibration replaces it.
const DEFAULT_MICROLITERS_PER_REV = '25';
// Where the PZ7035 instrument's peristaltic pumps answer (PS UART1 RS485): Sample slave 3, Sheath slave 4 (#501).
const PERISTALTIC_DEFAULT_ENDPOINTS = [{port: '/dev/ttyPS1', address: '3'}, {port: '/dev/ttyPS1', address: '4'}];
// Qt's defaults: the Sample pump answers at Modbus address 1, the Sheath pump at 2 (SyringePumpSettingsDialog.ui).
const defaultAddress = (slot: number) => String(slot + 1);
const SKIPPED: CmdResult = {ok: true, command: 10, message: 'Purge released before it started', operation_id: '0'};
const STATE_NAMES = ['Stopped', 'Forward', 'Backward', 'Paused'];
const unitLabel = (unit: number) => unit === UNIT_UL ? 'µL/min' : unit === UNIT_ML ? 'mL/min' : `unit ${unit}`;

/** Sample and Sheath pump panels (SyringePumpTab and its settings dialog in Qt). They are operator controls, as in Qt: no
 *  Service mode, no arm, no experiment lock (D1). Every connected pump is read from the device every 500 ms (HW-015); the
 *  read never takes a command lock, so Stop is never stuck behind it (#855). */
export function PumpControls({ready, append, capabilities = DESKTOP_CAPABILITIES}: Props) {
  const [pumps, setPumps] = useState<Array<PumpStatus | null>>([null, null]);
  const [statusError, setStatusError] = useState('');
  const known = useRef<Array<PumpStatus | null>>([null, null]);
  const polling = useRef(false);
  const generation = useRef(0);
  /** Read each connected pump from the device (Qt pollStatus), then take the status. Never queued, never `busy`, never overlapping. */
  const poll = useCallback(async () => {
    if (polling.current || !ready) return;
    polling.current = true;
    const current = generation.current;
    try {
      const next = await Promise.all([0, 1].map(async slot => {
        if (known.current[slot]?.connected) await bridge.pumpPollStatus(slot);
        return bridge.fetchPumpStatus(slot);
      }));
      if (current !== generation.current) return;
      if (next.some(status => !status.valid)) throw new Error('Hardware status is unavailable.');
      known.current = next; setPumps(next); setStatusError('');
    } catch (failure) {
      if (current === generation.current) { known.current = [null, null]; setPumps([null, null]); setStatusError(errorText(failure)); }
    } finally { polling.current = false; }
  }, [ready]);
  useEffect(() => {
    ++generation.current;
    known.current = [null, null]; setPumps([null, null]);
    if (!ready) return;
    void poll();
    const timer = window.setInterval(() => void poll(), POLL_MS);
    return () => { ++generation.current; window.clearInterval(timer); };
  }, [ready, poll]);
  const instrumentPump = capabilities.pump;
  return <div className="hardware-pumps">
    <p>Pump controls work as in the Qt app and need no Service mode: connect, flow rate, direction, syringe volume, Start and Stop stay available while an experiment runs. As in Qt, a flow rate is applied {AUTO_APPLY_MS} ms after you press Enter or leave the field, a unit or direction change {AUTO_APPLY_MS} ms after the change, also while a pump runs. Hold Purge to purge at full speed; letting go stops it. Stop is always available.</p>
    {capabilities.autofocus
      ? <p>Pumps accept system serial endpoints; devices may share a bus at distinct Modbus addresses.</p>
      : instrumentPump && <p>This instrument's peristaltic pumps share {instrumentPump.port}: Sample at Modbus address {instrumentPump.sample_address}, Sheath at {instrumentPump.sheath_address}.</p>}
    {!ready && <p role="status">Backend is not ready.</p>}
    {statusError && <p role="alert">Status unavailable: {statusError}</p>}
    <datalist id="pump-baud-rates">{BAUD_RATES.map(rate => <option key={rate} value={rate} />)}</datalist>
    {pumps.map((status, slot) => <PumpPanel key={slot} slot={slot} status={status} ready={ready} append={append} capabilities={capabilities} onCommandDone={() => void poll()} />)}
  </div>;
}

type PanelProps = { slot: number; status: PumpStatus | null; ready: boolean; append: (message: string) => void; capabilities: PlatformCapabilities; onCommandDone: () => void };
type Pending = { rate: boolean; direction: boolean };

function PumpPanel({slot, status, ready, append, capabilities, onCommandDone}: PanelProps) {
  const instrumentPump = capabilities.pump;
  const [model, setModel] = useState<number>(instrumentPump ? PUMP_MODELS.TushuiPeristaltic : PUMP_MODELS.DlspSyringe);
  const [calibration, setCalibration] = useState(instrumentPump ? String(instrumentPump.microliters_per_rev) : DEFAULT_MICROLITERS_PER_REV);
  const peristalticConnection = (): Connection => instrumentPump
    ? {port: instrumentPump.port, baud: '115200', address: String(slot === 0 ? instrumentPump.sample_address : instrumentPump.sheath_address)}
    : initialConnection(defaultAddress(slot));
  const [connection, setConnection] = useState<Connection>(peristalticConnection);
  const [rateText, setRateText] = useState('');
  const [unit, setUnit] = useState(UNIT_UL);
  const [direction, setDirection] = useState(0);
  const [volumeText, setVolumeText] = useState('');
  const [volumeUnit, setVolumeUnit] = useState(UNIT_UL);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const connected = !!status?.connected;
  const effectiveModel = connected && status?.model !== undefined ? status.model : model;
  const peristaltic = effectiveModel === PUMP_MODELS.TushuiPeristaltic;
  // The rpm unit exists for the peristaltic pump only; a syringe pump never keeps it (HW-070).
  const rateUnit = unit === RPM_UNIT && !peristaltic ? UNIT_UL : unit;
  // The capability report arrives after mount: adopt the instrument's pump once.
  useEffect(() => {
    if (!instrumentPump) return;
    setModel(PUMP_MODELS.TushuiPeristaltic);
    setCalibration(String(instrumentPump.microliters_per_rev));
    setConnection(peristalticConnection());
  }, [instrumentPump?.port, instrumentPump?.sample_address, instrumentPump?.sheath_address, instrumentPump?.microliters_per_rev]); // eslint-disable-line react-hooks/exhaustive-deps

  // The newest values, for the debounce timer and the purge hooks that outlive a render.
  const latest = useRef({rateText, rateUnit, direction, calibration, status, connected, ready, slot, append, onCommandDone});
  latest.current = {rateText, rateUnit, direction, calibration, status, connected, ready, slot, append, onCommandDone};
  const queue = useRef(new PumpCommandQueue());
  const pending = useRef<Pending>({rate: false, direction: false});
  const timer = useRef<number | undefined>(undefined);
  const wasConnected = useRef<boolean | null>(null);
  /** The rate text changed since it was last committed (Enter or focus out). */
  const rateDirty = useRef(false);

  const fail = (label: string, failure: unknown) => { const message = `${label}: ${errorText(failure)}`; setError(message); latest.current.append(message); };
  /** A queued command checks, right before it is sent, that its pump is still there: a status that turned unknown or
   *  disconnected while it waited cancels it. */
  const needsPump = (command: () => Promise<CmdResult>) => () => {
    if (!latest.current.ready || !latest.current.connected) throw new CommandCancelled('cancelled: the pump is not connected');
    return command();
  };
  /** Queued like every non-stop command, but without `busy`: typing into the rate field must not disable it. */
  async function applyQueued(label: string, command: () => Promise<CmdResult>) {
    setError('');
    try {
      const result = await queue.current.run(needsPump(command));
      latest.current.append(`${label}: ${result.message || 'Command accepted'}`);
      latest.current.onCommandDone();
    } catch (failure) { fail(label, failure); }
  }
  const sendRate = () => {
    const {rateText: text, rateUnit: chosen, status: current, calibration: perRev} = latest.current;
    if (!text.trim()) return Promise.resolve(); // nothing typed yet: a unit change alone has nothing to send
    return applyQueued('Set flow rate', async () => {
      if (chosen === RPM_UNIT) {
        const rpm = parseDecimal(text, 'Head speed', 0, RATE_MAX, RATE_DECIMALS);
        // The backend takes a flow: rpm x the calibration the pump was connected with (the status reports it; 0 means unknown).
        const calibrated = current?.microliters_per_rev && current.microliters_per_rev > 0 ? current.microliters_per_rev : Number(perRev);
        return bridge.pumpSetFlowRate(slot, rpmToMicrolitersPerMin(rpm, calibrated), MICROLITERS_PER_MIN_UNIT);
      }
      const rate = parseDecimal(text, 'Flow rate', 0, RATE_MAX, RATE_DECIMALS);
      if (current?.model === PUMP_MODELS.TushuiPeristaltic) return bridge.pumpSetFlowRate(slot, rate, chosen);
      const exact = syringeRateForDevice(rate, chosen);
      return bridge.pumpSetFlowRate(slot, exact.value, exact.unit);
    });
  };
  const sendDirection = () => applyQueued('Set direction', () => bridge.pumpSetDirection(slot, latest.current.direction));
  /** Qt's 300 ms timer firing: send what the operator committed, rate before direction. */
  const flushNow = () => {
    window.clearTimeout(timer.current); timer.current = undefined;
    const todo = pending.current; pending.current = {rate: false, direction: false};
    if (!latest.current.connected || !latest.current.ready) return;
    if (todo.rate) void sendRate();
    if (todo.direction) void sendDirection();
  };
  const edited = (what: keyof Pending) => {
    if (!latest.current.connected) return;
    pending.current[what] = true;
    window.clearTimeout(timer.current);
    timer.current = window.setTimeout(flushNow, AUTO_APPLY_MS);
  };
  const flushNowRef = useRef(flushNow); flushNowRef.current = flushNow;

  // Hold-to-purge (HW-012). The hooks read `latest`, so one PurgeHold serves every render.
  const purge = useRef<PurgeHold | null>(null);
  if (!purge.current) purge.current = new PurgeHold({
    start: async attempt => {
      const result = await queue.current.run(async () => {
        if (attempt.cancelled()) return SKIPPED;
        return needsPump(async () => { attempt.markSent(); return bridge.pumpPurge(latest.current.slot, latest.current.direction); })();
      });
      if (result !== SKIPPED) latest.current.append(`Purge pump: ${result.message || 'Command accepted'}`);
      return true;
    },
    stop: async () => {
      const result = await bridge.pumpStopPurge(latest.current.slot);
      if (!result.ok) throw new Error(result.message || 'Purge stop failed');
      latest.current.append(`Stop purge: ${result.message || 'Command accepted'}`);
      latest.current.onCommandDone();
    },
    onError: message => fail('Purge pump', message),
  });
  const holdPurge = () => {
    if (!latest.current.ready || !latest.current.connected) return;
    flushNowRef.current(); setError('');
    purge.current!.press();
  };
  const releasePurge = () => purge.current!.release();
  useEffect(() => {
    // The pointer may be released anywhere, and the window may lose focus mid-hold: never leave a pump purging.
    const release = () => purge.current!.release();
    const hidden = () => { if (document.hidden) release(); };
    document.addEventListener('pointerup', release); document.addEventListener('pointercancel', release);
    window.addEventListener('blur', release); document.addEventListener('visibilitychange', hidden);
    return () => {
      document.removeEventListener('pointerup', release); document.removeEventListener('pointercancel', release);
      window.removeEventListener('blur', release); document.removeEventListener('visibilitychange', hidden);
      window.clearTimeout(timer.current);
      queue.current.invalidate('cancelled: the panel was closed');
      release();
    };
  }, []);

  // A pump that just connected shows its configured rate, unit and direction (Qt shows the saved ones); a pump that went
  // away drops the pending edits and any hold. A status read that merely failed (null) changes nothing.
  useEffect(() => {
    if (!status) return;
    if (status.connected && wasConnected.current !== true) {
      setRateText(status.configured_flow_rate > 0 ? formatRate(status.configured_flow_rate) : '');
      if (status.flow_rate_unit === UNIT_UL || status.flow_rate_unit === UNIT_ML) setUnit(status.flow_rate_unit);
      setDirection(status.direction === 1 ? 1 : 0);
      rateDirty.current = false;
    }
    if (!status.connected && wasConnected.current === true) {
      window.clearTimeout(timer.current); timer.current = undefined;
      pending.current = {rate: false, direction: false};
      queue.current.invalidate('cancelled: the pump disconnected');
      purge.current!.abandon();
    }
    wasConnected.current = status.connected;
  }, [status]);

  async function runButton(label: string, command: () => Promise<CmdResult>, needsConnected = true) {
    if (busy) return;
    setBusy(true); setError('');
    try {
      const result = await queue.current.run(needsConnected ? needsPump(command) : command);
      append(`${label}: ${result.message || 'Command accepted'}`);
      onCommandDone();
    } catch (failure) { fail(label, failure); }
    finally { setBusy(false); }
  }
  const connect = () => runButton('Connect pump', async () => {
    const port = connection.port.trim(); if (!port) throw new Error('System serial port is required');
    const portName = /^\d+$/.test(port) ? `COM${port}` : port;
    const baud = numericInput(connection.baud, 'Baud rate', BAUD_MIN, BAUD_MAX, true);
    const address = numericInput(connection.address, 'Address', 1, 247, true);
    // A syringe pump needs no µL/rev calibration; the backend defaults it and ignores it.
    return peristaltic
      ? bridge.pumpConnectModel(slot, effectiveModel, portName, baud, address, numericInput(calibration, 'Calibration', 0.001, 100000))
      : bridge.pumpConnectEndpoint(slot, portName, baud, address);
  }, false);
  const disconnect = () => {
    queue.current.invalidate('cancelled by Disconnect');
    window.clearTimeout(timer.current); timer.current = undefined; pending.current = {rate: false, direction: false};
    return runButton('Disconnect pump', () => bridge.pumpDisconnect(slot));
  };
  const setVolume = () => runButton('Set syringe volume', async () => {
    const exact = syringeVolumeForDevice(parseDecimal(volumeText, 'Syringe volume', VOLUME_MIN, VOLUME_MAX, VOLUME_DECIMALS), volumeUnit);
    return bridge.pumpSetSyringeVolume(slot, exact.value, exact.unit);
  });
  /** Stop never waits for the command queue, `busy` or the connection state (#855). It also clears a full-speed purge, which
   *  the dLSP keeps in its own register; a lost purge release must not leave a pump running. */
  const stop = async () => {
    queue.current.invalidate('cancelled by Stop');   // nothing queued behind a slow command may start the pump after this Stop
    setError('');
    const problems: string[] = [];
    try {
      const result = await bridge.pumpStop(slot);
      if (!result.ok) throw new Error(result.message || 'Pump stop failed');
      append(`Stop pump: ${result.message || 'Command accepted'}`);
    } catch (failure) { problems.push(`Stop pump: ${errorText(failure)}`); }
    try {
      const result = await bridge.pumpStopPurge(slot);
      if (!result.ok) throw new Error(result.message || 'Purge stop failed');
    } catch (failure) { problems.push(`Stop purge: ${errorText(failure)}`); }
    if (problems.length) { setError(problems.join(' · ')); problems.forEach(message => append(message)); }
    onCommandDone();
  };
  const selectModel = (value: number) => {
    setModel(value);
    if (value !== PUMP_MODELS.TushuiPeristaltic && unit === RPM_UNIT) setUnit(UNIT_UL);
    const untouched = !connection.port.trim() && connection.address === defaultAddress(slot);
    if (value === PUMP_MODELS.TushuiPeristaltic && untouched) setConnection({...connection, ...PERISTALTIC_DEFAULT_ENDPOINTS[slot]});
  };
  const rateHint = (() => {
    if (!rateText.trim()) return null;
    try {
      if (rateUnit === RPM_UNIT) { parseDecimal(rateText, 'Head speed', 0, RATE_MAX, RATE_DECIMALS); return null; }
      const rate = parseDecimal(rateText, 'Flow rate', 0, RATE_MAX, RATE_DECIMALS);
      if (peristaltic) return null;
      const exact = syringeRateForDevice(rate, rateUnit);
      return exact.unit === rateUnit ? null : {problem: false, text: `Sent to the pump as ${exact.value} ${unitLabel(exact.unit)}.`};
    } catch (failure) { return {problem: true, text: errorText(failure)}; }
  })();
  const volumeHint = (() => {
    if (!volumeText.trim()) return null;
    try {
      const exact = syringeVolumeForDevice(parseDecimal(volumeText, 'Syringe volume', VOLUME_MIN, VOLUME_MAX, VOLUME_DECIMALS), volumeUnit);
      return exact.unit === volumeUnit ? null : {problem: false, text: `Sent to the pump as ${exact.value} ${exact.unit === UNIT_UL ? 'µL' : 'mL'}.`};
    } catch (failure) { return {problem: true, text: errorText(failure)}; }
  })();
  const control = ready && connected; // rate, unit, direction and volume: the pump must be there; nothing else gates them (D1)
  const startReason = operatorGate({ready, connected}, 'command');
  const configured = connected && status ? flowToRpm(status.configured_flow_rate, status.flow_rate_unit, status.microliters_per_rev) : null;
  const rateUnits = <select value={rateUnit} disabled={!control} onChange={e => { setUnit(Number(e.target.value)); edited('rate'); }}>
    {peristaltic && <option value={RPM_UNIT}>rpm (head)</option>}<option value={UNIT_UL}>µL/min</option><option value={UNIT_ML}>mL/min</option></select>;
  return <fieldset><legend>{slot === 0 ? 'Sample pump' : 'Sheath pump'}</legend>
    <p>{status ? `${connected ? 'Connected' : 'Disconnected'} · ${STATE_NAMES[status.run_status] ?? `Unknown state ${status.run_status}`} · ${status.stalled ? 'STALLED' : 'No stall reported'}` : 'Status unknown'}</p>
    {connected && status && <p>{peristaltic ? 'Peristaltic' : 'Syringe'} · {status.port_name || `COM ${status.com_port}`} · Address {status.modbus_address} · Configured rate {status.configured_flow_rate} ({unitLabel(status.flow_rate_unit)}) · Direction {status.direction === 0 ? 'Infuse' : status.direction === 1 ? 'Withdraw' : 'Unknown'} · Live rate {status.current_flow_rate}{peristaltic ? ' µL/min' : ''} · {peristaltic
      ? `Head ${(status.speed_rpm ?? 0).toFixed(2)} rpm at ${status.microliters_per_rev ?? '?'} µL/rev · Delivered ≈ ${status.accumulated_volume.toFixed(1)} µL (estimated from speed)`
      : `Accumulated volume ${status.accumulated_volume} (device units)`}</p>}
    {error && <p role="alert">{error}</p>}
    <div className="hardware-fields">
      <label>Pump model<select value={effectiveModel} disabled={connected} onChange={e => selectModel(Number(e.target.value))}>
        <option value={PUMP_MODELS.DlspSyringe}>Syringe (Longer dLSP)</option>
        <option value={PUMP_MODELS.TushuiPeristaltic}>Peristaltic (Tushui)</option>
      </select></label>
      {peristaltic && <label>Calibration (µL per revolution)<input type="number" min="0" step="any" value={calibration} disabled={connected} onChange={e => setCalibration(e.target.value)} /></label>}
    </div>
    <ConnectionFields systemPort value={connection} disabled={connected} onChange={setConnection} baud={{min: BAUD_MIN, max: BAUD_MAX, step: 1200, list: 'pump-baud-rates'}} />
    <div className="hardware-actions">
      <button data-tour={`pump-${slot}-connect`} disabled={busy || !ready || !status || connected} onClick={() => void connect()}>Connect</button>
      <button disabled={busy || !ready || !connected} onClick={() => disconnect()}>Disconnect</button>
    </div>
    <div className="hardware-fields">
      <label>Flow rate<input data-tour={`pump-${slot}-rate`} type="number" min="0" max={RATE_MAX} step="any" value={rateText} disabled={!control}
        onChange={e => { setRateText(e.target.value); rateDirty.current = true; }}
        onBlur={() => { if (rateDirty.current) { rateDirty.current = false; edited('rate'); } }}
        onKeyDown={e => { if (e.key === 'Enter') { rateDirty.current = false; edited('rate'); } }} /></label>
      <label>Rate unit{rateUnits}</label>
      {configured !== null && peristaltic && <span className="mono">configured ≈ {configured.toFixed(2)} rpm</span>}
      <label>Direction<select data-tour={`pump-${slot}-direction`} value={direction} disabled={!control} onChange={e => { setDirection(Number(e.target.value)); edited('direction'); }}><option value={0}>{directionLabel(peristaltic, 0)}</option><option value={1}>{directionLabel(peristaltic, 1)}</option></select></label>
    </div>
    {rateHint && <p className={rateHint.problem ? 'hardware-hint hardware-hint-problem' : 'hardware-hint'}>{rateHint.text}</p>}
    {!peristaltic && <div className="hardware-fields">
      <label>Syringe volume ({VOLUME_MIN}–{VOLUME_MAX})<input data-tour={`pump-${slot}-volume`} type="number" min={VOLUME_MIN} max={VOLUME_MAX} step="any" value={volumeText} disabled={!control} onChange={e => setVolumeText(e.target.value)} /></label>
      <label>Volume unit<select value={volumeUnit} disabled={!control} onChange={e => setVolumeUnit(Number(e.target.value))}><option value={UNIT_UL}>µL</option><option value={UNIT_ML}>mL</option></select></label>
      <button disabled={busy || !control || !volumeText.trim()} onClick={() => void setVolume()}>Apply volume</button>
    </div>}
    {!peristaltic && volumeHint && <p className={volumeHint.problem ? 'hardware-hint hardware-hint-problem' : 'hardware-hint'}>{volumeHint.text}</p>}
    <div className="hardware-actions">
      <button data-tour={`pump-${slot}-start`} disabled={busy || !!startReason} onClick={() => { flushNow(); void runButton('Start pump', () => bridge.pumpStart(slot)); }}>Run configured pump</button>
      <button className="purge-button" data-tour={`pump-${slot}-purge`} title={PURGE_TOOLTIP} disabled={!ready || !connected}
        onPointerDown={event => { if (!event.button) holdPurge(); }} onPointerUp={releasePurge} onPointerLeave={releasePurge} onPointerCancel={releasePurge} onBlur={releasePurge}
        onKeyDown={event => { if (event.key === ' ' || event.key === 'Enter') { event.preventDefault(); if (!event.repeat) holdPurge(); } }}
        onKeyUp={event => { if (event.key === ' ' || event.key === 'Enter') { event.preventDefault(); releasePurge(); } }}
        onContextMenu={event => event.preventDefault()}>Hold to purge {direction === 0 ? 'infuse' : 'withdraw'}{peristaltic ? (direction === 0 ? ' (CCW)' : ' (CW)') : ''}</button>
      <button data-tour={`pump-${slot}-stop`} disabled={!ready} onClick={() => void stop()}>Stop pump</button>
    </div>
  </fieldset>;
}
