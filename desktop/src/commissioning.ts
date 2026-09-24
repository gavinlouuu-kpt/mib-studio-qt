// Operator vs Service/Commissioning mode gating (UX-9, issue #313 / epic #304).
//
// Manual sort-trigger and periodic-trigger tests actuate hardware and were
// mixed into the routine monitoring UI. This separates a routine `operator`
// presentation from an explicit `service` (commissioning) mode: every new
// session starts in operator mode, commissioning controls are hidden there, and
// hardware-actuating controls require the mode + an explicit arm + a safe
// experiment state before they can fire.
//
// Pure module: no React/Tauri imports so the safety gate is unit-testable.

export type OperatingMode = "operator" | "service";

export interface ActuationCheck {
  allowed: boolean;
  /** Empty when allowed; otherwise why the control is disabled. */
  reason: string;
}

export interface ActuationInput {
  mode: OperatingMode;
  experimentActive: boolean;
  /** Trigger output attached to the camera. */
  triggerAttached: boolean;
  armed: boolean;
}

/** Whether a one-shot hardware-actuating commissioning control (e.g. a manual
 *  sort pulse) may fire right now. */
export function canActuate(i: ActuationInput): ActuationCheck {
  if (i.mode !== "service") {
    return { allowed: false, reason: "Enter Service / Commissioning mode to actuate hardware." };
  }
  if (i.experimentActive) {
    return { allowed: false, reason: "Cannot actuate while an experiment is active." };
  }
  if (!i.triggerAttached) {
    return { allowed: false, reason: "No trigger output is attached." };
  }
  if (!i.armed) {
    return { allowed: false, reason: "Arm the control before firing." };
  }
  return { allowed: true, reason: "" };
}

/** Starting a periodic test has the same gate as a single actuation. */
export function canStartPeriodic(i: ActuationInput): ActuationCheck {
  return canActuate(i);
}

/** Stopping a running periodic test must always be possible (an obvious,
 *  persistent Stop) regardless of mode/arming — a running actuator must be
 *  stoppable even after the session reset the mode to operator. */
export function canStopPeriodic(i: { periodicActive: boolean }): ActuationCheck {
  if (!i.periodicActive) {
    return { allowed: false, reason: "No periodic test is running." };
  }
  return { allowed: true, reason: "" };
}

/** The default mode for every new application session. */
export const DEFAULT_MODE: OperatingMode = "operator";
