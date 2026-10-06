// Z stage panel model (#464, ADR 0013). Pure functions: every decision the
// panel makes about what is allowed lives here, so it is unit-tested without
// React. The backend enforces the same rules (no motion before Home, soft
// limits, limits-verified Home gate, Stop always accepted, experiment lock);
// the panel mirrors them so an operator sees *why* a control is disabled.
// It never replaces them: a disabled button is a courtesy, not a safety gate.

import type { StageStatus } from '../bridge';
import { STAGE_MOVE_STATES } from '../bridgeContract';
import { canActuate, type OperatingMode } from '../commissioning';

export type StageGateKind = 'connect' | 'disconnect' | 'apply-profile' | 'move' | 'home' | 'stop';

export interface StageGateInput {
  ready: boolean;
  experimentActive: boolean;
  mode: OperatingMode;
  armed: boolean;
  status: StageStatus | null;
}

export const LIMITS_UNVERIFIED_REASON =
  'The limit switches of this controller have not been verified. Run `zc300ctl verify-limits --supervised` on the ' +
  'instrument with someone watching the stage; Home stays disabled until it passes.';
export const NOT_HOMED_REASON =
  'Position is unknown: the stage has not been homed since the controller powered up. Press Home first.';
export const HOME_WARNING =
  'Home drives the stage to both limit switches (about 6 mm of travel) and sets mid-travel as 0 µm. ' +
  'Your current focus position will be lost. Make sure nothing above the stage (objective, sample holder, tubing) ' +
  'is in the way of the full travel.';

/** Why a control is unavailable; an empty string means it is allowed. */
export function stageGate(input: StageGateInput, kind: StageGateKind): string {
  if (!input.ready) return 'Backend is not ready.';
  // Stop is always allowed: it needs neither a connection, nor arming, nor an idle experiment.
  if (kind === 'stop') return '';
  if (input.experimentActive) return 'Stage commands are locked while an experiment is active.';
  const s = input.status;
  if (!s) return 'Stage status is unavailable.';
  if (kind === 'connect') return s.connected ? 'The stage is already connected.' : '';
  if (!s.connected) return 'The stage is not connected.';
  if (kind === 'disconnect') return ''; // stops a running move first, so it is allowed while busy
  if (kind === 'apply-profile') {
    if (s.busy) return 'A move or Home is running. Stop it first.';
    return s.configured ? 'The controller already matches the stage profile.' : '';
  }
  // move / home
  if (!s.configured) return 'The controller settings do not match the stage profile. Apply the stage settings first.';
  if (s.emergency_stop) return 'The emergency stop is active.';
  if (s.driver_alarm) return 'The motor driver reports an alarm.';
  if (s.busy) return 'A move or Home is already running. Stop it first.';
  if (kind === 'move' && !s.referenced) return NOT_HOMED_REASON;
  if (kind === 'home' && !s.limits_verified) return LIMITS_UNVERIFIED_REASON;
  return canActuate({ mode: input.mode, experimentActive: false, triggerAttached: true, armed: input.armed }).reason;
}

/** A whole number of micrometres, as the backend requires (it never rounds). */
export function parseMicrons(text: string, name: string): number {
  const value = Number(text);
  if (!text.trim() || !Number.isFinite(value) || !Number.isInteger(value) || Math.abs(value) > 1_000_000) {
    throw new Error(`${name} must be a whole number of micrometres.`);
  }
  return value;
}

/** Pre-check against the soft limits (the backend checks again). '' = fine. */
export function softLimitProblem(status: StageStatus, value: number, relative: boolean): string {
  if (!status.referenced) return NOT_HOMED_REASON;
  const target = relative ? Math.round(status.position_um) + value : value;
  if (target < status.soft_min_um || target > status.soft_max_um) {
    return `${target} µm is outside the soft limits (${Math.round(status.soft_min_um)} to ${Math.round(status.soft_max_um)} µm).`;
  }
  return '';
}

/** The controller's counter is only a position once the stage has been homed. */
export function positionText(status: StageStatus): string {
  if (status.referenced) return `${status.position_um.toFixed(1)} µm`;
  return `unknown until Home (controller counter ${status.position_um.toFixed(1)} µm)`;
}

export function moveStateText(status: StageStatus): string {
  if (status.move_state === STAGE_MOVE_STATES.Faulted) return 'Faulted';
  if (status.move_state === STAGE_MOVE_STATES.Homing) return 'Homing';
  if (status.move_state === STAGE_MOVE_STATES.Moving || status.busy) return 'Moving';
  return 'Idle';
}

export function connectionText(status: StageStatus | null): string {
  if (!status) return 'Status unknown';
  if (!status.connected) return 'Disconnected';
  const where = status.port_name ? ` · ${status.port_name}` : '';
  return `Connected · ${status.model} s/n ${status.serial} · firmware ${status.firmware}${where}`;
}

export interface StageIndicator {
  key: string;
  label: string;
  active: boolean;
  alarm: boolean;
}

export function stageIndicators(status: StageStatus): StageIndicator[] {
  return [
    { key: 'limit-negative', label: 'Limit −', active: status.limit_negative, alarm: false },
    { key: 'limit-positive', label: 'Limit +', active: status.limit_positive, alarm: false },
    { key: 'home-sensor', label: 'Home sensor', active: status.home, alarm: false },
    { key: 'emergency-stop', label: 'Emergency stop', active: status.emergency_stop, alarm: true },
    { key: 'driver-alarm', label: 'Driver alarm', active: status.driver_alarm, alarm: true },
  ];
}

export const JOG_STEPS_UM = [1, 10, 100, 1000] as const;

/** Poll faster while something moves so the position readout is live. */
export function pollIntervalMs(status: StageStatus | null): number {
  return status && (status.busy || status.move_state === STAGE_MOVE_STATES.Moving) ? 250 : 1000;
}
