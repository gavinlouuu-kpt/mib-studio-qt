// Hardware (epic #868): the Operator / Service mode switch in the menu row and the Service-mode
// banner with the one-shot actuation arm. Moved from App.tsx without change (#869 wave 0).
import { useStudio } from "../../studio/StudioContext";

export function ModeToggle() {
  const { operatingMode, enterMode } = useStudio();
  return (
    <button
      type="button"
      className={`mode-toggle ${operatingMode}`}
      onClick={() => enterMode(operatingMode === "service" ? "operator" : "service")}
      title="Switch between routine Operator mode and Service / Commissioning mode"
      aria-label={`Operating mode: ${operatingMode === "service" ? "Service / Commissioning" : "Operator"}. Click to switch.`}
    >
      Mode: {operatingMode === "service" ? "Service ⚠" : "Operator"}
    </button>
  );
}

export function ServiceBanner() {
  const { operatingMode, triggerArmed, expActive, setTriggerArmed, enterMode } = useStudio();
  return (
    <>
      {operatingMode === "service" && (
        <div className="service-banner" role="alert">
          <span>
            <strong>Service / Commissioning mode</strong> — hardware-actuating controls are enabled.
            Not for routine operation.
          </span>
          <label><input type="checkbox" checked={triggerArmed} disabled={expActive} onChange={e => setTriggerArmed(e.target.checked)} /> Arm one hardware action</label>
          <button type="button" onClick={() => enterMode("operator")}>
            Exit to Operator
          </button>
        </div>
      )}
    </>
  );
}
