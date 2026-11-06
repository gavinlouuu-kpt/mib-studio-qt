// Hardware (epic #868): the limits and messages of the Monitoring trigger tests, as Qt's spin boxes have them
// (resources/ui/ExperimentMonitoringTab.ui: pulse duration 1-1,000,000 us, default 1; periodic interval 10-60000 ms,
// default 1000). The backend only checks >= 1, so Qt's upper bounds and the 10 ms floor live here (HW-044, HW-045).
// Out-of-range or blank input is refused with a message, never replaced by a default (App.tsx:2227 did).
export const PULSE_US_MIN = 1, PULSE_US_MAX = 1_000_000;
export const PERIODIC_MS_MIN = 10, PERIODIC_MS_MAX = 60_000;
/** Qt applies the pulse duration on every change; here, after the operator pauses typing. */
export const PULSE_APPLY_DEBOUNCE_MS = 300;

function wholeNumber(text: string): number {
  return text.trim() === '' ? NaN : Number(text);
}
const inRange = (value: number, min: number, max: number) => Number.isInteger(value) && value >= min && value <= max;

export function pulseUsProblem(text: string): string {
  return inRange(wholeNumber(text), PULSE_US_MIN, PULSE_US_MAX)
    ? '' : `Pulse duration must be a whole number of µs between ${PULSE_US_MIN} and ${PULSE_US_MAX.toLocaleString('en-US')}.`;
}
export function periodicMsProblem(text: string): string {
  return inRange(wholeNumber(text), PERIODIC_MS_MIN, PERIODIC_MS_MAX)
    ? '' : `Periodic interval must be a whole number of ms between ${PERIODIC_MS_MIN} and ${PERIODIC_MS_MAX}.`;
}
export function parsePulseUs(text: string): number {
  const problem = pulseUsProblem(text);
  if (problem) throw new Error(problem);
  return Number(text);
}
export function parsePeriodicMs(text: string): number {
  const problem = periodicMsProblem(text);
  if (problem) throw new Error(problem);
  return Number(text);
}
