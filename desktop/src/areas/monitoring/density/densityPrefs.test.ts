// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { MONITORING_SETTING_DEFAULTS, DEFAULT_MONITORING_SETTINGS } from "../monitoringSettings";
import {
  DEFAULT_DENSITY_PREFS, DENSITY_PREFS_KEY, DENSITY_PREFS_VERSION, densityPrefsOf, fractionOfPercent, loadDensityPrefs, normalizeDensityPrefs,
  percentOfFraction, saveDensityPrefs, settingsWithDensityPrefs,
} from "./densityPrefs";

// jsdom's own storage is not reliable under every Node version the suite runs on: a plain in-memory one, as the other tests do.
function memoryStorage() {
  const saved = new Map<string, string>();
  return {
    getItem: vi.fn((key: string) => saved.get(key) ?? null),
    setItem: vi.fn((key: string, value: string) => void saved.set(key, value)),
    removeItem: (key: string) => void saved.delete(key),
    clear: () => saved.clear(),
  };
}
let local: ReturnType<typeof memoryStorage>, session: ReturnType<typeof memoryStorage>;
beforeEach(() => {
  local = memoryStorage();
  session = memoryStorage();
  Object.defineProperty(window, "localStorage", { configurable: true, value: local });
  Object.defineProperty(window, "sessionStorage", { configurable: true, value: session });
});
afterEach(() => vi.restoreAllMocks());

const stored = (over: Record<string, unknown> = {}) => ({
  version: DENSITY_PREFS_VERSION, enabled: true, bandwidthFactor: 1.5, intervalMs: 4000, coreFraction: 0.75, referencePath: "/data/run7.h5", ...over,
});

describe("the defaults", () => {
  it("are Qt's: off, factor 1.0, 2000 ms, core 90 %, no reference", () => {
    expect(DEFAULT_DENSITY_PREFS).toEqual({ enabled: false, bandwidthFactor: 1, intervalMs: 2000, coreFraction: 0.9, referencePath: "" });
  });
  it("agree with the Monitoring Settings defaults the dialog starts from", () => {
    expect(settingsWithDensityPrefs(DEFAULT_MONITORING_SETTINGS, DEFAULT_DENSITY_PREFS)).toEqual(DEFAULT_MONITORING_SETTINGS);
    expect([MONITORING_SETTING_DEFAULTS.kdeBandwidthFactor, MONITORING_SETTING_DEFAULTS.kdeIntervalMs, MONITORING_SETTING_DEFAULTS.kdeCorePercent])
      .toEqual([1, 2000, 90]);
  });
});

describe("reading a stored record (MON-063, Qt loadKdePreferences)", () => {
  it("takes a current-version record as it is", () => {
    expect(normalizeDensityPrefs(stored())).toEqual({ enabled: true, bandwidthFactor: 1.5, intervalMs: 4000, coreFraction: 0.75, referencePath: "/data/run7.h5" });
  });
  it("falls back to the defaults for a missing or foreign record", () => {
    for (const raw of [null, undefined, 3, "x", [], {}]) expect(normalizeDensityPrefs(raw), JSON.stringify(raw)).toEqual(DEFAULT_DENSITY_PREFS);
  });
  it("falls back to the defaults for an obsolete or unknown version (the guard): the stored values may mean something else", () => {
    for (const version of [0, 2, "1", null]) {
      expect(normalizeDensityPrefs(stored({ version, referencePath: "" })), String(version)).toEqual(DEFAULT_DENSITY_PREFS);
    }
  });
  it("keeps the reference path outside the guard, as Qt's separate key", () => {
    expect(normalizeDensityPrefs(stored({ version: 99 }))).toEqual({ ...DEFAULT_DENSITY_PREFS, referencePath: "/data/run7.h5" });
    expect(normalizeDensityPrefs({ referencePath: "/x.h5" })).toEqual({ ...DEFAULT_DENSITY_PREFS, referencePath: "/x.h5" });
  });
  it("clamps a number to its range", () => {
    expect(normalizeDensityPrefs(stored({ bandwidthFactor: 99, intervalMs: 5, coreFraction: 7 }))).toMatchObject({ bandwidthFactor: 5, intervalMs: 500, coreFraction: 1 });
    expect(normalizeDensityPrefs(stored({ bandwidthFactor: -1, intervalMs: 1e9, coreFraction: 0 }))).toMatchObject({ bandwidthFactor: 0.2, intervalMs: 60000, coreFraction: 0.05 });
  });
  it("keeps the default for a number that is not finite, or not a number, and reads enabled strictly", () => {
    expect(normalizeDensityPrefs(stored({ bandwidthFactor: null, intervalMs: "2000", coreFraction: "x" }))).toMatchObject({ bandwidthFactor: 1, intervalMs: 2000, coreFraction: 0.9 });
    expect(normalizeDensityPrefs(stored({ bandwidthFactor: NaN }))).toMatchObject({ bandwidthFactor: 1 });
    expect(normalizeDensityPrefs(stored({ enabled: "true" })).enabled).toBe(false);
    expect(normalizeDensityPrefs(stored({ referencePath: 5 })).referencePath).toBe("");
  });
  it("rounds the interval to a whole millisecond", () => {
    expect(normalizeDensityPrefs(stored({ intervalMs: 2000.6 })).intervalMs).toBe(2001);
  });
});

describe("the store (localStorage: it survives a restart)", () => {
  it("round-trips a record under its key with the version", () => {
    saveDensityPrefs({ enabled: true, bandwidthFactor: 2, intervalMs: 1500, coreFraction: 0.5, referencePath: "/data/a.h5" });
    expect(JSON.parse(local.getItem(DENSITY_PREFS_KEY)!)).toEqual({ version: 1, enabled: true, bandwidthFactor: 2, intervalMs: 1500, coreFraction: 0.5, referencePath: "/data/a.h5" });
    expect(loadDensityPrefs()).toEqual({ enabled: true, bandwidthFactor: 2, intervalMs: 1500, coreFraction: 0.5, referencePath: "/data/a.h5" });
    expect(session.getItem(DENSITY_PREFS_KEY)).toBeNull();
  });
  it("loads the defaults when nothing is stored, the text is not JSON, or the store throws", () => {
    expect(loadDensityPrefs()).toEqual(DEFAULT_DENSITY_PREFS);
    local.setItem(DENSITY_PREFS_KEY, "{not json");
    expect(loadDensityPrefs()).toEqual(DEFAULT_DENSITY_PREFS);
    local.getItem.mockImplementation(() => { throw new Error("blocked"); });
    expect(loadDensityPrefs()).toEqual(DEFAULT_DENSITY_PREFS);
  });
  it("does not throw when the store refuses a write", () => {
    local.setItem.mockImplementation(() => { throw new Error("quota"); });
    expect(() => saveDensityPrefs(DEFAULT_DENSITY_PREFS)).not.toThrow();
  });
});

describe("the dialog's numbers", () => {
  it("shows the core fraction as a whole percent and sends it back as a fraction", () => {
    expect(percentOfFraction(0.9)).toBe(90);
    expect(percentOfFraction(0.05)).toBe(5);
    expect(percentOfFraction(0.286)).toBe(29);
    expect(fractionOfPercent(90)).toBe(0.9);
    expect(fractionOfPercent(5)).toBe(0.05);
  });
  it("puts the stored numbers over the settings and builds the record back from them", () => {
    const prefs = normalizeDensityPrefs(stored());
    const settings = settingsWithDensityPrefs(DEFAULT_MONITORING_SETTINGS, prefs);
    expect(settings).toMatchObject({ kdeBandwidthFactor: 1.5, kdeIntervalMs: 4000, kdeCorePercent: 75, scatterXMax: 1000 });
    expect(densityPrefsOf(true, "/data/run7.h5", settings)).toEqual(prefs);
  });
});
