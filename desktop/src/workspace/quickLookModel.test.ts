// The Summary sub-view's pure model (#716 A7b): the plan's closed runs, formatting only (a recorded 0
// stays 0, null stays null), the backend's deltas shown as sent, the flags as grey chips, and the pin
// that neither the model nor the view computes anything from the quick look's numbers.
import { describe, expect, it } from "vitest";
import golden from "../../../crates/mib-bridge/contract/fixtures/run-quick-look-v1.json";
import { decodeRunQuickLookReply, type RunStatistics } from "../runQuickLook";
import { emptyRow } from "./planModel";
import { newQueue, reduceQueue, type QueueEvent } from "./queueModel";
import {
  counterText,
  deltaText,
  differsChips,
  flagChips,
  hasDeferred,
  planClosedRuns,
  quickLookPaths,
  quickTiles,
  referenceOf,
  statusChip,
  TARGET_GROUP_LABEL,
} from "./quickLookModel";

const reply = () => decodeRunQuickLookReply(JSON.parse(JSON.stringify(golden)));

describe("formatting only", () => {
  it("counters are the backend's decimal strings in digit groups; 0 stays 0 and null stays null", () => {
    expect(counterText("0")).toBe("0");
    expect(counterText("1234567")).toBe("1 234 567");
    expect(counterText("18446744073709551615")).toBe("18 446 744 073 709 551 615"); // u64 max, no precision lost
    expect(counterText(null)).toBeNull();
  });

  it("deltas are the backend's: signed as sent, withheld with the reason, never recomputed", () => {
    expect(deltaText({ value: -0.0029999999999999957, withheld: null }, 4)).toBe("−0.0030");
    expect(deltaText({ value: 0.25, withheld: null }, 2)).toBe("+0.25");
    expect(deltaText({ value: 0, withheld: null }, 2)).toBe("0.00");
    expect(deltaText({ value: null, withheld: "lut_differs" }, 2)).toBe("withheld · E-LUT differs");
    expect(deltaText({ value: null, withheld: "factor_not_recorded" }, 2)).toBe("withheld · px→µm not recorded");
    expect(deltaText(null, 2)).toBeUndefined();
  });

  it("the golden v3 run: tiles with the backend's medians and deltas, the target group labelled as declared", () => {
    const [v3] = reply().runs;
    const tiles = quickTiles(v3.statistics!, v3.delta);
    const t = Object.fromEntries(tiles.map((x) => [x.key, x]));
    expect(t.valid.value).toBe("21");
    expect(t.area).toMatchObject({ value: "15.50", delta: "−0.25" });
    expect(t.deformability.delta).toBe("−0.0030");
    expect(t.emodulus.delta).toBe("withheld · E-LUT differs");
    expect(t.target.label).toBe(TARGET_GROUP_LABEL);
    expect(t.target.label).toBe("% in declared target group");
    // Flags: grey, descriptive, in the backend's order.
    expect(flagChips(v3.statistics).map((c) => c.tone)).toEqual(["neutral", "neutral", "neutral"]);
    expect(differsChips(v3.delta).map((c) => c.text)).toEqual(["Profile differs from the reference"]);
  });

  it("legacy and no-E: Not recorded, never 0; no fallback px→µm", () => {
    const [, legacy] = reply().runs;
    const t = Object.fromEntries(quickTiles(legacy.statistics!, legacy.delta).map((x) => [x.key, x]));
    expect(t.area.value).toBeNull();
    expect(t.area.missing).toBe("px→µm not recorded");
    expect(t.area.delta).toBe("withheld · px→µm not recorded");
    expect(t.emodulus.value).toBeNull();
    expect(t.emodulus.missing).toBe("Not recorded");
    expect(flagChips(legacy.statistics).find((c) => c.key === "legacy")).toMatchObject({ tone: "unknown", text: "Legacy file · no run snapshot" });
    const noLut = JSON.parse(JSON.stringify(legacy.statistics)) as RunStatistics;
    noLut.emodulus = { status: "no_lut", n: null, median_kpa: null, out_of_domain: null,
      lut: { loaded: false, lut_id: null, revision: null, source: null, sha256: null, provenance: "snapshot" } };
    expect(quickTiles(noLut, null).find((x) => x.key === "emodulus")!.missing).toBe("No E-LUT loaded");
    // A recorded 0 stays 0.
    const zero = JSON.parse(JSON.stringify(legacy.statistics)) as RunStatistics;
    zero.counts.valid = "0";
    zero.deformability.median = 0;
    const z = Object.fromEntries(quickTiles(zero, null).map((x) => [x.key, x]));
    expect(z.valid.value).toBe("0");
    expect(z.deformability.value).toBe("0.0000");
  });

  it("status chips: deferred, not available, error", () => {
    const r = reply();
    expect(statusChip(r.runs[2], false).text).toBe("Not available · raw recording");
    expect(statusChip(r.runs[3], false).text).toBe("Not read");
    expect(hasDeferred(r)).toBe(false);
    r.runs[2] = { ...r.runs[2], status: "deferred", message: "computed when the instrument is idle" };
    expect(hasDeferred(r)).toBe(true);
    expect(statusChip(r.runs[2], false).text).toBe("Deferred · computed when idle");
    // The reference alone deferred counts too (Codex round 1).
    const refOnly = reply();
    refOnly.runs = [refOnly.runs[0]];
    refOnly.runs[0] = { ...refOnly.runs[0], delta: null };
    refOnly.reference = { ...refOnly.reference!, confirmed: false, run: { ...refOnly.reference!.run!, status: "deferred", statistics: null, delta: null } };
    expect(hasDeferred(refOnly)).toBe(true);
  });
});

describe("the plan's closed runs", () => {
  const draft = { format: "queue", rows: [{ ...emptyRow("S1"), condition: "Vehicle" }, emptyRow("S2")] };
  const order = ["S1", "S2"];
  const ev = (q: ReturnType<typeof newQueue>, events: QueueEvent[]) => events.reduce((acc, e) => reduceQueue(acc, e, order), q);
  const status = (over: Record<string, unknown>) => ({
    valid: true, state: 0, start_time_ns: "0", end_time_ns: "0", valid_buffered: "0", invalid_buffered: "0", valid_saved: "0", invalid_saved: "0",
    dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false, output_path: "/d/s1.h5", message: "", start_generation: "1", readiness_generation: "1",
    capture_generation: "1", persistence_admitted: "0", persistence_committed: "0", persistence_failed: "0", terminal: false, finalization_ok: false,
    completion: 0, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "", ...over,
  }) as never;

  it("a queue row once finalized (awaiting the close confirmation) and once closed", () => {
    let q = ev(newQueue(), [
      { type: "start", ref: "S1", runIndex: 1, generationBefore: "0" },
      { type: "start-result", ref: "S1", runIndex: 1, generationBefore: "0", result: { kind: "started", path: "/d/s1", notice: "" } },
    ]);
    expect(planClosedRuns({ draft, queue: q, started: null })).toEqual([]);
    q = ev(q, [{ type: "status", status: status({ state: 2 }) }, { type: "status", status: status({ state: 0, terminal: true, finalization_ok: true }) }]);
    const runs = planClosedRuns({ draft, queue: q, started: null });
    expect(runs).toHaveLength(1);
    expect(runs[0]).toMatchObject({ path: "/d/s1.h5", runIndex: 1, ref: "S1", confirmed: false });
    expect(runs[0].row?.condition).toBe("Vehicle");
  });

  it("a single sample once its operation reported a good end, by its recorded path", () => {
    const single = { format: "single", rows: [emptyRow("S1")] };
    const started = { path: "/d/one", notice: "", runIndex: 1, outcome: "started" as const, operationId: "7" };
    expect(planClosedRuns({ draft: single, queue: newQueue(), started })).toEqual([]);
    expect(planClosedRuns({ draft: single, queue: newQueue(), started: { ...started, end: { ok: false, cancelled: false, message: "failed" } } })).toEqual([]);
    expect(planClosedRuns({ draft: single, queue: newQueue(), started: { ...started, end: { ok: true, cancelled: false, message: "done" } } }))
      .toMatchObject([{ path: "/d/one.h5", runIndex: 1, ref: "S1" }]);
  });

  it("reads at most the contract's 32 newest paths", () => {
    const runs = Array.from({ length: 34 }, (_, i) => ({ path: `/d/${i}.h5`, runIndex: i, ref: "S1", row: null, confirmed: true, outcome: { cancelled: false, headline: "" } }));
    const { paths, left } = quickLookPaths(runs);
    expect(paths).toHaveLength(32);
    expect(paths[0]).toBe("/d/2.h5");
    expect(left).toBe(2);
  });

  it("a reference is a computed run's file with the identity its quick look reported", () => {
    const r = reply();
    const run = { path: "/data/runs/fixture_0.h5", runIndex: 1, ref: "S1", row: null, confirmed: true, outcome: { cancelled: false, headline: "" } };
    expect(referenceOf(run, r.runs[0])).toEqual({ path: run.path, run_id: r.runs[0].statistics!.identity.run_id,
      start_generation: r.runs[0].statistics!.identity.start_generation, runIndex: 1, ref: "S1" });
    expect(referenceOf(run, r.runs[2])).toBeNull(); // not available: no numbers, no reference
  });
});

describe("no client-side arithmetic on the quick look's numbers (the A7 ruling 5)", () => {
  // Every number shown is the backend's; deltas are its run − reference. These sources format only.
  const FIELDS = "median|median_kpa|percent|per_s|span_s|value|valid|finite|excluded|set_rows|out_of_domain|admitted|processed|"
    + "persistence_committed|pixel_to_micron|n|of|area_um2|deformability|area_px|counts|delta|emodulus|event_rate|target_group";
  const OP_AFTER = new RegExp(`\\.(?:${FIELDS})\\b\\s*(?:\\[[^\\]]*\\])?\\s*[-+*/%](?![=>])`);
  const OP_BEFORE = new RegExp(`[-+*/%]\\s*[\\w$?.]*\\.(?:${FIELDS})\\b`);
  const BANNED = /\bMath\.|\bBigInt\(|\bNumber\(|\bparseFloat\(|\bparseInt\(|\.reduce\(|\bsum\b|\baverage\b/;
  /** The source without comments and string literals (template text kept, its expressions too). */
  const code = (src: string) => src
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/(^|[^:"'`\\])\/\/.*$/gm, "$1")
    .replace(/"(?:[^"\\\n]|\\.)*"/g, '""')
    .replace(/'(?:[^'\\\n]|\\.)*'/g, "''");
  const offending = (src: string) => code(src).split("\n").filter((l) => OP_AFTER.test(l) || OP_BEFORE.test(l) || BANNED.test(l));

  it("the pin catches arithmetic on the numbers (self-test)", () => {
    expect(offending("const d = run.area_um2.median - ref.area_um2.median;")).toHaveLength(1);
    expect(offending("const p = 100 * s.target_group.percent;")).toHaveLength(1);
    expect(offending("const n = Number(s.counts.valid) + 1;")).toHaveLength(1);
    expect(offending("const m = Math.abs(d.value);")).toHaveLength(1);
    expect(offending("const t = numberText(s.area_um2.median, 2);")).toHaveLength(0);
  });

  const sources = import.meta.glob(["./quickLookModel.ts", "./SummaryView.tsx", "./useQuickLook.ts"], { query: "?raw", import: "default", eager: true }) as Record<string, string>;
  it("reads the three sources", () => expect(Object.keys(sources).sort()).toEqual(["./SummaryView.tsx", "./quickLookModel.ts", "./useQuickLook.ts"]));
  for (const [file, src] of Object.entries(sources)) {
    it(`${file} computes nothing from them`, () => {
      expect(src.length).toBeGreaterThan(1000);
      expect(offending(src)).toEqual([]);
    });
  }
});
