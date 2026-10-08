// PZ7035 peristaltic pumps (Tushui, #495): the operator thinks in head rpm (the bench viewer's unit);
// the backend takes a flow rate. flow [µL/min] = rpm x calibration [µL/rev]; 0.4 rpm at 25 µL/rev is
// 10 µL/min on the instrument tubing.

/** Pseudo unit code for "rpm" in the rate unit select (the backend's own codes are 100 µL/min, 103 mL/min). */
export const RPM_UNIT = 200;
export const MICROLITERS_PER_MIN_UNIT = 100;

export function rpmToMicrolitersPerMin(rpm: number, microlitersPerRev: number): number {
  if (!(rpm >= 0) || !(microlitersPerRev > 0)) throw new Error("rpm needs a number ≥ 0 and a calibration above 0 µL/rev");
  return rpm * microlitersPerRev;
}

/** The head speed a configured flow corresponds to, or null when it cannot be told. */
export function flowToRpm(flow: number, unit: number, microlitersPerRev: number | undefined): number | null {
  if (!(flow >= 0) || !microlitersPerRev || !(microlitersPerRev > 0)) return null;
  const perMin = unit === 103 ? flow * 1000 : unit === MICROLITERS_PER_MIN_UNIT ? flow : null;
  return perMin === null ? null : perMin / microlitersPerRev;
}

/** Infuse is counter-clockwise on the instrument's Tushui heads (pz7035 notes); a syringe pump has no such label. */
export function directionLabel(peristaltic: boolean, direction: 0 | 1): string {
  const name = direction === 0 ? "Infuse" : "Withdraw";
  return peristaltic ? `${name} (${direction === 0 ? "CCW" : "CW"})` : name;
}
