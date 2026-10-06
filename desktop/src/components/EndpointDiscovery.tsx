import {useRef, useState} from 'react';
import {bridge, type DiscoveredDevice, type DiscoveryRequest} from '../bridge';
import {pollDiscovery} from '../discovery';
import {DISCOVERY_IDENTIFICATION_STATUSES, DISCOVERY_JOB_STATES} from '../bridgeContract';
/** Read-only discovery: selection fills a draft, never connects or actuates. */
export function EndpointDiscovery({disabled, request, onSelect}: {disabled: boolean; request: () => DiscoveryRequest; onSelect: (device: DiscoveredDevice) => void}) {
  const [busy, setBusy] = useState(false), [error, setError] = useState('');
  const [devices, setDevices] = useState<DiscoveredDevice[]>([]);
  const pending = useRef(false);
  async function scan() {
    if (pending.current || disabled) return;
    pending.current = true; setBusy(true); setError(''); setDevices([]);
    try {
      const result = await pollDiscovery({start: () => bridge.startDeviceDiscovery(request()), fetch: bridge.fetchDeviceDiscovery, cancel: bridge.cancelDeviceDiscovery});
      if (!result.complete || result.state !== DISCOVERY_JOB_STATES.Completed) throw new Error('Discovery incomplete; no endpoint selected.');
      setDevices(result.candidates);
      if (!result.candidates.length) setError(result.errors.map(e => e.message).join('; ') || 'No identified endpoint found.');
    } catch (failure) {setError(String(failure));}
    finally {pending.current = false; setBusy(false);}
  }
  return <div><button disabled={disabled || busy} onClick={() => void scan()}>{busy ? 'Scanning endpoints…' : 'Find endpoints'}</button>
    {error && <p role="status">{error}</p>}
    {devices.map((device, index) => <button key={index} disabled={disabled || busy || device.identification !== DISCOVERY_IDENTIFICATION_STATUSES.Identified} onClick={() => onSelect(device)}>{device.display_name} · {device.system_path} · {device.claimed_by.join(', ')}{device.identification !== 0 ? ' (not uniquely identified)' : ''}</button>)}
  </div>;
}
