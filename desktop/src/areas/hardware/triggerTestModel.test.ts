import { describe, expect, it } from "vitest";
import { APPLY_DELAY_MS, parsePeriodicMs, parsePulseUs, PERIODIC_MS, PULSE_US, TOOLTIP } from "./triggerTestModel";
// The Qt widgets these ranges, defaults and tooltips are copied from: this fails when the .ui and the model drift apart.
import qtUi from "../../../../resources/ui/ExperimentMonitoringTab.ui?raw";

/** The text of the property `name` of the widget `widget` in the Qt Designer file. */
function property(widget: string, name: string): string {
  const block = new RegExp(`name="${widget}"[\\s\\S]*?</widget>`).exec(qtUi)?.[0] ?? "";
  return new RegExp(`name="${name}">\\s*<(?:number|string)>([^<]*)<`).exec(block)?.[1] ?? "";
}

describe("Qt parity of the trigger test fields", () => {
  it("pulse duration: 1 - 1,000,000 us, default 1", () => {
    expect([property("triggerDurationSpin", "minimum"), property("triggerDurationSpin", "maximum"), property("triggerDurationSpin", "value")].map(Number))
      .toEqual([PULSE_US.min, PULSE_US.max, PULSE_US.default]);
  });
  it("periodic interval: 10 - 60,000 ms, default 1000", () => {
    expect([property("periodicTriggerIntervalSpin", "minimum"), property("periodicTriggerIntervalSpin", "maximum"), property("periodicTriggerIntervalSpin", "value")].map(Number))
      .toEqual([PERIODIC_MS.min, PERIODIC_MS.max, PERIODIC_MS.default]);
  });
  it("tooltips", () => {
    expect(property("sortTriggerBtn", "toolTip")).toBe(TOOLTIP.sortTrigger);
    expect(property("triggerDurationSpin", "toolTip")).toBe(TOOLTIP.pulseDuration);
    expect(property("periodicTriggerBtn", "toolTip")).toBe(TOOLTIP.periodicTest);
    expect(property("periodicTriggerIntervalSpin", "toolTip")).toBe(TOOLTIP.periodicInterval);
  });
});

describe("parsePulseUs", () => {
  it("accepts whole numbers in range, with surrounding blanks", () => {
    expect(parsePulseUs("1")).toEqual({ ok: true, value: 1 });
    expect(parsePulseUs(" 250 ")).toEqual({ ok: true, value: 250 });
    expect(parsePulseUs("1000000")).toEqual({ ok: true, value: 1_000_000 });
  });
  it.each(["", "  ", "0", "-1", "1000001", "2.5", "abc", "1e3x", "Infinity", "NaN"])("refuses %j with a reason, never a default", (text) => {
    const parsed = parsePulseUs(text);
    expect(parsed.ok).toBe(false);
    expect(parsed.ok ? "" : parsed.reason).toBe("Pulse duration must be a whole number of µs between 1 and 1,000,000.");
  });
});

describe("parsePeriodicMs", () => {
  it("accepts whole numbers in range", () => {
    expect(parsePeriodicMs("10")).toEqual({ ok: true, value: 10 });
    expect(parsePeriodicMs("1000")).toEqual({ ok: true, value: 1000 });
    expect(parsePeriodicMs("60000")).toEqual({ ok: true, value: 60_000 });
  });
  it.each(["", "9", "0", "60001", "99.5", "x"])("refuses %j with a reason, never 1000", (text) => {
    const parsed = parsePeriodicMs(text);
    expect(parsed.ok).toBe(false);
    expect(parsed.ok ? "" : parsed.reason).toBe("Interval must be a whole number of ms between 10 and 60,000.");
  });
});

it("applies a typed duration after a short pause", () => {
  expect(APPLY_DELAY_MS).toBeGreaterThanOrEqual(200);
  expect(APPLY_DELAY_MS).toBeLessThanOrEqual(500);
});
