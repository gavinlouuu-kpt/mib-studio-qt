import type { RunOutcome } from "../runOutcome";

// The result of the last finished run (#549). Loss and failure are alerts, a declared partial result
// is a status, and a clean run is a quiet note: "finalized" only meant the file was written.
export function RunOutcomeNotice({ outcome }: { outcome: RunOutcome | null }) {
  if (!outcome) return null;
  const alert = outcome.severity === "loss" || outcome.severity === "failed" || outcome.severity === "unknown" || outcome.attention;
  return (
    <p
      className={`run-outcome ${outcome.severity}${outcome.attention ? " attention" : ""}`}
      role={alert ? "alert" : "status"}
      title={outcome.reason ? `Backend: ${outcome.reason}` : undefined}
    >
      {outcome.headline}
    </p>
  );
}
