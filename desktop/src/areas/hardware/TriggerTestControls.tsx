// Hardware (epic #868): the trigger / pulse generator tests in the Monitoring toolbar (Service mode + arm, decision
// D4: UX-9 #313 is a deliberate Tauri design; a running periodic test stays stoppable in Operator mode).
// Qt's limits and behaviour are ported (triggerTestModel.ts): the pulse duration (1-1,000,000 µs) is applied live
// after a short pause in typing and by Sort Trigger / Periodic Test before they fire, so the event log never states
// a duration the backend does not have; the periodic interval (10-60000 ms) is locked while the test runs; out-of-range
// input is refused with a message. A running periodic test stops when the Monitoring view is hidden (useHardware.ts).
import { useEffect, useRef, useState } from "react";
import { bridge } from "../../bridge";
import { ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";
import {
  PERIODIC_MS_MAX, PERIODIC_MS_MIN, PULSE_APPLY_DEBOUNCE_MS, PULSE_US_MAX, PULSE_US_MIN,
  parsePeriodicMs, parsePulseUs, periodicMsProblem, pulseUsProblem,
} from "./triggerTestModel";

export function TriggerTestControls() {
  const {
    caps, operatingMode, triggerArmed, setTriggerArmed, expActive, trigStatus, append, pulseUs, setPulseUs, actuateCheck,
    periodicMs, setPeriodicMs, setTrigStatus, stopPeriodicCheck, startPeriodicCheck,
  } = useStudio();
  // The pause timer of the live apply.
  const pulseTimer = useRef<number | undefined>(undefined);
  const [pulseState, setPulseState] = useState("");
  const pulseProblem = pulseUsProblem(pulseUs), periodicProblem = periodicMsProblem(periodicMs);
  const active = !!trigStatus?.periodic_active;
  useEffect(() => () => window.clearTimeout(pulseTimer.current), []);
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
              // The duration in the field is applied first, so the pulse and its log line agree (HW-044).
              let us: number;
              try { us = parsePulseUs(pulseUs); } catch (failure) { append(`sort trigger not fired: ${failure instanceof Error ? failure.message : failure}`); return; }
              window.clearTimeout(pulseTimer.current);
              const set = await bridge.triggerSetPulseDuration(us);
              if (!set.ok) { append(`pulse duration failed: ${set.message}; sort trigger not fired`); return; }
              setPulseState("applied");
              const res = await bridge.triggerManualPulse();
              append(res.ok ? `manual sort pulse fired (${us} µs)` : `sort trigger failed: ${res.message}`);
              setTriggerArmed(false);
            }}
            disabled={!actuateCheck.allowed || !!pulseProblem}
            title={pulseProblem || (actuateCheck.allowed ? `Fire one manual sorter pulse (${pulseUs} µs)` : actuateCheck.reason)}
          >
            Sort Trigger
          </button>
          <label>
            <input
              type="number"
              style={{ width: 80 }}
              min={PULSE_US_MIN}
              max={PULSE_US_MAX}
              step={1}
              value={pulseUs}
              aria-invalid={!!pulseProblem}
              onChange={(e) => {
                const text = e.target.value;
                setPulseUs(text);
                window.clearTimeout(pulseTimer.current);
                if (pulseUsProblem(text)) { setPulseState(""); return; }
                setPulseState("applying…");
                pulseTimer.current = window.setTimeout(async () => {
                  const us = Number(text);
                  try {
                    const res = await bridge.triggerSetPulseDuration(us);
                    if (res.ok) setPulseState("applied");
                    else { setPulseState(""); append(`pulse duration failed: ${res.message}`); }
                  } catch (failure) { setPulseState(""); append(`pulse duration failed: ${failure}`); }
                }, PULSE_APPLY_DEBOUNCE_MS);
              }}
              aria-label="Pulse duration (µs)"
            />{" "}
            µs
          </label>
          {pulseProblem
            ? <span role="alert" className="commissioning-hint mono">{pulseProblem}</span>
            : pulseState && <span role="status" className="commissioning-hint mono">{pulseState}</span>}
          <button
            onClick={async () => {
              if (active) {
                const res = await bridge.triggerPeriodicStop();
                append(res.ok ? "periodic test stopped" : `periodic test failed: ${res.message}`);
                setTriggerArmed(false);
                setTrigStatus(await bridge.fetchTriggerStatus());
                return;
              }
              let ms: number, us: number;
              try { ms = parsePeriodicMs(periodicMs); us = parsePulseUs(pulseUs); }
              catch (failure) { append(`periodic test not started: ${failure instanceof Error ? failure.message : failure}`); return; }
              window.clearTimeout(pulseTimer.current);
              const set = await bridge.triggerSetPulseDuration(us);
              if (!set.ok) { append(`pulse duration failed: ${set.message}; periodic test not started`); return; }
              setPulseState("applied");
              const res = await bridge.triggerPeriodicStart(ms);
              append(res.ok ? `periodic test started (${ms} ms, ${us} µs pulses)` : `periodic test failed: ${res.message}`);
              setTriggerArmed(false);
              setTrigStatus(await bridge.fetchTriggerStatus());
            }}
            disabled={active ? !stopPeriodicCheck.allowed : !startPeriodicCheck.allowed || !!periodicProblem || !!pulseProblem}
            title={
              active
                ? "Stop the running periodic test"
                : periodicProblem || pulseProblem || (startPeriodicCheck.allowed
                  ? `Start a periodic test every ${periodicMs} ms`
                  : startPeriodicCheck.reason)
            }
          >
            {active ? "Stop Periodic Test" : "Periodic Test"}
          </button>
          <label>
            <input
              type="number"
              style={{ width: 84 }}
              min={PERIODIC_MS_MIN}
              max={PERIODIC_MS_MAX}
              step={1}
              value={periodicMs}
              aria-invalid={!!periodicProblem}
              onChange={(e) => setPeriodicMs(e.target.value)}
              disabled={active}
              aria-label="Periodic interval (ms)"
            />{" "}
            ms
          </label>
          {periodicProblem && !active && <span role="alert" className="commissioning-hint mono">{periodicProblem}</span>}
        </>
      ) : active ? (
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
