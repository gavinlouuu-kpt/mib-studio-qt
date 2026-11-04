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

type ServiceModeDeps = Pick<ShellState, "append" | "instrumentModes">
  & Pick<HardwareState, "operatingMode" | "setOperatingMode" | "triggerArmed" | "setTriggerArmed" | "trigStatus">
  & Pick<MonitoringState, "expActive">;

// UX-9 Operator vs Service/Commissioning mode and the actuation gates of the trigger tests.
export function useServiceMode({
  append, instrumentModes, operatingMode, setOperatingMode, triggerArmed, setTriggerArmed, trigStatus, expActive,
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
      append("entered Service / Commissioning mode");
    } else {
      append("returned to Operator mode");
    }
    setOperatingMode(next);
    if (next !== "service") setTriggerArmed(false);
    // The backend refuses raw LED values outside Service mode (#501 P1).
    if (instrumentModes) void bridge.setServiceMode(next === "service").catch(() => {});
  };

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
