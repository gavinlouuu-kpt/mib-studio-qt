import { describe, expect, it } from "vitest";
import { RUN_COMPLETION_STATES as C } from "../../bridgeContract";
import type { RunAccounting } from "../../bridge";
import type { ExperimentStatus } from "../../eventAdapter";
import { ACCOUNTING_REMEDIATION, accountingCounts, accountingDialog, completionName, runLostData } from "./runFinish";

const status = (over: Partial<ExperimentStatus> = {}): ExperimentStatus => ({
  valid: true, state: 0, start_time_ns: "1", end_time_ns: "9", valid_buffered: "0", invalid_buffered: "0", valid_saved: "0", invalid_saved: "0",
  dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false, output_path: "/d/run.h5", message: "", start_generation: "4",
  readiness_generation: "1", capture_generation: "1", persistence_admitted: "120", persistence_committed: "118", persistence_failed: "2",
  terminal: true, finalization_ok: true, completion: C.IncompleteLoss, completion_reason: "undeclared loss: storeOverwritten=2 sequenceGaps=0",
  fault_revision: "0", fault_code: "", fault_message: "", ...over,
});
const acc: RunAccounting = {
  available: true, source: "last_run", start_generation: 4, admitted: 120, empty: 5, processed: 100, scientifically_rejected: 3, processing_failed: 1,
  store_overwritten: 2, store_not_committed: 1, store_malformed: 0,
};

describe("the run accounting modal (MON-023)", () => {
  it("names the contract completion values", () => {
    expect([C.Complete, C.IntentionallyPartial, C.IncompleteLoss, C.Failed, C.Unknown, 77].map(completionName)).toEqual(["Complete", "IntentionallyPartial", "IncompleteLoss", "Failed", "Unknown", "Unknown"]);
  });
  it("is due only for a finished run that is neither Complete nor IntentionallyPartial", () => {
    expect(runLostData(status())).toBe(true);
    expect(runLostData(status({ completion: C.Failed }))).toBe(true);
    expect(runLostData(status({ completion: C.Unknown }))).toBe(true);
    expect(runLostData(status({ completion: C.Complete }))).toBe(false);
    expect(runLostData(status({ completion: C.IntentionallyPartial }))).toBe(false);
    expect(runLostData(status({ terminal: false }))).toBe(false);
    expect(runLostData(status({ cancelled: true }))).toBe(false);
    expect(runLostData(status({ valid: false }))).toBe(false);
    expect(runLostData(null)).toBe(false);
  });
  it("leaves the fatal save error to its recovery panel", () => {
    expect(runLostData(status({ fault_code: "experiment.saveFailed" }))).toBe(false);
    expect(runLostData(status({ fault_code: "experiment.flushFailed" }))).toBe(true);
  });
  it("lists empty, processed, rejected, failed, store loss and persisted counts", () => {
    expect(accountingCounts(status(), acc)).toBe("Empty 5 · processed 100 · rejected 3 · processing failed 1 · store loss 3 · persisted 118/120 · persistence failed 2");
  });
  it("reads n/a for the counts it does not have, and never uses another run's accounting", () => {
    const n = "Empty n/a · processed n/a · rejected n/a · processing failed n/a · store loss n/a · persisted 118/120 · persistence failed 2";
    expect(accountingCounts(status(), null)).toBe(n);
    expect(accountingCounts(status(), { available: false, source: "last_run" })).toBe(n);
    expect(accountingCounts(status(), { ...acc, start_generation: 3 })).toBe(n);
  });
  it("is Qt's warning with the reason, the counts and the remediation", () => {
    const d = accountingDialog(status(), acc)!;
    expect(d).toMatchObject({ kind: "warning", title: "Experiment Accounting" });
    expect(d.text.startsWith("The run is recorded as 'IncompleteLoss': undeclared loss: storeOverwritten=2 sequenceGaps=0\n\nEmpty 5 ·")).toBe(true);
    expect(d.text.endsWith(ACCOUNTING_REMEDIATION)).toBe(true);
    expect(accountingDialog(status({ completion: C.Complete }), acc)).toBeNull();
    expect(accountingDialog(null, acc)).toBeNull();
  });
});
