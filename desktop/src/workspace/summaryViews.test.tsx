// @vitest-environment jsdom
// O5 Run summary and O6 Quick look (#716 A7b; separate sub-views aligned with the Penpot boards in
// A7c) over a mocked transport and the real plan controller (usePlan, the queue driven by its Start,
// its stop and the statuses): each sub-view reads only on its transitions and Refresh (never polled),
// deferred / not available / error / legacy / no E / null shown as such, the backend's deltas only (a
// withheld one with its reason, `differs` flagged, never recomputed), the reference chosen and
// replaced, the overlay, O5's lifecycle checklist from observed status (unknown when unobserved), its
// duration and stop reason, End queue, the calibration-run block (Apply leads to Prepare, nothing is
// applied from O5), O6's placeholder tiles in plan order and its "Refine the experiment" decisions, a
// viewer's gating, and the active run's "numbers at close".
import { act, StrictMode, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import golden from "../../../crates/mib-bridge/contract/fixtures/run-quick-look-v1.json";
import { EXPERIMENT_STATES, OPERATION_STATES } from "../bridgeContract";
import type { ExperimentStatus } from "../eventAdapter";
import { emptyRow } from "./planModel";
import type { MethodPurpose } from "../calibrationRecords";
import { calList, calRecord } from "./calibrationFixtures";
import { NO_OPERATIONS, rowState, type OperationTrack } from "./queueModel";
import { QuickLookView } from "./QuickLookView";
import { RunSummaryView } from "./RunSummaryView";
import { usePlan, type PlanController, type PlanStartResult, type StopReply } from "./usePlan";

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
const calReads = () => calls().filter((c) => c.cmd === "calibration_records_list");
/** The calibration records list the backend answers (null: the read fails). */
let calListReply: unknown = calList([]);
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
/** The applied method's purpose the Start's readiness check reports (undefined: an older backend). */
let purpose: MethodPurpose | null | undefined = "measurement";
const runStart = vi.fn(async (): Promise<PlanStartResult> => ({ kind: "started", path: nextPath, notice: "", ...(operationId ? { operationId } : {}),
  ...(purpose !== undefined ? { methodPurpose: purpose } : {}) }));
const stop = vi.fn(async (_gen: string): Promise<StopReply> => ({ ok: true, message: "stopping" }));
const onView = vi.fn();
const onAlign = vi.fn();
const onPrepare = vi.fn();
const openReview = vi.fn();
let reviewReason = "";
let planRef: PlanController;

type Sub = "Run summary" | "Quick look";
interface Setup { status: ExperimentStatus | null; controlHeld?: boolean; show?: boolean; operations?: OperationTrack; sub?: Sub }
function Harness({ status: s, controlHeld = true, show = true, operations = NO_OPERATIONS, sub = "Quick look" }: Setup) {
  const plan = usePlan(runStart, { status: s, controlHeld, operations, stop });
  planRef = plan;
  // Held by the Experiment stage in the app: the run both sub-views inspect.
  const [inspected, setInspected] = useState<string | null>(null);
  const active = !!s?.valid && !s.terminal && (s.state === EXPERIMENT_STATES.Starting || s.state === EXPERIMENT_STATES.Active || s.state === EXPERIMENT_STATES.Stopping);
  const review = { reason: reviewReason, open: openReview };
  if (!show) return null;
  return sub === "Run summary"
    ? <RunSummaryView tabs={null} plan={plan} experimentActive={active} onView={onView} inspected={inspected} onInspect={setInspected} onPrepare={onPrepare} review={review} />
    : <QuickLookView tabs={null} plan={plan} experimentActive={active} onView={onView} onAlign={onAlign} inspected={inspected} onInspect={setInspected} review={review} />;
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
    if (c === "calibration_records_list") {
      if (calListReply === null) throw new Error("TRANSPORT_LOST: no reply");
      return clone(calListReply);
    }
    throw new Error(`unexpected command ${c}`);
  });
  variants = {}; deltaOverride = null; operationId = undefined; refRunStatus = "computed"; holdReads = false; heldReads.length = 0; failReads = false; refConfirmed = true; reviewReason = ""; current = { status: null };
  purpose = "measurement"; calListReply = calList([]);
  runStart.mockClear(); stop.mockClear(); onView.mockClear(); onAlign.mockClear(); onPrepare.mockClear(); openReview.mockClear();
  host = document.createElement("div");
  document.body.appendChild(host);
  root = createRoot(host);
});
afterEach(() => {
  act(() => root.unmount());
  host.remove();
});

/** Run the queue's current row and stop it with Next sample (stop accepted, a Stopping status seen). */
async function runAndStop(gen: number, path: string, times?: { start: string; end: string }) {
  nextPath = path;
  await act(async () => { await planRef.queue.startCurrent(); });
  await render({ status: activeAt(String(gen), `${path}.h5`) });
  await act(async () => { await planRef.queue.stopCurrent(); });
  await render({ status: status({ state: EXPERIMENT_STATES.Stopping, flushing: true, start_generation: String(gen), output_path: `${path}.h5`, valid_saved: "120" }) });
  await render({ status: status({ state: EXPERIMENT_STATES.Idle, terminal: true, finalization_ok: true, start_generation: String(gen), output_path: `${path}.h5`,
    ...(times ? { start_time_ns: times.start, end_time_ns: times.end } : {}) }) });
  await flush();
}
const disabled = (id: string) => ($(id) as HTMLButtonElement | null)?.disabled;
const stepState = (key: string) => $(`o5-step-${key}`)?.getAttribute("data-state");

describe("the sub-views read apart: only on their transitions and Refresh, never polled", () => {
  it("Run summary reads its one run; Quick look the plan's closed runs; each on showing, on a close, on Refresh; polls, the confirmation and a run read nothing", async () => {
    await render({ sub: "Run summary" });
    await queuePlan();
    expect($("summary-empty")).not.toBeNull();
    expect(reads()).toHaveLength(0);

    await runToFinalized(1, "/d/s1");
    expect(reads().map((r) => r.args.paths)).toEqual([["/d/s1.h5"]]);
    expect(reads()[0].args.reference).toBeUndefined(); // O5 has no deltas: no reference is read
    expect(text("o5-outcome")).toContain("Run 1 · S1");
    expect(text("summary-awaiting")).toBe("Awaiting your close confirmation");
    for (let i = 0; i < 5; i++) await render({ status: closedAt("1", "/d/s1.h5") });
    await act(async () => planRef.queue.confirmClosed());
    await flush();
    expect(reads()).toHaveLength(1);

    // S2 runs: nothing is read while it runs; its close shows (and reads) S2, the newest.
    nextPath = "/d/s2";
    await act(async () => { await planRef.queue.startCurrent(); });
    for (let i = 0; i < 5; i++) await render({ status: activeAt("2", "/d/s2.h5") });
    expect(reads()).toHaveLength(1);
    await render({ status: closedAt("2", "/d/s2.h5") });
    await flush();
    expect(reads().map((r) => r.args.paths)).toEqual([["/d/s1.h5"], ["/d/s2.h5"]]);
    await click("summary-refresh");
    expect(reads()).toHaveLength(3);

    // Quick look: one read of both runs on showing; back to the Run summary: one read of its run.
    await render({ sub: "Quick look" });
    await flush();
    expect(reads()).toHaveLength(4);
    expect(reads()[3].args.paths).toEqual(["/d/s1.h5", "/d/s2.h5"]);
    for (let i = 0; i < 3; i++) await render({ status: closedAt("2", "/d/s2.h5") });
    expect(reads()).toHaveLength(4);
    await click("summary-run-1-set-reference");
    expect(reads()).toHaveLength(5);
    expect(reads()[4].args.reference).toEqual({ path: "/d/s1.h5", run_id: "run-/d/s1.h5", start_generation: "1" });
    await render({ sub: "Run summary" });
    await flush();
    expect(reads()).toHaveLength(6);
    expect(reads()[5].args.paths).toEqual(["/d/s2.h5"]);
    // Choosing another run to show reads that run.
    await act(async () => { const sel = $("o5-run-select") as HTMLSelectElement; sel.value = "/d/s1.h5"; sel.dispatchEvent(new Event("change", { bubbles: true })); });
    await flush();
    expect(reads()[6].args.paths).toEqual(["/d/s1.h5"]);
    expect(text("o5-outcome")).toContain("Run 1 · S1");
    // A measurement method's runs read no calibration records; nothing but reads was sent.
    expect(calReads()).toHaveLength(0);
    expect(calls().every((c) => c.cmd === "run_quick_look")).toBe(true);
  });

  it("Quick look's Run summary button shows that run in the Run summary", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    await runToFinalized(2, "/d/s2");
    await click("summary-run-1-summary");
    expect(onView).toHaveBeenLastCalledWith("Run summary");
    await render({ sub: "Run summary" });
    await flush();
    expect(text("o5-outcome")).toContain("Run 1 · S1");
    expect(reads()[reads().length - 1].args.paths).toEqual(["/d/s1.h5"]);
  });

  it("a deferred run reads as computed when idle and is read again once the instrument turns idle, not before", async () => {
    await render({ sub: "Run summary" });
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    await render({ show: false });
    // Another client's run is active: a fresh view's read is answered deferred (no cache hit).
    variants["/d/s1.h5"] = "deferred";
    await render({ status: activeAt("2", "/d/other.h5"), show: true });
    await flush();
    const n = reads().length;
    expect(text("summary-deferred")).toContain("Quick look computed when the instrument is idle");
    expect(text("summary-status")).toBe("Deferred · computed when idle");
    for (let i = 0; i < 5; i++) await render({ status: activeAt("2", "/d/other.h5") });
    expect(reads()).toHaveLength(n);
    variants["/d/s1.h5"] = "v3";
    await render({ status: closedAt("2", "/d/other.h5") });
    await flush();
    expect(reads()).toHaveLength(n + 1);
    expect($("summary-deferred")).toBeNull();
    expect(text("summary-counts")).toContain("21");
  });
});

describe("O5 Outcome: the lifecycle from what this page observed", () => {
  it("a run that ended by itself: Active and its first frames seen, Stopping not observed (unknown, not ticked), closed once confirmed", async () => {
    await render({ sub: "Run summary" });
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    expect(stepState("requested")).toBe("done");
    expect(stepState("acknowledged")).toBe("done");
    expect(stepState("active")).toBe("done");
    expect(stepState("stopping")).toBe("unknown");
    expect(text("o5-step-stopping")).toContain("unknown · not observed");
    expect(stepState("drain")).toBe("done");
    expect(stepState("closed")).toBe("current");
    expect(text("o5-step-closed")).toContain("awaiting your close confirmation");
    expect(text("o5-stop-reason")).toBe("Stop reason: not observed here");
    expect(text("o5-duration")).toBe("Not recorded"); // the terminal status recorded no start or end
    await act(async () => planRef.queue.confirmClosed());
    await flush();
    expect(stepState("closed")).toBe("done");
    // The counts and provenance are the file's, the snapshot its display-only subsample; no medians here.
    expect(text("summary-counts")).toContain("Admitted");
    expect(text("summary-provenance")).toContain("run-/d/s1.h5");
    expect(text("o5-experiment-run")).toContain("run 1 · sample S1");
    expect(text("o5-file")).toMatch(/^s1\.h5 · 55.096 bytes$/);
    expect(host.querySelectorAll("[data-testid=o5-snapshot-chart] circle").length).toBe(19);
    expect(text("o5-snapshot")).toContain("display only, the Review app does the analysis");
    expect(text("o5-next")).toContain("statistics: Quick look (descriptive) and the Review app");
    expect(host.textContent).not.toContain("no statistics on the instrument");
    expect(host.querySelector("[data-testid$=-tiles-area], [data-testid=summary-tiles]")).toBeNull();
    expect(host.textContent).not.toMatch(/Median area|Median deformability|Median E/);
  });

  it("a run never seen Active, with no frame counter: acknowledged and Active unknown", async () => {
    await render({ sub: "Run summary" });
    await queuePlan();
    nextPath = "/d/s1";
    await act(async () => { await planRef.queue.startCurrent(); });
    await render({ status: status({ state: EXPERIMENT_STATES.Starting, start_generation: "1", output_path: "/d/s1.h5" }) });
    await render({ status: closedAt("1", "/d/s1.h5") });
    await flush();
    expect(stepState("acknowledged")).toBe("unknown");
    expect(stepState("active")).toBe("unknown");
    expect(host.querySelectorAll("[data-testid=o5-lifecycle] [data-state=done]").length).toBe(2); // requested, drain
  });

  it("Next sample's accepted stop: Stopping done, the stop reason, and the duration from the recorded start and end", async () => {
    await render({ sub: "Run summary" });
    await queuePlan();
    await runAndStop(1, "/d/s1", { start: "1000000000000", end: "1261400000000" });
    expect(stop).toHaveBeenCalledWith("1");
    expect(stepState("stopping")).toBe("done");
    expect(text("o5-step-stopping")).toContain("accepted this page's Next sample");
    expect(text("o5-stop-reason")).toBe("Stop reason: Next sample · this page's stop, accepted by the backend");
    expect(text("o5-duration")).toBe("4 min 21 s");
  });

  it("a single sample: followed by its operation only, so the status steps are unknown; closed by the backend", async () => {
    await render({ status: status(), sub: "Run summary" });
    nextPath = "/d/one"; operationId = "op-1";
    await act(async () => { await planRef.start('{"x":1}', 1); });
    expect($("summary-empty")).not.toBeNull();
    expect(reads()).toHaveLength(0);
    await render({ operations: { ends: new Map([["op-1", { state: OPERATION_STATES.Completed, message: "run completed" }]]), loss: 0 } });
    await flush();
    expect(reads().map((r) => r.args.paths)).toEqual([["/d/one.h5"]]);
    expect(["acknowledged", "active", "stopping"].map(stepState)).toEqual(["unknown", "unknown", "unknown"]);
    expect(["requested", "drain", "closed"].map(stepState)).toEqual(["done", "done", "done"]);
    expect(text("o5-duration")).toBe("Not recorded");
    expect(disabled("decision-repeat")).toBe(true);
    expect($("decision-repeat")?.title).toContain("a single-sample plan has one run");
    expect(disabled("decision-end-queue")).toBe(true);
    expect($("decision-end-queue")?.title).toBe("a single-sample plan has no queue to end");
    expect(text("o5-next-name")).toContain("A single-sample plan");
  });
});

describe("O5 Next: Next sample, Repeat this sample, End queue", () => {
  it("names the next sample; Next sample confirms the close and shows the Plan; Repeat re-queues and sends nothing", async () => {
    await render({ sub: "Run summary" });
    await queuePlan();
    await act(async () => planRef.edit((d) => ({ ...d, rows: d.rows.map((r) => (r.ref === "S2" ? { ...r, sample: "Cytochalasin D" } : r)) })));
    await runToFinalized(1, "/d/s1");
    expect(text("o5-next-name")).toBe("Next · S2 · Cytochalasin D · Drug");
    const before = calls().length;
    await click("decision-repeat");
    expect(rowState(planRef.queue.state, "S1").phase).toBe("planned");
    expect(rowState(planRef.queue.state, "S1").history?.map((h) => h.runIndex)).toEqual([1]);
    expect(planRef.queue.state.nextRunIndex).toBe(2);
    expect(calls().length).toBe(before);
    expect(text("o5-outcome")).toContain("Run 1 · S1"); // the kept run is still summarised
    expect(disabled("decision-repeat")).toBe(true);
    expect(text("o5-next-name")).toBe("Next · S1 · Vehicle"); // the repeat is next
  });

  it("Next sample on a finalized row confirms its close and shows the Plan (the toolbar's too)", async () => {
    await render({ sub: "Run summary" });
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    expect($("o5-next-sample")?.title).toContain("Confirms S1's close");
    await click("decision-next");
    expect(rowState(planRef.queue.state, "S1").phase).toBe("closed");
    expect(onView).toHaveBeenLastCalledWith("Plan");
  });

  it("End queue: after a finalized run it confirms the close and ends the queue, sending nothing; refused while a run runs, at the last sample and for a viewer", async () => {
    await render({ sub: "Run summary" });
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    expect(disabled("decision-end-queue")).toBe(false);
    expect($("decision-end-queue")?.title).toContain("Confirms S1's close, then ends the queue");
    const before = calls().length;
    await click("o5-end-queue");
    expect(rowState(planRef.queue.state, "S1").phase).toBe("closed");
    expect(planRef.queue.state.ended).toBe(true);
    expect(calls().length).toBe(before);
    expect(disabled("decision-end-queue")).toBe(true);
    expect($("decision-end-queue")?.title).toBe("the queue was already ended");
  });

  it("End queue between samples ends the queue; while a sample runs it is unavailable with the reason", async () => {
    await render({ sub: "Run summary" });
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    await flush();
    expect(disabled("decision-end-queue")).toBe(false);
    expect($("decision-end-queue")?.title).toContain("Ends the queue before S2");
    nextPath = "/d/s2";
    await act(async () => { await planRef.queue.startCurrent(); });
    await render({ status: activeAt("2", "/d/s2.h5") });
    expect(disabled("decision-end-queue")).toBe(true);
    expect($("decision-end-queue")?.title).toContain("S2 is running: end the queue once its run is closed");
    expect(disabled("decision-next")).toBe(true);
    await render({ status: closedAt("2", "/d/s2.h5") });
    await act(async () => planRef.queue.confirmClosed());
    await runToFinalized(3, "/d/s3");
    expect(disabled("decision-end-queue")).toBe(true);
    expect($("decision-end-queue")?.title).toContain("S3 is the last sample");
    expect(text("o5-next-name")).toBe("No sample is left to run in this plan.");
  });

  it("a viewer: Next sample (confirming), Repeat and End queue disabled with the shared control block; reads allowed", async () => {
    await render({ sub: "Run summary" });
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await render({ controlHeld: false });
    expect(text("summary-control")).toContain("Another tab or client holds control");
    for (const id of ["decision-next", "decision-repeat", "decision-end-queue", "o5-next-sample", "o5-end-queue"]) expect(disabled(id), id).toBe(true);
    expect(disabled("summary-refresh")).toBe(false);
    const n = reads().length;
    await click("summary-refresh");
    expect(reads()).toHaveLength(n + 1);
    await click("decision-end-queue");
    expect(planRef.queue.state.ended).toBe(false);
  });
});

describe("O5 calibration run: the record the backend derived, never applied from here", () => {
  const record = (over = {}) => calRecord("cal-0042", "proposed", { source: { run_id: "run-/d/s1.h5", file: "/d/s1.h5", file_sha256: "ab", start_generation: 1 }, ...over });

  it("shows the record matched by its source run_id: current → proposed, the CV and the gates; Apply and Dismiss lead to Prepare and send nothing", async () => {
    purpose = "calibration";
    calListReply = calList([calRecord("cal-0041", "applied"), record()]);
    await render({ sub: "Run summary" });
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    expect(calReads()).toHaveLength(1);
    expect(text("o5-calibration-record")).toBe("cal-0042");
    expect(text("o5-calibration-factor")).toBe("0.5 → 0.52 µm/px · current → proposed");
    expect(text("o5-calibration-cv")).toBe("3.1 % · limit 10 %");
    expect(text("o5-calibration-gates")).toContain("Valid objects: met · 812, limit 200");
    expect(text("o5-calibration-gates")).toContain("Diameter CV: met · 3.1, limit 10");
    const before = calls().length;
    await click("o5-calibration-apply");
    expect(calls().length).toBe(before);
    expect(onPrepare).toHaveBeenCalledWith("cal-0042");
    await click("o5-calibration-dismiss");
    expect(text("o5-calibration-dismissed")).toContain("stays proposed in Prepare's list");
    await click("o5-calibration-show");
    expect(onPrepare).toHaveBeenCalledTimes(2);
    expect(calls().length).toBe(before);
    expect(calls().some((c) => c.cmd === "calibration_apply")).toBe(false);
    // Read on showing and on Refresh only, never on a status poll.
    for (let i = 0; i < 3; i++) await render({ status: closedAt("1", "/d/s1.h5") });
    expect(calReads()).toHaveLength(1);
    await click("summary-refresh");
    expect(calReads()).toHaveLength(2);
    // Codex round 1: a dismissed proposal applied meanwhile (another client; read on Refresh) shows again.
    calListReply = calList([record({ status: "applied", version: 2, sequence: 2, apply: { previous_factor: 0.5, new_factor: 0.52 } })]);
    await click("summary-refresh");
    expect($("o5-calibration-dismissed")).toBeNull();
    expect(text("o5-calibration-status")).toBe("Applied · not in effect");
  });

  it("a calibration run without a record says so, with the reason", async () => {
    purpose = "calibration";
    calListReply = calList([calRecord("cal-0041", "applied")]); // its source run is another
    await render({ sub: "Run summary" });
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    expect(text("o5-calibration-none")).toContain("No calibration record was derived for this run");
    expect(text("o5-calibration-none")).toContain("none of the backend's records names this run (run id run-/d/s1.h5) as its source");
    // Codex round 1: a failed read is "not known", never "none was derived".
    calListReply = null;
    await click("summary-refresh");
    expect($("o5-calibration-none")).toBeNull();
    expect(text("o5-calibration-unknown")).toContain("Calibration record not known");
    expect(text("o5-calibration-unknown")).toContain("the calibration records could not be read (TRANSPORT_LOST: no reply)");
    calListReply = calList([], { store: { revision: 1, max_versions: 2048, error: "corrupt" } });
    await click("summary-refresh");
    expect(text("o5-calibration-unknown")).toContain("store cannot be read");
  });

  it("an older backend (purpose not reported): the list is read and a matching record shows; a measurement run reads none", async () => {
    purpose = undefined;
    calListReply = calList([record({ status: "insufficient" })]);
    await render({ sub: "Run summary" });
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    expect(calReads()).toHaveLength(1);
    expect(text("o5-calibration-status")).toBe("Insufficient");
    expect(text("o5-calibration-factor")).toBe("0.5 → 0.52 µm/px · current → proposed · never applicable");
    expect($("o5-calibration-apply")).toBeNull(); // never applicable: only Show in Prepare
    expect($("o5-calibration-show")).not.toBeNull();
    await act(async () => planRef.queue.confirmClosed());
    purpose = "measurement";
    await runToFinalized(2, "/d/s2");
    expect(calReads()).toHaveLength(1);
    expect($("o5-calibration")).toBeNull();
    expect($("o5-calibration-none")).toBeNull();
  });
});

describe("honesty: the backend's states and numbers as sent", () => {
  it("not available, an error, legacy, no E and a recorded 0", async () => {
    await queuePlan();
    variants = { "/d/s1.h5": "raw" };
    await runToFinalized(1, "/d/s1");
    expect(text("summary-run-1-not-available")).toContain("Not available until exported to HDF5");
    expect(text("summary-run-1-not-available")).toContain("not available until exported to HDF5");
    expect($("summary-run-1-tiles")).toBeNull();
    await act(async () => planRef.queue.confirmClosed());

    variants["/d/s2.h5"] = "legacy";
    await runToFinalized(2, "/d/s2");
    expect(text("summary-run-2")).toContain("Legacy file · no run snapshot");
    expect(text("summary-run-2-tiles-area")).toContain("px→µm not recorded");
    expect(text("summary-run-2-tiles-emodulus")).toContain("Not recorded");
    expect($("flag-legacy")?.getAttribute("data-tone")).toBe("unknown"); // grey, no verdict colour
    await render({ sub: "Run summary" });
    await flush();
    expect(text("summary-provenance")).toContain("Not recorded"); // no run id, no snapshot
    await render({ sub: "Quick look" });
    await flush();
    await act(async () => planRef.queue.confirmClosed());

    variants["/d/s3.h5"] = "noLut";
    await runToFinalized(3, "/d/s3");
    expect(text("summary-run-3-tiles-emodulus")).toContain("No E-LUT loaded");

    variants["/d/s3.h5"] = "zero";
    await click("summary-refresh");
    expect(text("summary-run-3-tiles-valid")).toMatch(/^Valid rows0$/);
    expect(text("summary-run-3-tiles-target")).toContain("0.0");
    expect(text("summary-run-3-tiles-target")).toContain("% in declared target group");

    variants["/d/s3.h5"] = "error";
    await click("summary-refresh");
    expect(text("summary-run-3-error")).toContain("No such file");
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

    await click("summary-run-2-set-reference");
    expect(lastRead()?.args.reference?.path).toBe("/d/s2.h5");
    expect($("summary-run-1-reference")).toBeNull();
    expect(text("summary-run-2-reference")).toBe("Reference");
    expect(text("summary-run-1-tiles-area")).toContain("+99.00");

    refConfirmed = false;
    await click("summary-refresh");
    expect(text("summary-reference-unconfirmed")).toContain("run_id does not match the file");
    expect(text("summary-run-1-tiles-area")).not.toContain("+99.00");

    await click("summary-reference-clear");
    expect(planRef.reference).toBeNull();
    expect(lastRead()?.args.reference).toBeUndefined();
  });

  it("the overlay draws the reference and the chosen runs as a point subsample, display only", async () => {
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
    expect(text("summary-overlay")).toContain("Point subsample from the backend");
    expect(text("summary-overlay")).not.toMatch(/contour lines|KDE/);
    expect(text("summary-overlay-note")).toContain("Display only");
  });
});

describe("O6 experiment board: closed runs and placeholders in plan order", () => {
  const order = () => [...host.querySelectorAll("[data-testid=summary-runs] > [data-testid]")].map((e) => e.getAttribute("data-testid"));

  it("queued rows are grey placeholders with no numbers, beside the closed runs, in plan order; the run in progress takes its row's place", async () => {
    await queuePlan();
    expect(order()).toEqual(["summary-queued-S1", "summary-queued-S2", "summary-queued-S3"]);
    expect(reads()).toHaveLength(0);
    await runToFinalized(1, "/d/s1");
    expect(order()).toEqual(["summary-run-1", "summary-queued-S2", "summary-queued-S3"]);
    expect(text("summary-queued-S2-chip")).toBe("Queued");
    expect($("summary-queued-S2-chip")?.getAttribute("data-tone")).toBe("neutral");
    expect($("summary-queued-S2")?.querySelector("[data-testid$=-tiles]")).toBeNull();
    expect(text("summary-queued-S2")).toContain("No numbers until its run is closed");
    // The tile's mini scatter is the backend's overlay points (display only).
    expect($("summary-run-1-thumb")?.querySelectorAll("circle").length).toBe(19);

    await act(async () => planRef.queue.confirmClosed());
    nextPath = "/d/s2";
    await act(async () => { await planRef.queue.startCurrent(); });
    await render({ status: activeAt("2", "/d/s2.h5") });
    expect(order()).toEqual(["summary-run-1", "summary-active-run", "summary-queued-S3"]);
    expect(text("summary-active-run")).toContain("numbers at close");
    await render({ status: closedAt("2", "/d/s2.h5") });
    await flush();
    // Repeat S1: its run stays, a "Repeat queued" placeholder follows it, before S2.
    await click("summary-run-1-inspect");
    await act(async () => planRef.queue.confirmClosed());
    await flush();
    await click("decision-repeat");
    expect(order()).toEqual(["summary-run-1", "summary-queued-S1", "summary-run-2", "summary-queued-S3"]);
    expect(text("summary-queued-S1-chip")).toBe("Repeat queued");
    // Ended: the rows not run read "Not run".
    await act(async () => planRef.queue.end());
    await flush();
    expect(text("summary-queued-S3-chip")).toBe("Not run");
  });
});

describe("O6 decisions: Refine the experiment, beside the overlay", () => {
  it("Continue · load the next sample, Repeat, Adjust next sample…, Open in Review in the card; Next sample and Open in Review in the toolbar", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    const refine = $("o6-refine")!;
    for (const id of ["decision-continue", "decision-repeat", "decision-adjust", "decision-adjust-align", "decision-review"])
      expect(refine.querySelector(`[data-testid=${id}]`), id).not.toBeNull();
    expect(text("decision-continue")).toBe("Continue · load S2");
    expect(text("decision-repeat")).toBe("Repeat S1");
    expect(text("o6-refine")).toContain("Decisions, not conclusions");
    expect($("o6-next-sample")).not.toBeNull();
    expect($("o6-open-review")).not.toBeNull();
    // O5's decisions are elsewhere: its Next card has no Open in Review.
    expect($("decision-end-queue")).toBeNull();

    await click("decision-adjust-align");
    expect(onAlign).toHaveBeenCalledTimes(1);
    await click("decision-adjust");
    expect(onView).toHaveBeenLastCalledWith("Plan");
    await click("decision-review");
    expect(openReview).toHaveBeenCalledWith("/d/s1.h5");
    await click("o6-open-review");
    expect(openReview).toHaveBeenCalledTimes(2);
    reviewReason = "a run is in progress";
    await render({});
    expect(disabled("decision-review")).toBe(true);
    expect($("decision-review")?.title).toBe("a run is in progress");
    await click("decision-continue");
    expect(rowState(planRef.queue.state, "S1").phase).toBe("closed");
    expect(onView).toHaveBeenLastCalledWith("Plan");
  });

  it("a viewer: Repeat, Continue (confirming the close) and Open in Review disabled with the shared control block; reads, Adjust and the reference allowed", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await render({ controlHeld: false });
    expect(text("summary-control")).toContain("Another tab or client holds control");
    for (const id of ["decision-repeat", "decision-continue", "decision-review", "o6-next-sample", "o6-open-review"]) expect(disabled(id), id).toBe(true);
    for (const id of ["decision-adjust", "decision-adjust-align", "summary-refresh", "summary-run-1-set-reference", "summary-run-1-summary"]) expect(disabled(id), id).toBe(false);
    const n = reads().length;
    await click("summary-refresh");
    expect(reads()).toHaveLength(n + 1);
    await click("decision-repeat");
    expect(rowState(planRef.queue.state, "S1").phase).toBe("finalized");
  });
});

describe("no assessment of a run anywhere (ruling 6)", () => {
  const ASSESSMENT = /looks different|\bclean\b|\brecommend|\bsuggest|\bsignifican|\babnormal|\bas expected\b|\blooks (good|fine|normal|ok)\b|\bbetter\b|\bworse\b|\bhealthy\b/i;
  const sources = import.meta.glob(["./RunSummaryView.tsx", "./QuickLookView.tsx", "./summaryParts.tsx", "./runSummaryModel.ts", "./quickLookModel.ts"],
    { query: "?raw", import: "default", eager: true }) as Record<string, string>;
  it("the sources carry no assessment wording (the board's \"Run 03 looks different from the vehicle and clean\" is not used)", () => {
    expect(Object.keys(sources)).toHaveLength(5);
    // The words shown (string and template literals, JSX text); comments that state the rule are not shown.
    const shown = (src: string) => src.replace(/\/\*[\s\S]*?\*\//g, "").replace(/(^|[^:"'`\\])\/\/.*$/gm, "$1");
    for (const [file, src] of Object.entries(sources)) expect(shown(src).match(ASSESSMENT), file).toBeNull();
    expect(ASSESSMENT.test("Run 03 looks different from the vehicle and clean (0 dropped, n = 100 000).")).toBe(true);
  });

  it("neither rendered sub-view says one", async () => {
    await queuePlan();
    await runToFinalized(1, "/d/s1");
    await act(async () => planRef.queue.confirmClosed());
    await runToFinalized(2, "/d/s2");
    await click("summary-run-1-set-reference");
    expect(host.textContent).not.toMatch(ASSESSMENT);
    await render({ sub: "Run summary" });
    await flush();
    expect(host.textContent).not.toMatch(ASSESSMENT);
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

describe("Codex round 2: failures and a sub-view that is gone", () => {
  it("a read that fails for new paths says why", async () => {
    await queuePlan();
    failReads = true;
    await runToFinalized(1, "/d/s1");
    expect(text("summary-read-error")).toContain("TRANSPORT_LOST: no reply");
    failReads = false;
    await click("summary-refresh");
    expect($("summary-read-error")).toBeNull();
    expect(text("summary-run-1-tiles-valid")).toContain("21");
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
