// Hardware (epic #868): the trigger / pulse generator tests in the Monitoring toolbar (Service mode;
// a running periodic test stays stoppable in Operator mode). HW-043..HW-045: the Qt tooltips and ranges
// (triggerTestModel.ts), the pulse duration applied as it is typed instead of through a Set Pulse button, a validated
// periodic interval, and the periodic test stopped when the Monitoring tab is left.
import { useEffect, useRef, useState } from "react";
import { bridge, type CmdResult } from "../../bridge";
import { useViewerOnly, ViewerGate } from "../../components/ViewerGate";
import { useStudio } from "../../studio/StudioContext";
import { APPLY_DELAY_MS, parsePeriodicMs, parsePulseUs, PERIODIC_MS, PULSE_US, TOOLTIP } from "./triggerTestModel";

/** The three commands that actuate the sorter or change what it does. They are passed in from the JSX below so each
 *  call stays textually inside the `<ViewerGate>` (components/ViewerGate.test.tsx scans this file for them). */
type TriggerCommands = {
  setPulseDuration: (pulseUs: number) => Promise<CmdResult>;
  firePulse: () => Promise<CmdResult>;
  startPeriodic: (intervalMs: number) => Promise<CmdResult>;
};

export function TriggerTestControls() {
  const { caps, operatingMode, trigStatus, append, setTrigStatus } = useStudio();
  const viewerOnly = useViewerOnly();
  const viewerRef = useRef(viewerOnly);
  viewerRef.current = viewerOnly;
  // Qt stops the periodic test when the Monitoring tab hides (ExperimentMonitoringTab::hideEvent). Here the backend
  // keeps pulsing on its own, so this panel stops it as it goes away. It is mounted only while the Experiment stage's
  // Monitoring sub-tab is shown (areas/monitoring/ExperimentTab.tsx), so unmount is exactly "the tab hides". The status
  // is read fresh: the polled one stops updating once the tab is hidden.
  useEffect(() => () => {
    if (viewerRef.current) return; // another client controls: the server would refuse the stop
    void (async () => {
      const status = await bridge.fetchTriggerStatus();
      if (!status.valid || !status.periodic_active) return;
      const res = await bridge.triggerPeriodicStop();
      append(res.ok ? "periodic test stopped (Monitoring tab left)" : `periodic test failed to stop: ${res.message}`);
      setTrigStatus(await bridge.fetchTriggerStatus());
    })().catch((failure) => append(`periodic test stop on leaving Monitoring failed: ${String(failure)}`));
  }, []); // eslint-disable-line react-hooks/exhaustive-deps
  return (
    <>
      {/* UX-9: trigger tests are commissioning controls, gated
          behind Service mode; a running periodic test stays
          stoppable in Operator mode as a safety fallback. */}
      <ViewerGate>{caps.trigger && (operatingMode === "service" ? (
        <ServiceTriggerTests commands={{
          setPulseDuration: (pulseUs) => bridge.triggerSetPulseDuration(pulseUs),
          firePulse: () => bridge.triggerManualPulse(),
          startPeriodic: (intervalMs) => bridge.triggerPeriodicStart(intervalMs),
        }} />
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

function ServiceTriggerTests({ commands }: { commands: TriggerCommands }) {
  const {
    triggerArmed, setTriggerArmed, expActive, trigStatus, setTrigStatus, append, pulseUs, setPulseUs, actuateCheck, ready,
    periodicMs, setPeriodicMs, startPeriodicCheck, stopPeriodicCheck,
  } = useStudio();
  const [applyError, setApplyError] = useState("");
  const timer = useRef<number | undefined>(undefined);
  const edited = useRef(false);
  const appliedUs = trigStatus?.valid ? trigStatus.pulse_duration_us : null;
  const live = useRef({ pulseUs, appliedUs, ready });
  live.current = { pulseUs, appliedUs, ready };
  const pulse = parsePulseUs(pulseUs);
  const interval = parsePeriodicMs(periodicMs);
  const periodicActive = !!trigStatus?.periodic_active;

  // Until the operator types, the field shows what the backend has applied (it keeps its value across tab visits).
  useEffect(() => {
    if (!edited.current && appliedUs !== null && String(appliedUs) !== live.current.pulseUs) setPulseUs(String(appliedUs));
  }, [appliedUs]);

  /** Apply `text` as the sorter pulse duration unless it is invalid or already applied. Resolves to the duration the
   *  backend reports (what the next Sort Trigger fires with), or null when nothing usable could be applied. */
  const applyPulse = async (text: string): Promise<number | null> => {
    window.clearTimeout(timer.current);
    timer.current = undefined;
    const parsed = parsePulseUs(text);
    if (!parsed.ok) return null; // the reason is shown beside the field
    if (parsed.value === live.current.appliedUs) { setApplyError(""); return parsed.value; }
    if (!live.current.ready) { setApplyError("Backend is not ready."); return null; }
    const res = await commands.setPulseDuration(parsed.value);
    if (!res.ok) {
      setApplyError(`Not applied: ${res.message}`);
      append(`pulse duration failed: ${res.message}`);
      return null;
    }
    setApplyError("");
    const next = await bridge.fetchTriggerStatus();
    setTrigStatus(next);
    const applied = next.valid ? next.pulse_duration_us : parsed.value;
    append(`pulse duration ${applied} µs`);
    return applied;
  };
  const onPulseChange = (text: string) => {
    edited.current = true;
    setPulseUs(text);
    setApplyError("");
    window.clearTimeout(timer.current);
    timer.current = window.setTimeout(() => void applyPulse(text), APPLY_DELAY_MS);
  };
  // Enter or leaving the field applies at once; a value still waiting when the panel goes away is applied too.
  const flushPulse = () => { if (timer.current !== undefined) void applyPulse(live.current.pulseUs); };
  useEffect(() => () => flushPulse(), []);

  const pulseNote = !pulse.ok ? pulse.reason : applyError;
  return (
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
          const us = await applyPulse(pulseUs); // fire with the duration that is applied, not a stale one
          if (us === null) return;
          const res = await commands.firePulse();
          append(res.ok ? `manual sort pulse fired (${us} µs)` : `sort trigger failed: ${res.message}`);
          setTriggerArmed(false);
        }}
        disabled={!actuateCheck.allowed || !pulse.ok}
        title={!actuateCheck.allowed ? actuateCheck.reason : !pulse.ok ? pulse.reason : TOOLTIP.sortTrigger}
      >
        Sort Trigger
      </button>
      <label title={TOOLTIP.pulseDuration}>
        <input
          type="number"
          style={{ width: 84 }}
          min={PULSE_US.min}
          max={PULSE_US.max}
          value={pulseUs}
          onChange={(e) => onPulseChange(e.target.value)}
          onBlur={flushPulse}
          onKeyDown={(e) => { if (e.key === "Enter") flushPulse(); }}
          aria-label="Pulse duration (µs)"
          aria-invalid={!pulse.ok}
        />{" "}
        µs
      </label>
      {pulseNote
        ? <span role="alert" className="commissioning-hint">{pulseNote}</span>
        : appliedUs !== null && <span className="commissioning-hint mono">{pulse.ok && pulse.value !== appliedUs ? `applying (now ${appliedUs} µs)` : `applied ${appliedUs} µs`}</span>}
      <button
        onClick={async () => {
          if (periodicActive) {
            const res = await bridge.triggerPeriodicStop();
            append(res.ok ? "periodic test stopped" : `periodic test failed: ${res.message}`);
          } else {
            if (!interval.ok || (await applyPulse(pulseUs)) === null) return;
            const res = await commands.startPeriodic(interval.value);
            append(res.ok ? `periodic test started (${interval.value} ms)` : `periodic test failed: ${res.message}`);
          }
          setTriggerArmed(false);
          setTrigStatus(await bridge.fetchTriggerStatus());
        }}
        disabled={periodicActive ? !stopPeriodicCheck.allowed : !startPeriodicCheck.allowed || !interval.ok || !pulse.ok}
        title={
          periodicActive
            ? "Stop the running periodic test"
            : !startPeriodicCheck.allowed
              ? startPeriodicCheck.reason
              : !interval.ok
                ? interval.reason
                : !pulse.ok
                  ? pulse.reason
                  : TOOLTIP.periodicTest
        }
      >
        {periodicActive ? "Stop Periodic Test" : "Periodic Test"}
      </button>
      <label title={TOOLTIP.periodicInterval}>
        <input
          type="number"
          style={{ width: 72 }}
          min={PERIODIC_MS.min}
          max={PERIODIC_MS.max}
          value={periodicMs}
          onChange={(e) => setPeriodicMs(e.target.value)}
          disabled={periodicActive}
          aria-label="Periodic interval (ms)"
          aria-invalid={!interval.ok}
        />{" "}
        ms
      </label>
      {!interval.ok && !periodicActive && <span role="alert" className="commissioning-hint">{interval.reason}</span>}
    </>
  );
}
