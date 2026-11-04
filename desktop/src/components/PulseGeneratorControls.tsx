import {useEffect, useRef, useState} from 'react';
import {bridge, type PulseGeneratorStatus} from '../bridge';
import {COMMAND_TYPES} from '../bridgeContract';
import type {OperatingMode} from '../commissioning';
import {pollDiscovery} from '../discovery';
import {hardwareGate, HardwareCommandOwner} from './hardwareControlModel';
import {
  BAUD_RATES, channelCount, connectStep, DATA_BITS, DEFAULT_PULSE_FORM, disconnectStep, PARITIES, PULSE_LIMITS, runSteps, scanRequest,
  setSteps, startSteps, statusLabel, STOP_BITS, stopStep, summariseScan, type PulseForm, type PulseStep, type ScanSummary,
} from './pulseGeneratorModel';
import {RIG_BENCH_FIELDS, RIG_PRESET_TOOLTIP, saveRigPreset} from './pulseGeneratorRig';
import {loadPulseGeneratorSettings, savePulseGeneratorSettings} from './pulseGeneratorSettings';
type Props = {ready: boolean; experimentActive: boolean; mode: OperatingMode; armed: boolean; onDisarm: () => void; append: (message: string) => void};
type Kind = 'configure' | 'actuate' | 'stop';
const messageOf = (failure: unknown) => (failure instanceof Error ? failure.message : String(failure));
export function PulseGeneratorControls({ready, experimentActive, mode, armed, onDisarm, append}: Props) {
  const [status, setStatus] = useState<PulseGeneratorStatus | null>(null);
  const [form, setForm] = useState<PulseForm>(DEFAULT_PULSE_FORM);
  const [error, setError] = useState(''), [busy, setBusy] = useState(false);
  const [scan, setScan] = useState<{running: boolean; summary: ScanSummary | null}>({running: false, summary: null});
  const scanJob = useRef('');
  const owner = useRef(new HardwareCommandOwner());
  const inFlight = useRef(false); // synchronous: a second click in the same tick must not start a second sequence
  const update = (patch: Partial<PulseForm>) => setForm(current => ({...current, ...patch}));
  // Settings saved by an earlier session (HW-056); only valid fields come back.
  useEffect(() => {
    let alive = true;
    void loadPulseGeneratorSettings().then(saved => { if (alive) update(saved); });
    return () => { alive = false; };
  }, []);
  // Leaving the panel mid-scan releases the port.
  useEffect(() => () => { if (scanJob.current) void bridge.cancelDeviceDiscovery(scanJob.current).catch(() => false); }, []);
  const refresh = async () => { try { const next = await bridge.pulseGeneratorStatus(); setStatus(next.valid ? next : null); } catch { setStatus(null); } };
  useEffect(() => {
    let alive = true, polling = false;
    setStatus(null);
    const poll = async () => {
      if (!ready || polling) return;
      polling = true;
      try { const next = await bridge.pulseGeneratorStatus(); if (alive) setStatus(next.valid ? next : null); }
      catch { if (alive) setStatus(null); }
      finally { polling = false; }
    };
    void poll(); const timer = window.setInterval(() => void poll(), 1000);
    return () => { alive = false; window.clearInterval(timer); };
  }, [ready]);
  const connected = !!status?.connected;
  const gate = (kind: Kind) => hardwareGate({ready, experimentActive, mode, armed, connected}, kind);
  const disabled = (kind: Kind) => busy || !!gate(kind) || (kind !== 'stop' && (!status || status.owned));
  const locked = disabled('configure') || scan.running;
  /** One click, one arm: the steps run in order under one hardware-command lock and stop at the first failure. */
  async function run(kind: Kind, verb: string, plan: () => PulseStep[], onSuccess?: () => void) {
    if (disabled(kind) || inFlight.current) return;
    inFlight.current = true; setBusy(true); setError('');
    try {
      const steps = plan(); // validated before the first write
      const result = await owner.current.run(async () => {
        if (kind === 'actuate') onDisarm();
        const outcome = await runSteps(verb, steps, bridge.pulseGeneratorCommand);
        return {ok: outcome.ok, command: COMMAND_TYPES.PulseGenerator, operation_id: '0', message: outcome.message};
      });
      append(result.message); onSuccess?.();
    } catch (failure) { setError(messageOf(failure)); }
    finally { inFlight.current = false; setBusy(false); void refresh(); }
  }
  async function onScan() {
    if (scan.running) { if (scanJob.current) await bridge.cancelDeviceDiscovery(scanJob.current).catch(() => false); return; }
    if (locked || connected) return;
    setError('');
    try {
      const request = scanRequest(form);
      setScan({running: true, summary: null});
      const snapshot = await pollDiscovery({
        start: async () => { const started = await bridge.startDeviceDiscovery(request); if (started.accepted) scanJob.current = started.job_id; return started; },
        fetch: bridge.fetchDeviceDiscovery, cancel: bridge.cancelDeviceDiscovery,
      });
      const summary = summariseScan(snapshot, request.serialPortName ?? '');
      setScan({running: false, summary});
      if (summary.firstGenerator !== null) update({address: String(summary.firstGenerator)});
      if (summary.message) append(summary.message);
    } catch (failure) { setScan({running: false, summary: null}); setError(messageOf(failure)); }
    finally { scanJob.current = ''; }
  }
  async function onRigPreset() {
    if (busy || scan.running || inFlight.current) return;
    inFlight.current = true; setBusy(true); setError('');
    try {
      const message = await saveRigPreset(form, experimentActive);
      if (message) { append(message); update(RIG_BENCH_FIELDS); }
    } catch (failure) { setError(messageOf(failure)); }
    finally { setBusy(false); }
  }
  const startGate = gate('actuate');
  const channels = Array.from({length: channelCount(status)}, (_, index) => index);
  const portNames = [...new Set([form.port.trim(), status?.port ?? ''].filter(Boolean))];
  return <fieldset><legend>Acquisition pulse generator (RS485)</legend>
    <p>{statusLabel(status, scan.running, form.channel)}</p>
    <p>This controls camera acquisition pulses, not sorter trigger pulses. Disconnect does not stop an existing output; press Stop for each channel first.</p>
    {error && <p role="alert">{error}</p>}
    <label>System serial port<input value={form.port} list="pulse-generator-ports" disabled={locked || connected} onChange={e => update({port: e.target.value})} placeholder="COM3 or /dev/ttyUSB0" /></label>
    <datalist id="pulse-generator-ports">{portNames.map(name => <option key={name} value={name} />)}</datalist>
    <label>Baud<select value={form.baud} disabled={locked || connected} onChange={e => update({baud: Number(e.target.value)})}>{BAUD_RATES.map(rate => <option key={rate} value={rate}>{rate}</option>)}</select></label>
    <label>Data bits<select value={form.dataBits} disabled={locked || connected} onChange={e => update({dataBits: Number(e.target.value)})}>{DATA_BITS.map(bits => <option key={bits} value={bits}>{bits}</option>)}</select></label>
    <label>Parity<select value={form.parity} disabled={locked || connected} onChange={e => update({parity: e.target.value as PulseForm['parity']})}>{PARITIES.map(parity => <option key={parity} value={parity}>{parity}</option>)}</select></label>
    <label>Stop bits<select value={form.stopBits} disabled={locked || connected} onChange={e => update({stopBits: Number(e.target.value)})}>{STOP_BITS.map(bits => <option key={bits} value={bits}>{bits}</option>)}</select></label>
    <label>Modbus address<input type="number" min={PULSE_LIMITS.address.min} max={PULSE_LIMITS.address.max} value={form.address} disabled={locked || connected} onChange={e => update({address: e.target.value})} /></label>
    <button disabled={scan.running ? false : locked || connected} onClick={() => void onScan()} title="Probe addresses 1–16 on the selected port with a read-only register read. Never writes to any device.">{scan.running ? 'Cancel scan' : 'Scan'}</button>
    {scan.summary && scan.summary.outcome !== 'cancelled' && <div role="status"><p>{scan.summary.message}</p>{scan.summary.lines.length > 0 && <ul>{scan.summary.lines.map(line => <li key={line}>{line}</li>)}</ul>}</div>}
    <button disabled={locked || connected} onClick={() => void run('configure', 'Connect', () => [connectStep(form)], () => void savePulseGeneratorSettings(form))}>Connect pulse generator</button>
    <button disabled={locked || !connected} onClick={() => void run('configure', 'Disconnect', () => [disconnectStep()])}>Disconnect pulse generator</button>
    <label>Channel<select value={form.channel} disabled={busy} onChange={e => update({channel: Number(e.target.value)})}>{channels.map(index => <option key={index} value={index}>{index + 1}</option>)}</select></label>
    <label>Frequency (Hz)<input value={form.frequency} onChange={e => update({frequency: e.target.value})} /></label>
    <label>Duty (%)<input value={form.duty} onChange={e => update({duty: e.target.value})} /></label>
    <button disabled={disabled('actuate')} onClick={() => void run('actuate', 'Set', () => setSteps(form), () => void savePulseGeneratorSettings(form))} title="Write frequency and duty to the selected channel without enabling it.">Set</button>
    <button disabled={disabled('actuate')} onClick={() => void run('actuate', 'Start', () => startSteps(form), () => void savePulseGeneratorSettings(form))} title="Write frequency and duty, then enable the pulse train (one arm).">Start</button>
    <button disabled={disabled('stop') || !!status?.owned} onClick={() => void run('stop', 'Stop', () => [stopStep(form.channel)])} title="Gate the pulse train off (writes duty 0%, line idles low).">Stop</button>
    <button disabled={busy || scan.running || !ready || experimentActive} onClick={() => void onRigPreset()} title={RIG_PRESET_TOOLTIP}>Use XGC + R5D preset for Live View</button>
    {startGate && <p>{startGate}</p>}
  </fieldset>;
}
