// Settings > Syringe Pump Settings… (Qt's SyringePumpSettingsDialog; epic #868: HW-003, HW-004, HW-020, HW-021, HW-023): both
// pumps' serial port (an enumerated list with Refresh), baud rate, Modbus address with Scan 1-8 and 'Use', and syringe
// volume. OK / Cancel / Apply, with an atomic Apply: both pumps are checked first, then saved in one write. The saved
// connection is what the pump panel's Connect uses; the host reserves the saved ports against discovery probes.
import { useEffect, useRef, useState } from 'react';
import { bridge, type SerialPortEntry } from '../bridge';
import { DESKTOP_CAPABILITIES } from '../platformCapabilities';
import type { PlatformCapabilities } from '../bridge';
import { validPorts } from './pulseGeneratorModel';
import { savePumpSettings, useHostSettings } from './hostSettingsStore';
import { BAUD_RATES, UNIT_ML, UNIT_UL, VOLUME_MAX, VOLUME_MIN, errorText } from './pumpControlModel';
import { ADDRESS_MAX, ADDRESS_MIN, PUMP_LABELS, PUMP_SCAN, SCAN_GIVE_UP_MS, SCAN_NO_PORT, SCAN_POLL_MS, draftOf, instrumentFallback, listedName, numberOrNull, planApply, portChoices, portName, savedPump, scanMessage, type PumpDraft } from './pumpSettingsModel';
import './PumpSettingsDialog.css';

type Props = { ready: boolean; append: (message: string) => void; capabilities?: PlatformCapabilities; onClose: () => void };
type ScanState = { running: boolean; found: number[]; chosen: string; message: string; problem: boolean };
const idleScan = (): ScanState => ({running: false, found: [], chosen: '', message: '', problem: false});

export function PumpSettingsDialog({ready, append, capabilities = DESKTOP_CAPABILITIES, onClose}: Props) {
  const {doc, loaded} = useHostSettings(ready);
  const [ports, setPorts] = useState<SerialPortEntry[]>([]);
  const [drafts, setDrafts] = useState<PumpDraft[] | null>(null);
  const [scans, setScans] = useState<ScanState[]>([idleScan(), idleScan()]);
  const [message, setMessage] = useState<{text: string; problem: boolean}>({text: '', problem: false});
  const [busy, setBusy] = useState(false);
  const alive = useRef(true);
  const syringes = !capabilities.pump; // the instrument's peristaltic pumps have no syringe volume

  useEffect(() => { alive.current = true; return () => { alive.current = false; }; }, []);
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => { if (event.key === 'Escape') onClose(); };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [onClose]);

  async function refreshPorts() {
    try { const list = validPorts(await bridge.listSerialPorts()); if (alive.current) { setPorts(list); setMessage({text: '', problem: false}); } }
    catch (failure) { if (alive.current) setMessage({text: `Could not list the serial ports: ${errorText(failure)}`, problem: true}); }
  }
  useEffect(() => { if (ready) void refreshPorts(); }, [ready]); // eslint-disable-line react-hooks/exhaustive-deps
  // The form starts from what is saved, once (a save elsewhere does not overwrite what the operator is typing).
  useEffect(() => {
    if (!loaded || drafts) return;
    setDrafts([0, 1].map(slot => draftOf(savedPump(doc?.settings, slot), slot, instrumentFallback(capabilities, slot))));
  }, [loaded, doc, drafts, capabilities]);

  const patch = (slot: number, change: Partial<PumpDraft>) => setDrafts(old => old && old.map((d, i) => i === slot ? {...d, ...change} : d));
  const patchScan = (slot: number, change: Partial<ScanState>) => setScans(old => old.map((s, i) => i === slot ? {...s, ...change} : s));

  /** Qt's Scan: the dialog's port and baud, addresses 1-8 at 300 ms. The answer is read from the host once it is in. */
  async function scan(slot: number) {
    if (!drafts || scans[slot].running) return;
    const draft = drafts[slot];
    const port = portName(draft.port);
    if (!port) { patchScan(slot, {message: SCAN_NO_PORT, problem: true}); return; }
    const baud = numberOrNull(draft.baud);
    if (baud === null || !Number.isInteger(baud) || baud < 1) { patchScan(slot, {message: 'Baud rate must be a whole number.', problem: true}); return; }
    patchScan(slot, {running: true, found: [], chosen: '', message: 'Scanning…', problem: false});
    try {
      const started = await bridge.hostSettingsCommand({action: 'scan_pump_addresses', port, baud, start: PUMP_SCAN.start, end: PUMP_SCAN.end, timeout_ms: PUMP_SCAN.timeoutMs});
      if (!started.ok) throw new Error(started.message || 'The scan was refused.');
      const deadline = Date.now() + SCAN_GIVE_UP_MS;
      for (;;) {
        await new Promise<void>(resolve => window.setTimeout(resolve, SCAN_POLL_MS));
        if (!alive.current) return;
        const next = await bridge.fetchHostSettings();
        const result = next.pump_scan;
        if (result.operation_id === started.operation_id && result.state !== 'running') {
          const text = scanMessage(result, port, baud);
          patchScan(slot, {running: false, found: result.found, chosen: result.found.length ? String(result.found[0]) : '', message: text.text, problem: text.problem});
          append(`${PUMP_LABELS[slot]} pump scan: ${text.text}`);
          return;
        }
        if (Date.now() > deadline) throw new Error('The scan did not finish.');
      }
    } catch (failure) {
      if (alive.current) patchScan(slot, {running: false, message: `Scan failed: ${errorText(failure)}`, problem: true});
    }
  }

  /** Atomic Apply: both pumps are checked, then saved in one write; a volume is then applied live to a pump that is connected. */
  async function apply(): Promise<boolean> {
    if (!drafts || busy) return false;
    setBusy(true); setMessage({text: '', problem: false});
    try {
      let planned;
      try { planned = planApply(drafts, syringes); } catch (failure) { setMessage({text: errorText(failure), problem: true}); return false; }
      const saved = await savePumpSettings(planned.patch);
      if (!saved.ok) { setMessage({text: `The pump settings were not saved: ${saved.message}`, problem: true}); return false; }
      const problems: string[] = [];
      for (const live of planned.volumes) {
        if (!live) continue;
        try {
          const status = await bridge.fetchPumpStatus(live.slot);
          if (!status.connected) continue;
          const result = await bridge.pumpSetSyringeVolume(live.slot, live.value, live.unit);
          if (!result.ok) throw new Error(result.message || 'refused');
          append(`${PUMP_LABELS[live.slot]} pump: syringe volume set to ${live.value} ${live.unit === UNIT_ML ? 'mL' : 'µL'}`);
        } catch (failure) { problems.push(`${PUMP_LABELS[live.slot]} pump did not take the syringe volume: ${errorText(failure)}`); }
      }
      append('Pump settings saved');
      if (problems.length) { setMessage({text: `Saved, but ${problems.join(' · ')}`, problem: true}); problems.forEach(p => append(p)); return false; }
      setMessage({text: 'Saved.', problem: false});
      return true;
    } finally { if (alive.current) setBusy(false); }
  }

  return <div className="modal-backdrop" onClick={onClose}>
    <div className="modal pump-settings" role="dialog" aria-modal="true" aria-label="Syringe Pump Settings" onClick={event => event.stopPropagation()}>
      <h3>Syringe Pump Settings</h3>
      <p className="pump-settings-help">Where each pump is and how to talk to it. Connect on the pump panel uses these. {syringes ? 'A syringe volume is also sent to a pump that is connected. ' : ''}Saved ports are never opened by the nanopositioner search.</p>
      {!drafts ? <p role="status">{loaded ? 'Reading the saved settings…' : 'Waiting for the backend…'}</p>
        : <div className="pump-settings-pumps">
          {drafts.map((draft, slot) => {
            const label = PUMP_LABELS[slot];
            const scanning = scans[slot];
            const choices = portChoices(ports, draft.port);
            const shown = listedName(ports, portName(draft.port));
            return <fieldset key={slot} className="pump-settings-pump" data-tour={`pump-settings-${slot}`}><legend>{label} pump</legend>
              <div className="pump-settings-row">
                <label>COM port
                  <select aria-label={`${label} pump serial port`} value={shown} onChange={event => patch(slot, {port: event.target.value})}>
                    <option value="">— select a port —</option>
                    {choices.map(choice => <option key={choice.value} value={choice.value}>{choice.label}</option>)}
                  </select></label>
                <button type="button" onClick={() => void refreshPorts()} disabled={!ready}>Refresh</button>
              </div>
              <div className="pump-settings-row">
                <label>Baud rate
                  <input aria-label={`${label} pump baud rate`} type="number" min={1200} max={921600} step={1200} list="pump-settings-baud" value={draft.baud} onChange={event => patch(slot, {baud: event.target.value})} /></label>
                <label>Modbus address
                  <input aria-label={`${label} pump Modbus address`} type="number" min={ADDRESS_MIN} max={ADDRESS_MAX} value={draft.address} onChange={event => patch(slot, {address: event.target.value})} /></label>
              </div>
              <div className="pump-settings-row">
                <span>Addr scan</span>
                <button type="button" aria-label={`Scan ${label} pump addresses`} disabled={!ready || scanning.running} onClick={() => void scan(slot)}>{scanning.running ? 'Scanning…' : 'Scan'}</button>
                <select aria-label={`${label} pump scan result`} value={scanning.chosen} disabled={!scanning.found.length} onChange={event => patchScan(slot, {chosen: event.target.value})}>
                  {scanning.found.map(address => <option key={address} value={address}>{address}</option>)}
                </select>
                <button type="button" aria-label={`Use scanned ${label} pump address`} disabled={!scanning.chosen} onClick={() => patch(slot, {address: scanning.chosen})}>Use</button>
              </div>
              {scanning.message && <p role={scanning.problem ? 'alert' : 'status'} className={scanning.problem ? 'pump-settings-problem' : undefined}>{scanning.message}</p>}
              {syringes && <div className="pump-settings-row">
                <label>Syringe volume ({VOLUME_MIN}–{VOLUME_MAX})
                  <input aria-label={`${label} pump syringe volume`} type="number" min={VOLUME_MIN} max={VOLUME_MAX} step="any" value={draft.volume} onChange={event => patch(slot, {volume: event.target.value})} /></label>
                <label>Unit
                  <select aria-label={`${label} pump syringe unit`} value={draft.volumeUnit} onChange={event => patch(slot, {volumeUnit: Number(event.target.value)})}><option value={UNIT_UL}>µL</option><option value={UNIT_ML}>mL</option></select></label>
              </div>}
            </fieldset>;
          })}
        </div>}
      <datalist id="pump-settings-baud">{BAUD_RATES.map(rate => <option key={rate} value={rate} />)}</datalist>
      {message.text && <p role={message.problem ? 'alert' : 'status'} className={message.problem ? 'pump-settings-problem' : undefined}>{message.text}</p>}
      <div className="actions">
        <button type="button" className="btn" disabled={!drafts || busy || !ready} onClick={() => void apply().then(done => { if (done && alive.current) onClose(); })}>OK</button>
        <button type="button" className="btn" onClick={onClose}>Cancel</button>
        <button type="button" className="btn" disabled={!drafts || busy || !ready} onClick={() => void apply()}>Apply</button>
      </div>
    </div>
  </div>;
}
