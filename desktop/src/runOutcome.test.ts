import { describe, expect, it } from "vitest";
import { RUN_COMPLETION_STATES } from "./bridgeContract";
import type { ExperimentStatus } from "./eventAdapter";
import { describeRunOutcome, parseLossCounts, runKey } from "./runOutcome";

const BASE = {
  valid: true, state: 0, start_time_ns: "1", end_time_ns: "9", valid_buffered: "0", invalid_buffered: "0",
  valid_saved: "27162", invalid_saved: "0", dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false,
  output_path: "/tmp/run.h5", message: "finalized", start_generation: "3", readiness_generation: "1", capture_generation: "5",
  persistence_admitted: "27162", persistence_committed: "27162", persistence_failed: "0", terminal: true,
  finalization_ok: true, completion: RUN_COMPLETION_STATES.Complete, completion_reason: "all admitted frames reconciled",
  fault_revision: "0", fault_code: "", fault_message: "",
} as ExperimentStatus;

// The line the backend wrote on the board in the #510 run.
const LOSS = "undeclared loss: storeOverwritten=0 storeNotCommitted=0 storeMalformed=1 processingFailed=0 sequenceGaps=0";

describe("run outcome (#549)", () => {
  it("parses the non-zero loss counts and ignores the zeros", () => {
    expect(parseLossCounts(LOSS)).toEqual([expect.objectContaining({ key: "storeMalformed", count: 1 })]);
    expect(parseLossCounts("undeclared loss: storeOverwritten=2 storeNotCommitted=0 storeMalformed=1 processingFailed=3 sequenceGaps=4").map((c) => [c.key, c.count]))
      .toEqual([["storeOverwritten", 2], ["storeMalformed", 1], ["processingFailed", 3], ["sequenceGaps", 4]]);
    expect(parseLossCounts("all admitted frames reconciled")).toEqual([]);
  });

  it("reports an undeclared loss with the counts and the fraction instead of saying only 'finalized'", () => {
    const o = describeRunOutcome({ ...BASE, completion: RUN_COMPLETION_STATES.IncompleteLoss, completion_reason: LOSS })!;
    expect(o.severity).toBe("loss");
    expect(o.losses).toHaveLength(1);
    expect(o.lossFraction).toBeCloseTo(1 / 27162, 9);
    expect(o.headline).toContain("undeclared loss");
    expect(o.headline).toContain("of 27162 admitted (0.004 %)");
    expect(o.headline).toContain("1 frame was malformed");
    expect(o.reason).toBe(LOSS);
  });

  it("pluralises and lists every kind of loss", () => {
    const o = describeRunOutcome({
      ...BASE, persistence_admitted: "1000", completion: RUN_COMPLETION_STATES.IncompleteLoss,
      completion_reason: "undeclared loss: storeOverwritten=2 storeNotCommitted=0 storeMalformed=0 processingFailed=0 sequenceGaps=3",
    })!;
    expect(o.headline).toContain("2 frames were overwritten");
    expect(o.headline).toContain("3 frames were missing from the frame sequence");
    expect(o.lossFraction).toBeCloseTo(0.005, 9);
  });

  it("keeps the backend's text when it cannot parse the counts, and copes with nothing admitted", () => {
    const o = describeRunOutcome({ ...BASE, persistence_admitted: "0", completion: RUN_COMPLETION_STATES.IncompleteLoss, completion_reason: "something new" })!;
    expect(o.severity).toBe("loss");
    expect(o.lossFraction).toBeNull();
    expect(o.headline).toBe("Run finished with undeclared loss: something new.");
  });

  it("names the other outcomes", () => {
    expect(describeRunOutcome(BASE)).toMatchObject({ severity: "ok", headline: "Run complete: all 27162 admitted frames reconciled." });
    expect(describeRunOutcome({ ...BASE, completion: RUN_COMPLETION_STATES.IntentionallyPartial })?.severity).toBe("partial");
    expect(describeRunOutcome({ ...BASE, completion: RUN_COMPLETION_STATES.Failed, completion_reason: "persistence failures" })?.headline)
      .toBe("Run failed: persistence failures.");
    expect(describeRunOutcome({ ...BASE, completion: RUN_COMPLETION_STATES.Unknown, completion_reason: "" })?.severity).toBe("unknown");
  });

  it("reports nothing before a run has finished, for a cancelled run, or without a status", () => {
    expect(describeRunOutcome(null)).toBeNull();
    expect(describeRunOutcome({ ...BASE, terminal: false })).toBeNull();
    expect(describeRunOutcome({ ...BASE, cancelled: true })).toBeNull();
    expect(describeRunOutcome({ ...BASE, valid: false })).toBeNull();
  });

  it("identifies a finished run once", () => {
    expect(runKey(BASE)).toBe("3:9");
    expect(runKey({ ...BASE, terminal: false })).toBe("");
    expect(runKey(null)).toBe("");
  });
});
