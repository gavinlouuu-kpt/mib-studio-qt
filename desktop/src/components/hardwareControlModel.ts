import type { AutofocusConfig, CmdResult } from '../bridge';
import { canActuate, type OperatingMode } from '../commissioning';
export function numericInput(value: string, name: string, min: number, max = Infinity, integer = false): number {
  const result = Number(value);
  if (!value.trim() || !Number.isFinite(result) || result < min || result > max || (integer && !Number.isInteger(result))) throw new Error(`${name} must be ${integer ? 'an integer' : 'a finite number'} between ${min} and ${max}.`);
  return result;
}
export function validateFocusConfig(config: AutofocusConfig): void {
  for (const [key, value] of Object.entries(config)) if (typeof value === 'number' && !Number.isFinite(value)) throw new Error(`${key} must be finite.`);
  if (!(config.min_voltage < config.max_voltage)) throw new Error('Minimum voltage must be below maximum voltage.');
  for (const key of ['initial_voltage', 'safe_shutdown_voltage'] as const) if (config[key] < config.min_voltage || config[key] > config.max_voltage) throw new Error(`${key} must be inside the voltage range.`);
  for (const key of ['voltage_step', 'fine_voltage_step', 'manual_voltage_step', 'ring_ratio_stale_ms'] as const) if (!(config[key] > 0)) throw new Error(`${key} must be positive.`);
  if (config.focus_range < 0 || config.focus_setpoint < 0) throw new Error('Focus range and setpoint must be non-negative.');
  if (!Number.isInteger(config.ring_ratio_stale_ms) || !Number.isInteger(config.min_samples_per_step) || config.min_samples_per_step < 1) throw new Error('Staleness and minimum samples must be positive integers.');
}
export function hardwareGate(input: {ready: boolean; experimentActive: boolean; connected: boolean; mode: OperatingMode; armed: boolean}, kind: 'configure' | 'actuate' | 'stop'): string {
  if (!input.ready) return 'Backend is not ready.';
  if (kind === 'stop') return '';
  if (input.experimentActive) return 'Hardware changes are locked while an experiment is active.';
  if (kind === 'configure') return '';
  if (!input.connected) return 'Device is not connected.';
  return canActuate({...input, triggerAttached: input.connected}).reason;
}
/** Synchronous ownership before awaiting blocks double clicks and serial overlap. */
export class HardwareCommandOwner {
  private busy = false;
  async run(command: () => Promise<CmdResult>): Promise<CmdResult> {
    if (this.busy) throw new Error('A hardware command is already in progress.');
    this.busy = true;
    try {
      const result = await command();
      if (!result.ok) throw new Error(result.message || 'Hardware command failed.');
      return result;
    } finally { this.busy = false; }
  }
}
