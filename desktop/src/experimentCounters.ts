import type { ExperimentStatus } from "./eventAdapter";

/** Images admitted for saving that are neither committed, failed nor dropped yet (never below 0). */
export function pendingSaveCount(status: ExperimentStatus): string {
  const unsettled = BigInt(status.persistence_admitted) - BigInt(status.persistence_committed)
    - BigInt(status.persistence_failed) - BigInt(status.dropped_valid) - BigInt(status.dropped_invalid);
  return String(unsettled > 0n ? unsettled : 0n);
}

export function experimentCounterRows(status: ExperimentStatus | null) {
  const value = (field: keyof ExperimentStatus) => status?.valid ? String(status[field]) : "Unavailable";
  const pending = status?.valid ? pendingSaveCount(status) : "Unavailable";
  return [
    ["Valid Images Saved:", value("valid_saved")],
    ["Invalid Images Saved:", value("invalid_saved")],
    ["Images Pending Save:", pending],
    ["Valid Images Dropped:", value("dropped_valid")],
    ["Invalid Images Dropped:", value("dropped_invalid")],
    ["Images Failed to Save:", value("persistence_failed")],
  ];
}
