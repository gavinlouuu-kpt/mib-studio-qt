// A poll that fails once is a blip; one that keeps failing is something the operator has to be told
// (epic #866, suspicious items App.tsx:533 and 542-544: the Monitoring polls swallowed every error).
// Pure: no React or Tauri imports.

/** Consecutive failures before a notice shows: one lost tick at 500 ms is not worth a banner. */
export const POLL_FAILURE_THRESHOLD = 3;

export interface PollHealth { failures: number; lastError: string }
export const POLL_HEALTHY: PollHealth = { failures: 0, lastError: "" };

function errorText(error: unknown): string {
  const text = error instanceof Error ? error.message : String(error);
  return text.length > 200 ? `${text.slice(0, 200)}…` : text;
}

/** The health after one more poll: `error` undefined = the poll succeeded (the count starts over). */
export function pollHealthAfter(prev: PollHealth, error?: unknown): PollHealth {
  if (error === undefined) return prev.failures === 0 ? prev : POLL_HEALTHY;
  return { failures: prev.failures + 1, lastError: errorText(error) };
}

/** The operator-facing sentence once the failures persist, "" before. `what` names the read ("Monitoring data"). */
export function pollNotice(what: string, health: PollHealth, threshold = POLL_FAILURE_THRESHOLD): string {
  if (health.failures < threshold) return "";
  return `${what} could not be read from the backend (${health.failures} attempts in a row): ${health.lastError || "no reason given"}`;
}
