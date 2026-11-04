// Hardware (epic #868): the trigger / pulse generator tests in the Monitoring toolbar (Service mode;
// a running periodic test stays stoppable in Operator mode). Moved from App.tsx without change (#869 wave 0).
import { bridge } from "../../bridge";
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";

export function TriggerTestControls() {
  const {
    caps, operatingMode, triggerArmed, setTriggerArmed, expActive, trigStatus, append, pulseUs, setPulseUs, actuateCheck,
    ready, periodicMs, setPeriodicMs, setTrigStatus, stopPeriodicCheck, startPeriodicCheck,
  } = useStudio();
  return (
    <>
      {/* UX-9: trigger tests are commissioning controls, gated
          behind Service mode; a running periodic test stays
          stoppable in Operator mode as a safety fallback. */}
      <ViewerGate>{caps.trigger && (operatingMode === "service" ? (
        <>
          <label className="arm-toggle" title="Arm the hardware-actuating trigger controls">
            <input
              type="checkbox"
              checked={triggerArmed}
              onChange={(e) => setTriggerArmed(e.target.checked)}
              disabled={expActive || !trigStatus?.camera_attached}
            />{" "}
            Arm
          </label>
          <button
            onClick={async () => {
              const res = await bridge.triggerManualPulse();
              append(res.ok ? `manual sort pulse fired (${pulseUs} µs)` : `sort trigger failed: ${res.message}`);
              setTriggerArmed(false);
            }}
            disabled={!actuateCheck.allowed}
            title={actuateCheck.allowed ? `Fire one manual sorter pulse (${pulseUs} µs)` : actuateCheck.reason}
          >
            Sort Trigger
          </button>
          <label>
            <input
              type="number"
              style={{ width: 64 }}
              value={pulseUs}
              onChange={(e) => setPulseUs(e.target.value)}
              aria-label="Pulse duration (µs)"
            />{" "}
            µs
          </label>
          <button
            className="btn"
            onClick={async () => {
              const res = await bridge.triggerSetPulseDuration(Number(pulseUs) || 1);
              append(res.ok ? `pulse duration ${pulseUs} µs` : `pulse duration failed: ${res.message}`);
            }}
            disabled={!ready}
          >
            Set Pulse
          </button>
          <button
            onClick={async () => {
              const active = trigStatus?.periodic_active;
              const res = active
                ? await bridge.triggerPeriodicStop()
                : await bridge.triggerPeriodicStart(Number(periodicMs) || 1000);
              append(
                res.ok
                  ? active
                    ? "periodic test stopped"
                    : `periodic test started (${periodicMs} ms)`
                  : `periodic test failed: ${res.message}`,
              );
              setTriggerArmed(false);
              setTrigStatus(await bridge.fetchTriggerStatus());
            }}
            disabled={trigStatus?.periodic_active ? !stopPeriodicCheck.allowed : !startPeriodicCheck.allowed}
            title={
              trigStatus?.periodic_active
                ? "Stop the running periodic test"
                : startPeriodicCheck.allowed
                  ? `Start a periodic test every ${periodicMs} ms`
                  : startPeriodicCheck.reason
            }
          >
            {trigStatus?.periodic_active ? "Stop Periodic Test" : "Periodic Test"}
          </button>
          <label>
            <input
              type="number"
              style={{ width: 72 }}
              value={periodicMs}
              onChange={(e) => setPeriodicMs(e.target.value)}
              disabled={trigStatus?.periodic_active}
              aria-label="Periodic interval (ms)"
            />{" "}
            ms
          </label>
        </>
      ) : trigStatus?.periodic_active ? (
        <button
          onClick={async () => {
            const res = await bridge.triggerPeriodicStop();
            append(res.ok ? "periodic test stopped" : `periodic test failed: ${res.message}`);
            setTrigStatus(await bridge.fetchTriggerStatus());
          }}
          title="Stop the running periodic test"
        >
          Stop Periodic Test
        </button>
      ) : (
        <span className="commissioning-hint mono">
          Trigger tests are in Service / Commissioning mode
        </span>
      ))}</ViewerGate>
    </>
  );
}
