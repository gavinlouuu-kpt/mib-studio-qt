// Hardware (epic #868): Operator vs Service/Commissioning mode and the one-shot actuation arm, the
// trigger/pulse generator test state, and the autofocus and pump snapshots. The device panels
// themselves are components/HardwareControls.tsx and components/StageControls.tsx. Moved from
// App.tsx without change (#869 wave 0); studio/useStudioController.ts calls these hooks in
// App.tsx's original effect order.
import { useEffect, useState } from "react";
import { bridge, type AutofocusStatus, type PumpStatus, type TriggerStatus } from "../../bridge";
import {
  canActuate,
  canStartPeriodic,
  canStopPeriodic,
  DEFAULT_MODE,
  type OperatingMode,
} from "../../commissioning";
import { confirm } from "../../transport/dialogs";
import type { MonitoringState } from "../monitoring/useMonitoring";
import type { ShellState } from "../shell/useShell";

export function useHardwareState() {
  const [trigStatus, setTrigStatus] = useState<TriggerStatus | null>(null);
  const [pulseUs, setPulseUs] = useState("1");
  const [periodicMs, setPeriodicMs] = useState("1000");
  // UX-9: routine Operator vs Service/Commissioning mode. Every session starts
  // in Operator mode; `triggerArmed` is a one-shot safety arm for actuation.
  const [operatingMode, setOperatingMode] = useState<OperatingMode>(DEFAULT_MODE);
  const [triggerArmed, setTriggerArmed] = useState(false);

  // Autofocus status (schema v11, BE-8).
  const [afStatus, setAfStatus] = useState<AutofocusStatus | null>(null);
  // UX-3 preflight: pump/trigger snapshots polled while on the Preflight stage.
  const [samplePump, setSamplePump] = useState<PumpStatus | null>(null);
  const [sheathPump, setSheathPump] = useState<PumpStatus | null>(null);
  return {
    trigStatus, setTrigStatus, pulseUs, setPulseUs, periodicMs, setPeriodicMs, operatingMode, setOperatingMode,
    triggerArmed, setTriggerArmed, afStatus, setAfStatus, samplePump, setSamplePump, sheathPump, setSheathPump,
  };
}
export type HardwareState = ReturnType<typeof useHardwareState>;

type ServiceModeDeps = Pick<ShellState, "append" | "instrumentModes" | "ready">
  & Pick<HardwareState, "operatingMode" | "setOperatingMode" | "triggerArmed" | "setTriggerArmed" | "trigStatus" | "setTrigStatus">
  & Pick<MonitoringState, "expActive"> & { monitoringVisible: boolean; viewerOnly: boolean };

/** Tell the backend about the mode (the PZ7035 refuses raw LED values outside Service mode, #501 P1). Resolves to
 *  the refusal text, or '' when the backend accepted it: a refusal must reach the operator, never be swallowed. */
export async function setBackendServiceMode(on: boolean): Promise<string> {
  try {
    const result = await bridge.setServiceMode(on);
    return result.ok ? "" : result.message || "the instrument refused";
  } catch (failure) {
    return failure instanceof Error ? failure.message : String(failure);
  }
}

// UX-9 Operator vs Service/Commissioning mode and the actuation gates of the trigger tests.
export function useServiceMode({
  append, instrumentModes, ready, operatingMode, setOperatingMode, triggerArmed, setTriggerArmed, trigStatus, setTrigStatus,
  expActive, monitoringVisible, viewerOnly,
}: ServiceModeDeps) {
  // ---- UX-9 Operator vs Service/Commissioning mode (issue #313) ----
  // Leaving Service mode, or an experiment going active, disarms actuation.
  useEffect(() => {
    if (operatingMode !== "service" || expActive) setTriggerArmed(false);
  }, [operatingMode, expActive]);

  const enterMode = async (next: OperatingMode) => {
    if (next === "service") {
      const ok = await confirm(
        "Enter Service / Commissioning mode?\n\nThis exposes hardware-actuating controls (trigger tests). Use only for bring-up and diagnostics.",
      );
      if (!ok) return;
      // The backend decides first on an instrument: a refused request leaves the app in Operator mode, with the reason in the log.
      if (instrumentModes) {
        const refusal = await setBackendServiceMode(true);
        if (refusal) { append(`Service / Commissioning mode refused: ${refusal}`); return; }
      }
      append("entered Service / Commissioning mode");
      setOperatingMode(next);
      return;
    }
    append("returned to Operator mode");
    // Leaving is never blocked: the app shows Operator mode and disarms at once; a refusal from the backend is reported.
    setOperatingMode(next);
    setTriggerArmed(false);
    if (instrumentModes) {
      const refusal = await setBackendServiceMode(false);
      if (refusal) append(`the instrument did not leave Service mode: ${refusal}; raw LED values may still be accepted until it does`);
    }
  };

  // A running periodic test stops when the Monitoring view is hidden (Qt ExperimentMonitoringTab::hideEvent unchecks the
  // Periodic Test button, so no test pulses unseen in the background; HW-045). The status is last polled while the view
  // was visible, so the flag is current at the moment it hides. A viewer cannot stop another client's test.
  const periodicActive = trigStatus?.periodic_active ?? false;
  useEffect(() => {
    if (monitoringVisible || !ready || viewerOnly || !periodicActive) return;
    void bridge.triggerPeriodicStop()
      .then(async (res) => {
        append(res.ok ? "periodic test stopped (the Monitoring view was hidden)" : `periodic test stop failed: ${res.message}`);
        setTrigStatus(await bridge.fetchTriggerStatus());
      })
      .catch((failure) => append(`periodic test stop failed: ${failure}`));
  }, [monitoringVisible, periodicActive, ready, viewerOnly]);

  const actuateCheck = canActuate({
    mode: operatingMode,
    experimentActive: expActive,
    triggerAttached: trigStatus?.camera_attached ?? false,
    armed: triggerArmed,
  });
  const startPeriodicCheck = canStartPeriodic({
    mode: operatingMode,
    experimentActive: expActive,
    triggerAttached: trigStatus?.camera_attached ?? false,
    armed: triggerArmed,
  });
  const stopPeriodicCheck = canStopPeriodic({ periodicActive: trigStatus?.periodic_active ?? false });
  return { enterMode, actuateCheck, startPeriodicCheck, stopPeriodicCheck };
}
