import { DESKTOP_CAPABILITIES } from '../platformCapabilities';
import { DEFAULT_MODE, type OperatingMode } from '../commissioning';
import type { PlatformCapabilities } from '../bridge';
import './HardwareControls.css';
import { StartupDiscoveryControls } from './StartupDiscoveryControls';
import { PumpControls } from './PumpControls';
import { AutofocusControls } from './AutofocusControls';
import { PulseGeneratorControls } from './PulseGeneratorControls';

type Props = { ready: boolean; experimentActive: boolean; append: (message: string) => void; mode?: OperatingMode; armed?: boolean; onDisarm: () => void; onSelectionChanged?: () => void; capabilities?: PlatformCapabilities;
  /** The backend was already running before this page loaded (a resumed session): remembered settings are not replayed into it. */
  resumedSession?: boolean;
  /** An experiment Start request is in flight (MIBQT-45: the autofocus Home button is refused then). */
  startPending?: boolean };

/** The device panel: pumps, nanopositioner/autofocus and the acquisition pulse generator, in Qt's order.
 *  Each device is its own component (PumpControls, AutofocusControls, PulseGeneratorControls) so the streams that
 *  extend them edit separate files; this one only composes them. */
export function HardwareControls({ready, experimentActive, append, mode = DEFAULT_MODE, armed = false, onDisarm, onSelectionChanged, capabilities = DESKTOP_CAPABILITIES, resumedSession = false, startPending = false}: Props) {
  const autofocus = capabilities.autofocus;
  return <section className="hardware-controls" aria-label={autofocus ? 'Pump and autofocus controls' : 'Pump controls'}>
    <h2>{autofocus ? 'Pumps and autofocus' : 'Pumps'}</h2>
    {autofocus && <StartupDiscoveryControls ready={ready} experimentActive={experimentActive} append={append} onSelectionChanged={onSelectionChanged} />}
    <PumpControls ready={ready} append={append} capabilities={capabilities} />
    {autofocus && <AutofocusControls ready={ready} experimentActive={experimentActive} append={append} resumedSession={resumedSession} startPending={startPending} />}
    {capabilities.trigger && <PulseGeneratorControls ready={ready} experimentActive={experimentActive} mode={mode} armed={armed} onDisarm={onDisarm} append={append} />}
  </section>;
}
