import type { ExperimentStatus } from "./eventAdapter";

export function experimentCounterRows(status: ExperimentStatus | null) {
  const value = (field: keyof ExperimentStatus) => status?.valid ? String(status[field]) : "Unavailable";
  let pending = "Unavailable";
  if (status?.valid) {
    const unsettled = BigInt(status.persistence_admitted) - BigInt(status.persistence_committed)
      - BigInt(status.persistence_failed) - BigInt(status.dropped_valid) - BigInt(status.dropped_invalid);
    pending = String(unsettled > 0n ? unsettled : 0n);
  }
  return [
    ["Valid Images Saved:", value("valid_saved")],
    ["Invalid Images Saved:", value("invalid_saved")],
    ["Images Pending Save:", pending],
    ["Valid Images Dropped:", value("dropped_valid")],
    ["Invalid Images Dropped:", value("dropped_invalid")],
    ["Images Failed to Save:", value("persistence_failed")],
  ];
}
