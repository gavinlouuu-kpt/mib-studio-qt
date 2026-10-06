// Live PL result-stream statistics for the PZ7035 Run view (#501). The backend reports cumulative
// counters of the provider (`fetch_instrument_status.results`), valid while an experiment runs;
// the UI turns successive polls into rates and outcome fractions.
//
// Pure module: no React/Tauri imports so it is unit-testable in plain Node.

export interface ResultsCounters {
  frames: number;
  results: number;
  empty_frames: number;
  invalid_frames: number;
  truncated_frames: number;
}

export interface ResultsSample extends ResultsCounters {
  /** Host time of the poll, in milliseconds (any monotonic clock). */
  atMs: number;
}

export interface ResultsRates {
  framesPerS: number;
  resultsPerS: number;
  /** Fractions of the frames in the interval, 0..1. */
  invalid: number;
  empty: number;
  truncated: number;
  /** Frames in the interval (the denominator). */
  frames: number;
}

/** Rates between two polls; null when there is no previous poll, no time passed, no frame arrived,
 *  or a counter went down (a new run restarts the counters: measure again from the new values). */
export function resultsRates(prev: ResultsSample | null, cur: ResultsSample | null): ResultsRates | null {
  if (!prev || !cur) return null;
  const dt = (cur.atMs - prev.atMs) / 1000;
  if (dt <= 0) return null;
  const df = cur.frames - prev.frames;
  if (df < 0 || cur.results < prev.results || cur.invalid_frames < prev.invalid_frames ||
      cur.empty_frames < prev.empty_frames || cur.truncated_frames < prev.truncated_frames) return null;
  if (df === 0) return null;
  return {
    framesPerS: df / dt,
    resultsPerS: (cur.results - prev.results) / dt,
    invalid: (cur.invalid_frames - prev.invalid_frames) / df,
    empty: (cur.empty_frames - prev.empty_frames) / df,
    truncated: (cur.truncated_frames - prev.truncated_frames) / df,
    frames: df,
  };
}

/** "5000 frames/s · 4800 results/s · invalid 0.01 % · empty 31.2 % · truncated 0.00 %" */
export function formatResultsRates(r: ResultsRates | null): string {
  if (!r) return "results: waiting for frames";
  const pct = (v: number) => `${(v * 100).toFixed(2)} %`;
  return `${Math.round(r.framesPerS)} frames/s · ${Math.round(r.resultsPerS)} results/s · invalid ${pct(r.invalid)} · empty ${pct(r.empty)} · truncated ${pct(r.truncated)}`;
}
