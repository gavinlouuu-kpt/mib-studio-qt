// The hardware settings dialogs (Settings > Syringe Pump Settings…, Settings > Boot Service Toggles…), mounted once by the
// shell and opened by a window event (hardwareDialogEvents.ts) from the menu and from the pump panel. Epic #868.
import { useContext, useEffect, useState } from 'react';
import type { PlatformCapabilities } from '../bridge';
import { StudioContext } from '../studio/StudioContext';
import { DESKTOP_CAPABILITIES } from '../platformCapabilities';
import { BootServicesDialog } from './BootServicesDialog';
import { OPEN_BOOT_SERVICES_EVENT, OPEN_PUMP_SETTINGS_EVENT } from './hardwareDialogEvents';
import { PumpSettingsDialog } from './PumpSettingsDialog';

type Props = { ready?: boolean; append?: (message: string) => void; capabilities?: PlatformCapabilities };

export function HardwareDialogs(props: Props) {
  const studio = useContext(StudioContext);
  const ready = props.ready ?? studio?.ready ?? false;
  const append = props.append ?? studio?.append ?? (() => undefined);
  const capabilities = props.capabilities ?? studio?.caps ?? DESKTOP_CAPABILITIES;
  const [pump, setPump] = useState(false), [boot, setBoot] = useState(false);
  useEffect(() => {
    const openPump = () => setPump(true), openBoot = () => setBoot(true);
    window.addEventListener(OPEN_PUMP_SETTINGS_EVENT, openPump); window.addEventListener(OPEN_BOOT_SERVICES_EVENT, openBoot);
    return () => { window.removeEventListener(OPEN_PUMP_SETTINGS_EVENT, openPump); window.removeEventListener(OPEN_BOOT_SERVICES_EVENT, openBoot); };
  }, []);
  return <>
    {pump && <PumpSettingsDialog ready={ready} append={append} capabilities={capabilities} onClose={() => setPump(false)} />}
    {boot && <BootServicesDialog ready={ready} append={append} onClose={() => setBoot(false)} />}
  </>;
}
