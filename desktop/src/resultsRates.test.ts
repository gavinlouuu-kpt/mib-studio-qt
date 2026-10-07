import { describe, expect, it } from "vitest";
import { formatResultsRates, resultsRates, type ResultsSample } from "./resultsRates";

const at = (atMs: number, o: Partial<ResultsSample>): ResultsSample => ({
  atMs, frames: 0, results: 0, empty_frames: 0, invalid_frames: 0, truncated_frames: 0, ...o,
});

describe("live PL result statistics (#501)", () => {
  it("turns two polls into rates and outcome fractions", () => {
    const r = resultsRates(
      at(1000, { frames: 100_000, results: 90_000, empty_frames: 30_000, invalid_frames: 5, truncated_frames: 0 }),
      at(2000, { frames: 105_000, results: 94_000, empty_frames: 31_500, invalid_frames: 5, truncated_frames: 50 }))!;
    expect(r.framesPerS).toBe(5000);
    expect(r.resultsPerS).toBe(4000);
    expect(r.empty).toBeCloseTo(0.3, 6);
    expect(r.invalid).toBe(0);
    expect(r.truncated).toBeCloseTo(0.01, 6);
    expect(r.frames).toBe(5000);
  });

  it("uses the real interval, not the nominal one", () => {
    const r = resultsRates(at(0, { frames: 0 }), at(2500, { frames: 12_500 }))!;
    expect(r.framesPerS).toBe(5000);
  });

  it("restarts after a new run (counters go down) and while nothing arrives", () => {
    expect(resultsRates(at(0, { frames: 5000, results: 10 }), at(1000, { frames: 20, results: 1 }))).toBeNull();
    expect(resultsRates(at(0, { frames: 5000, invalid_frames: 3 }), at(1000, { frames: 9000, invalid_frames: 1 }))).toBeNull();
    expect(resultsRates(at(0, { frames: 5000 }), at(1000, { frames: 5000 }))).toBeNull();
    expect(resultsRates(null, at(1000, { frames: 5000 }))).toBeNull();
    expect(resultsRates(at(1000, {}), at(1000, { frames: 5 }))).toBeNull();
  });

  it("formats the line for the Run status", () => {
    const r = resultsRates(at(0, {}), at(1000, { frames: 5000, results: 4800, empty_frames: 1560, invalid_frames: 1 }));
    expect(formatResultsRates(r)).toBe("5000 frames/s · 4800 results/s · invalid 0.02 % · empty 31.20 % · truncated 0.00 %");
    expect(formatResultsRates(null)).toBe("results: waiting for frames");
  });
});
