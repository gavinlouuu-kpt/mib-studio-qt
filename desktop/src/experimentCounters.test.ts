import { expect, it } from "vitest";
import type { ExperimentStatus } from "./eventAdapter";
import { experimentCounterRows } from "./experimentCounters";

it("separates saved classes, pending writes, policy drops and write failures", () => {
  const status = { valid: true, valid_saved: "29", invalid_saved: "41",
    persistence_admitted: "80", persistence_committed: "70", persistence_failed: "2",
    dropped_valid: "1", dropped_invalid: "2" } as ExperimentStatus;
  expect(experimentCounterRows(status)).toEqual([
    ["Valid Images Saved:", "29"], ["Invalid Images Saved:", "41"],
    ["Images Pending Save:", "5"], ["Valid Images Dropped:", "1"],
    ["Invalid Images Dropped:", "2"], ["Images Failed to Save:", "2"],
  ]);
  expect(experimentCounterRows({ ...status, persistence_admitted: "70", persistence_failed: "0",
    dropped_valid: "0", dropped_invalid: "0" })[2][1]).toBe("0");
});

it("keeps pending arithmetic exact above JavaScript's safe integer range", () => {
  const status = { valid: true, valid_saved: "9007199254740993", invalid_saved: "0",
    persistence_admitted: "9007199254740998", persistence_committed: "9007199254740993",
    persistence_failed: "0", dropped_valid: "0", dropped_invalid: "0" } as ExperimentStatus;
  expect(experimentCounterRows(status)[2][1]).toBe("5");
  expect(experimentCounterRows(null).every(([, value]) => value === "Unavailable")).toBe(true);
});
