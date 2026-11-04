import { useCallback, useEffect, useRef, useState } from 'react';
import { metricNumber } from '../metricFormat';
import { bridge, type AutofocusConfig, type AutofocusStatus, type CmdResult } from '../bridge';
import { DEFAULT_MODE } from '../commissioning';
import { DISCOVERY_DEVICE_KINDS, DISCOVERY_JOB_STATES } from '../bridgeContract';
import { pollDiscovery } from '../discovery';
import { hardwareGate, operatorGate, validateFocusConfig } from './hardwareControlModel';
import { recentAutofocusMessages, resetAutofocusMessages, subscribeAutofocusMessages, type AutofocusMessage } from './autofocusMessages';
import {
  ADDRESS_MAX, ADDRESS_MIN, AutofocusCommandQueue, BACKEND_CHOICES, COREMOR_BAUD_RATES, CONFIG_POLL_EVERY, DEFAULT_BAUD, JOG_STEP_MAX, JOG_STEP_MIN,
  NO_DEVICE_HINT, OPERATOR_CONFIG_KEYS, SELECTION_SAVED_EVENT, SETPOINT_APPLY_MS, SETPOINT_MAX, SETPOINT_MIN, SKIPPED,
  STATUS_POLL_MS, candidateFromDevice, candidateFromPreference, connectTarget, errorText, formatSetpoint, listCandidates, parseJogStep,
  parseSetpoint, rememberSelection, rememberSetpoint, rememberedSelection, rememberedSetpoint, selectionPreference, stepFromConfig,
  usesCoremorSettings, type BackendChoice, type Candidate, type CommandContext, type CommandKind,
} from './autofocusControlModel';
import type { StartupPreference } from '../startupPreference';
import './HardwareControls.css';

type Props = { ready: boolean; experimentActive: boolean; append: (message: string) => void;
  /** True when the backend was already running before this page loaded: its settings are not replayed (as the profile restore). */
  resumedSession?: boolean };
/** The tuning editor (HW-035, Tauri only). The target ring width and the jog step are operator controls now (below). */
const focusFields: Array<[keyof AutofocusConfig, string]> = [
  ['focus_range', 'Focus range'], ['voltage_step', 'Voltage step (V)'],
  ['fine_voltage_step', 'Fine step (V)'], ['min_voltage', 'Minimum voltage (V)'], ['max_voltage', 'Maximum voltage (V)'],
  ['initial_voltage', 'Initial voltage (V)'],
  ['ring_ratio_stale_ms', 'Metric stale after (ms)'], ['min_samples_per_step', 'Minimum samples per step'], ['safe_shutdown_voltage', 'Shutdown voltage (V)'],
];

/** Nanopositioner / autofocus panel, as Qt's NanopositionerTab (epic #868, PR3): backend, device list from the discovery job,
 *  connect, Enable autofocus, target ring width applied live, voltage, manual jog with its step, runtime messages, and the
 *  remembered selection. Rendered only with `capabilities.autofocus`. Enable, Disable, the setpoint and Jog are operator controls
 *  (D2): no Service mode, no arm, no experiment lock; Disable is available whenever the backend is ready (D3) and a queued Enable
 *  or Jog is never sent after it. Connection fields and the tuning editor stay locked during an experiment. */
export function AutofocusControls({ready, experimentActive, append, resumedSession = false}: Props) {
  const remembered = useRef<StartupPreference | null>(null);
  if (!remembered.current) remembered.current = rememberedSelection();
  const initial = remembered.current;
  const [choice, setChoice] = useState<BackendChoice>(initial.backend);
  const [scanned, setScanned] = useState<Candidate[]>([]);
  const [scanState, setScanState] = useState<'never' | 'running' | 'done'>('never');
  const [scanNote, setScanNote] = useState('');
  const [selectedId, setSelectedId] = useState(initial.endpoint);
  const [savedPreference, setSavedPreference] = useState<StartupPreference>(initial);
  const [baud, setBaud] = useState(String(COREMOR_BAUD_RATES.includes(initial.baud) ? initial.baud : DEFAULT_BAUD));
  const [address, setAddress] = useState(String(initial.address));
  const [focus, setFocus] = useState<AutofocusStatus | null>(null);
  const [config, setConfig] = useState<AutofocusConfig | null>(null);
  const [tuning, setTuning] = useState<AutofocusConfig | null>(null);
  const [setpointText, setSetpointText] = useState('');
  const [setpointProblem, setSetpointProblem] = useState('');
  const [stepText, setStepText] = useState(String(1));
  const [messages, setMessages] = useState<AutofocusMessage[]>(() => recentAutofocusMessages());
  const [busy, setBusy] = useState(false);
  // The check box shows what the operator just chose until the backend's answer is read back (Qt's box flips at once).
  const [chosenEnabled, setChosenEnabled] = useState<boolean | null>(null);
  const [error, setError] = useState('');
  const [statusError, setStatusError] = useState('');
  const queue = useRef(new AutofocusCommandQueue());
  const polling = useRef(false);
  const generation = useRef(0);
  const pollCount = useRef(0);
  const settled = useRef(false);
  const setpointDirty = useRef(false);
  const setpointTimer = useRef<number | undefined>(undefined);
  const alive = useRef(true);

  const candidates = listCandidates(scanned, choice, candidateFromPreference(savedPreference));
  // The remembered device, else (unlike Qt, which takes the first row) the first one the scan identified: the first row may be the
  // pumps' RS485 adapter, and Connect would then write OEABT frames onto the pump bus.
  const selected = candidates.find(candidate => candidate.id === selectedId) ?? candidates.find(candidate => candidate.identification === 'identified') ?? candidates[0] ?? null;
  const connected = !!focus?.connected;
  const configureReason = hardwareGate({ready, experimentActive, connected, mode: DEFAULT_MODE, armed: false}, 'configure');
  const scanning = scanState === 'running';
  const connectionLocked = busy || scanning || !!configureReason;
  const commandReason = operatorGate({ready, connected}, 'command');
  const coremorSettings = usesCoremorSettings(choice, selected);

  // The newest values, for timers and handlers that outlive a render.
  const latest = useRef({ready, experimentActive, append, resumedSession, choice, selected, baud, address, savedPreference, setpointText, stepText, connected, focus, tuning});
  latest.current = {ready, experimentActive, append, resumedSession, choice, selected, baud, address, savedPreference, setpointText, stepText, connected, focus, tuning};

  useEffect(() => { alive.current = true; return () => { alive.current = false; window.clearTimeout(setpointTimer.current); }; }, []);
  useEffect(() => subscribeAutofocusMessages(message => setMessages(old => [...old.slice(-4), message])), []);

  // ---- remembered selection (HW-036): written on connect, disconnect and when the page goes away; read at startup ----
  const rememberNow = useCallback(() => {
    const now = latest.current;
    const preference = selectionPreference({choice: now.choice, candidate: now.selected, baud: now.baud, address: now.address, previous: now.savedPreference});
    if (!preference) return;
    const outcome = rememberSelection(preference);
    if (!outcome.saved || !outcome.preference) { setError(`Nanopositioner selection could not be remembered: ${outcome.error}`); return; }
    setSavedPreference(outcome.preference);
    // The backend's startup policy prefers it too (refused while a startup scan runs: the next start applies the stored one).
    void Promise.resolve().then(() => bridge.startupDiscoverySetPreference(outcome.preference!)).catch(() => undefined);
  }, []);
  useEffect(() => {
    const onHide = () => rememberNow();
    const onSaved = (event: Event) => { const detail = (event as CustomEvent<StartupPreference>).detail; if (detail) setSavedPreference(detail); };
    window.addEventListener('pagehide', onHide);
    window.addEventListener(SELECTION_SAVED_EVENT, onSaved);
    return () => { window.removeEventListener('pagehide', onHide); window.removeEventListener(SELECTION_SAVED_EVENT, onSaved); };
  }, [rememberNow]);

  // ---- status and configuration polling ----
  /** The configuration as the backend holds it (a profile may have applied one): fields follow unless the operator is typing. */
  const adoptConfig = useCallback((value: AutofocusConfig) => {
    setConfig(value);
    if (!setpointDirty.current) { setSetpointText(formatSetpoint(value.focus_setpoint)); setSetpointProblem(''); }
  }, []);
  const readConfig = useCallback(async (current: number) => {
    const value = await bridge.fetchAutofocusConfig();
    if (current !== generation.current) return null;
    if (!value.valid) throw new Error('Autofocus configuration unavailable.');
    adoptConfig(value);
    return value;
  }, [adoptConfig]);
  const refresh = useCallback(async () => {
    if (polling.current || !ready) return;
    polling.current = true;
    const current = generation.current;
    try {
      const autofocus = await bridge.fetchAutofocusStatus();
      if (current !== generation.current) return;
      if (!autofocus.valid) throw new Error('Hardware status is unavailable.');
      setFocus(autofocus); setStatusError('');
      if (settled.current && pollCount.current++ % CONFIG_POLL_EVERY === 0) await readConfig(current).catch(() => undefined);
    } catch (failure) {
      if (current === generation.current) { setFocus(null); setStatusError(String(failure)); }
    } finally { polling.current = false; }
  }, [ready, readConfig]);

  useEffect(() => {
    const current = ++generation.current;
    setFocus(null); setConfig(null); settled.current = false; pollCount.current = 0;
    if (!ready) return;
    void refresh();
    void (async () => {
      try {
        const value = await readConfig(current);
        if (!value || current !== generation.current) return;
        setStepText(stepFromConfig(value.manual_voltage_step));
        // Qt reads the saved target ring width at startup (loadConfig). The settings of a running backend are not replayed.
        const wanted = rememberedSetpoint();
        if (wanted !== null && !latest.current.resumedSession && !latest.current.experimentActive && Math.abs(wanted - value.focus_setpoint) >= 0.005) {
          try {
            const applied = await queue.current.run('config', () => patchConfig({focus_setpoint: wanted}));
            if (applied !== SKIPPED && current === generation.current) {
              adoptConfig(applied);
              latest.current.append(`Autofocus target ring width restored to ${formatSetpoint(wanted)} (remembered from the last session)`);
            }
          } catch (failure) { if (current === generation.current) { const message = `Restore target ring width: ${errorText(failure)}`; setError(message); latest.current.append(message); } }
        }
        if (current === generation.current) settled.current = true;
      } catch (failure) { if (current === generation.current) setError(String(failure)); }
    })();
    const timer = window.setInterval(() => void refresh(), STATUS_POLL_MS);
    return () => { ++generation.current; window.clearInterval(timer); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, refresh]);

  // ---- commands ----
  /** Read the live configuration, change only `patch`, write it back (nothing is written when nothing changed). */
  async function patchConfig(patch: Partial<AutofocusConfig>, validate?: (merged: AutofocusConfig) => void): Promise<AutofocusConfig> {
    const fresh = await bridge.fetchAutofocusConfig();
    if (!fresh.valid) throw new Error('Autofocus configuration unavailable.');
    const merged = {...fresh, ...patch};
    validate?.(merged);
    if (!(Object.keys(patch) as Array<keyof AutofocusConfig>).some(key => fresh[key] !== patch[key])) return fresh;
    const result = await bridge.autofocusSetConfig(merged);
    if (!result.ok) throw new Error(result.message || 'Autofocus configuration was refused.');
    return merged;
  }
  const fail = (label: string, failure: unknown) => { const message = `${label}: ${errorText(failure)}`; setError(message); latest.current.append(message); };
  /** A queued command with the busy flag; Enable and Jog are dropped if the operator disabled or disconnected meanwhile. */
  async function runCommand(label: string, kind: CommandKind, command: (context: CommandContext) => Promise<CmdResult | typeof SKIPPED>, reason = '') {
    if (reason) { setError(reason); return; }
    if (busy) return;
    setBusy(true); setError('');
    try {
      const result = await queue.current.run(kind, command);
      if (result === SKIPPED) latest.current.append(`${label}: not sent, autofocus was disabled or disconnected first`);
      else {
        if (!result.ok) throw new Error(result.message || 'Hardware command failed.');
        latest.current.append(`${label}: ${result.message || 'Command accepted'}`);
      }
      await refresh();
      return result;
    } catch (failure) { fail(label, failure); }
    finally { if (alive.current) setBusy(false); }
  }
  /** Disable never waits for another command or for a connected device (#855, D3), and nothing queued enables it again. */
  async function disable() {
    queue.current.cancelActuation();
    setError(''); setChosenEnabled(false);
    try {
      const result = await bridge.autofocusSetEnabled(false);
      if (!result.ok) throw new Error(result.message || 'Autofocus disable failed');
      append(`Disable autofocus: ${result.message || 'Command accepted'}`);
    } catch (failure) { fail('Disable autofocus', failure); }
    await refresh();
    if (alive.current) setChosenEnabled(null);
  }
  async function enable() {
    setChosenEnabled(true);
    try { await runCommand('Enable autofocus', 'actuate', async context => context.cancelled() ? SKIPPED : bridge.autofocusSetEnabled(true), commandReason); }
    finally { if (alive.current) setChosenEnabled(null); }
  }
  const jog = (up: boolean) => {
    let step: number;
    try { step = parseJogStep(latest.current.stepText); } catch (failure) { fail('Jog voltage', failure); return; }
    // Qt copies the step into the configuration before every jog (NanopositionerTab::onIncreaseVoltage).
    return runCommand('Jog voltage', 'actuate', async context => {
      await patchConfig({manual_voltage_step: step});
      return context.cancelled() ? SKIPPED : bridge.autofocusJog(up);
    }, commandReason);
  };
  async function connect() {
    let target;
    try { target = connectTarget({choice, candidate: selected, baud, address}); } catch (failure) { fail('Connect autofocus', failure); return; }
    const result = await runCommand('Connect autofocus', 'connection', () => bridge.autofocusConnectEndpoint(target.backend, target.endpoint, target.comPort, target.baud, target.address), configureReason);
    // A new connection supersedes the failures of the one before.
    if (result && result !== SKIPPED && result.ok) { setSelectedId(target.endpoint); rememberNow(); resetAutofocusMessages(); setMessages([]); }
  }
  async function disconnect() {
    queue.current.cancelActuation();
    rememberNow();
    await runCommand('Disconnect autofocus', 'connection', () => bridge.autofocusDisconnect(), configureReason);
  }
  async function refreshDevices() {
    if (scanning || connected || configureReason) return;
    setScanState('running'); setScanNote(''); setError('');
    try {
      const result = await pollDiscovery({start: () => bridge.startDeviceDiscovery({kinds: [DISCOVERY_DEVICE_KINDS.Nanopositioner], origin: 'tauri-nanopositioner-picker'}), fetch: bridge.fetchDeviceDiscovery, cancel: bridge.cancelDeviceDiscovery});
      if (!alive.current) return;
      setScanned(result.candidates.filter(device => device.kind === DISCOVERY_DEVICE_KINDS.Nanopositioner).map(candidateFromDevice));
      const incomplete = !result.complete || result.state !== DISCOVERY_JOB_STATES.Completed;
      const reasons = result.errors.map(item => item.message).filter(Boolean);
      setScanNote(incomplete ? `Discovery did not complete; the list may be partial.${reasons.length ? ` ${reasons.join('; ')}` : ''}` : reasons.join('; '));
    } catch (failure) { if (alive.current) { setScanNote(`Refresh failed: ${errorText(failure)}`); } }
    finally { if (alive.current) setScanState('done'); }
  }

  // ---- target ring width (HW-030): applied live, 500 ms after the last edit, at once on Enter or leaving the field ----
  const applySetpoint = useCallback(() => {
    window.clearTimeout(setpointTimer.current); setpointTimer.current = undefined;
    if (!latest.current.ready) return;
    let value: number;
    try { value = parseSetpoint(latest.current.setpointText); } catch (failure) { setSetpointProblem(errorText(failure)); return; }
    setSetpointProblem('');
    setSetpointText(formatSetpoint(value));
    void (async () => {
      try {
        const applied = await queue.current.run('config', () => patchConfig({focus_setpoint: value}));
        if (applied === SKIPPED) return;
        setpointDirty.current = false;
        rememberSetpoint(value);
        if (alive.current) { setConfig(applied); setError(''); }
        latest.current.append(`Target ring width set to ${formatSetpoint(value)}`);
      } catch (failure) { setpointDirty.current = false; if (alive.current) fail('Target ring width', failure); }
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  const setpointEdited = (text: string) => {
    setSetpointText(text); latest.current.setpointText = text; setpointDirty.current = true;
    window.clearTimeout(setpointTimer.current);
    setpointTimer.current = window.setTimeout(applySetpoint, SETPOINT_APPLY_MS);
  };

  // ---- tuning editor (HW-035): applies its own fields to the live configuration, never the operator's setpoint or step ----
  useEffect(() => { setTuning(config); }, [config?.valid, config?.focus_range, config?.voltage_step, config?.fine_voltage_step, config?.min_voltage, config?.max_voltage, config?.initial_voltage, config?.ring_ratio_stale_ms, config?.min_samples_per_step, config?.safe_shutdown_voltage, config?.require_new_sample_per_step, config?.focus_direction]); // eslint-disable-line react-hooks/exhaustive-deps
  const tuningDisabled = connectionLocked || !focus || focus.enabled;
  const applyTuning = () => {
    const draft = tuning;
    if (!draft) return;
    const patch: Partial<AutofocusConfig> = {...draft};
    delete patch.valid;
    for (const key of OPERATOR_CONFIG_KEYS) delete patch[key];
    return runCommand('Apply autofocus configuration', 'config', async () => {
      const applied = await patchConfig(patch, merged => validateFocusConfig(merged));
      adoptConfig(applied);
      return {ok: true, command: 11, message: 'Autofocus config applied', operation_id: '0'};
    }, configureReason);
  };

  const endpointLabel = focus?.connected ? `${focus.backend_name ?? ''} ${focus.endpoint_id ?? ''}`.trim() : '';
  const lastMessages = messages.slice(-3);
  const scannedShown = listCandidates(scanned, choice, null).length;
  const deviceNote = scanState === 'running' ? 'Searching for nanopositioners…'
    : scanState === 'never' ? (candidates.length ? 'Showing the saved selection. Click Refresh to search for nanopositioners.' : NO_DEVICE_HINT)
    : scannedShown === 0 ? 'No matching serial devices found. Check USB/power, then click Refresh.'
    : `Found ${scannedShown} candidate endpoint(s). Identity is verified only when connecting.`;
  return <>
    {error && <p role="alert">{error}</p>}{statusError && <p role="alert">Status unavailable: {statusError}</p>}
    <fieldset className="autofocus-controls"><legend>Autofocus / nanopositioner</legend>
      <p>Enable, Disable, the target ring width and Jog work as in the Qt app: no Service mode, available while an experiment runs. Disable is always available. Connection and tuning stay locked during an experiment.</p>
      <p data-tour="autofocus-state">{focus ? `${focus.connected ? 'Connected' : 'Disconnected'} · ${focus.enabled ? 'Enabled' : 'Disabled'}${endpointLabel ? ` · ${endpointLabel}` : ''}` : 'Status unknown'}</p>
      <div className="hardware-fields">
        <label>Backend<select data-tour="autofocus-backend" value={choice} disabled={connectionLocked || connected} onChange={e => setChoice(e.target.value as BackendChoice)}>
          {BACKEND_CHOICES.map(item => <option key={item.value} value={item.value}>{item.label}</option>)}</select></label>
        <label>Serial device<select data-tour="autofocus-device" value={selected?.id ?? ''} disabled={connectionLocked || connected || !candidates.length} onChange={e => setSelectedId(e.target.value)}>
          {candidates.map(item => <option key={item.id} value={item.id} title={`${item.systemPath} · ${item.identification}`}>{item.label}</option>)}</select></label>
        <button data-tour="autofocus-refresh" title="Rescan for available nanopositioner serial devices" disabled={connectionLocked || connected} onClick={() => void refreshDevices()}>{scanning ? 'Scanning…' : 'Refresh'}</button>
      </div>
      <p className="hardware-hint" role="status">{deviceNote}{scanNote ? ` ${scanNote}` : ''}</p>
      <div className="hardware-fields">
        <label>Baud rate<select value={baud} disabled={connectionLocked || connected || !coremorSettings} onChange={e => setBaud(e.target.value)}>
          {COREMOR_BAUD_RATES.map(rate => <option key={rate} value={rate}>{rate}</option>)}</select></label>
        <label>Device address<input type="number" min={ADDRESS_MIN} max={ADDRESS_MAX} step={1} value={address} disabled={connectionLocked || connected || !coremorSettings} onChange={e => setAddress(e.target.value)} /></label>
      </div>
      <div className="hardware-actions">
        <button data-tour="autofocus-connect" disabled={connectionLocked || connected || !focus || !selected} onClick={() => void connect()}>Connect autofocus</button>
        <button disabled={connectionLocked || !focus?.connected} onClick={() => void disconnect()}>Disconnect autofocus</button>
      </div>
      <div className="hardware-actions">
        <label><input data-tour="autofocus-enable" type="checkbox" checked={chosenEnabled ?? !!focus?.enabled} disabled={!!commandReason}
          onChange={e => { if (e.target.checked) void enable(); else void disable(); }} />Enable autofocus</label>
        <button data-tour="autofocus-disable" disabled={!ready} onClick={() => void disable()}>Disable autofocus</button>
      </div>
      <div className="hardware-fields">
        <label title="Ring width setpoint for autofocus (median ring ratio target)">Target ring width<input data-tour="autofocus-setpoint" type="number" min={SETPOINT_MIN} max={SETPOINT_MAX} step="any"
          value={setpointText} disabled={!ready || !config} aria-invalid={!!setpointProblem}
          onChange={e => setpointEdited(e.target.value)} onBlur={() => { if (setpointDirty.current) applySetpoint(); }}
          onKeyDown={e => { if (e.key === 'Enter') applySetpoint(); }} /></label>
      </div>
      {setpointProblem && <p role="alert" className="hardware-hint hardware-hint-problem">{setpointProblem}</p>}
      <p data-tour="autofocus-voltage">Voltage: {connected && focus ? `${focus.current_voltage.toFixed(2)} V` : '-- V'}</p>
      {focus && <p>Ring ratio: {metricNumber(focus.average_ring_ratio, 3)} average / {metricNumber(focus.median_ring_ratio, 3)} median · {focus.last_ring_ratio_update_us === 0 ? 'No focus sample received' : `Sample age ${metricNumber(focus.ring_ratio_age_us / 1000, 0)} ms${config && focus.ring_ratio_age_us > config.ring_ratio_stale_ms * 1000 ? ' (stale)' : ''}`}</p>}
      <div className="hardware-actions" aria-label="Manual control">
        <span>Manual control:</span>
        {[false, true].map(up => <button key={String(up)} data-tour={up ? 'autofocus-jog-up' : 'autofocus-jog-down'} disabled={busy || !!commandReason} onClick={() => void jog(up)}>Jog voltage {up ? '+' : '−'}</button>)}
        <label>Step (V)<input data-tour="autofocus-jog-step" type="number" min={JOG_STEP_MIN} max={JOG_STEP_MAX} step={1} value={stepText} disabled={!!commandReason} onChange={e => setStepText(e.target.value)} /></label>
      </div>
      <div data-tour="autofocus-status" aria-label="Autofocus messages">
        {lastMessages.map((message, index) => <p key={`${message.at}-${index}`} role="status" className="hardware-hint hardware-hint-problem" title={new Date(message.at).toLocaleTimeString()}>{message.text}</p>)}
      </div>
      {configureReason && <p role="status">{configureReason}</p>}
      {config && tuning && <details><summary>Autofocus configuration</summary><div className="hardware-fields">
        {focusFields.map(([key, label]) => <label key={key}>{label}<input type="number" step="any" disabled={tuningDisabled} value={Number.isNaN(Number(tuning[key])) ? '' : Number(tuning[key])} onChange={e => setTuning({...tuning, [key]: e.target.value.trim() === '' ? NaN : Number(e.target.value)})} /></label>)}
        {(['require_new_sample_per_step', 'focus_direction'] as const).map(key => <label key={key}><input type="checkbox" disabled={tuningDisabled} checked={tuning[key]} onChange={e => setTuning({...tuning, [key]: e.target.checked})} />{key === 'focus_direction' ? 'Positive focus direction' : 'Require new sample per step'}</label>)}
      </div><button disabled={tuningDisabled} onClick={() => void applyTuning()}>Apply autofocus configuration</button><p>Applies to the running backend; the target ring width and the jog step are set above and are not touched here.</p></details>}
    </fieldset>
  </>;
}
