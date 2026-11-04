import { describe, expect, it } from "vitest";
import {
  applyMonitoringSettings, coerceSetting, DEFAULT_MONITORING_SETTINGS, MONITORING_SETTING_DEFAULTS, MONITORING_SETTINGS_GROUPS,
  parseSettingText, ringRatioRangeOfConfig, settingText, withHistogramXRange, type SettingFieldSpec,
} from "./monitoringSettings";

const field = (key: string) => MONITORING_SETTINGS_GROUPS.flatMap((g) => g.fields).find((f) => f.key === key) as SettingFieldSpec;
const values = (over: Partial<typeof MONITORING_SETTING_DEFAULTS> = {}) => ({ ...MONITORING_SETTING_DEFAULTS, ...over });

describe("Monitoring Settings fields (MON-087..091)", () => {
  it("has the Qt ranges, decimals, steps and defaults", () => {
    const spec = Object.fromEntries(MONITORING_SETTINGS_GROUPS.flatMap((g) => g.fields).map((f) => [f.key, [f.min, f.max, f.decimals, f.step]]));
    expect(spec).toEqual({
      scatterXMin: [0, 100000, 2, 1], scatterXMax: [0, 100000, 2, 1],
      scatterYMin: [0, 10, 3, 1], scatterYMax: [0, 10, 3, 1],
      histogramXMin: [0, 100, 2, 1], histogramXMax: [0, 100, 2, 1],
      histogramYMax: [1, 1_000_000, 0, 1], histogramBinWidth: [0.01, 10, 2, 0.1],
      // Density (KDE), MON-083..085: bandwidth factor 0.2-5.0 step 0.1 (2 decimals), interval 500-60000 ms step 500, core 5-100 % step 5.
      kdeBandwidthFactor: [0.2, 5, 2, 0.1], kdeIntervalMs: [500, 60000, 0, 500], kdeCorePercent: [5, 100, 0, 5],
    });
    expect(MONITORING_SETTING_DEFAULTS).toEqual({
      scatterXMin: 0, scatterXMax: 1000, scatterYMin: 0, scatterYMax: 1,
      histogramXMin: 15, histogramXMax: 25, histogramYMax: 100, histogramBinWidth: 0.5,
      kdeBandwidthFactor: 1, kdeIntervalMs: 2000, kdeCorePercent: 90,
    });
  });

  it("every field has a tour anchor and a tooltip", () => {
    for (const f of MONITORING_SETTINGS_GROUPS.flatMap((g) => g.fields)) {
      expect(f.tour).toMatch(/^monitoring-settings-/);
      expect(f.title).not.toBe("");
    }
  });

  it("clamps to the range and rounds to the decimals like a spin box", () => {
    expect(coerceSetting(field("scatterXMax"), 250000)).toBe(100000);
    expect(coerceSetting(field("scatterXMax"), -5)).toBe(0);
    expect(coerceSetting(field("scatterYMax"), 0.12345)).toBe(0.123);
    expect(coerceSetting(field("histogramYMax"), 99.6)).toBe(100);
    expect(coerceSetting(field("histogramBinWidth"), 0)).toBe(0.01);
    expect(settingText(field("scatterYMax"), 1)).toBe("1.000");
    expect(settingText(field("histogramYMax"), 100)).toBe("100");
  });

  it("reads text, keeping the last value for an empty or non-numeric field", () => {
    expect(parseSettingText(field("scatterXMax"), "1500.5")).toBe(1500.5);
    expect(parseSettingText(field("scatterXMax"), "")).toBeNull();
    expect(parseSettingText(field("scatterXMax"), "abc")).toBeNull();
    expect(parseSettingText(field("scatterXMax"), "1e9")).toBe(100000);
  });
});

describe("applying the dialog (MON-087..091)", () => {
  it("sets every valid range and the bin width, and resets both charts' zoom", () => {
    const { settings, ignored } = applyMonitoringSettings(DEFAULT_MONITORING_SETTINGS, values({
      scatterXMin: 10, scatterXMax: 500, scatterYMin: 0.1, scatterYMax: 0.9, histogramXMin: 10, histogramXMax: 30, histogramYMax: 250, histogramBinWidth: 0.25,
    }));
    expect(ignored).toEqual([]);
    expect(settings).toMatchObject({
      scatterXMin: 10, scatterXMax: 500, scatterYMin: 0.1, scatterYMax: 0.9, histogramXMin: 10, histogramXMax: 30, histogramYMax: 250, histogramBinWidth: 0.25,
      scatterEpoch: 1, histogramEpoch: 1,
    });
  });

  it("silently keeps a range whose minimum is not below its maximum, applying the rest", () => {
    const { settings, ignored } = applyMonitoringSettings(DEFAULT_MONITORING_SETTINGS, values({ scatterXMin: 800, scatterXMax: 800, scatterYMax: 0.5, histogramXMin: 30, histogramXMax: 20 }));
    expect(ignored).toEqual(["scatterX", "histogramX"]);
    expect([settings.scatterXMin, settings.scatterXMax]).toEqual([0, 1000]);
    expect([settings.histogramXMin, settings.histogramXMax]).toEqual([15, 25]);
    expect(settings.scatterYMax).toBe(0.5);
    // The scatter's Y range was valid, so it reset the scatter zoom; the histogram's Y max too.
    expect(settings.scatterEpoch).toBe(1);
    expect(settings.histogramEpoch).toBe(1);
  });

  it("clamps out-of-range values before applying", () => {
    const { settings } = applyMonitoringSettings(DEFAULT_MONITORING_SETTINGS, values({ scatterXMax: 5e6, scatterYMax: 99, histogramBinWidth: 0 }));
    expect(settings.scatterXMax).toBe(100000);
    expect(settings.scatterYMax).toBe(10);
    expect(settings.histogramBinWidth).toBe(0.01);
  });

  it("bumps an epoch once per Apply and not at all when the chart's settings were all ignored", () => {
    const first = applyMonitoringSettings(DEFAULT_MONITORING_SETTINGS, values({ scatterXMax: 400, scatterYMax: 0.8 })).settings;
    expect(first.scatterEpoch).toBe(1);
    const again = applyMonitoringSettings(first, values({ scatterXMax: 400, scatterYMax: 0.8 })).settings;
    expect(again.scatterEpoch).toBe(2); // unchanged values still reset the zoom, as Qt does
    const bad = applyMonitoringSettings(again, values({ scatterXMin: 5, scatterXMax: 5, scatterYMin: 1, scatterYMax: 1, histogramYMax: 100 })).settings;
    expect(bad.scatterEpoch).toBe(2);
  });
});

describe("the histogram's X range from the runtime config (MON-050)", () => {
  it("takes the config's range and resets the histogram zoom", () => {
    const next = withHistogramXRange(DEFAULT_MONITORING_SETTINGS, 12, 30);
    expect([next.histogramXMin, next.histogramXMax, next.histogramEpoch]).toEqual([12, 30, 1]);
  });

  it("ignores an empty or inverted range like Qt", () => {
    expect(withHistogramXRange(DEFAULT_MONITORING_SETTINGS, 30, 12)).toBe(DEFAULT_MONITORING_SETTINGS);
    expect(withHistogramXRange(DEFAULT_MONITORING_SETTINGS, 20, 20)).toBe(DEFAULT_MONITORING_SETTINGS);
    expect(withHistogramXRange(DEFAULT_MONITORING_SETTINGS, NaN, 20)).toBe(DEFAULT_MONITORING_SETTINGS);
  });

  it("reads ring_ratio_min/max from the config document", () => {
    expect(ringRatioRangeOfConfig(JSON.stringify({ image_processing: { ring_ratio_min: 15, ring_ratio_max: 22.5 } }))).toEqual({ min: 15, max: 22.5 });
    expect(ringRatioRangeOfConfig("")).toBeNull();
    expect(ringRatioRangeOfConfig("{broken")).toBeNull();
    expect(ringRatioRangeOfConfig("{}")).toBeNull();
    expect(ringRatioRangeOfConfig(JSON.stringify({ image_processing: { ring_ratio_min: "15", ring_ratio_max: 20 } }))).toBeNull();
  });
});
