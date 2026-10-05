import { describe, expect, it, vi } from 'vitest';
import { hardwareGate, HardwareCommandOwner, numericInput, validateFocusConfig } from './hardwareControlModel';
import type { AutofocusConfig, CmdResult } from '../bridge';
const allowed = {ready: true, experimentActive: false, connected: true, mode: 'service' as const, armed: true};
export const focusConfig: AutofocusConfig = {valid: true, focus_setpoint: 1, focus_range: .1, voltage_step: 1, fine_voltage_step: .1, min_voltage: 0, max_voltage: 100, initial_voltage: 50, manual_voltage_step: 1, ring_ratio_stale_ms: 500, min_samples_per_step: 1, safe_shutdown_voltage: 0, require_new_sample_per_step: true, focus_direction: true};
describe('hardware command safeguards', () => {
  it('keeps stops possible after disarming, experiment start and status loss', () => {
    const state = {...allowed, mode: 'operator' as const, armed: false, experimentActive: true, connected: false};
    expect(hardwareGate(state, 'stop')).toBe('');
    expect(hardwareGate(state, 'configure')).not.toBe('');
    expect(hardwareGate(state, 'actuate')).not.toBe('');
    expect(hardwareGate({...state, ready: false}, 'stop')).not.toBe('');
  });
  it('requires service mode, arming and connection for actuation', () => {
    expect(hardwareGate(allowed, 'actuate')).toBe('');
    for (const change of [{mode: 'operator' as const}, {armed: false}, {connected: false}, {experimentActive: true}, {ready: false}]) expect(hardwareGate({...allowed, ...change}, 'actuate')).not.toBe('');
  });
  it('rejects blank, infinite, fractional and out-of-range serial fields', () => {
    for (const value of ['', ' ', 'NaN', 'Infinity', '1.5', '0', '248']) expect(() => numericInput(value, 'address', 1, 247, true)).toThrow();
    expect(numericInput('247', 'address', 1, 247, true)).toBe(247);
  });
  it('rejects unsafe voltage bounds and invalid sample policy', () => {
    expect(() => validateFocusConfig(focusConfig)).not.toThrow();
    for (const change of [{min_voltage: 100}, {safe_shutdown_voltage: -1}, {initial_voltage: 101}, {voltage_step: 0}, {fine_voltage_step: NaN}, {ring_ratio_stale_ms: .5}, {min_samples_per_step: 0}]) expect(() => validateFocusConfig({...focusConfig, ...change})).toThrow();
  });
  it('holds ownership until completion and releases after rejected commands', async () => {
    const owner = new HardwareCommandOwner();
    let finish!: (value: CmdResult) => void;
    const pending = owner.run(() => new Promise(resolve => { finish = resolve; }));
    const duplicate = vi.fn();
    await expect(owner.run(duplicate)).rejects.toThrow('already in progress');
    expect(duplicate).not.toHaveBeenCalled();
    finish({ok: false, command: 10, operation_id: '0', message: 'Port occupied'});
    await expect(pending).rejects.toThrow('Port occupied');
    await expect(owner.run(async () => ({ok: true, command: 10, operation_id: '0', message: 'Stopped'}))).resolves.toMatchObject({ok: true});
  });
});
