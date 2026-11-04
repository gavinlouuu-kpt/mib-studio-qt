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

// Epic #868 (Qt parity): routine pump and autofocus controls follow the Qt panels, which gate only
// on "connected" (SyringePumpTab.cpp:293-342, NanopositionerTab.cpp:326-332): no Service mode, no
// arm, and usable while an experiment runs. Connecting or disconnecting a device stays locked during
// an experiment (a recorded Tauri difference). Stop and disable need only a ready backend.
export type DeviceAction = 'connect' | 'operate' | 'stop';
export function deviceGate(input: {ready: boolean; experimentActive: boolean; connected: boolean}, kind: DeviceAction): string {
  if (!input.ready) return 'Backend is not ready.';
  if (kind === 'stop') return '';
  if (kind === 'connect') return input.experimentActive ? 'Connecting or disconnecting a device is locked while an experiment is active.' : '';
  return input.connected ? '' : 'Device is not connected.';
}

// Ranges of the Qt widgets (resources/ui/SyringePumpTab.ui:68-82, SyringePumpSettingsDialog.ui:93-106,
// NanopositionerTab.ui:212-245,308-361).
export const PUMP_FLOW_MAX = 100000;
export const PUMP_FLOW_DECIMALS = 4;
export const SYRINGE_VOLUME_MIN = 0.1;
export const SYRINGE_VOLUME_MAX = 9999;
export const RING_WIDTH_MIN = 1, RING_WIDTH_MAX = 100, RING_WIDTH_DEFAULT = 20;
export const JOG_STEP_MIN = 1, JOG_STEP_MAX = 100, JOG_STEP_DEFAULT = 1;
export const AUTO_APPLY_DEBOUNCE_MS = 300;
export const STATUS_POLL_MS = 500;

/** A flow-rate entry rounded to the Qt spin box's 4 decimals. */
export function flowRateInput(value: string, name = 'Flow rate'): number {
  const rate = numericInput(value, name, 0, PUMP_FLOW_MAX);
  return Math.round(rate * 10 ** PUMP_FLOW_DECIMALS) / 10 ** PUMP_FLOW_DECIMALS;
}

/** The pump register holds a whole number (Qt casts to uint16 and drops decimals). A fractional
 *  mL volume is sent as whole µL when it fits; anything else that is not whole is refused. */
export function syringeVolumeForDevice(value: string, unit: number): {volume: number; unit: number} {
  const volume = numericInput(value, 'Syringe volume', SYRINGE_VOLUME_MIN, SYRINGE_VOLUME_MAX);
  const tenths = Math.round(volume * 10) / 10;
  if (Number.isInteger(tenths)) return {volume: tenths, unit};
  const microliters = Math.round(tenths * 1000);
  if (unit === 103 && microliters <= SYRINGE_VOLUME_MAX) return {volume: microliters, unit: 100};
  throw new Error(`The pump stores whole ${unit === 103 ? 'mL' : 'µL'}: enter ${Math.round(tenths)} or a whole value.`);
}
