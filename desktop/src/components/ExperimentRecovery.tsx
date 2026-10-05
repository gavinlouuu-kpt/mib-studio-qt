import {useEffect, useRef, useState} from 'react';
import {bridge, type ExperimentStatus} from '../bridge';
import {EXPERIMENT_STATES} from '../bridgeContract';
export function ExperimentRecovery({ready, status, onStatus}: {ready: boolean; status: ExperimentStatus | null; onStatus: (value: ExperimentStatus) => void}) {
  const [confirmedIdentity, setConfirmedIdentity] = useState(''), [busy, setBusy] = useState(false), [message, setMessage] = useState('');
  const pending = useRef(false);
  const identity = `${status?.start_generation}/${status?.fault_revision}/${status?.fault_code}/${status?.fault_message}`;
  const confirmed = confirmedIdentity === identity;
  useEffect(() => {setMessage('');}, [identity]);
  if (!status || (!status.fault_code && !(status.terminal && !status.finalization_ok))) return null;
  const eligible = ready && !!status.fault_code && !status.flushing && (status.state === EXPERIMENT_STATES.Idle || (status.state === EXPERIMENT_STATES.Failed && status.terminal));
  async function acknowledge() {
    if (!status || !eligible || !confirmed || pending.current) return;
    pending.current = true; setBusy(true);
    try {
      const result = await bridge.experimentAcknowledgeFault(status.start_generation, status.fault_revision, status.fault_code, status.fault_message, true);
      setMessage(result.message); setConfirmedIdentity('');
      onStatus(await bridge.fetchExperimentStatus());
    } catch (error) {setMessage(String(error));}
    finally {pending.current = false; setBusy(false);}
  }
  return <fieldset><legend>Experiment recovery</legend>
    <p role="alert">{status.fault_message || status.completion_reason || status.message}</p>
    <p>Output retained at: <code>{status.output_path || 'No output file was opened'}</code></p>
    <p>Committed {status.persistence_committed} / admitted {status.persistence_admitted}; failed writes {status.persistence_failed}. {status.flushing ? 'Finalization is still running.' : 'Acknowledgment does not repair, delete, or mark this output complete.'}</p>
    {status.fault_code && <><label><input type="checkbox" checked={confirmed} disabled={!eligible || busy} onChange={e => setConfirmedIdentity(e.target.checked ? identity : '')} />I have reviewed this fault and retained output and want to re-check readiness.</label>
      <button disabled={!eligible || !confirmed || busy} onClick={() => void acknowledge()}>Acknowledge fault and re-check</button></>}
    {message && <p role="status">{message}</p>}
  </fieldset>;
}
