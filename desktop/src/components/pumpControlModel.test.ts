import { describe, expect, it, vi } from 'vitest';
import type { CmdResult } from '../bridge';
import { PumpCommandQueue, PurgeHold, UNIT_ML, UNIT_UL, formatRate, parseDecimal, syringeRateForDevice, syringeVolumeForDevice, type PurgeHooks } from './pumpControlModel';

const ok = (message = 'ok'): CmdResult => ({ok: true, command: 10, message, operation_id: '0'});
const deferred = <T,>() => { let resolve!: (value: T) => void, reject!: (reason: unknown) => void; const promise = new Promise<T>((a, b) => { resolve = a; reject = b; }); return {promise, resolve, reject}; };
const settle = async () => { for (let i = 0; i < 6; i++) await Promise.resolve(); };

describe('parseDecimal (Qt spin box ranges)', () => {
  it('accepts plain decimals inside the range and the decimals limit', () => {
    expect(parseDecimal('12', 'Rate', 0, 100000, 4)).toBe(12);
    expect(parseDecimal(' 0.5 ', 'Rate', 0, 100000, 4)).toBe(0.5);
    expect(parseDecimal('.25', 'Rate', 0, 100000, 4)).toBe(0.25);
    expect(parseDecimal('100000', 'Rate', 0, 100000, 4)).toBe(100000);
    expect(parseDecimal('0.0001', 'Rate', 0, 100000, 4)).toBe(0.0001);
  });
  it('refuses blank, signs, exponents, non-numbers, too many decimals and out-of-range values', () => {
    for (const text of ['', ' ', '-1', '+1', '1e3', 'NaN', 'Infinity', '1,5', '0x10', '1.2.3', '100000.0001', '100001', '0.00001'])
      expect(() => parseDecimal(text, 'Rate', 0, 100000, 4), text).toThrow(/Rate/);
    expect(() => parseDecimal('0.0', 'Volume', 0.1, 9999, 1)).toThrow('from 0.1 to 9999');
    expect(() => parseDecimal('1.25', 'Volume', 0.1, 9999, 1)).toThrow('at most 1 decimal');
  });
  it('formats a configured rate without trailing zeros', () => {
    expect([formatRate(12.5), formatRate(100), formatRate(0.0001), formatRate(1 / 3)]).toEqual(['12.5', '100', '0.0001', '0.3333']);
  });
});

describe('exact device numbers (D9: no silent truncation)', () => {
  it('sends a volume that is already a whole number as typed', () => {
    expect(syringeVolumeForDevice(100, UNIT_UL)).toEqual({value: 100, unit: UNIT_UL});
    expect(syringeVolumeForDevice(5, UNIT_ML)).toEqual({value: 5, unit: UNIT_ML});
    expect(syringeVolumeForDevice(9999, UNIT_UL)).toEqual({value: 9999, unit: UNIT_UL});
    expect(syringeVolumeForDevice(1, UNIT_UL)).toEqual({value: 1, unit: UNIT_UL});
  });
  it('converts to the other unit when that makes a whole number: 0.5 mL is 500 µL', () => {
    expect(syringeVolumeForDevice(0.5, UNIT_ML)).toEqual({value: 500, unit: UNIT_UL});
    expect(syringeVolumeForDevice(0.1, UNIT_ML)).toEqual({value: 100, unit: UNIT_UL});
    expect(syringeVolumeForDevice(2.5, UNIT_ML)).toEqual({value: 2500, unit: UNIT_UL});
    expect(syringeVolumeForDevice(1000, UNIT_UL)).toEqual({value: 1000, unit: UNIT_UL});
    expect(syringeVolumeForDevice(5000, UNIT_UL)).toEqual({value: 5000, unit: UNIT_UL});
  });
  it('refuses what the device cannot take exactly, with a message, instead of truncating', () => {
    for (const [volume, unit] of [[0.1, UNIT_UL], [0.5, UNIT_UL], [1.5, UNIT_UL], [12.5, UNIT_ML]] as const)
      expect(() => syringeVolumeForDevice(volume, unit), `${volume} ${unit}`).toThrow('cannot be sent exactly');
    expect(() => syringeVolumeForDevice(0.5, UNIT_UL)).toThrow('0.5 µL cannot be sent exactly');
    expect(() => syringeVolumeForDevice(12.5, UNIT_ML)).toThrow('12.5 mL');
  });
  it('applies the same rule to the dLSP flow rate, which the backend would clamp to a whole 1-9999 in silence', () => {
    expect(syringeRateForDevice(20, UNIT_UL)).toEqual({value: 20, unit: UNIT_UL});
    expect(syringeRateForDevice(2, UNIT_ML)).toEqual({value: 2, unit: UNIT_ML});
    expect(syringeRateForDevice(0.5, UNIT_ML)).toEqual({value: 500, unit: UNIT_UL});
    expect(syringeRateForDevice(12000, UNIT_UL)).toEqual({value: 12, unit: UNIT_ML});
    expect(() => syringeRateForDevice(2.5, UNIT_UL)).toThrow('2.5 µL/min cannot be sent exactly');
    expect(() => syringeRateForDevice(0.0001, UNIT_ML)).toThrow('cannot be sent exactly');
    expect(syringeRateForDevice(100000, UNIT_UL)).toEqual({value: 100, unit: UNIT_ML});
    expect(() => syringeRateForDevice(10000.5, UNIT_UL)).toThrow('cannot be sent exactly');
    expect(() => syringeRateForDevice(10001, UNIT_UL)).toThrow('cannot be sent exactly'); // 10001 µL/min is no whole mL/min
    expect(() => syringeRateForDevice(0, UNIT_UL)).toThrow('Stop');
  });
});

describe('PumpCommandQueue', () => {
  it('runs commands one after another in call order and keeps going after a failure', async () => {
    const queue = new PumpCommandQueue();
    const first = deferred<CmdResult>();
    const log: string[] = [];
    const a = queue.run(async () => { log.push('a start'); const r = await first.promise; log.push('a end'); return r; });
    const b = queue.run(async () => { log.push('b'); return {...ok(), ok: false, message: 'Device stalled'}; });
    const c = queue.run(async () => { log.push('c'); return ok('c done'); });
    const failed = expect(b).rejects.toThrow('Device stalled');
    await settle();
    expect(log).toEqual(['a start']);
    first.resolve(ok());
    await a; await failed;
    await expect(c).resolves.toMatchObject({message: 'c done'});
    expect(log).toEqual(['a start', 'a end', 'b', 'c']);
  });
});

function hold(over: Partial<PurgeHooks> = {}) {
  const started = deferred<boolean>();
  const calls: string[] = [];
  const hooks: PurgeHooks = {
    start: vi.fn(async attempt => { calls.push('start requested'); if (attempt.cancelled()) return true; attempt.markSent(); calls.push('start sent'); return started.promise; }),
    stop: vi.fn(async () => { calls.push('stop'); }),
    onError: vi.fn(),
    ...over,
  };
  return {hooks, started, calls, purge: new PurgeHold(hooks)};
}

describe('PurgeHold (hold-to-purge, HW-012)', () => {
  it('starts on press and stops on release', async () => {
    const {purge, started, calls} = hold();
    purge.press();
    started.resolve(true); await settle();
    expect(purge.active).toBe(true);
    purge.release(); await settle();
    expect(calls).toEqual(['start requested', 'start sent', 'stop']);
    expect(purge.active).toBe(false);
  });
  it('ignores a second press while held and a release when idle', async () => {
    const {purge, hooks, started} = hold();
    purge.release();
    purge.press(); purge.press();
    started.resolve(true); await settle(); purge.press();
    expect(hooks.start).toHaveBeenCalledTimes(1);
    purge.release(); purge.release(); await settle();
    expect(hooks.stop).toHaveBeenCalledTimes(1);
  });
  it('sends the stop after a start that was still in flight when the operator let go', async () => {
    const {purge, hooks, started, calls} = hold();
    purge.press(); await settle();
    purge.release(); await settle();
    expect(hooks.stop).not.toHaveBeenCalled();          // the start has not returned yet: stopping now could overtake it
    started.resolve(true); await settle();
    expect(calls).toEqual(['start requested', 'start sent', 'stop']);
    expect(purge.active).toBe(false);
  });
  it('sends nothing when the start never left the queue before the release', async () => {
    const queued = deferred<void>();
    const {purge, hooks} = hold({start: vi.fn(async attempt => { await queued.promise; if (attempt.cancelled()) return true; attempt.markSent(); return true; })});
    purge.press(); purge.release();
    queued.resolve(); await settle();
    expect(hooks.stop).not.toHaveBeenCalled();
    expect(purge.active).toBe(false);
    purge.press(); await settle();                       // usable again
    expect(hooks.start).toHaveBeenCalledTimes(2);
  });
  it('stops a purge whose start reported a failure when the operator already let go, and reports the failure', async () => {
    const {purge, hooks} = hold({start: vi.fn(async attempt => { attempt.markSent(); throw new Error('Pump purge failed'); })});
    purge.press(); purge.release(); await settle();
    expect(hooks.onError).toHaveBeenCalledWith('Pump purge failed');
    expect(hooks.stop).toHaveBeenCalledTimes(1);
  });
  it('is idle again after a failed start while still held, and can be pressed once more', async () => {
    let fail = true;
    const {purge, hooks} = hold({start: vi.fn(async attempt => { attempt.markSent(); if (fail) throw new Error('busy'); return true; })});
    purge.press(); await settle();
    expect(purge.active).toBe(false);
    expect(hooks.stop).not.toHaveBeenCalled();
    fail = false; purge.press(); await settle();
    expect(purge.active).toBe(true);
  });
  it('reports a failing stop', async () => {
    const {purge, hooks, started} = hold({stop: vi.fn(async () => { throw new Error('Port gone'); })});
    purge.press(); started.resolve(true); await settle();
    purge.release(); await settle();
    expect(hooks.onError).toHaveBeenCalledWith('Port gone');
  });
  it('abandon forgets a hold without sending a stop the device cannot take', async () => {
    const held = hold();
    held.purge.press(); held.started.resolve(true); await settle();
    held.purge.abandon(); await settle();
    expect(held.hooks.stop).not.toHaveBeenCalled();
    expect(held.purge.active).toBe(false);
    const starting = hold();
    starting.purge.press(); await settle();
    starting.purge.abandon(); starting.started.resolve(true); await settle();
    expect(starting.hooks.stop).not.toHaveBeenCalled();
    expect(starting.purge.active).toBe(false);
  });
});
