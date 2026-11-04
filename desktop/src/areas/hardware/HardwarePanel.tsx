// Hardware (epic #868): the device panel shown with the Connect stage: pumps, nanopositioner/autofocus,
// pulse generator (components/HardwareControls) and the Z stage (components/StageControls).
// Moved from App.tsx without change (#869 wave 0).
import { HardwareControls } from "../../components/HardwareControls";
import { StageControls } from "../../components/StageControls";
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";

export function HardwarePanel() {
  const {
    tab, ready, expActive, append, caps, operatingMode, triggerArmed, setTriggerArmed, refreshCameraState, pz7035, resumedNative,
  } = useStudio();
  return (
    <div hidden={tab !== "connect"}><ViewerGate>
      <HardwareControls ready={ready} experimentActive={expActive} append={append} capabilities={caps} resumedSession={resumedNative}
        mode={operatingMode} armed={triggerArmed} onDisarm={() => setTriggerArmed(false)} onSelectionChanged={refreshCameraState} />
      {/* Z stage (#464): hidden on the PZ7035 until the board has a serial path for it. */}
      {!pz7035 && <StageControls ready={ready} experimentActive={expActive} append={append}
        mode={operatingMode} armed={triggerArmed} onDisarm={() => setTriggerArmed(false)} />}
    </ViewerGate></div>
  );
}
