// @vitest-environment node
// The Monitoring trigger tests' limits (HW-044, HW-045): Qt's spin box ranges, refused with a message, never replaced.
import { describe, expect, it } from 'vitest';
import { PERIODIC_MS_MAX, PERIODIC_MS_MIN, PULSE_US_MAX, PULSE_US_MIN, parsePeriodicMs, parsePulseUs, periodicMsProblem, pulseUsProblem } from './triggerTestModel';

describe('pulse duration', () => {
  it("is 1 to 1,000,000 µs, as Qt's spin box", () => {
    expect([PULSE_US_MIN, PULSE_US_MAX]).toEqual([1, 1_000_000]);
    expect(parsePulseUs('1')).toBe(1); expect(parsePulseUs('1000000')).toBe(1_000_000); expect(parsePulseUs(' 250 ')).toBe(250);
  });
  it('blank, zero, fractional, negative, over the limit or not a number is refused with a message (the old code turned blank/0 into 1)', () => {
    for (const bad of ['', '   ', '0', '-5', '1.5', '1000001', 'abc', '1e9']) {
      expect(pulseUsProblem(bad), JSON.stringify(bad)).toMatch(/between 1 and 1,000,000/);
      expect(() => parsePulseUs(bad)).toThrow(/between 1 and 1,000,000/);
    }
    expect(pulseUsProblem('12')).toBe('');
  });
});
describe('periodic interval', () => {
  it("is 10 to 60000 ms, as Qt's spin box", () => {
    expect([PERIODIC_MS_MIN, PERIODIC_MS_MAX]).toEqual([10, 60000]);
    expect(parsePeriodicMs('10')).toBe(10); expect(parsePeriodicMs('60000')).toBe(60000); expect(parsePeriodicMs('1000')).toBe(1000);
  });
  it('anything else is refused with a message (the old code turned blank/0 into 1000 and let 1 ms through)', () => {
    for (const bad of ['', '0', '9', '60001', '10.5', 'x']) {
      expect(periodicMsProblem(bad), JSON.stringify(bad)).toMatch(/between 10 and 60000/);
      expect(() => parsePeriodicMs(bad)).toThrow(/between 10 and 60000/);
    }
  });
});
