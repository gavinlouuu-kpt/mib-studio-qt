// Z stage panel model (#464, ADR 0013 Amendment 1). Pure functions: every
// decision the panel makes about what is allowed lives here, so it is
// unit-tested without React. The backend enforces the same rules (no motion
// before the operator sets zero, the travel envelope around that zero, Stop
// always accepted, experiment lock); the panel mirrors them so an operator
// sees *why* a control is disabled. It never replaces them: a disabled button
// is a courtesy, not a safety gate. The stage is never homed.

import type { StageStatus } from '../bridge';
import { STAGE_MOVE_STATES } from '../bridgeContract';
import { canActuate, type OperatingMode } from '../commissioning';

export type StageGateKind = 'connect' | 'disconnect' | 'apply-profile' | 'move' | 'set-zero' | 'stop';

export interface StageGateInput {
  ready: boolean;
  experimentActive: boolean;
  mode: OperatingMode;
  armed: boolean;
  status: StageStatus | null;
}

export const NO_ZERO_REASON =
  'Position is unknown: nobody has said where zero is since the controller powered up. Use Set zero here first.';
export const SET_ZERO_WARNING =
  'Set zero here declares the stage’s current position as 0 µm. It does not move the stage. ' +
  'Nothing checks where the stage physically is: the controller only counts the steps it was told to make, ' +
  'so a hand move, a stall or a collision silently shifts the real position against the counter. ' +
  'Travel is limited to ±1000 µm around this point unless you declare that the stage is at mid-travel.';
export const MID_TRAVEL_LABEL = 'The stage is at mid-travel (allows ±2900 µm around this point)';
export const LIMITS_UNVERIFIED_NOTE =
  'wiring unverified: the limit switches have not been checked, so do not rely on them to stop the stage';
export const LIMITS_VERIFIED_NOTE = 'wiring verified by a supervised check';

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
    if (s.busy) return 'A move is running. Stop it first.';
    return s.configured ? 'The controller already matches the stage profile.' : '';
  }
  // move / set-zero
  if (!s.configured) return 'The controller settings do not match the stage profile. Apply the stage settings first.';
  if (s.emergency_stop) return 'The emergency stop is active.';
  if (s.driver_alarm) return 'The motor driver reports an alarm.';
  if (s.busy) return 'A move is already running. Stop it first.';
  if (kind === 'move' && !s.zero_set) return NO_ZERO_REASON;
  // The limit wiring is never a gate: it only labels the limit indicators.
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

/** Pre-check against the travel envelope around the zero (the backend checks again, and also refuses an approach
 *  overshoot that would leave it). '' = fine. */
export function envelopeProblem(status: StageStatus, value: number, relative: boolean): string {
  if (!status.zero_set) return NO_ZERO_REASON;
  const target = relative ? Math.round(status.position_um) + value : value;
  if (target < status.envelope_min_um || target > status.envelope_max_um) {
    return `${target} µm is outside the allowed travel (${Math.round(status.envelope_min_um)} to ${Math.round(status.envelope_max_um)} µm around the zero you set).`;
  }
  return '';
}

/** The controller's counter is only a position once the operator has set zero. */
export function positionText(status: StageStatus): string {
  if (status.zero_set) return `${status.position_um.toFixed(1)} µm`;
  return `unknown until zero is set (controller counter ${status.position_um.toFixed(1)} µm)`;
}

/** One line on the zero and the travel the backend allows around it. */
export function zeroText(status: StageStatus): string {
  if (!status.zero_set) return 'Zero not set';
  const declared = status.mid_travel_declared ? 'mid-travel declared' : 'mid-travel not declared';
  return `Zero set · travel ${Math.round(status.envelope_min_um)} to ${Math.round(status.envelope_max_um)} µm (${declared})`;
}

export function limitWiringText(status: StageStatus): string {
  return status.limits_verified ? LIMITS_VERIFIED_NOTE : LIMITS_UNVERIFIED_NOTE;
}

export function moveStateText(status: StageStatus): string {
  if (status.move_state === STAGE_MOVE_STATES.Faulted) return 'Faulted';
  // The software never homes; this means someone started a Home on the controller itself.
  if (status.move_state === STAGE_MOVE_STATES.Homing) return 'Homing (started at the controller)';
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

/** Limits, e-stop and alarm. The home sensor is deliberately not shown: its input floats on this stage and always
 *  reads active, so it would only mislead. */
export function stageIndicators(status: StageStatus): StageIndicator[] {
  return [
    { key: 'limit-negative', label: 'Limit −', active: status.limit_negative, alarm: false },
    { key: 'limit-positive', label: 'Limit +', active: status.limit_positive, alarm: false },
    { key: 'emergency-stop', label: 'Emergency stop', active: status.emergency_stop, alarm: true },
    { key: 'driver-alarm', label: 'Driver alarm', active: status.driver_alarm, alarm: true },
  ];
}

export const JOG_STEPS_UM = [1, 10, 100, 1000] as const;

/** Poll faster while something moves so the position readout is live. */
export function pollIntervalMs(status: StageStatus | null): number {
  return status && (status.busy || status.move_state === STAGE_MOVE_STATES.Moving) ? 250 : 1000;
}
