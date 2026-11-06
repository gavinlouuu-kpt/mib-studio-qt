// The Monitoring density (KDE) preferences (epic #866 PR E2: MON-062, MON-063, MON-083..085). Qt keeps them in
// QSettings under `Monitoring/Kde*` with a version guard: a missing or obsolete version falls back to the
// defaults instead of reading values whose meaning may have changed; values are clamped to their ranges, and
// the reference contour's file path is a key of its own that the guard does not cover. Here the record is one
// JSON value in the app's preference store (persistedState.ts, localStorage: it must survive a restart, which
// the per-tab sessionStorage does not). Pure apart from that store; every access is guarded.
import { MONITORING_DENSITY } from "../../../bridgeContract";
import { readPersisted, writePersisted } from "../../../persistedState";
import type { MonitoringSettings } from "../monitoringSettings";

export const DENSITY_PREFS_KEY = "yofo.monitoring.kde";
/** Qt kKdePreferenceVersion: bump it when the meaning of a stored value changes. */
export const DENSITY_PREFS_VERSION = 1;

export interface DensityPrefs {
  enabled: boolean;
  /** Factor on the per-axis Silverman bandwidth. */
  bandwidthFactor: number;
  /** Recompute interval, ms. */
  intervalMs: number;
  /** Fraction of the population the core contour encloses (0.05..1); the dialog shows it as a percent. */
  coreFraction: number;
  /** The experiment file the reference contour was read from, on the instrument; "" = none (a pinned reference is not remembered). */
  referencePath: string;
}

export const DEFAULT_DENSITY_PREFS: Readonly<DensityPrefs> = {
  enabled: false,
  bandwidthFactor: MONITORING_DENSITY.bandwidth_factor_default,
  intervalMs: MONITORING_DENSITY.interval_ms_default,
  coreFraction: MONITORING_DENSITY.core_fraction_default,
  referencePath: "",
};

const clamp = (v: number, min: number, max: number) => Math.min(max, Math.max(min, v));
const isRecord = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);

/** A stored value as the preferences it stands for: Qt loadKdePreferences. Anything that is not a record, or has
 *  another version, is the defaults (the reference path still counts: it is outside the guard). A number outside
 *  its range is clamped to it; one that is not a finite number keeps the default. */
export function normalizeDensityPrefs(raw: unknown): DensityPrefs {
  const prefs: DensityPrefs = { ...DEFAULT_DENSITY_PREFS };
  if (!isRecord(raw)) return prefs;
  if (typeof raw.referencePath === "string") prefs.referencePath = raw.referencePath;
  if (raw.version !== DENSITY_PREFS_VERSION) return prefs;
  prefs.enabled = raw.enabled === true;
  if (typeof raw.bandwidthFactor === "number" && Number.isFinite(raw.bandwidthFactor))
    prefs.bandwidthFactor = clamp(raw.bandwidthFactor, MONITORING_DENSITY.bandwidth_factor_min, MONITORING_DENSITY.bandwidth_factor_max);
  if (typeof raw.intervalMs === "number" && Number.isFinite(raw.intervalMs))
    prefs.intervalMs = clamp(Math.round(raw.intervalMs), MONITORING_DENSITY.interval_ms_min, MONITORING_DENSITY.interval_ms_max);
  if (typeof raw.coreFraction === "number" && Number.isFinite(raw.coreFraction))
    prefs.coreFraction = clamp(raw.coreFraction, MONITORING_DENSITY.core_fraction_min, MONITORING_DENSITY.core_fraction_max);
  return prefs;
}

/** The stored preferences, or the defaults (nothing stored, storage unavailable, an obsolete version). */
export function loadDensityPrefs(): DensityPrefs {
  return normalizeDensityPrefs(readPersisted<unknown>(DENSITY_PREFS_KEY, null, (v): v is unknown => isRecord(v), "local"));
}

export function saveDensityPrefs(prefs: DensityPrefs): void {
  writePersisted(DENSITY_PREFS_KEY, { version: DENSITY_PREFS_VERSION, ...prefs }, "local");
}

/** The dialog's core field is a percent (QSpinBox 5..100), the backend and the record a fraction. */
export const percentOfFraction = (fraction: number): number => Math.round(fraction * 100);
export const fractionOfPercent = (percent: number): number => percent / 100;

/** The Monitoring Settings the preferences stand for: the three numbers the dialog edits, over `base`. */
export function settingsWithDensityPrefs(base: MonitoringSettings, prefs: DensityPrefs): MonitoringSettings {
  return {
    ...base,
    kdeBandwidthFactor: prefs.bandwidthFactor,
    kdeIntervalMs: prefs.intervalMs,
    kdeCorePercent: percentOfFraction(prefs.coreFraction),
  };
}

/** The record to store: the toggle and the path from the shell's state, the numbers from the settings. */
export function densityPrefsOf(enabled: boolean, referencePath: string, settings: Pick<MonitoringSettings, "kdeBandwidthFactor" | "kdeIntervalMs" | "kdeCorePercent">): DensityPrefs {
  return {
    enabled,
    bandwidthFactor: settings.kdeBandwidthFactor,
    intervalMs: settings.kdeIntervalMs,
    coreFraction: fractionOfPercent(settings.kdeCorePercent),
    referencePath,
  };
}
