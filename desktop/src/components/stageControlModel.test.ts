import { describe, expect, it } from 'vitest';
import type { StageStatus } from '../bridge';
import { STAGE_MOVE_STATES } from '../bridgeContract';
import { zeroed } from './stageTestFixtures';
import {
  JOG_STEPS_UM, LIMITS_UNVERIFIED_NOTE, LIMITS_VERIFIED_NOTE, NO_ZERO_REASON, SET_ZERO_WARNING, connectionText,
  envelopeProblem, limitWiringText, moveStateText, parseMicrons, pollIntervalMs, positionText, stageGate,
  stageIndicators, zeroText, type StageGateInput, type StageGateKind,
} from './stageControlModel';

const input = (change: Partial<StageGateInput> = {}, status: Partial<StageStatus> | null = {}): StageGateInput => ({
  ready: true, experimentActive: false, mode: 'service', armed: true, status: status ? { ...zeroed, ...status } : null, ...change,
});
const all: StageGateKind[] = ['connect', 'disconnect', 'apply-profile', 'move', 'set-zero', 'stop'];

describe('stage gate mirrors the backend rules', () => {
  it('allows a zeroed, armed stage to move and to set zero again', () => {
    expect(stageGate(input(), 'move')).toBe('');
    expect(stageGate(input(), 'set-zero')).toBe('');
  });
  it('keeps Stop available in every state except an unready backend', () => {
    for (const change of [{ experimentActive: true }, { mode: 'operator' as const }, { armed: false }, { status: null }]) {
      expect(stageGate(input(change), 'stop')).toBe('');
    }
    expect(stageGate(input({ status: null }), 'stop')).toBe('');
    expect(stageGate(input({ ready: false }), 'stop')).not.toBe('');
  });
  it('locks every command except Stop during an experiment', () => {
    for (const kind of all.filter(k => k !== 'stop')) {
      expect(stageGate(input({ experimentActive: true }), kind)).toContain('experiment');
    }
  });
  it('refuses everything but Stop when status is unavailable or the backend is not ready', () => {
    for (const kind of all.filter(k => k !== 'stop')) {
      expect(stageGate(input({}, null), kind)).not.toBe('');
      expect(stageGate(input({ ready: false }), kind)).not.toBe('');
    }
  });
  it('refuses moves until the operator set zero this power-up, but allows setting it', () => {
    expect(stageGate(input({}, { zero_set: false }), 'move')).toBe(NO_ZERO_REASON);
    expect(stageGate(input({}, { zero_set: false }), 'set-zero')).toBe(''); // that is how the zero gets set
  });
  it('never gates on the limit-switch wiring or the home bit', () => {
    for (const status of [{ limits_verified: false }, { limits_verified: true }, { home: true }, { home: false }]) {
      expect(stageGate(input({}, status), 'move')).toBe('');
      expect(stageGate(input({}, status), 'set-zero')).toBe('');
    }
  });
  it('refuses motion and Set zero when the controller does not match the profile, on e-stop, alarm or while busy', () => {
    for (const status of [{ configured: false }, { emergency_stop: true }, { driver_alarm: true }, { busy: true }]) {
      expect(stageGate(input({}, status), 'move')).not.toBe('');
      expect(stageGate(input({}, status), 'set-zero')).not.toBe('');
    }
  });
  it('needs Service mode and arming for motion and Set zero, but not for connecting', () => {
    expect(stageGate(input({ mode: 'operator' }), 'move')).toContain('Service');
    expect(stageGate(input({ armed: false }), 'set-zero')).toContain('Arm');
    expect(stageGate(input({ mode: 'operator', armed: false }, { connected: false }), 'connect')).toBe('');
  });
  it('allows Disconnect while a move runs (the backend stops it first) but not Apply', () => {
    expect(stageGate(input({}, { busy: true }), 'disconnect')).toBe('');
    expect(stageGate(input({}, { busy: true, configured: false }), 'apply-profile')).toContain('Stop');
    expect(stageGate(input({}, { configured: false }), 'apply-profile')).toBe('');
    expect(stageGate(input(), 'apply-profile')).toContain('already matches');
  });
  it('connects only while disconnected, and disconnects only while connected', () => {
    expect(stageGate(input({}, { connected: false }), 'connect')).toBe('');
    expect(stageGate(input(), 'connect')).toContain('already connected');
    expect(stageGate(input({}, { connected: false }), 'disconnect')).toContain('not connected');
    expect(stageGate(input({}, { connected: false }), 'move')).toContain('not connected');
  });
  it('has no Home: the gate kinds and texts never offer one', () => {
    expect(all).not.toContain('home');
    expect(SET_ZERO_WARNING).toContain('does not move the stage');
    expect(SET_ZERO_WARNING).toContain('hand move, a stall or a collision');
  });
});

describe('stage inputs', () => {
  it('accepts only whole micrometres', () => {
    expect(parseMicrons('-250', 'x')).toBe(-250);
    expect(parseMicrons(' 40 ', 'x')).toBe(40);
    for (const bad of ['', ' ', '12.5', 'abc', 'NaN', 'Infinity', '1e9', '2000000']) expect(() => parseMicrons(bad, 'Target position')).toThrow('whole number of micrometres');
  });
  it('checks targets against the travel envelope, relative to the rounded position', () => {
    expect(envelopeProblem(zeroed, 1000, false)).toBe('');
    expect(envelopeProblem(zeroed, -1000, false)).toBe('');
    expect(envelopeProblem(zeroed, 1001, false)).toContain('outside the allowed travel');
    expect(envelopeProblem(zeroed, -1001, false)).toContain('outside the allowed travel');
    expect(envelopeProblem(zeroed, 100, true)).toBe(''); // 120 + 100 = 220
    expect(envelopeProblem({ ...zeroed, position_um: 950.4 }, 100, true)).toContain('1050 µm is outside');
    expect(envelopeProblem({ ...zeroed, zero_set: false }, 0, false)).toBe(NO_ZERO_REASON);
  });
  it('follows an envelope the backend narrowed or widened', () => {
    const wide = { ...zeroed, mid_travel_declared: true, envelope_min_um: -2900, envelope_max_um: 2900 };
    expect(envelopeProblem(wide, 2900, false)).toBe('');
    expect(envelopeProblem(wide, 2901, false)).toContain('outside');
    const edge = { ...zeroed, envelope_min_um: -1000, envelope_max_um: 0, position_um: 0 };
    expect(envelopeProblem(edge, 1, true)).toContain('outside');
  });
  it('offers small whole-micrometre jog steps only', () => {
    expect(JOG_STEPS_UM.every(v => Number.isInteger(v) && v >= 1 && v <= 1000)).toBe(true);
  });
});

describe('stage display', () => {
  it('never presents the counter as a position before zero is set', () => {
    expect(positionText(zeroed)).toBe('120.4 µm');
    expect(positionText({ ...zeroed, zero_set: false, position_um: -6564.99 })).toBe('unknown until zero is set (controller counter -6565.0 µm)');
  });
  it('states the zero and the travel the backend allows around it', () => {
    expect(zeroText({ ...zeroed, zero_set: false })).toBe('Zero not set');
    expect(zeroText(zeroed)).toBe('Zero set · travel -1000 to 1000 µm (mid-travel not declared)');
    expect(zeroText({ ...zeroed, mid_travel_declared: true, envelope_min_um: -2900, envelope_max_um: 2900 })).toBe('Zero set · travel -2900 to 2900 µm (mid-travel declared)');
  });
  it('labels the limit wiring until a supervised check passed', () => {
    expect(limitWiringText(zeroed)).toBe(LIMITS_UNVERIFIED_NOTE);
    expect(limitWiringText({ ...zeroed, limits_verified: true })).toBe(LIMITS_VERIFIED_NOTE);
    expect(LIMITS_UNVERIFIED_NOTE).toContain('do not rely on them');
  });
  it('summarizes connection and motion state', () => {
    expect(connectionText(null)).toBe('Status unknown');
    expect(connectionText({ ...zeroed, connected: false })).toBe('Disconnected');
    expect(connectionText(zeroed)).toBe('Connected · ZC300-1A s/n 26017 · firmware 1.2 · ttyUSB1');
    expect(moveStateText(zeroed)).toBe('Idle');
    expect(moveStateText({ ...zeroed, busy: true })).toBe('Moving');
    expect(moveStateText({ ...zeroed, move_state: STAGE_MOVE_STATES.Faulted })).toBe('Faulted');
    expect(moveStateText({ ...zeroed, move_state: STAGE_MOVE_STATES.Homing })).toContain('started at the controller');
  });
  it('lists limit, e-stop and alarm indicators, flagging the dangerous ones, and never the floating home bit', () => {
    const items = stageIndicators({ ...zeroed, limit_negative: true, emergency_stop: true });
    expect(items.map(i => i.key)).toEqual(['limit-negative', 'limit-positive', 'emergency-stop', 'driver-alarm']);
    expect(items.filter(i => i.active).map(i => i.key)).toEqual(['limit-negative', 'emergency-stop']);
    expect(items.filter(i => i.alarm).map(i => i.key)).toEqual(['emergency-stop', 'driver-alarm']);
    expect(stageIndicators({ ...zeroed, home: true }).some(i => i.active)).toBe(false);
  });
  it('polls faster while the stage moves', () => {
    expect(pollIntervalMs(null)).toBe(1000);
    expect(pollIntervalMs(zeroed)).toBe(1000);
    expect(pollIntervalMs({ ...zeroed, busy: true })).toBe(250);
    expect(pollIntervalMs({ ...zeroed, move_state: STAGE_MOVE_STATES.Moving })).toBe(250);
  });
});
