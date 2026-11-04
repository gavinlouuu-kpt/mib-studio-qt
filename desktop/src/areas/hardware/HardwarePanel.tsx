// Hardware (epic #868): the device panel: pumps, nanopositioner/autofocus, pulse generator
// (components/HardwareControls) and the Z stage (components/StageControls). HardwareDock.tsx frames it as the
// right-hand panel beside every stage tab (HW-018); it was shown only with the Connect stage before.
import { HardwareControls } from "../../components/HardwareControls";
import { StageControls } from "../../components/StageControls";
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";

export function HardwarePanel() {
  const {
    ready, expActive, append, caps, operatingMode, triggerArmed, setTriggerArmed, refreshCameraState, pz7035,
  } = useStudio();
  return (
    <div><ViewerGate>
      <HardwareControls ready={ready} experimentActive={expActive} append={append} capabilities={caps}
        mode={operatingMode} armed={triggerArmed} onDisarm={() => setTriggerArmed(false)} onSelectionChanged={refreshCameraState} />
      {/* Z stage (#464): hidden on the PZ7035 until the board has a serial path for it. */}
      {!pz7035 && <StageControls ready={ready} experimentActive={expActive} append={append}
        mode={operatingMode} armed={triggerArmed} onDisarm={() => setTriggerArmed(false)} />}
    </ViewerGate></div>
  );
}
