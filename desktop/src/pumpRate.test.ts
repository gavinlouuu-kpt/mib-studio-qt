import { describe, expect, it } from "vitest";
import { MICROLITERS_PER_MIN_UNIT, RPM_UNIT, directionLabel, flowToRpm, rpmToMicrolitersPerMin } from "./pumpRate";

describe("peristaltic pump rate in rpm (#501)", () => {
  it("converts head rpm to the flow the backend takes, and back", () => {
    expect(rpmToMicrolitersPerMin(0.4, 25)).toBeCloseTo(10, 9); // the instrument's 0.4 rpm = 10 µL/min
    expect(rpmToMicrolitersPerMin(0, 25)).toBe(0);
    expect(flowToRpm(10, MICROLITERS_PER_MIN_UNIT, 25)).toBeCloseTo(0.4, 9);
    expect(flowToRpm(0.01, 103, 25)).toBeCloseTo(0.4, 9); // mL/min
    expect(RPM_UNIT).not.toBe(MICROLITERS_PER_MIN_UNIT);
  });

  it("refuses an rpm without a usable calibration, instead of sending a wrong flow", () => {
    expect(() => rpmToMicrolitersPerMin(1, 0)).toThrow(/calibration/);
    expect(() => rpmToMicrolitersPerMin(-1, 25)).toThrow();
    expect(() => rpmToMicrolitersPerMin(Number.NaN, 25)).toThrow();
    expect(flowToRpm(10, 100, undefined)).toBeNull();
    expect(flowToRpm(10, 77, 25)).toBeNull(); // an unknown unit
  });

  it("labels the direction as CW/CCW only for the peristaltic heads", () => {
    expect(directionLabel(true, 0)).toBe("Infuse (CCW)");
    expect(directionLabel(true, 1)).toBe("Withdraw (CW)");
    expect(directionLabel(false, 0)).toBe("Infuse");
  });
});
