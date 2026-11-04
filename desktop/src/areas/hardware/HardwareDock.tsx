// Hardware (epic #868, HW-018 / HW-042): the collapsible, resizable right-hand hardware panel beside every
// stage tab (Qt: the hardware sidebar, src/frontend/utils/SidebarWidget.cpp + MainWindow::setupSidebar). It frames
// HardwarePanel (pumps, autofocus, pulse generator, Z stage), which stays mounted while the panel is collapsed or
// hidden for space, so command ownership and form drafts survive. Visibility, width and the narrow-window rules
// are useHardwarePanel (useHardware.ts); the shell's menu item and Ctrl+Shift+H call `toggleHardwarePanel`.
import { useRef, type KeyboardEvent, type PointerEvent } from "react";
import { PanelErrorBoundary } from "../../components/PanelErrorBoundary";
import { useStudio } from "../../studio/StudioContext";
import { HardwarePanel } from "./HardwarePanel";
import { HW_PANEL_KEY_STEP } from "./hardwarePanelLayout";

export function HardwareDock() {
  const {
    hardwarePanelVisible, hardwarePanelHiddenForSpace, hardwarePanelWidth, hardwarePanelBounds,
    toggleHardwarePanel, resizeHardwarePanel, resetHardwarePanelWidth,
  } = useStudio();
  const bodyRef = useRef<HTMLDivElement>(null);
  const drag = useRef<{ startX: number; startWidth: number; width: number } | null>(null);
  const { min, max } = hardwarePanelBounds;

  // A drag moves the panel's width on the element itself and commits it once on release, so the whole
  // classic view does not re-render on every pointer move.
  const onPointerDown = (e: PointerEvent<HTMLDivElement>) => {
    if (e.button !== 0) return;
    try { e.currentTarget.setPointerCapture?.(e.pointerId); } catch { /* not capturable: the drag still follows the pointer over the handle */ }
    drag.current = { startX: e.clientX, startWidth: hardwarePanelWidth, width: hardwarePanelWidth };
    bodyRef.current?.closest(".hw-dock")?.classList.add("dragging");
  };
  const onPointerMove = (e: PointerEvent<HTMLDivElement>) => {
    const d = drag.current;
    if (!d) return;
    // The panel is on the right: dragging left widens it.
    d.width = Math.min(max, Math.max(min, Math.round(d.startWidth + (d.startX - e.clientX))));
    if (bodyRef.current) bodyRef.current.style.width = `${d.width}px`;
  };
  const endDrag = () => {
    const d = drag.current;
    drag.current = null;
    bodyRef.current?.closest(".hw-dock")?.classList.remove("dragging");
    if (d) resizeHardwarePanel(d.width, true);
  };
  const onKeyDown = (e: KeyboardEvent<HTMLDivElement>) => {
    const step = HW_PANEL_KEY_STEP * (e.shiftKey ? 4 : 1);
    const next = e.key === "ArrowLeft" ? hardwarePanelWidth + step
      : e.key === "ArrowRight" ? hardwarePanelWidth - step
      : e.key === "Home" ? min
      : e.key === "End" ? max
      : null;
    if (next === null) return;
    e.preventDefault();
    resizeHardwarePanel(next, true);
  };

  const label = hardwarePanelVisible ? "Hide hardware panel" : "Show hardware panel";
  const title = hardwarePanelHiddenForSpace
    ? "Window too narrow for the hardware panel: show it anyway (the main area gets narrower)"
    : `${label} (pumps, autofocus, pulse generator, Z stage)`;
  return (
    <aside className="hw-dock" aria-label="Hardware panel" data-state={hardwarePanelVisible ? "open" : hardwarePanelHiddenForSpace ? "hidden-for-space" : "collapsed"}>
      <button type="button" className="hw-dock-toggle" onClick={toggleHardwarePanel} title={title} aria-label={label}
        aria-expanded={hardwarePanelVisible} aria-controls="hw-dock-body">
        <span aria-hidden="true">{hardwarePanelVisible ? "▸" : "◂"}</span>
        <span className="hw-dock-toggle-label" aria-hidden="true">Hardware</span>
      </button>
      {hardwarePanelVisible && (
        <div className="hw-dock-handle" role="separator" aria-orientation="vertical" aria-label="Resize hardware panel"
          aria-valuenow={hardwarePanelWidth} aria-valuemin={min} aria-valuemax={max} tabIndex={0}
          title="Drag to resize; double-click for the default width"
          onPointerDown={onPointerDown} onPointerMove={onPointerMove} onPointerUp={endDrag} onPointerCancel={endDrag}
          onDoubleClick={resetHardwarePanelWidth} onKeyDown={onKeyDown} />
      )}
      <div id="hw-dock-body" ref={bodyRef} className="hw-dock-body" style={{ width: hardwarePanelWidth }} hidden={!hardwarePanelVisible}>
        <PanelErrorBoundary>{() => <HardwarePanel />}</PanelErrorBoundary>
      </div>
    </aside>
  );
}
