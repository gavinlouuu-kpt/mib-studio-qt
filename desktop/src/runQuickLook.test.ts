// Bridge ABI 44 / review ABI 6 (#716 A7a): the run quick look client against a mocked transport.
// The golden reply (crates/mib-bridge/contract/fixtures/run-quick-look-v1.json, the review bridge's
// output on the parity fixture, pinned by crates/mib-bridge/tests/review_bridge.rs) decodes as the
// backend sent it; anything off contract is rejected; a malformed request never reaches the
// transport; both bridges send the same arguments.
import { beforeEach, expect, it, vi } from "vitest";
import golden from "../../crates/mib-bridge/contract/fixtures/run-quick-look-v1.json";
import { bridge } from "./bridge";
import { BRIDGE_ABI_VERSION, RUN_QUICK_LOOK } from "./bridgeContract";
import { reviewBridge } from "./review/reviewBridge";
import { REVIEW_ABI_VERSION, REVIEW_RUN_STATISTICS } from "./review/reviewContract";
import { decodeRunQuickLookReply, runQuickLookArgs } from "./runQuickLook";

const { invoke } = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
beforeEach(() => { invoke.mockReset(); });

const clone = <T,>(v: T): T => JSON.parse(JSON.stringify(v)) as T;
const reference = { path: "/data/runs/fixture_3.h5", run_id: "33333333-3333-4333-8333-333333333333", start_generation: "8" };

it("pins the contract's versions and bounds", () => {
  expect(BRIDGE_ABI_VERSION).toBe(44);
  expect(REVIEW_ABI_VERSION).toBe(6);
  expect(RUN_QUICK_LOOK).toEqual({ max_paths: 32, path_max_bytes: 4096, run_id_max_bytes: 128, overlay_max_points: 2000,
    low_n_valid_rows: 200, cache_entries: 64, schema_version: 1 });
  expect(REVIEW_RUN_STATISTICS).toEqual({ max_paths: 32, path_max_bytes: 4096 });
});

it("decodes the backend's golden reply unchanged: numbers, nulls and decimal strings as sent", () => {
  const r = decodeRunQuickLookReply(clone(golden));
  expect(r).toEqual(golden);
  const [v3, legacy, raw, missing] = r.runs;
  expect(v3.status).toBe("computed");
  expect(v3.statistics?.counts).toEqual({ set_rows: "25", valid: "21", finite: "19", excluded: "2" });
  expect(v3.statistics?.area_um2.median).toBeCloseTo(15.5, 12);
  expect(v3.statistics?.emodulus).toMatchObject({ status: "computed", n: "14", out_of_domain: "4" });
  expect(v3.statistics?.emodulus.lut?.provenance).toBe("snapshot"); // the identity's (#783)
  expect(v3.statistics?.flags).toEqual(["low_n", "loss", "e_out_of_domain"]);
  expect(v3.delta?.emodulus_median_kpa).toEqual({ value: null, withheld: "lut_differs" });
  expect(v3.delta?.differs.profile).toBe("differs");
  // Not recorded stays null, never 0.
  expect(legacy.statistics?.area_um2).toEqual({ n: null, median: null, pixel_to_micron: null });
  expect(legacy.statistics?.emodulus.status).toBe("not_recorded");
  expect(legacy.delta?.area_um2_median.withheld).toBe("factor_not_recorded");
  expect(raw).toMatchObject({ status: "not_available", message: "not available until exported to HDF5", statistics: null });
  expect(missing).toMatchObject({ status: "error", statistics: null, delta: null });
  expect(r.reference?.confirmed).toBe(true);
});

it("rejects replies off the contract", () => {
  const broken: Array<(g: any) => void> = [
    (g) => { g.runs[0].status = "maybe"; },
    (g) => { g.runs[0].statistics.counts.valid = 21; },
    (g) => { g.runs[0].statistics.flags.push("verdict"); },
    (g) => { g.runs[0].statistics.emodulus.status = "estimated"; },
    (g) => { g.runs[1].statistics.area_um2.median = 1.5; },          // area without a recorded factor
    (g) => { g.runs[1].statistics.emodulus.n = "0"; },               // E numbers when not computed
    (g) => { g.runs[0].statistics.emodulus.lut.provenance = "guessed"; }, // not an E-LUT provenance
    (g) => { g.runs[0].statistics.emodulus.lut = null; },            // computed without a LUT record
    (g) => { g.runs[0].delta.area_um2_median = { value: 1, withheld: "no_value" }; },
    (g) => { g.runs[0].delta.area_um2_median = { value: null, withheld: null }; },
    (g) => { g.runs[0].delta.differs.core = "maybe"; },
    (g) => { g.runs[2].delta = g.runs[0].delta; },                   // a delta on a run not computed
    (g) => { g.runs[2].statistics = g.runs[0].statistics; },         // statistics exactly when computed
    (g) => { g.reference.confirmed = false; },                       // deltas without a confirmed reference
    (g) => { g.reference = null; },                                  // deltas without a reference
    (g) => { g.runs[0].statistics.overlay.area_px.push(1); },
    (g) => { g.ok = false; },                                        // a refused request carries nothing
  ];
  for (const breakIt of broken) {
    const g = clone(golden);
    breakIt(g);
    expect(() => decodeRunQuickLookReply(g)).toThrow(/RUN_QUICK_LOOK_REPLY_INVALID/);
  }
  expect(decodeRunQuickLookReply({ ok: false, message: "invalid", schema_version: 1, runs: [], reference: null }).ok).toBe(false);
});

it("checks the request before anything is sent", () => {
  const many = Array.from({ length: RUN_QUICK_LOOK.max_paths + 1 }, (_, i) => `/r${i}.h5`);
  for (const bad of [[], many, [""], ["x".repeat(RUN_QUICK_LOOK.path_max_bytes + 1)]])
    expect(() => runQuickLookArgs(bad)).toThrow("INVALID_PATHS");
  expect(() => runQuickLookArgs(["/a.h5"], { path: "", run_id: null })).toThrow("INVALID_REFERENCE_PATH");
  expect(() => runQuickLookArgs(["/a.h5"], { path: "/r.h5", run_id: "" })).toThrow("INVALID_REFERENCE_RUN_ID");
  expect(() => runQuickLookArgs(["/a.h5"], { path: "/r.h5", run_id: null, start_generation: "-1" }))
    .toThrow("INVALID_REFERENCE_START_GENERATION");
  expect(runQuickLookArgs(["/a.h5"])).toEqual({ paths: ["/a.h5"] });
  expect(runQuickLookArgs(["/a.h5"], { path: "/r.h5", run_id: null })).toEqual({ paths: ["/a.h5"], reference: { path: "/r.h5", run_id: null } });
});

it("sends the same arguments through both bridges and decodes the reply", async () => {
  invoke.mockResolvedValueOnce(clone(golden));
  const a = await bridge.runQuickLook(["/data/runs/fixture_0.h5"], reference);
  expect(invoke).toHaveBeenLastCalledWith("run_quick_look", { paths: ["/data/runs/fixture_0.h5"], reference });
  invoke.mockResolvedValueOnce(clone(golden));
  const b = await reviewBridge.runStatistics(["/data/runs/fixture_0.h5"], reference);
  expect(invoke).toHaveBeenLastCalledWith("review_run_statistics", { paths: ["/data/runs/fixture_0.h5"], reference });
  expect(a).toEqual(b);
  await expect(bridge.runQuickLook([])).rejects.toThrow("INVALID_PATHS");
  expect(invoke).toHaveBeenCalledTimes(2);
  invoke.mockResolvedValueOnce({ ok: true, message: "", schema_version: 1, runs: [{ status: "computed" }], reference: null });
  await expect(bridge.runQuickLook(["/a.h5"])).rejects.toThrow(/RUN_QUICK_LOOK_REPLY_INVALID/);
});
