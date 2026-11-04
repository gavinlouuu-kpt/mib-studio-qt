// Hardware (epic #868): the device panel: pumps, nanopositioner/autofocus, pulse generator
// (components/HardwareControls) and the Z stage (components/StageControls).
// HW-018: Qt keeps the pump and autofocus controls in a sidebar beside every tab
// (SidebarWidget.cpp:54-65). The panel stays mounted on every tab, so its fields, timers and a held
// purge survive a tab change; it is shown in full on Connect and behind a "Hardware" toggle elsewhere.
import { useState } from "react";
import { HardwareControls } from "../../components/HardwareControls";
import { StageControls } from "../../components/StageControls";
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";

export function HardwarePanel() {
  const {
    tab, ready, expActive, append, caps, operatingMode, triggerArmed, setTriggerArmed, refreshCameraState, pz7035,
  } = useStudio();
  const [dockOpen, setDockOpen] = useState(false);
  const docked = tab !== "connect";
  const title = caps.autofocus ? "pumps and autofocus" : "pumps";
  return (
    <div className={docked ? "hardware-dock" : undefined}>
      {docked && (
        <button type="button" className="hardware-dock-toggle" aria-expanded={dockOpen} onClick={() => setDockOpen(open => !open)}
          title={`Show or hide the hardware controls (${title}) on this tab`}>
          {dockOpen ? "▾" : "▸"} Hardware: {title}
        </button>
      )}
      <div hidden={docked && !dockOpen}><ViewerGate>
        <HardwareControls ready={ready} experimentActive={expActive} append={append} capabilities={caps}
          mode={operatingMode} armed={triggerArmed} onDisarm={() => setTriggerArmed(false)} onSelectionChanged={refreshCameraState} />
        {/* Z stage (#464): hidden on the PZ7035 until the board has a serial path for it. */}
        {!pz7035 && <StageControls ready={ready} experimentActive={expActive} append={append}
          mode={operatingMode} armed={triggerArmed} onDisarm={() => setTriggerArmed(false)} />}
      </ViewerGate></div>
    </div>
  );
}
