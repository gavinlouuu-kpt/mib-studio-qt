// Operator pump rules (Tauri parity wave 1, hardware stream; epic #868, bug #855): input ranges and the exact-integer
// conversions the dLSP syringe pump needs, the serial command queue, and the hold-to-purge state machine. Pure logic, no
// React, so the behaviour is unit-tested without a DOM.
import type { CmdResult } from '../bridge';
import { HardwareCommandOwner } from './hardwareControlModel';

/** Backend flow/volume unit codes (the dLSP register values; see SyringePumpService). */
export const UNIT_UL = 100;
export const UNIT_ML = 103;
/** Qt applies a rate/unit/direction edit 300 ms after its trigger: the rate spin box's editingFinished (Enter or focus out), a
 *  unit or direction change (SyringePumpTab.cpp:99-116). */
export const AUTO_APPLY_MS = 300;
/** Qt reads every connected pump every 500 ms (SyringePumpTab.cpp:127). */
export const POLL_MS = 500;
export const PURGE_TOOLTIP = 'Run pump at full speed (hold to purge)';
/** Qt's baud spin box: 1200-921600 in steps of 1200, default 115200 (SyringePumpSettingsDialog.ui:48-53). */
export const BAUD_MIN = 1200;
export const BAUD_MAX = 921600;
export const DEFAULT_BAUD = 115200;
export const BAUD_RATES = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
export const RATE_MAX = 100000;
export const RATE_DECIMALS = 4;
export const VOLUME_MIN = 0.1;
export const VOLUME_MAX = 9999;
export const VOLUME_DECIMALS = 1;
/** The dLSP holds flow rate and syringe volume as whole numbers 1-9999 (registers 0x006A and 0x0061). */
export const DEVICE_INTEGER_MAX = 9999;

const errorText = (failure: unknown) => failure instanceof Error ? failure.message : String(failure);
export { errorText };

/** A plain decimal ("12", "0.5", ".25") within [min, max] with at most `decimals` decimals. */
export function parseDecimal(text: string, name: string, min: number, max: number, decimals: number): number {
  const trimmed = text.trim();
  if (!/^(\d+\.?\d*|\.\d+)$/.test(trimmed)) throw new Error(`${name} must be a number from ${min} to ${max}.`);
  const fraction = trimmed.split('.')[1] ?? '';
  if (fraction.length > decimals) throw new Error(`${name} takes at most ${decimals} decimal${decimals === 1 ? '' : 's'}.`);
  const value = Number(trimmed);
  if (!(value >= min && value <= max)) throw new Error(`${name} must be from ${min} to ${max}.`);
  return value;
}

const unitName = (unit: number, kind: 'rate' | 'volume') => `${unit === UNIT_ML ? 'mL' : 'µL'}${kind === 'rate' ? '/min' : ''}`;
const trimNumber = (value: number, decimals: number) => String(Number(value.toFixed(decimals)));
export const formatRate = (value: number) => trimNumber(value, RATE_DECIMALS);

/** The whole number (1-9999) and unit the dLSP takes for `value` in `unit`, exactly: the requested unit when the value is
 *  already a whole number there, else the other unit (0.5 mL -> 500 µL). Null when no exact whole number exists. */
function exactDeviceNumber(value: number, decimals: number, unit: number): {value: number; unit: number} | null {
  const scale = 10 ** decimals;
  const scaled = Math.round(value * scale);          // value in 1/scale of `unit`: an integer for a value with <= decimals decimals
  const micro = unit === UNIT_ML ? scaled * 1000 : scaled; // the same quantity in 1/scale µL
  for (const candidate of unit === UNIT_ML ? [UNIT_ML, UNIT_UL] : [UNIT_UL, UNIT_ML]) {
    const divisor = (candidate === UNIT_ML ? 1000 : 1) * scale;
    if (micro % divisor !== 0) continue;
    const whole = micro / divisor;
    if (whole >= 1 && whole <= DEVICE_INTEGER_MAX) return {value: whole, unit: candidate};
  }
  return null;
}

/** HW-019 / D9. Qt casts the volume to uint16 (0.5 mL would arrive as 0): send an exact whole number or refuse. */
export function syringeVolumeForDevice(volume: number, unit: number): {value: number; unit: number} {
  const exact = exactDeviceNumber(volume, VOLUME_DECIMALS, unit);
  if (!exact) throw new Error(`The syringe pump takes a whole number of 1 to ${DEVICE_INTEGER_MAX} µL or mL; ${trimNumber(volume, VOLUME_DECIMALS)} ${unitName(unit, 'volume')} cannot be sent exactly.`);
  return exact;
}

/** Same rule for the dLSP flow rate. The backend clamps the rate to a whole 1-9999 without a word (a 0.5 mL/min request
 *  would run at 1 mL/min), so the panel sends an exact whole number or refuses. A peristaltic pump takes any rate. */
export function syringeRateForDevice(rate: number, unit: number): {value: number; unit: number} {
  if (rate === 0) throw new Error('The syringe pump cannot be set to a flow rate of 0; press Stop to stop it.');
  const exact = exactDeviceNumber(rate, RATE_DECIMALS, unit);
  if (!exact) throw new Error(`The syringe pump takes a whole number of 1 to ${DEVICE_INTEGER_MAX} µL/min or mL/min; ${formatRate(rate)} ${unitName(unit, 'rate')} cannot be sent exactly.`);
  return exact;
}

/** A queued command that was dropped before it reached the bridge (Stop, disconnect, the pump gone). */
export class CommandCancelled extends Error {
  constructor(reason: string) { super(reason); this.name = 'CommandCancelled'; }
}

/** Operator commands on one pump run one at a time (a set rate never overlaps a Start on the serial bus), each waiting for
 *  its turn instead of being refused. Stop and the purge release never go through it (#855). `invalidate()` drops every
 *  command that is still waiting for its turn, so a Start queued behind a slow command cannot fire after the operator pressed
 *  Stop; a command already handed to the bridge cannot be taken back. */
export class PumpCommandQueue {
  private tail: Promise<unknown> = Promise.resolve();
  private readonly owner = new HardwareCommandOwner();
  private generation = 0;
  private reason = 'cancelled';
  invalidate(reason: string): void { this.generation++; this.reason = reason; }
  run(command: () => Promise<CmdResult>): Promise<CmdResult> {
    const generation = this.generation;
    const result = this.tail.then(() => {
      // Checked right before the command is handed to the bridge, in the same tick.
      if (generation !== this.generation) throw new CommandCancelled(this.reason);
      return this.owner.run(command);
    });
    this.tail = result.catch(() => undefined);
    return result;
  }
}

export type PurgeStart = {
  /** True once the operator let go: a queued start that has not been sent yet must not be sent. */
  cancelled: () => boolean;
  /** Call right before the purge command is handed to the bridge. */
  markSent: () => void;
};
export type PurgeHooks = {
  /** Queued purge start; resolves true when the device accepted it. May reject. */
  start: (attempt: PurgeStart) => Promise<boolean>;
  /** Purge release; never queued behind other commands. */
  stop: () => Promise<unknown>;
  onError: (message: string) => void;
};

/** HW-012: the purge runs only while the operator holds the control, as Qt's pressed/released signals do. The release is
 *  never lost: let go while the start is still queued and nothing is sent; let go while the start is in flight and the stop
 *  goes out as soon as it returned. */
export class PurgeHold {
  private phase: 'idle' | 'starting' | 'held' = 'idle';
  private released = false;
  private abandoned = false;
  constructor(private readonly hooks: PurgeHooks) {}
  get active() { return this.phase !== 'idle'; }
  press(): void {
    if (this.phase !== 'idle') return;
    this.phase = 'starting'; this.released = false; this.abandoned = false;
    let sent = false;
    const attempt: PurgeStart = {cancelled: () => this.released, markSent: () => { sent = true; }};
    void this.hooks.start(attempt).then(
      accepted => ({accepted, failure: undefined as unknown}),
      failure => ({accepted: false, failure}),
    ).then(({accepted, failure}) => {
      if (failure !== undefined) this.hooks.onError(errorText(failure));
      if (this.released) {
        this.phase = 'idle';
        // The device may hold a purge even when the start reported a failure; stop it unless the connection is gone.
        if (sent && !this.abandoned) this.sendStop();
      // Still held: a start that reached the device stays held even when it reported a failure (a lost reply may leave the
      // pump purging), so the release sends the stop, as Qt's release always does (SyringePumpTab.cpp:79-81).
      } else this.phase = accepted || sent ? 'held' : 'idle';
    });
  }
  release(): void {
    if (this.phase === 'idle') return;
    if (this.phase === 'held') { this.phase = 'idle'; this.sendStop(); return; }
    this.released = true;
  }
  /** The pump went away (disconnected): forget the hold without a stop command the device cannot take. */
  abandon(): void {
    if (this.phase === 'held') this.phase = 'idle';
    else if (this.phase === 'starting') { this.released = true; this.abandoned = true; }
  }
  private sendStop(): void {
    void Promise.resolve().then(() => this.hooks.stop()).catch(failure => this.hooks.onError(errorText(failure)));
  }
}
