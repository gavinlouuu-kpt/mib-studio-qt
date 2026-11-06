// Acquisition pulse generator panel (RS485 external trigger source), as Qt's (ConfigTabs.cpp:483-654, 1501-1839):
// an enumerated port list, the serial framing, the Modbus address with a read-only Scan of addresses 1-16, Connect,
// then Ch / Freq / Duty with Set, Start (frequency + duty + output on, one click) and Stop, Qt's status wording,
// settings that survive a restart, and the one-click XGC + R5D preset for Live View. Rules and wording live in
// pulseGeneratorModel.ts; the settings store is pulseGeneratorStore.ts. Epic #868, decisions D5 and D6:
// no Service mode and no arm (rig setup, as in Qt); Connect, Disconnect, Set and Start are locked while an
// experiment is active (the generator is the acquisition clock), Stop never; the coordinated live view's ownership
// is still respected.
import { useContext, useEffect, useRef, useState } from 'react';
import { bridge, type PulseGeneratorStatus, type SerialPortEntry } from '../bridge';
import type { OperatingMode } from '../commissioning';
import { StudioContext } from '../studio/StudioContext';
import {
  PULSE_ADDRESS_MAX, PULSE_ADDRESS_MIN, PULSE_BAUD_RATES, PULSE_DATA_BITS, PULSE_PARITIES, PULSE_SCAN, PULSE_STOP_BITS, XGC_R5D_PANEL_VALUES,
  buildLiveViewDocument, connectCommand, defaultPulseDraft, describeScan, draftFromSettings, parseDuty, parseFrequency, portLabel, pulseGate,
  pulseLimits, pulseStatusLabel, resolvePort, runPulseScan, scanRequest, setCommand, settingsFromDraft, startCommand, stopCommand, validPorts,
  type Parity, type PulseAction, type PulseDraft, type PulseScanReport,
} from './pulseGeneratorModel';
import { loadPulseSettings, savePulseSettings, type PreferenceApi } from './pulseGeneratorStore';
import './PulseGeneratorControls.css';

// `mode`, `armed` and `onDisarm` are what the panel took while its writes were Service-mode actions; D5 removed that
// gate. They stay optional so a caller that still passes them (components/HardwareControls.tsx) compiles.
type Props = { ready: boolean; experimentActive: boolean; append: (message: string) => void; mode?: OperatingMode; armed?: boolean; onDisarm?: () => void };

const preferenceApi: PreferenceApi = { get: () => bridge.getPreferences(), set: (preferences) => bridge.setPreferences(preferences) };
const localStore = (): Storage | null => { try { return globalThis.localStorage ?? null; } catch { return null; } };
const reason = (failure: unknown) => (failure instanceof Error ? failure.message : String(failure));

export function PulseGeneratorControls({ ready, experimentActive, append }: Props) {
  const [status, setStatus] = useState<PulseGeneratorStatus | null>(null);
  const [ports, setPorts] = useState<SerialPortEntry[]>([]);
  const [portNote, setPortNote] = useState('');
  const [draft, setDraft] = useState<PulseDraft>(defaultPulseDraft);
  const [error, setError] = useState(''), [notice, setNotice] = useState('');
  const [busy, setBusy] = useState(false);
  const [scanning, setScanning] = useState(false);
  const [scanReport, setScanReport] = useState<PulseScanReport | null>(null);
  const [presetNote, setPresetNote] = useState('');
  // Synchronous ownership before awaiting blocks double clicks; a Stop pressed meanwhile is queued, never lost.
  const busyRef = useRef(false), stopQueued = useRef(false), cancelScan = useRef(false), alive = useRef(true);
  const studio = useContext(StudioContext);
  const limits = pulseLimits(status);
  const connected = !!status?.connected, owned = !!status?.owned;

  // What the form shows: while connected, the connection that is really open (a reload keeps the backend connected).
  const shown: PulseDraft = status?.connected
    ? { ...draft, portName: status.port || draft.portName, baud: status.baud || draft.baud, address: String(status.address || draft.address),
        dataBits: status.data_bits ?? draft.dataBits, parity: (status.parity as Parity | undefined) ?? draft.parity, stopBits: status.stop_bits ?? draft.stopBits }
    : draft;
  const latest = useRef({ shown, limits, ports, draft });
  latest.current = { shown, limits, ports, draft };
  const patch = (change: Partial<PulseDraft>) => setDraft(old => ({ ...old, ...change }));

  const gate = (action: PulseAction) => pulseGate({ ready, experimentActive, connected, owned, busy, scanning }, action);

  useEffect(() => { alive.current = true; return () => { alive.current = false; cancelScan.current = true; }; }, []);

  // Status, once a second while the backend is ready.
  useEffect(() => {
    let live = true, polling = false;
    setStatus(null);
    const poll = async () => {
      if (!ready || polling) return;
      polling = true;
      try { const next = await bridge.pulseGeneratorStatus(); if (live) setStatus(next.valid ? next : null); }
      catch { if (live) setStatus(null); }
      finally { polling = false; }
    };
    void poll(); const timer = window.setInterval(() => void poll(), 1000);
    return () => { live = false; window.clearInterval(timer); };
  }, [ready]);

  // Ports and the saved settings, once the backend is ready (Qt: restorePulseGenSettings). Nothing is connected or written.
  useEffect(() => {
    if (!ready) return;
    let live = true;
    void (async () => {
      try {
        const [list, saved] = await Promise.all([
          Promise.resolve().then(() => bridge.listSerialPorts()).then(validPorts).catch(() => [] as SerialPortEntry[]),
          loadPulseSettings(preferenceApi, localStore()),
        ]);
        if (!live) return;
        setPorts(list);
        const restored = draftFromSettings(saved);
        const found = resolvePort(list, saved);
        let note = '';
        if (found.via === 'usb') note = `Port ${saved.portName} is gone; ${found.portName} has the saved USB identity.`;
        else if (found.via === 'ambiguous') note = 'Several ports share the saved USB identity; choose the pulse-generator port.';
        setPortNote(note);
        // Qt selects the first listed port when the saved one is gone and its identity is not unique.
        setDraft({ ...restored, portName: found.portName || list[0]?.system_name || '' });
      } catch { /* the form keeps its defaults; Refresh tries again */ }
    })();
    return () => { live = false; };
  }, [ready]);

  async function refreshPorts() {
    if (gate('refresh')) return;
    try {
      const list = validPorts(await bridge.listSerialPorts());
      setPorts(list); setPortNote('');
      setDraft(old => ({ ...old, portName: list.some(p => p.system_name === old.portName) ? old.portName : list[0]?.system_name ?? '' }));
    } catch (failure) { setError(`Could not list the serial ports: ${reason(failure)}`); }
  }

  async function save(extra?: Partial<PulseDraft>) {
    const { shown: current, limits: lim, ports: list } = latest.current;
    const failure = await savePulseSettings(preferenceApi, localStore(), settingsFromDraft({ ...current, ...extra }, list, lim));
    if (failure && alive.current) setError(`The pulse generator settings were not saved: ${failure}`);
  }

  /** Frees the command lock as soon as the generator answered and sends a Stop pressed meanwhile at once (D5): a queued Stop
   *  never waits for the settings save or the status read that follow a command, and never outlives the command it queued
   *  behind (a Stop pressed during a Stop is sent right after it, not after the next Start). */
  function releaseLock() {
    busyRef.current = false; if (alive.current) setBusy(false);
    if (stopQueued.current) { stopQueued.current = false; void onStop(); }
  }
  async function refreshStatus() {
    try { if (alive.current) setStatus(await bridge.pulseGeneratorStatus()); } catch { /* the next poll reports it */ }
  }

  /** One command at a time (Connect, Disconnect, Set, Start). */
  async function run(action: Exclude<PulseAction, 'stop' | 'refresh' | 'scan'>, build: () => Record<string, unknown>, done: () => string, failed: (message: string) => string, remember: boolean) {
    if (busyRef.current || gate(action)) return;
    setError(''); setNotice('');
    let body: Record<string, unknown>;
    try { body = build(); } catch (failure) { setError(reason(failure)); return; }
    busyRef.current = true; setBusy(true);
    let applied = false;
    try {
      const result = await bridge.pulseGeneratorCommand(body);
      if (!result.ok) throw new Error(failed(result.message));
      append(done()); applied = true;
    } catch (failure) { const message = reason(failure); setError(message); append(`pulse generator: ${message}`); }
    finally { releaseLock(); }
    if (applied && remember) await save();
    await refreshStatus();
  }

  async function onStop() {
    const { shown: current, limits: lim } = latest.current;
    const blocked = gate('stop');
    if (blocked) { setError(blocked); return; }
    if (busyRef.current) { stopQueued.current = true; setNotice('Stop is queued behind the running command.'); return; }
    let body: Record<string, unknown>;
    try { body = stopCommand(current, lim); } catch (failure) { setError(reason(failure)); return; }
    busyRef.current = true; setBusy(true); setError(''); setNotice('');
    try {
      const result = await bridge.pulseGeneratorCommand(body);
      if (!result.ok) throw new Error(`Failed to stop the pulse train: ${result.message}`);
      append(`pulse generator Ch${current.channel}: stopped (output off)`);
    } catch (failure) { const message = reason(failure); setError(message); append(`pulse generator: ${message}`); }
    finally { releaseLock(); }
    await refreshStatus();
  }

  async function onScan() {
    if (scanning) { cancelScan.current = true; return; }
    if (gate('scan')) return;
    setError(''); setNotice(''); setScanReport(null);
    let request;
    try { request = scanRequest(shown); } catch { setError('Select a serial port before scanning.'); return; }
    cancelScan.current = false; setScanning(true);
    try {
      const snapshot = await runPulseScan({ start: bridge.startDeviceDiscovery, fetch: bridge.fetchDeviceDiscovery, cancel: bridge.cancelDeviceDiscovery }, request, () => cancelScan.current);
      if (!alive.current) return;
      if (!snapshot) { append('pulse generator scan cancelled'); return; }
      const report = describeScan(snapshot, shown.portName);
      setScanReport(report);
      if (report.failed) setError(report.summary);
      else if (report.firstGenerator !== null) patch({ address: String(report.firstGenerator) });
      append(report.failed || !report.lines.length ? report.summary : `pulse generator scan on ${shown.portName}: ${report.lines.join('; ')}`);
    } catch (failure) { if (alive.current) { setError(reason(failure)); append(`pulse generator: ${reason(failure)}`); } }
    finally { if (alive.current) setScanning(false); }
  }

  const onConnectToggle = () => connected
    ? run('disconnect', () => ({ action: 'disconnect' }), () => 'pulse generator disconnected (an output that was on keeps running)', m => `Failed to disconnect: ${m}`, false)
    : run('connect', () => connectCommand(shown), () => `pulse generator connected on ${shown.portName} (addr ${shown.address}, ${shown.baud} baud)`,
      m => `Failed to connect on ${shown.portName} (baud ${shown.baud}, addr ${shown.address}): ${m}. Check wiring, port and Modbus address.`, true);
  const values = () => ({ f: parseFrequency(shown.frequency, limits), d: parseDuty(shown.duty) });
  const onSet = () => run('set', () => setCommand(shown, limits), () => { const v = values(); return `pulse generator Ch${shown.channel}: set ${v.f} Hz, ${v.d}% duty`; }, m => `Failed to write settings to the module: ${m}`, true);
  const onStart = () => run('start', () => startCommand(shown, limits), () => { const v = values(); return `pulse generator Ch${shown.channel}: started ${v.f} Hz, ${v.d}% duty`; }, m => `Failed to start the pulse train: ${m}`, true);

  // ---- XGC + R5D preset for Live View (HW-057) -------------------------------------------------------------
  const camera = studio?.mindvisionDocument;
  async function onPreset() {
    if (!studio || !camera) return;
    setPresetNote(''); setError('');
    try {
      if (studio.running) throw new Error('Stop Live View before saving setup.');
      if (!camera.doc) throw new Error('Load the MindVision camera JSON first (Configuration, Camera script, MindVision JSON editor).');
      if (!camera.doc.path) throw new Error('Save the MindVision camera JSON to a file first (Save As…).');
      const text = buildLiveViewDocument(camera.text, shown);
      if (!await camera.run('save', text)) throw new Error('The camera file was not saved; see the camera editor for the reason.');
      patch({ frequency: XGC_R5D_PANEL_VALUES.frequency, duty: XGC_R5D_PANEL_VALUES.duty });
      await save({ frequency: XGC_R5D_PANEL_VALUES.frequency, duty: XGC_R5D_PANEL_VALUES.duty });
      const message = `XGC + R5D preset saved to ${camera.doc.path} with generator ${shown.portName} (addr ${shown.address}, Ch${shown.channel}). Apply the saved file to the camera before Play; nothing was started.`;
      setPresetNote(message); append(message);
    } catch (failure) { setError(reason(failure)); }
  }
  const showPreset = !!studio && !!camera && studio.caps.egrabber_script;

  const label = pulseStatusLabel({ status, scanning, channel: shown.channel });
  const refuse = (action: PulseAction) => gate(action) || undefined;
  const portOptions = ports.some(p => p.system_name === shown.portName) || !shown.portName ? ports : [{ system_name: shown.portName, system_location: '', description: 'not in the current list', manufacturer: '', serial_number: '', vendor_id: 0, product_id: 0 }, ...ports];
  const locked = connected || scanning;
  const bauds: number[] = (PULSE_BAUD_RATES as readonly number[]).includes(shown.baud) ? [...PULSE_BAUD_RATES] : [...PULSE_BAUD_RATES, shown.baud];
  return <fieldset className="pulse-generator"><legend>Pulse generator (external trigger source, RS485)</legend>
    <p className="pulse-status" role="status" aria-label="Pulse generator status">{label}</p>
    {error && <p role="alert">{error}</p>}
    {notice && <p role="status">{notice}</p>}
    <div className="pulse-row">
      <label>Port<select aria-label="Pulse generator port" value={shown.portName} disabled={locked || !ready} title="System serial port (e.g. /dev/ttyUSB0 or COM3)." onChange={e => patch({ portName: e.target.value })}>
        {portOptions.length === 0 && <option value="">No serial ports found</option>}
        {portOptions.map(p => <option key={p.system_name} value={p.system_name} title={p.system_location}>{portLabel(p)}</option>)}
      </select></label>
      <button type="button" disabled={!ready || locked} title={refuse('refresh') ?? 'Re-enumerate serial ports.'} onClick={() => void refreshPorts()}>Refresh</button>
      <label>Baud<select aria-label="Pulse generator baud" value={shown.baud} disabled={locked} onChange={e => patch({ baud: Number(e.target.value) })}>
        {bauds.map(b => <option key={b} value={b}>{b}</option>)}</select></label>
      <label>Data bits<select aria-label="Pulse generator data bits" value={shown.dataBits} disabled={locked} onChange={e => patch({ dataBits: Number(e.target.value) })}>
        {PULSE_DATA_BITS.map(b => <option key={b} value={b}>{b}</option>)}</select></label>
      <label>Parity<select aria-label="Pulse generator parity" value={shown.parity} disabled={locked} onChange={e => patch({ parity: e.target.value as Parity })}>
        {PULSE_PARITIES.map(p => <option key={p} value={p}>{p}</option>)}</select></label>
      <label>Stop bits<select aria-label="Pulse generator stop bits" value={shown.stopBits} disabled={locked} onChange={e => patch({ stopBits: Number(e.target.value) })}>
        {PULSE_STOP_BITS.map(b => <option key={b} value={b}>{b}</option>)}</select></label>
      <label>Addr<input type="number" aria-label="Pulse generator Modbus address" min={PULSE_ADDRESS_MIN} max={PULSE_ADDRESS_MAX} step={1} value={shown.address} disabled={locked}
        title="Modbus slave address of the pulse generator on this bus." onChange={e => patch({ address: e.target.value })} /></label>
      <button type="button" disabled={!ready || connected} title={connected ? 'Disconnect to scan the bus.' : `Probe addresses ${PULSE_SCAN.addressFrom}–${PULSE_SCAN.addressTo} on the selected port with a read-only register read. Never writes to any device.`}
        onClick={() => void onScan()}>{scanning ? 'Cancel scan' : 'Scan'}</button>
      <button type="button" aria-label={connected ? 'Disconnect pulse generator' : 'Connect pulse generator'} disabled={!!gate(connected ? 'disconnect' : 'connect')}
        title={refuse(connected ? 'disconnect' : 'connect')} onClick={() => void onConnectToggle()}>{connected ? 'Disconnect' : 'Connect'}</button>
    </div>
    {portNote && <p role="status">{portNote}</p>}
    <div className="pulse-row">
      <label>Ch<select aria-label="Pulse generator channel" value={shown.channel} disabled={busy} onChange={e => patch({ channel: Number(e.target.value) })}>
        {Array.from({ length: limits.channelCount }, (_, i) => <option key={i} value={i + 1}>{i + 1}</option>)}</select></label>
      <label>Freq (Hz)<input type="number" aria-label="Pulse generator frequency (Hz)" min={limits.minFrequencyHz} max={limits.maxFrequencyHz} step="any" value={shown.frequency} onChange={e => patch({ frequency: e.target.value })} /></label>
      <label>Duty (%)<input type="number" aria-label="Pulse generator duty (%)" min={0} max={100} step="any" value={shown.duty} onChange={e => patch({ duty: e.target.value })} /></label>
      <button type="button" disabled={!!gate('set')} title={gate('set') || 'Write the frequency and the duty to the module.'} onClick={() => void onSet()}>Set</button>
      <button type="button" disabled={!!gate('start')} title={gate('start') || 'Enable the pulse train: writes the frequency, the duty and turns the output on.'} onClick={() => void onStart()}>Start</button>
      <button type="button" disabled={!!gate('stop')} title={gate('stop') || 'Gate the pulse train off (duty 0 %, the line idles low).'} onClick={() => void onStop()}>Stop</button>
    </div>
    {scanReport && !scanReport.failed && <div className="pulse-scan" role="status" aria-label="Pulse generator scan result"><strong>{scanReport.summary}</strong>
      {scanReport.lines.length > 0 && <ul>{scanReport.lines.map((line, i) => <li key={i}>{line}</li>)}</ul>}</div>}
    {showPreset && <div className="pulse-preset">
      <button type="button" disabled={!ready || camera!.busy || studio!.running} title="Saves the selected generator connection and channel with the rig preset (1000 Hz rising-edge trigger, 20 µs pulse). Does not start hardware." onClick={() => void onPreset()}>Use XGC + R5D preset for Live View</button>
      {presetNote && <p role="status">{presetNote}</p>}</div>}
    <p className="pulse-help">This controls camera acquisition pulses, not sorter trigger pulses. No Service mode is needed (rig setup). Connect, Disconnect, Set and Start are locked while an experiment is active; Stop always works. Disconnect does not stop an existing output; press Stop first.</p>
  </fieldset>;
}
