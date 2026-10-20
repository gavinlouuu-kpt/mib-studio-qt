// R1 Open's experiment library model (#717 B1b part 2): grouping by declared
// experiment, ordering by run index, the probe chunks and the group summary.
// A member a file does not record stays absent; nothing is invented.
import { describe, expect, it } from "vitest";
import { RUN_COMPLETION_STATES } from "../../bridgeContract";
import type { ReviewRunIdentity, ReviewRunProbe } from "../reviewBridge";
import { REVIEW_PROBE } from "../reviewContract";
import { groupRuns, groupSummary, probeChunks, recordsLabel, runLabel, sampleLabel, snapshotLabel } from "./libraryModel";
import { identityFacts, runFilesIn } from "./openModel";

const absent = (): ReviewRunIdentity => ({
  has_snapshot: false,
  snapshot_unreadable: false,
  snapshot_version: null,
  run_id: null,
  experiment_id: null,
  run_index: null,
  sample_ref: null,
  calibration_record_id: null,
  operator: null,
  annotation: null,
  annotation_unreadable: false,
});

function probe(path: string, identity: Partial<ReviewRunIdentity> = {}, over: Partial<ReviewRunProbe> = {}): ReviewRunProbe {
  return {
    path,
    ok: true,
    message: "",
    recording_file: false,
    start_time_ns: null,
    end_time_ns: null,
    total_valid: null,
    total_invalid: null,
    accounting_completion: null,
    core_version: null,
    identity: { ...absent(), ...identity },
    ...over,
  };
}

const declared = (experiment: string, runIndex: number | null, sampleRef: string | null = null) => ({
  has_snapshot: true,
  snapshot_version: 3,
  experiment_id: experiment,
  run_index: runIndex,
  sample_ref: sampleRef,
});

describe("groupRuns", () => {
  const runs = runFilesIn("/d", ["z.h5", "a.h5", "m.h5", "n.h5", "legacy.h5", "v1.h5", "bad.h5", "later.h5", "x.h5"]);
  const probes = new Map<string, ReviewRunProbe>(
    [
      probe("/d/z.h5", declared("E1", 1), { start_time_ns: "1000" }),
      probe("/d/a.h5", declared("E1", 3), { start_time_ns: "3000" }),
      probe("/d/m.h5", declared("E1", 2), { start_time_ns: "2000" }),
      probe("/d/n.h5", declared("E1", null)), // declares no run index: after the indexed runs
      probe("/d/x.h5", declared("E2", 1), { start_time_ns: "9000" }), // the newest experiment
      probe("/d/legacy.h5"),
      probe("/d/v1.h5", { has_snapshot: true, snapshot_version: 1 }),
      probe("/d/bad.h5", {}, { ok: false, message: "Not an HDF5 file" }),
    ].map((p) => [p.path, p]),
  );
  const groups = groupRuns(runs, probes);

  it("one group per declared experiment, newest first, runs by run index", () => {
    expect(groups.map((g) => g.key)).toEqual(["experiment:E2", "experiment:E1", "no-experiment", "legacy", "unreadable", "unprobed"]);
    expect(groups[1].runs.map((r) => r.run.name)).toEqual(["z.h5", "m.h5", "a.h5", "n.h5"]);
  });

  it("puts snapshots without an experiment, legacy, unreadable and unread files in their own groups", () => {
    expect(groups[2].runs.map((r) => r.run.name)).toEqual(["v1.h5"]);
    expect(groups[3].runs.map((r) => r.run.name)).toEqual(["legacy.h5"]);
    expect(groups[4].runs.map((r) => r.probe?.message)).toEqual(["Not an HDF5 file"]);
    expect(groups[5].runs.map((r) => [r.run.name, r.probe])).toEqual([["later.h5", null]]);
  });

  it("orders experiments without a recorded start after those with one", () => {
    const g = groupRuns(runFilesIn("/d", ["p.h5", "q.h5"]), new Map([["/d/p.h5", probe("/d/p.h5", declared("A", 1))], ["/d/q.h5", probe("/d/q.h5", declared("B", 1), { start_time_ns: "5" })]]));
    expect(g.map((x) => x.experimentId)).toEqual(["B", "A"]);
  });
});

describe("cells and summaries", () => {
  it("reads absent members as null, never 0 or an empty string", () => {
    const legacy = probe("/d/l.h5");
    expect(runLabel(legacy.identity)).toBeNull();
    expect(sampleLabel(legacy.identity)).toBeNull();
    expect(recordsLabel(legacy)).toBeNull();
    expect(recordsLabel(probe("/d/r.h5", {}, { total_valid: "0", total_invalid: null }))).toBe("0 valid · – invalid");
    expect(recordsLabel(probe("/d/r.h5", {}, { recording_file: true, total_valid: "9" }))).toBe("9 frames");
    expect(runLabel(probe("/d/i.h5", { run_index: 0 }).identity)).toBe("Run 0");
    expect(snapshotLabel(legacy.identity)).toEqual({ label: "No snapshot · legacy file", tone: "unknown" });
    expect(snapshotLabel(probe("/d/u.h5", { snapshot_unreadable: true }).identity).tone).toBe("warn");
  });

  it("summarizes a group from what its runs record", () => {
    const g = groupRuns(
      runFilesIn("/d", ["a.h5", "b.h5"]),
      new Map([
        ["/d/a.h5", probe("/d/a.h5", declared("E", 1, "S-1"), { accounting_completion: RUN_COMPLETION_STATES.Complete, core_version: "2.1.0" })],
        ["/d/b.h5", probe("/d/b.h5", declared("E", 2, "S-1"), { accounting_completion: RUN_COMPLETION_STATES.Failed, core_version: "2.1.0" })],
      ]),
    )[0];
    const s = groupSummary(g);
    expect(s.runs).toBe("2 runs · 1 sample");
    expect(s.dates).toBeNull();
    expect(s.conditions).toBeNull();
    expect(s.calibrations).toBeNull();
    expect(s.cores).toBe("2.1.0");
    expect(s.status).toEqual({ tone: "blocked", label: "1 of 2 runs · Failed" });
  });
});

describe("probeChunks", () => {
  it("splits a folder into calls of at most review_probe.max_paths", () => {
    expect(REVIEW_PROBE.max_paths).toBe(256);
    const paths = Array.from({ length: 513 }, (_, i) => `/d/${i}.h5`);
    expect(probeChunks(paths).map((c) => c.length)).toEqual([256, 256, 1]);
    expect(probeChunks(paths).flat()).toEqual(paths);
    expect(probeChunks([])).toEqual([]);
  });
});

describe("identityFacts", () => {
  it("says once that a legacy file has no run snapshot; every other member is absent", () => {
    const facts = identityFacts(absent());
    expect(facts[0]).toEqual({ label: "Run snapshot", value: "Not recorded · legacy file", tone: "unknown" });
    expect(facts.slice(1).every((f) => f.value === null)).toBe(true);
  });

  it("marks an unreadable snapshot or annotation, not as legacy", () => {
    const facts = identityFacts({ ...absent(), snapshot_unreadable: true, annotation_unreadable: true });
    expect(facts.find((f) => f.label === "Run snapshot")).toMatchObject({ value: "Present but unreadable", tone: "warn" });
    expect(facts.find((f) => f.label === "Annotation")).toMatchObject({ value: "Present but unreadable", tone: "warn" });
  });
});
