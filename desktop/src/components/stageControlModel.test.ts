import { describe, expect, it } from 'vitest';
import type { StageStatus } from '../bridge';
import { STAGE_MOVE_STATES } from '../bridgeContract';
import { homed } from './stageTestFixtures';
import {
  JOG_STEPS_UM, LIMITS_UNVERIFIED_REASON, NOT_HOMED_REASON, connectionText, moveStateText, parseMicrons,
  pollIntervalMs, positionText, softLimitProblem, stageGate, stageIndicators, type StageGateInput, type StageGateKind,
} from './stageControlModel';

const input = (change: Partial<StageGateInput> = {}, status: Partial<StageStatus> | null = {}): StageGateInput => ({
  ready: true, experimentActive: false, mode: 'service', armed: true, status: status ? { ...homed, ...status } : null, ...change,
});
const all: StageGateKind[] = ['connect', 'disconnect', 'apply-profile', 'move', 'home', 'stop'];

describe('stage gate mirrors the backend rules', () => {
  it('allows a homed, verified, armed stage to move and to home', () => {
    expect(stageGate(input(), 'move')).toBe('');
    expect(stageGate(input(), 'home')).toBe('');
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
  it('refuses moves until the stage was homed this power-up', () => {
    expect(stageGate(input({}, { referenced: false }), 'move')).toBe(NOT_HOMED_REASON);
    expect(stageGate(input({}, { referenced: false }), 'home')).toBe(''); // Home is how it becomes homed
  });
  it('refuses Home until the limit switches were verified', () => {
    expect(stageGate(input({}, { limits_verified: false }), 'home')).toBe(LIMITS_UNVERIFIED_REASON);
    expect(LIMITS_UNVERIFIED_REASON).toContain('zc300ctl verify-limits --supervised');
    expect(stageGate(input({}, { limits_verified: false }), 'move')).toBe(''); // already-homed moves are unaffected
  });
  it('refuses motion when the controller does not match the profile, on e-stop, alarm or while busy', () => {
    for (const status of [{ configured: false }, { emergency_stop: true }, { driver_alarm: true }, { busy: true }]) {
      expect(stageGate(input({}, status), 'move')).not.toBe('');
      expect(stageGate(input({}, status), 'home')).not.toBe('');
    }
  });
  it('needs Service mode and arming for motion, but not for connecting', () => {
    expect(stageGate(input({ mode: 'operator' }), 'move')).toContain('Service');
    expect(stageGate(input({ armed: false }), 'home')).toContain('Arm');
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
});

describe('stage inputs', () => {
  it('accepts only whole micrometres', () => {
    expect(parseMicrons('-250', 'x')).toBe(-250);
    expect(parseMicrons(' 40 ', 'x')).toBe(40);
    for (const bad of ['', ' ', '12.5', 'abc', 'NaN', 'Infinity', '1e9', '2000000']) expect(() => parseMicrons(bad, 'Target position')).toThrow('whole number of micrometres');
  });
  it('checks targets against the soft limits, relative to the rounded position', () => {
    expect(softLimitProblem(homed, 2900, false)).toBe('');
    expect(softLimitProblem(homed, -2900, false)).toBe('');
    expect(softLimitProblem(homed, 2901, false)).toContain('outside the soft limits');
    expect(softLimitProblem(homed, 100, true)).toBe(''); // 120 + 100 = 220
    expect(softLimitProblem({ ...homed, position_um: 2850.4 }, 100, true)).toContain('2950 µm is outside');
    expect(softLimitProblem({ ...homed, referenced: false }, 0, false)).toBe(NOT_HOMED_REASON);
  });
  it('offers small whole-micrometre jog steps only', () => {
    expect(JOG_STEPS_UM.every(v => Number.isInteger(v) && v >= 1 && v <= 1000)).toBe(true);
  });
});

describe('stage display', () => {
  it('never presents the counter as a position before Home', () => {
    expect(positionText(homed)).toBe('120.4 µm');
    expect(positionText({ ...homed, referenced: false, position_um: -6564.99 })).toBe('unknown until Home (controller counter -6565.0 µm)');
  });
  it('summarizes connection and motion state', () => {
    expect(connectionText(null)).toBe('Status unknown');
    expect(connectionText({ ...homed, connected: false })).toBe('Disconnected');
    expect(connectionText(homed)).toBe('Connected · ZC300-1A s/n 26017 · firmware 1.2 · ttyUSB1');
    expect(moveStateText(homed)).toBe('Idle');
    expect(moveStateText({ ...homed, busy: true })).toBe('Moving');
    expect(moveStateText({ ...homed, move_state: STAGE_MOVE_STATES.Faulted })).toBe('Faulted');
  });
  it('lists limit, e-stop and alarm indicators, flagging the dangerous ones', () => {
    const items = stageIndicators({ ...homed, limit_negative: true, emergency_stop: true });
    expect(items.map(i => i.key)).toEqual(['limit-negative', 'limit-positive', 'home-sensor', 'emergency-stop', 'driver-alarm']);
    expect(items.filter(i => i.active).map(i => i.key)).toEqual(['limit-negative', 'emergency-stop']);
    expect(items.filter(i => i.alarm).map(i => i.key)).toEqual(['emergency-stop', 'driver-alarm']);
  });
  it('polls faster while the stage moves', () => {
    expect(pollIntervalMs(null)).toBe(1000);
    expect(pollIntervalMs(homed)).toBe(1000);
    expect(pollIntervalMs({ ...homed, busy: true })).toBe(250);
    expect(pollIntervalMs({ ...homed, move_state: STAGE_MOVE_STATES.Moving })).toBe(250);
  });
});
