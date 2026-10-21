// @vitest-environment jsdom
// O5 Run summary and O6 Quick look (#716 A7b) over a mocked transport and the real plan controller
// (usePlan, the queue driven by its Start and the statuses): the quick look read only on the
// transitions and Refresh (never polled), deferred / not available / error / legacy / no E / null
// shown as such, the backend's deltas only (a withheld one with its reason, `differs` flagged, never
// recomputed), the reference chosen and replaced, the overlay, the decisions (Repeat sample,
// Adjust next, Continue, Open in Review) and a viewer's gating, and the active run's "numbers at close".
import { act, StrictMode } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import golden from "../../../crates/mib-bridge/contract/fixtures/run-quick-look-v1.json";
import { EXPERIMENT_STATES, OPERATION_STATES } from "../bridgeContract";
import type { ExperimentStatus } from "../eventAdapter";
import { emptyRow } from "./planModel";
import { NO_OPERATIONS, rowState, type OperationTrack } from "./queueModel";
import { SummaryView } from "./SummaryView";
import { usePlan, type PlanController, type PlanStartResult } from "./usePlan";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));

// ---- the mock backend: run_quick_look built from the golden reply ----
type Variant = "v3" | "legacy" | "raw" | "error" | "deferred" | "noLut" | "zero";
let variants: Record<string, Variant> = {};
let deltaOverride: unknown = null;
let refConfirmed = true;
/** The reference's own run as the backend answers it: computed, or deferred (a miss during a run). */
let refRunStatus: "computed" | "deferred" = "computed";
/** Set to hold every read until the test releases it. */
let holdReads = false;
/** Set to make every read fail (a transport error). */
let failReads = false;
const heldReads: (() => void)[] = [];
const clone = <T,>(v: T): T => JSON.parse(JSON.stringify(v)) as T;
// eslint-disable-next-line @typescript-eslint/no-explicit-any
const G = golden as any;

function quickReply(args: { paths: string[]; reference?: { path: string; run_id: string | null; start_generation?: string | null } }) {
  const runs = args.paths.map((p, i) => {
    const v = variants[p] ?? "v3";
    const run = clone(v === "legacy" ? G.runs[1] : v === "raw" ? G.runs[2] : v === "error" ? G.runs[3] : G.runs[0]);
    run.path = p;
    if (run.statistics) {
      run.statistics.identity.run_id = `run-${p}`;
      run.statistics.identity.start_generation = String(i + 1);
    }
    if (v === "deferred") Object.assign(run, { status: "deferred", statistics: null, delta: null, message: "computed when the instrument is idle", computed_at: null, from_cache: false, file: null });
    if (v === "noLut") run.statistics.emodulus = { status: "no_lut", n: null, median_kpa: null, out_of_domain: null,
      lut: { loaded: false, lut_id: null, revision: null, source: null, sha256: null, provenance: "snapshot" } };
    if (v === "zero") { run.statistics.counts.valid = "0"; run.statistics.target_group.n = "0"; run.statistics.target_group.percent = 0; }
    if (!args.reference || !refConfirmed || run.status !== "computed") run.delta = null;
    else if (deltaOverride) run.delta = clone(deltaOverride);
    return run;
  });
  // An unconfirmed reference may still carry the file's computed numbers (another run on that path).
  const refRun = refRunStatus === "deferred"
    ? { ...clone(G.reference.run), path: args.reference?.path, status: "deferred", statistics: null, delta: null, computed_at: null, message: "computed when the instrument is idle" }
    : { ...clone(G.reference.run), path: args.reference?.path };
  const confirmed = refConfirmed && refRunStatus === "computed";
  if (!confirmed) runs.forEach((r: { delta: unknown }) => { r.delta = null; });
  const reference = args.reference ? {
    requested: { path: args.reference.path, run_id: args.reference.run_id, start_generation: args.reference.start_generation ?? null },
    confirmed, message: confirmed ? "" : refRunStatus === "deferred" ? "the reference is computed when the instrument is idle" : "run_id does not match the file",
    run: refRun,
  } : null;
  return { ok: true, message: `${runs.length} run(s)`, schema_version: 1, runs, reference };
}

const calls = () => vi.mocked(invoke).mock.calls.map(([c, a]) => ({ cmd: c as string, args: a as { paths: string[]; reference?: { path: string } } }));
const reads = () => calls().filter((c) => c.cmd === "run_quick_look");
const lastRead = () => { const r = reads(); return r[r.length - 1]; };

// ---- the plan and the statuses ----
const status = (over: Partial<ExperimentStatus> = {}): ExperimentStatus => ({
  transport_version: 1, valid: true, state: EXPERIMENT_STATES.Idle, start_time_ns: "0", end_time_ns: "0", valid_buffered: "0", invalid_buffered: "0",
  valid_saved: "0", invalid_saved: "0", dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false, output_path: "", message: "",
  start_generation: "0", readiness_generation: "1", capture_generation: "1", persistence_admitted: "0", persistence_committed: "0", persistence_failed: "0",
  terminal: false, finalization_ok: false, completion: 0, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "", ...over,
} as ExperimentStatus);
const activeAt = (gen: string, path: string) => status({ state: EXPERIMENT_STATES.Active, start_generation: gen, output_path: path, valid_saved: "120" });
const closedAt = (gen: string, path: string) => status({ state: EXPERIMENT_STATES.Idle, terminal: true, finalization_ok: true, start_generation: gen, output_path: path });

let nextPath = "/d/s1";
let operationId: string | undefined;
const runStart = vi.fn(async (): Promise<PlanStartResult> => ({ kind: "started", path: nextPath, notice: "", ...(operationId ? { operationId } : {}) }));
const onView = vi.fn();
const onAlign = vi.fn();
const openReview = vi.fn();
let reviewReason = "";
let planRef: PlanController;

interface Setup { status: ExperimentStatus | null; controlHeld?: boolean; show?: boolean; operations?: OperationTrack }
function Harness({ status: s, controlHeld = true, show = true, operations = NO_OPERATIONS }: Setup) {
  const plan = usePlan(runStart, { status: s, controlHeld, operations });
  planRef = plan;
  const active = !!s?.valid && !s.terminal && (s.state === EXPERIMENT_STATES.Starting || s.state === EXPERIMENT_STATES.Active || s.state === EXPERIMENT_STATES.Stopping);
  return show ? <SummaryView tabs={null} plan={plan} experimentActive={active} onView={onView} onAlign={onAlign} review={{ reason: reviewReason, open: openReview }} /> : null;
}

let host: HTMLDivElement, root: Root, current: Setup = { status: null };
const render = (s: Partial<Setup> = {}) => { current = { ...current, ...s }; return act(async () => root.render(<StrictMode><Harness {...current} /></StrictMode>)); };
const flush = () => act(async () => { await Promise.resolve(); await Promise.resolve(); });
const $ = (id: string) => host.querySelector<HTMLElement>(`[data-testid=${id}]`);
const text = (id: string) => $(id)?.textContent ?? "";
const click = async (id: string) => {
  const el = $(id);
  if (!el) throw new Error(`no ${id}`);
  await act(async () => el.click());
  await flush();
};

/** A queue plan: S1 Vehicle, S2 Drug, S3. */
async function queuePlan() {
  await render({ status: status() });
  await act(async () => planRef.edit((d) => ({ ...d, format: "queue", rows: [{ ...emptyRow("S1"), condition: "Vehicle" }, { ...emptyRow("S2"), condition: "Drug" }, emptyRow("S3")] })));
}
/** Run the queue's current row from Start to finalized (the run ends by itself), on generation `gen`. */
async function runToFinalized(gen: number, path: string) {
  nextPath = path;
  await render({ status: status({ start_generation: String(gen - 1), terminal: gen > 1, finalization_ok: gen > 1, output_path: gen > 1 ? current.status?.output_path ?? "" : "" }) });
  await act(async () => { await planRef.queue.startCurrent(); });
  await render({ status: activeAt(String(gen), `${path}.h5`) });
  await render({ status: closedAt(String(gen), `${path}.h5`) });
  await flush();
}

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.mocked(invoke).mockReset();
  vi.mocked(invoke).mockImplementation(async (c: string, a?: unknown) => {
    if (c === "run_quick_look") {
      if (failReads) throw new Error("TRANSPORT_LOST: no reply");
      // A held read is answered as the backend would at its release.
      const answer = () => quickReply(a as Parameters<typeof quickReply>[0]);
      return holdReads ? new Promise((resolve) => heldReads.push(() => resolve(answer()))) : answer();
    }
    throw new Error(`unexpected command ${c}`);
  });
  variants = {}; deltaOverride = null; operationId = undefined; refRunStatus = "computed"; holdReads = false; heldReads.length = 0; failReads = false; refConfirmed = true; reviewReason = ""; current = { status: null };
  runStart.mockClear(); onView.mockClear(); onAlign.mockClear(); openReview.mockClear();
  host = document.createElement("div");
  document.body.appendChild(host);
  root = createRoot(host);
});
afterEach(() => {
  act(() => root.unmount());
  host.remove();
});

describe("reads: only on the transitions and Refresh, never polled", () => {
  it("no closed run reads nothing; a run's close reads once; status polls, the close confirmation and a run read nothing; Refresh and the reference read", async () => {
    await queuePlan();
    expect($("summary-empty")).not.toBeNull();
    expect(reads()).toHaveLength(0);

    await runToFinalized(1, "/d/s1");
    expect(reads().map((r) => r.args.paths)).toEqual([["/d/s1.h5"]]);
    expect(text("summary-run")).toContain("Run summary · Run 1 · S1");
    expect(text("summary-awaiting")).toBe("Awaiting your close confirmation");
    // Polls of the same status (new objects every half second) read nothing.
    for (let i = 0; i < 5; i++) await render({ status: closedAt("1", "/d/s1.h5") });
    expect(reads()).toHaveLength(1);
    // Confirming the close changes nothing that is read.
    await act(async () => planRef.queue.confirmClosed());
    await flush();
    expect(reads()).toHaveLength(1);

    // S2 runs: nothing is read while it runs; its close reads both runs once.
    nextPath = "/d/s2";
    await act(async () => { await planRef.queue.startCurrent(); });
    await render({ status: activeAt("2", "/d/s2.h5") });
    for (let i = 0; i < 5; i++) await render({ status: activeAt("2", "/d/s2.h5") });
    expect(reads()).toHaveLength(1);
    expect(text("summary-active-run")).toContain("numbers at close");
    await render({ status: closedAt("2", "/d/s2.h5") });
    await flush();
    expect(reads().map((r) => r.args.paths)).toEqual([["/d/s1.h5"], ["/d/s1.h5", "/d/s2.h5"]]);
    expect($("summary-active-run")).toBeNull();

    await click("summary-refresh");
    expect(reads()).toHaveLength(3);
    await click("summary-run-1-set-reference");
    expect(reads()).toHaveLength(4);
    expect(reads()[3].args.reference).toEqual({ path: "/d/s1.h5", run_id: "run-/d/s1.h5", start_generation: "1" });
    // Nothing but run_quick_look was sent from the Summary.
    expect(calls().every((c) => c.cmd === "run_quick_look")).toBe(true);
  });

  it("a deferred run reads as computed when idle and is read again once the instrument turns idle, not before", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    await render({ show: false });
    // Another client's run is active: a fresh Summary's read is answered deferred (no cache hit).
    variants["/d/s1.h5"] = "deferred";
    await render({ status: activeAt("2", "/d/other.h5"), show: true });
    await flush();
    const n = reads().length;
    expect(text("summary-deferred")).toContain("Quick look computed when the instrument is idle");
    expect(text("summary-status")).toBe("Deferred · computed when idle");
    for (let i = 0; i < 5; i++) await render({ status: activeAt("2", "/d/other.h5") });
    expect(reads()).toHaveLength(n);
    // The run ends: the idle instrument computes it.
    variants["/d/s1.h5"] = "v3";
    await render({ status: closedAt("2", "/d/other.h5") });
    await flush();
    expect(reads()).toHaveLength(n + 1);
    expect($("summary-deferred")).toBeNull();
    expect(text("summary-tiles-valid")).toContain("21");
  });
});

describe("honesty: the backend's states and numbers as sent", () => {
  it("not available, an error, legacy, no E and a recorded 0", async () => {
    await queuePlan();
    variants = { "/d/s1.h5": "raw" };
    await runToFinalized(1, "/d/s1");
    expect(text("summary-not-available")).toContain("Not available until exported to HDF5");
    expect(text("summary-not-available")).toContain("not available until exported to HDF5");
    expect($("summary-tiles")).toBeNull();
    await act(async () => planRef.queue.confirmClosed());

    variants["/d/s2.h5"] = "legacy";
    await runToFinalized(2, "/d/s2");
    const legacy = text("summary-run");
    expect(legacy).toContain("Legacy file · no run snapshot");
    expect(text("summary-tiles-area")).toContain("px→µm not recorded");
    expect(text("summary-tiles-emodulus")).toContain("Not recorded");
    expect(text("summary-provenance")).toContain("Not recorded"); // no run id, no snapshot
    expect($("flag-legacy")?.getAttribute("data-tone")).toBe("unknown"); // grey, no verdict colour
    await act(async () => planRef.queue.confirmClosed());

    variants["/d/s3.h5"] = "noLut";
    await runToFinalized(3, "/d/s3");
    expect(text("summary-tiles-emodulus")).toContain("No E-LUT loaded");

    variants["/d/s3.h5"] = "zero";
    await click("summary-refresh");
    expect(text("summary-tiles-valid")).toMatch(/^Valid rows0$/);
    expect(text("summary-tiles-target")).toContain("0.0");
    expect(text("summary-tiles-target")).toContain("% in declared target group");

    variants["/d/s3.h5"] = "error";
    await click("summary-refresh");
    expect(text("summary-error")).toContain("No such file");
    // No verdict colours anywhere: every chip is grey or informational.
    for (const chip of host.querySelectorAll("[data-tone]")) expect(["neutral", "unknown", "info", "unsupported"]).toContain(chip.getAttribute("data-tone"));
    expect(host.textContent).not.toMatch(/\bshould\b|\brecommended\b|\bpass(ed)?\b|\bfail(ed)?\b|\bbetter\b|\bworse\b/i);
  });
});

describe("the reference and the backend's deltas", () => {
  it("a withheld delta shows its reason, differs is flagged, the value is the backend's (never recomputed), and a replaced reference moves", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    await runToFinalized(2, "/d/s2");
    expect(text("summary-reference-chip")).toBe("Reference · none chosen");
    expect(text("summary-run-2-tiles-area")).not.toMatch(/[+−]/); // no reference, no deltas

    // An inconsistent delta on purpose: the medians are 15.50 and 15.75, the backend says +99.
    deltaOverride = { ...G.runs[0].delta, area_um2_median: { value: 99, withheld: null } };
    await click("summary-run-1-set-reference");
    expect(planRef.reference).toMatchObject({ path: "/d/s1.h5", runIndex: 1, ref: "S1" });
    expect(text("summary-reference-chip")).toBe("Reference · confirmed by the backend");
    expect(text("summary-run-1-reference")).toBe("Reference");
    expect(text("summary-run-2-tiles-area")).toContain("+99.00");
    expect(text("summary-run-2-tiles-emodulus")).toContain("withheld · E-LUT differs");
    expect(text("summary-run-2")).toContain("Profile differs from the reference");
    expect(text("summary-run-1-tiles-area")).not.toContain("+99.00"); // the reference shows no delta to itself

    // Replaced: run 2 is the reference now.
    await click("summary-run-2-set-reference");
    expect(lastRead()?.args.reference?.path).toBe("/d/s2.h5");
    expect($("summary-run-1-reference")).toBeNull();
    expect(text("summary-run-2-reference")).toBe("Reference");
    expect(text("summary-run-1-tiles-area")).toContain("+99.00");

    // Not confirmed: no deltas, the backend's reason.
    refConfirmed = false;
    await click("summary-refresh");
    expect(text("summary-reference-unconfirmed")).toContain("run_id does not match the file");
    expect(text("summary-run-1-tiles-area")).not.toContain("+99.00");

    // Cleared: none again (held with the plan; a new plan clears it too).
    await click("summary-reference-clear");
    expect(planRef.reference).toBeNull();
    expect(lastRead()?.args.reference).toBeUndefined();
  });

  it("the overlay draws the reference and the chosen runs, display only", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    await runToFinalized(2, "/d/s2");
    const points = () => host.querySelectorAll("[data-testid=summary-chart] circle").length;
    expect(points()).toBe(19); // the inspected run (run 2): its overlay as sent (19 finite rows)
    await click("summary-run-1-overlay");
    expect(points()).toBe(38);
    await click("summary-run-1-set-reference");
    expect(points()).toBe(39); // run 1 is drawn once, as the reference (its overlay as the backend sent it: 20 points)
    expect(text("summary-overlay")).toContain("Reference · run 1 · S1");
    expect(text("summary-overlay-note")).toContain("Display only");
  });
});

describe("the decisions: each one explicit step", () => {
  it("Repeat sample re-queues the same sample_ref as the next run and sends nothing; Continue confirms the close and shows the Plan; Adjust navigates", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    const before = calls().length;
    await click("decision-repeat");
    expect(rowState(planRef.queue.state, "S1").phase).toBe("planned");
    expect(rowState(planRef.queue.state, "S1").history?.map((h) => h.runIndex)).toEqual([1]);
    expect(planRef.queue.state.nextRunIndex).toBe(2);
    expect(calls().length).toBe(before); // nothing sent
    expect(text("summary-run")).toContain("Run 1 · S1"); // the kept run is still summarised
    expect(($("decision-repeat") as HTMLButtonElement).disabled).toBe(true); // already re-queued

    await click("decision-adjust-align");
    expect(onAlign).toHaveBeenCalledTimes(1);
    await click("decision-adjust-plan");
    expect(onView).toHaveBeenLastCalledWith("Plan");
    expect(calls().length).toBe(before);
  });

  it("Continue on a finalized row confirms its close and shows the Plan", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await click("decision-continue");
    expect(rowState(planRef.queue.state, "S1").phase).toBe("closed");
    expect(onView).toHaveBeenLastCalledWith("Plan");
  });

  it("Open in Review opens the run's file; unavailable with the reason", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await click("decision-review");
    expect(openReview).toHaveBeenCalledWith("/d/s1.h5");
    reviewReason = "a run is in progress";
    await render({});
    expect(($("decision-review") as HTMLButtonElement).disabled).toBe(true);
    expect($("decision-review")?.title).toBe("a run is in progress");
  });

  it("a viewer: Repeat, Continue (confirming the close) and Open in Review disabled with the shared control block; reads, Adjust and the reference allowed", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await render({ controlHeld: false });
    expect(text("summary-control")).toContain("Another tab or client holds control");
    for (const id of ["decision-repeat", "decision-continue", "decision-review"]) expect(($(id) as HTMLButtonElement).disabled, id).toBe(true);
    for (const id of ["decision-adjust-align", "decision-adjust-plan", "summary-refresh", "summary-run-1-set-reference"]) expect(($(id) as HTMLButtonElement).disabled, id).toBe(false);
    const n = reads().length;
    await click("summary-refresh");
    expect(reads()).toHaveLength(n + 1);
    await click("decision-repeat");
    expect(rowState(planRef.queue.state, "S1").phase).toBe("finalized");
  });

  it("a single sample: summarised once its operation reported the end; Repeat is unavailable with the reason", async () => {
    await render({ status: status() });
    nextPath = "/d/one"; operationId = "op-1";
    await act(async () => { await planRef.start('{"x":1}', 1); });
    expect($("summary-empty")).not.toBeNull(); // started, its end not yet reported
    expect(reads()).toHaveLength(0);
    await render({ operations: { ends: new Map([["op-1", { state: OPERATION_STATES.Completed, message: "run completed" }]]), loss: 0 } });
    await flush();
    expect(reads().map((r) => r.args.paths)).toEqual([["/d/one.h5"]]);
    expect(text("summary-run")).toContain("Run summary · Run 1 · S1");
    expect(($("decision-repeat") as HTMLButtonElement).disabled).toBe(true);
    expect($("decision-repeat")?.title).toContain("a single-sample plan has one run");
  });
});

describe("Codex round 1: replies for what they were read for", () => {
  it("a late reply for an earlier reference never shows its deltas under a newer one", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    await runToFinalized(2, "/d/s2");
    deltaOverride = { ...G.runs[0].delta, area_um2_median: { value: 99, withheld: null } };
    holdReads = true;
    await click("summary-run-1-set-reference"); // read A (reference run 1), held
    await click("summary-run-2-set-reference"); // read B (reference run 2), held
    expect(heldReads).toHaveLength(2);
    // A answers late, after B was asked: ignored.
    await act(async () => { heldReads[0](); });
    await flush();
    expect(text("summary-run-2-tiles-area")).not.toContain("+99.00");
    expect(text("summary-reference-chip")).toBe("Reference · not read yet");
    await act(async () => { heldReads[1](); });
    await flush();
    expect(text("summary-reference-chip")).toBe("Reference · confirmed by the backend");
    expect(text("summary-run-1-tiles-area")).toContain("+99.00"); // run 1 against reference run 2
    expect(text("summary-run-2-reference")).toBe("Reference");
  });

  it("an unconfirmed reference is not drawn, even when the backend sent the file's numbers", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    await runToFinalized(2, "/d/s2");
    const points = () => host.querySelectorAll("[data-testid=summary-chart] circle").length;
    await click("summary-run-1-set-reference");
    expect(points()).toBe(19 + 20);
    refConfirmed = false;
    await click("summary-refresh");
    expect(points()).toBe(19);
    expect(text("summary-overlay")).toContain("the backend has not confirmed it");
  });

  it("a deferred reference alone is read again once the instrument turns idle", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    await runToFinalized(2, "/d/s2");
    await act(async () => planRef.queue.confirmClosed());
    await click("summary-run-1-set-reference");
    // Another client's run: the runs are served from the cache, the reference is deferred.
    refRunStatus = "deferred";
    await render({ status: activeAt("3", "/d/other.h5") });
    await click("summary-refresh");
    const n = reads().length;
    expect(text("summary-reference-unconfirmed")).toContain("computed when the instrument is idle");
    refRunStatus = "computed";
    await render({ status: closedAt("3", "/d/other.h5") });
    await flush();
    expect(reads()).toHaveLength(n + 1);
    expect(text("summary-reference-chip")).toBe("Reference · confirmed by the backend");
  });
});

describe("Codex round 2: failures and a Summary that is gone", () => {
  it("a read that fails for new paths says why", async () => {
    await queuePlan();
    failReads = true;
    await runToFinalized(1, "/d/s1");
    expect(text("summary-read-error")).toContain("TRANSPORT_LOST: no reply");
    failReads = false;
    await click("summary-refresh");
    expect($("summary-read-error")).toBeNull();
    expect(text("summary-tiles-valid")).toContain("21");
  });

  it("a late answer after the Summary was left reads nothing more", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    // Another client's run; the Summary's read is held.
    holdReads = true;
    await render({ status: activeAt("2", "/d/other.h5") });
    await click("summary-refresh");
    // The instrument turns idle, then the Summary is left before the answer (a deferral) arrives.
    await render({ status: closedAt("2", "/d/other.h5") });
    await render({ show: false });
    const n = reads().length;
    variants["/d/s1.h5"] = "deferred";
    holdReads = false;
    for (const release of heldReads.splice(0)) await act(async () => { release(); });
    await flush();
    expect(reads()).toHaveLength(n);
  });
});
