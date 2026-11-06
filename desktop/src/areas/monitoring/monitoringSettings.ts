// Monitoring Settings (epic #866, MON-082, MON-087..091): the fixed chart axes and the histogram bin
// width, as the Qt Settings > Monitoring Settings... dialog sets them (src/frontend/dialogs/
// MonitoringSettingsDialog.cpp, resources/ui/MonitoringSettingsDialog.ui,
// ExperimentMonitoringTab::setScatterXRange and friends). Pure: no React, no DOM.
//
// The values are a view preference of this client (a viewer in a browser has its own), kept in memory:
// Qt does not persist them across restarts either. The Density (KDE) fields (MON-083..086) join the
// same shape later: add their keys to MonitoringSettingsValues and a group to MONITORING_SETTINGS_GROUPS
// and the dialog renders them (MonitoringSettingsDialog.tsx maps over the groups).

export interface MonitoringSettingsValues {
  /** Scatter X axis: area in µm². */
  scatterXMin: number;
  scatterXMax: number;
  /** Scatter Y axis: deformability. */
  scatterYMin: number;
  scatterYMax: number;
  /** Histogram X axis: ring ratio (Qt calls it ring width). */
  histogramXMin: number;
  histogramXMax: number;
  /** Histogram Y axis: frequency, 0..max. */
  histogramYMax: number;
  histogramBinWidth: number;
}

export type MonitoringSettingKey = keyof MonitoringSettingsValues;

/** The settings plus two change counters: a chart drops its user zoom when its counter changes, as Qt's
 *  setScatterXRange / setHistogramXRange... call resetZoom() (also when the values are unchanged). */
export interface MonitoringSettings extends MonitoringSettingsValues {
  scatterEpoch: number;
  histogramEpoch: number;
}

/** Qt defaults (ExperimentMonitoringTab.h:440-448). */
export const MONITORING_SETTING_DEFAULTS: Readonly<MonitoringSettingsValues> = {
  scatterXMin: 0,
  scatterXMax: 1000,
  scatterYMin: 0,
  scatterYMax: 1,
  histogramXMin: 15,
  histogramXMax: 25,
  histogramYMax: 100,
  histogramBinWidth: 0.5,
};

export const DEFAULT_MONITORING_SETTINGS: Readonly<MonitoringSettings> = {
  ...MONITORING_SETTING_DEFAULTS,
  scatterEpoch: 0,
  histogramEpoch: 0,
};

export interface SettingFieldSpec {
  key: MonitoringSettingKey;
  label: string;
  /** The tooltip (the .ui file's toolTip). */
  title: string;
  min: number;
  max: number;
  decimals: number;
  /** Arrow-key / spinner step (QDoubleSpinBox singleStep; Qt's default is 1). */
  step: number;
  /** Unit shown after the field, if any. */
  suffix?: string;
  tour: string;
}

/** Two fields that must satisfy min < max or the pair is not applied (Qt: min >= max is ignored). */
export interface SettingPair {
  id: "scatterX" | "scatterY" | "histogramX";
  min: MonitoringSettingKey;
  max: MonitoringSettingKey;
  label: string;
}

export interface SettingsGroup {
  id: string;
  title: string;
  fields: readonly SettingFieldSpec[];
  pairs: readonly SettingPair[];
}

export const MONITORING_SETTINGS_GROUPS: readonly SettingsGroup[] = [
  {
    id: "scatter",
    title: "Scatter plot axis (Area vs Deformability)",
    fields: [
      { key: "scatterXMin", label: "X (Area) min", title: "Fixed scatter plot X-axis minimum (Area μm²)", min: 0, max: 100000, decimals: 2, step: 1, tour: "monitoring-settings-scatter-x-min" },
      { key: "scatterXMax", label: "X (Area) max", title: "Fixed scatter plot X-axis maximum (Area μm²)", min: 0, max: 100000, decimals: 2, step: 1, tour: "monitoring-settings-scatter-x-max" },
      { key: "scatterYMin", label: "Y (Deformability) min", title: "Fixed scatter plot Y-axis minimum", min: 0, max: 10, decimals: 3, step: 1, tour: "monitoring-settings-scatter-y-min" },
      { key: "scatterYMax", label: "Y (Deformability) max", title: "Fixed scatter plot Y-axis maximum", min: 0, max: 10, decimals: 3, step: 1, tour: "monitoring-settings-scatter-y-max" },
    ],
    pairs: [
      { id: "scatterX", min: "scatterXMin", max: "scatterXMax", label: "Scatter X (Area)" },
      { id: "scatterY", min: "scatterYMin", max: "scatterYMax", label: "Scatter Y (Deformability)" },
    ],
  },
  {
    id: "histogram",
    // Qt: "Histogram axis (Ring width distribution)". The measured value is the ring ratio, not a
    // physical width (MonitoringCharts says so too), so the dialog says ring ratio.
    title: "Histogram axis (Ring-ratio distribution)",
    fields: [
      { key: "histogramXMin", label: "X (Ring ratio) min", title: "Fixed histogram X-axis minimum", min: 0, max: 100, decimals: 2, step: 1, tour: "monitoring-settings-histogram-x-min" },
      { key: "histogramXMax", label: "X (Ring ratio) max", title: "Fixed histogram X-axis maximum", min: 0, max: 100, decimals: 2, step: 1, tour: "monitoring-settings-histogram-x-max" },
      { key: "histogramYMax", label: "Y (Frequency) max", title: "Fixed histogram Y-axis maximum (frequency)", min: 1, max: 1_000_000, decimals: 0, step: 1, tour: "monitoring-settings-histogram-y-max" },
      { key: "histogramBinWidth", label: "Bin width", title: "Histogram bin width (number of bins = (X max - X min) / bin width)", min: 0.01, max: 10, decimals: 2, step: 0.1, tour: "monitoring-settings-histogram-bin-width" },
    ],
    pairs: [{ id: "histogramX", min: "histogramXMin", max: "histogramXMax", label: "Histogram X (Ring ratio)" }],
  },
];

const FIELD_BY_KEY = new Map<MonitoringSettingKey, SettingFieldSpec>(
  MONITORING_SETTINGS_GROUPS.flatMap((g) => g.fields).map((f) => [f.key, f]),
);

/** What a QDoubleSpinBox holds after an edit: the value clamped to its range and rounded to its decimals. */
export function coerceSetting(spec: SettingFieldSpec, value: number): number {
  const clamped = Math.min(spec.max, Math.max(spec.min, value));
  return Number(clamped.toFixed(spec.decimals));
}

/** A field's text to a coerced number, or null when it is empty or not a finite number (the spin box
 *  then keeps its last value). */
export function parseSettingText(spec: SettingFieldSpec, text: string): number | null {
  if (!text.trim()) return null;
  const n = Number(text);
  return Number.isFinite(n) ? coerceSetting(spec, n) : null;
}

export function settingText(spec: SettingFieldSpec, value: number): string {
  return value.toFixed(spec.decimals);
}

export interface ApplyResult {
  settings: MonitoringSettings;
  /** Pairs whose min >= max: those two fields were not applied (Qt ignores them silently). */
  ignored: SettingPair["id"][];
}

/** Apply a dialog's values like MonitoringSettingsDialog::applySettings: each range is set only when
 *  its min is below its max; the histogram Y max and the bin width only when positive; a valid
 *  scatter / histogram range or Y max resets that chart's zoom (the epoch counters). Values are
 *  coerced to the fields' ranges first. */
export function applyMonitoringSettings(current: MonitoringSettings, values: MonitoringSettingsValues): ApplyResult {
  const v = { ...values };
  for (const [key, spec] of FIELD_BY_KEY) v[key] = Number.isFinite(v[key]) ? coerceSetting(spec, v[key]) : current[key];
  const next: MonitoringSettings = { ...current };
  const ignored: SettingPair["id"][] = [];
  let scatterReset = false, histogramReset = false;
  const range = (pair: SettingPair["id"], min: MonitoringSettingKey, max: MonitoringSettingKey) => {
    if (v[min] >= v[max]) {
      ignored.push(pair);
      return false;
    }
    next[min] = v[min];
    next[max] = v[max];
    return true;
  };
  if (range("scatterX", "scatterXMin", "scatterXMax")) scatterReset = true;
  if (range("scatterY", "scatterYMin", "scatterYMax")) scatterReset = true;
  if (range("histogramX", "histogramXMin", "histogramXMax")) histogramReset = true;
  if (v.histogramYMax > 0) {
    next.histogramYMax = v.histogramYMax;
    histogramReset = true;
  }
  if (v.histogramBinWidth > 0) next.histogramBinWidth = v.histogramBinWidth;
  // One bump per chart per Apply however many of its settings were set.
  if (scatterReset) next.scatterEpoch = current.scatterEpoch + 1;
  if (histogramReset) next.histogramEpoch = current.histogramEpoch + 1;
  return { settings: next, ignored };
}

/** The histogram X range follows the runtime config's ring_ratio_min/max (Qt setHistogramXRange from
 *  loadCurrentConfig: when the tab is shown and after Apply / Revert). Ignored when min >= max, like Qt. */
export function withHistogramXRange(current: MonitoringSettings, min: number, max: number): MonitoringSettings {
  const spec = (k: MonitoringSettingKey) => FIELD_BY_KEY.get(k)!;
  if (!Number.isFinite(min) || !Number.isFinite(max)) return current;
  const lo = coerceSetting(spec("histogramXMin"), min);
  const hi = coerceSetting(spec("histogramXMax"), max);
  if (!(lo < hi)) return current;
  return { ...current, histogramXMin: lo, histogramXMax: hi, histogramEpoch: current.histogramEpoch + 1 };
}

/** ring_ratio_min / ring_ratio_max of the runtime config document (`image_processing`), or null when
 *  the text is empty, not JSON or lacks them. */
export function ringRatioRangeOfConfig(configText: string): { min: number; max: number } | null {
  if (!configText) return null;
  try {
    const ip = (JSON.parse(configText) as { image_processing?: { ring_ratio_min?: unknown; ring_ratio_max?: unknown } }).image_processing;
    const min = ip?.ring_ratio_min, max = ip?.ring_ratio_max;
    return typeof min === "number" && typeof max === "number" && Number.isFinite(min) && Number.isFinite(max) ? { min, max } : null;
  } catch {
    return null;
  }
}
