import { describe, expect, it } from "vitest";
import { LOOP, referenceFailed, referenceLoaded } from "./densityFixtures";
import {
  isBusyRefusal, referenceErrorText, referenceFailureReason, referenceOfReply, referenceStartDir, restoreOutcome, restoreOutcomeOfError,
} from "./densityReference";

const ok = referenceLoaded;
const failed = referenceFailed;

// bridge-contract.json kde_reference_from_file: Qt's reasons, then the bridge's own.
const FILE_REASONS = [
  "file not found", "not a readable HDF5 file", "stored core contour unreadable (bad json)", "the experiment has no stored core contour",
  "the stored core contour is empty (too few cells)", "the stored core contour is too large (30000 vertices, at most 20000)", "path refused: outside the storage root",
];
const BUSY_REASONS = [
  "a review job is running; the file can be read when it finishes",
  "a raw recording is writing an HDF5 file; the file can be read when it stops",
  "an experiment is running or starting, or the instrument is busy (HDF5 files are read only while it is idle); try again when it is idle",
];

describe("a refusal because the instrument is busy (MON-062)", () => {
  it("is told apart from a file that is gone or unusable", () => {
    for (const r of BUSY_REASONS) expect(isBusyRefusal(r), r).toBe(true);
    for (const r of FILE_REASONS) expect(isBusyRefusal(r), r).toBe(false);
    expect(isBusyRefusal(null)).toBe(false);
    expect(isBusyRefusal(undefined)).toBe(false);
  });
});

describe("the start-up restore of the remembered file", () => {
  it("restores a loaded reference with Qt's label", () => {
    const outcome = restoreOutcome(ok());
    expect(outcome).toEqual({ kind: "restored", reference: { label: "run7.h5, full-run 90%", contours: [LOOP] } });
    expect(restoreOutcome(ok({ source: "live", core_percent: 85, core_fraction: 0.85 }))).toMatchObject({ reference: { label: "run7.h5, live estimate 85%" } });
  });
  it("drops the path when the file is gone or has no contour: the file, not the moment, is at fault", () => {
    for (const r of FILE_REASONS) expect(restoreOutcome(failed(r)), r).toEqual({ kind: "drop", why: r });
  });
  it("keeps the path and tries again when a run, a review job or a recording makes the instrument refuse the read", () => {
    for (const r of BUSY_REASONS) expect(restoreOutcome(failed(r)), r).toEqual({ kind: "retry", why: r });
  });
  it("a call that threw: an argument the bridge refuses drops the path, anything else (a lost connection) retries", () => {
    expect(restoreOutcomeOfError(new Error("INVALID_ARGUMENTS: a file path of 1 to 4096 bytes is required")).kind).toBe("drop");
    expect(restoreOutcomeOfError(new Error("connection lost")).kind).toBe("retry");
    expect(restoreOutcomeOfError("timeout")).toEqual({ kind: "retry", why: "timeout" });
  });
});

describe("the error dialog (MON-086)", () => {
  it("is Qt's text: the path, a blank line, the reason", () => {
    expect(referenceErrorText("/data/run7.h5", "file not found")).toBe("No reference contour loaded from\n/data/run7.h5\n\nfile not found");
  });
  it("takes the reason of a failed reply, the busy refusal included", () => {
    expect(referenceFailureReason(failed("file not found"))).toBe("file not found");
    expect(referenceFailureReason(failed(BUSY_REASONS[2]))).toContain("try again when it is idle");
  });
  it("makes a reference only of a loaded reply", () => {
    expect(referenceOfReply(failed("file not found"))).toBeNull();
    expect(referenceOfReply(ok())?.contours).toEqual([LOOP]);
  });
});

describe("the picker's start folder (Qt: the remembered file's folder)", () => {
  it("is the folder of the remembered path, on the instrument's side of the file system", () => {
    expect(referenceStartDir("/data/run7.h5")).toBe("/data");
    expect(referenceStartDir("C:\\data\\run7.h5")).toBe("C:\\data");
    expect(referenceStartDir("run7.h5")).toBeUndefined();
    expect(referenceStartDir("")).toBeUndefined();
  });
});
