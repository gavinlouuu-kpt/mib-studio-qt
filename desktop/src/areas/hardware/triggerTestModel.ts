// Sorter trigger test model (epic #868: HW-043..HW-045). Pure helpers, no React or Tauri imports. The Qt reference is
// resources/ui/ExperimentMonitoringTab.ui (:36-100) and src/frontend/tabs/ExperimentMonitoringTab.cpp (:535-553, :1259-1272,
// :1759-1777): the ranges, defaults and tooltips below are copied from there.

export type Parsed = {ok: true; value: number} | {ok: false; reason: string};

/**
 * Qt's two spin boxes. The backend only checks `>= 1` for both (BackendFacade::handleTriggerCommand), so these
 * are the real upper bounds and the interval floor: the frontend enforces them.
 */
export const PULSE_US = {min: 1, max: 1_000_000, default: 1} as const;
export const PERIODIC_MS = {min: 10, max: 60_000, default: 1000} as const;

export const TOOLTIP = {
  sortTrigger: 'Manually fire a sort trigger pulse for testing',
  pulseDuration: 'Trigger pulse duration in microseconds',
  periodicTest: 'Toggle periodic sort trigger pulses at the interval below',
  periodicInterval: 'Interval between periodic test trigger pulses',
} as const;
/** How long a typed pulse duration waits before it is applied (Enter or leaving the field applies at once). */
export const APPLY_DELAY_MS = 300;

function wholeNumber(text: string, name: string, unit: string, min: number, max: number): Parsed {
  const trimmed = text.trim();
  const value = Number(trimmed);
  if (!trimmed || !Number.isInteger(value) || value < min || value > max) {
    return {ok: false, reason: `${name} must be a whole number of ${unit} between ${min.toLocaleString('en-US')} and ${max.toLocaleString('en-US')}.`};
  }
  return {ok: true, value};
}
/** A typed pulse duration, or why it cannot be used; blank, 0 and out-of-range are refused, never replaced by a default. */
export const parsePulseUs = (text: string): Parsed => wholeNumber(text, 'Pulse duration', 'µs', PULSE_US.min, PULSE_US.max);
/** A typed periodic interval, or why it cannot be used. */
export const parsePeriodicMs = (text: string): Parsed => wholeNumber(text, 'Interval', 'ms', PERIODIC_MS.min, PERIODIC_MS.max);
