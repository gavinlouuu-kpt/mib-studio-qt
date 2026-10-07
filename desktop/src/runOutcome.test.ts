import { describe, expect, it } from "vitest";
import { RUN_COMPLETION_STATES } from "./bridgeContract";
import type { ExperimentStatus } from "./eventAdapter";
import type { RunAccounting } from "./bridge";
import { describeReviewOutcome, describeRunOutcome, parseLossCounts, runKey } from "./runOutcome";

const BASE = {
  valid: true, state: 0, start_time_ns: "1", end_time_ns: "9", valid_buffered: "0", invalid_buffered: "0",
  valid_saved: "27162", invalid_saved: "0", dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false,
  output_path: "/tmp/run.h5", message: "finalized", start_generation: "3", readiness_generation: "1", capture_generation: "5",
  persistence_admitted: "27162", persistence_committed: "27162", persistence_failed: "0", terminal: true,
  finalization_ok: true, completion: RUN_COMPLETION_STATES.Complete, completion_reason: "all admitted frames reconciled",
  fault_revision: "0", fault_code: "", fault_message: "",
} as ExperimentStatus;

// Undeclared loss: a hole in the frame sequence (the backend's own text).
const LOSS = "undeclared loss: storeOverwritten=0 storeNotCommitted=0 storeMalformed=0 processingFailed=0 sequenceGaps=2";
// What a clean PZ7035 run says: one malformed frame (an ingress error) is a DECLARED loss.
const DECLARED = "declared policy: storeMalformed=1 cancelledByPolicy=0 pendingAtStop=0 persistencePendingAtStop=0 persistenceCancelledByPolicy=0";

describe("run outcome (#549)", () => {
  it("parses the non-zero loss counts and ignores the zeros", () => {
    expect(parseLossCounts(LOSS)).toEqual([expect.objectContaining({ key: "sequenceGaps", count: 2 })]);
    expect(parseLossCounts(DECLARED)).toEqual([expect.objectContaining({ key: "storeMalformed", count: 1 })]);
    expect(parseLossCounts("undeclared loss: storeOverwritten=2 storeNotCommitted=0 storeMalformed=1 processingFailed=3 sequenceGaps=4").map((c) => [c.key, c.count]))
      .toEqual([["storeOverwritten", 2], ["storeMalformed", 1], ["processingFailed", 3], ["sequenceGaps", 4]]);
    expect(parseLossCounts("all admitted frames reconciled")).toEqual([]);
  });

  it("reports an undeclared loss with the counts and the fraction instead of saying only 'finalized'", () => {
    const o = describeRunOutcome({ ...BASE, completion: RUN_COMPLETION_STATES.IncompleteLoss, completion_reason: LOSS })!;
    expect(o.severity).toBe("loss");
    expect(o.losses).toHaveLength(1);
    expect(o.lossFraction).toBeCloseTo(2 / 27162, 9);
    expect(o.attention).toBe(false);
    expect(o.headline).toContain("undeclared loss");
    expect(o.headline).toContain("of 27162 admitted (0.007 %)");
    expect(o.headline).toContain("2 frames were missing from the frame sequence");
    expect(o.reason).toBe(LOSS);
  });

  it("reports a malformed frame as a declared partial result, informational at the sensor-link baseline", () => {
    const o = describeRunOutcome({ ...BASE, completion: RUN_COMPLETION_STATES.IntentionallyPartial, completion_reason: DECLARED })!;
    expect(o.severity).toBe("partial");
    expect(o.attention).toBe(false);
    expect(o.headline).toBe("Run finished with a declared partial result: 1 frame was malformed (an ingress error from the sensor link, or an unusable frame) (0.004 % of 27162 admitted).");
    expect(o.headline).not.toContain("undeclared");
  });

  it("asks for attention when malformed frames are above 0.1 % of the admitted frames", () => {
    const at = (malformed: number) => describeRunOutcome({
      ...BASE, persistence_admitted: "10000", completion: RUN_COMPLETION_STATES.IntentionallyPartial,
      completion_reason: `declared policy: storeMalformed=${malformed} cancelledByPolicy=0 pendingAtStop=0 persistencePendingAtStop=0 persistenceCancelledByPolicy=0`,
    })!;
    expect(at(10).attention).toBe(false); // exactly 0.1 %
    const high = at(11);
    expect(high.attention).toBe(true);
    expect(high.severity).toBe("partial");
    expect(high.headline).toContain("check the sensor link");
  });

  it("lists policy-declared counts and keeps the old wording when there is nothing to count", () => {
    const o = describeRunOutcome({
      ...BASE, completion: RUN_COMPLETION_STATES.IntentionallyPartial,
      completion_reason: "declared policy: storeMalformed=0 cancelledByPolicy=3 pendingAtStop=0 persistencePendingAtStop=0 persistenceCancelledByPolicy=0",
    })!;
    expect(o.headline).toContain("3 frames were cancelled by the delivery policy");
    expect(describeRunOutcome({ ...BASE, completion: RUN_COMPLETION_STATES.IntentionallyPartial, completion_reason: "" })?.headline)
      .toBe("Run finished with a declared partial result (by policy).");
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

// What fetch_run_accounting returns (ABI 31): the true admitted-frame count, not the saved rows.
const ACC = (o: Partial<RunAccounting>): RunAccounting => ({
  available: true, source: "last_run", recorded: true, start_generation: 3, completion: RUN_COMPLETION_STATES.IntentionallyPartial,
  completion_reason: DECLARED, reconciled: true, admitted: 50407, store_malformed: 1, ...o,
});

describe("run accounting denominators and the Review tab (#549, ABI 31)", () => {
  const partial = { ...BASE, completion: RUN_COMPLETION_STATES.IntentionallyPartial, completion_reason: DECLARED };

  it("divides by the frames the run admitted when the accounting is there, not by the saved rows", () => {
    // 27162 rows were saved but 50407 frames were admitted (the rest were empty).
    const withAcc = describeRunOutcome(partial, ACC({}))!;
    expect(withAcc.admitted).toBe(50407);
    expect(withAcc.headline).toContain("0.002 % of 50407 admitted");
    const without = describeRunOutcome(partial)!;
    expect(without.admitted).toBe(27162);
    expect(without.headline).toContain("0.004 % of 27162 admitted");
  });

  it("ignores accounting that belongs to another run or is not recorded", () => {
    expect(describeRunOutcome(partial, ACC({ start_generation: 2 }))!.admitted).toBe(27162);
    expect(describeRunOutcome(partial, ACC({ available: false }))!.admitted).toBe(27162);
    expect(describeRunOutcome(partial, ACC({ recorded: false }))!.admitted).toBe(27162);
    expect(describeRunOutcome(partial, null)!.admitted).toBe(27162);
  });

  it("describes the accounting saved in the review file", () => {
    const o = describeReviewOutcome(ACC({ source: "review", start_generation: undefined, file_path: "/tmp/run.h5" }))!;
    expect(o.severity).toBe("partial");
    expect(o.admitted).toBe(50407);
    expect(o.headline).toContain("1 frame was malformed");
    const loss = describeReviewOutcome(ACC({ source: "review", completion: RUN_COMPLETION_STATES.IncompleteLoss, completion_reason: LOSS }))!;
    expect(loss.severity).toBe("loss");
    expect(loss.headline).toContain("2 frames were missing from the frame sequence");
  });

  it("is quiet for a raw recording or a legacy file, and says nothing without a file", () => {
    expect(describeReviewOutcome(ACC({ source: "review", recorded: false, completion: undefined }))!.severity).toBe("legacy");
    expect(describeReviewOutcome({ available: true, source: "review", recorded: false })!.headline).toContain("No run accounting");
    expect(describeReviewOutcome({ available: false, source: "review", error: "no file is open for review" })).toBeNull();
    expect(describeReviewOutcome(null)).toBeNull();
  });

  it("calls a file whose counters do not reconcile a failure", () => {
    const o = describeReviewOutcome(ACC({ source: "review", reconciled: false, completion: RUN_COMPLETION_STATES.Complete, completion_reason: "all admitted frames reconciled" }))!;
    expect(o.severity).toBe("failed");
    expect(o.headline).toContain("does not reconcile");
  });
});
