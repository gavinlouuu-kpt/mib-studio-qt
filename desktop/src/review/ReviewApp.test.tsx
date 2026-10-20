// @vitest-environment jsdom
// The YOFO Review shell on the @mib/ui kit (#717 B1, #269): the stage tabs,
// R1 Open's folder list and open flow, the honest Overview / Export notices,
// and the launch-path and OS open-request behaviour, over a mocked Tauri IPC
// (`window.__TAURI_INTERNALS__.invoke`). Selecting a stage never reaches the
// backend.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import ReviewApp from "./ReviewApp";
import { LIBRARY_FOLDER_KEY } from "./open/OpenView";
import type { ReviewRunAnnotation, ReviewRunIdentity, ReviewRunProbe } from "./reviewBridge";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

type Call = { cmd: string; args: Record<string, unknown> };
const calls: Call[] = [];
let opened = "";
let launchPath = "";
let pendingOpen = "";
let refuse = new Set<string>();
let folderPick: string | null = "/data/lib";
// Folders whose listing waits until the test resolves it (out of order on purpose).
let deferDirs = new Set<string>();
const pendingLists = new Map<string, Array<(names: string[]) => void>>();
const folderFiles: Record<string, string[]> = {
  "/data/a": ["a1.h5", "a2.h5"],
  "/data/b": ["b1.h5"],
  "/data/c": ["c1.h5", "c2.h5", "c3.h5"],
  "/data/d": ["d1.h5"],
  // R1 grouping: file names in another order than the declared run indices.
  "/data/exp": ["alpha.h5", "beta.h5", "gamma.h5", "older.h5", "nox.h5", "unread.h5", "old.h5", "broken.h5"],
  "/data/big": Array.from({ length: 300 }, (_, i) => `f${String(i + 1).padStart(3, "0")}.h5`),
};

// What each file's header says (review_probe_runs); a path without an entry
// is a legacy file. Answers are taken when the probe is called.
type ProbeFixture = Partial<Omit<ReviewRunProbe, "identity">> & { identity?: Partial<ReviewRunIdentity> };
const absentIdentity = (): ReviewRunIdentity => ({
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
const planned = (sample: string | null, condition: string | null): ReviewRunAnnotation => ({
  schema_version: 1,
  sample,
  condition,
  experimental_unit: "donor-4",
  concentration: null,
  channel: null,
  medium: "PBS",
  temperature_c: null,
  notes: null,
  free: [],
});
const EXP_A = "4f1c2a9e-0b3d-4c5e-8f7a-1b2c3d4e5f60";
const EXP_B = "9a8b7c6d-5e4f-4a3b-9c2d-1e0f9a8b7c6d";
const OCT14 = "1760400000000000000"; // 2026-10-14 (UTC)
const OCT09 = "1759968000000000000"; // 2026-10-09 (UTC)
function v3(experiment: string, runIndex: number, sampleRef: string, condition: string): Partial<ReviewRunIdentity> {
  return {
    has_snapshot: true,
    snapshot_version: 3,
    run_id: `run-${experiment.slice(0, 4)}-${runIndex}`,
    experiment_id: experiment,
    run_index: runIndex,
    sample_ref: sampleRef,
    calibration_record_id: "cal-0042",
    operator: { id: null, name: "Gavin" },
    annotation: planned(`Donor 4 · ${condition}`, condition),
  };
}
const COMPLETE = 0;
const LOSS = 2;
function defaultProbes(): Record<string, ProbeFixture> {
  const v3run = (identity: Partial<ReviewRunIdentity>, start: string, completion: number): ProbeFixture => ({
    identity,
    start_time_ns: start,
    total_valid: "100",
    total_invalid: "2",
    accounting_completion: completion,
    core_version: "2.1.0",
  });
  return {
    "/data/lib/run_2.h5": v3run(v3(EXP_A, 2, "S-02", "Vehicle"), OCT14, COMPLETE),
    "/data/lib/run_10.h5": v3run(v3(EXP_A, 10, "S-10", "CytoD 0.1"), OCT14, COMPLETE),
    "/data/exp/alpha.h5": v3run(v3(EXP_A, 3, "S-03", "CytoD 1"), OCT14, LOSS),
    "/data/exp/beta.h5": v3run(v3(EXP_A, 1, "S-01", "Vehicle"), OCT14, COMPLETE),
    "/data/exp/gamma.h5": v3run(v3(EXP_A, 2, "S-02", "CytoD 0.1"), OCT14, COMPLETE),
    "/data/exp/older.h5": v3run(v3(EXP_B, 1, "S-01", "Vehicle"), OCT09, COMPLETE),
    "/data/exp/nox.h5": { identity: { has_snapshot: true, snapshot_version: 1 }, start_time_ns: OCT09, total_valid: "40", total_invalid: "1" },
    "/data/exp/unread.h5": { identity: { snapshot_unreadable: true }, total_valid: "8", total_invalid: "0" },
    "/data/exp/old.h5": { start_time_ns: OCT09, total_valid: "12", total_invalid: "3", core_version: "1.9.3" },
    "/data/exp/broken.h5": {
      ok: false,
      message: "Not an HDF5 file, or it cannot be opened (locked or unreadable): /data/exp/broken.h5",
      total_valid: null,
      total_invalid: null,
    },
  };
}
let probeFixtures: Record<string, ProbeFixture> = {};
let probeRefusal = "";
let deferProbes = false;
const pendingProbes: { paths: string[]; answer: () => void }[] = [];
function probeOf(path: string): ReviewRunProbe {
  const f = probeFixtures[path] ?? {};
  return {
    path,
    ok: true,
    message: "",
    recording_file: false,
    start_time_ns: null,
    end_time_ns: null,
    total_valid: "10",
    total_invalid: "0",
    accounting_completion: null,
    core_version: null,
    ...f,
    identity: { ...absentIdentity(), ...(f.identity ?? {}) },
  };
}
const listener = { id: 0 };
const callbacks = new Map<number, (event: unknown) => void>();

const dataset = (count: number) => ({ present: count > 0, count: String(count), height: 96, width: 512, channels: 1 });

function info() {
  const open = opened !== "";
  const legacy = opened.endsWith("legacy.h5");
  return {
    valid: true,
    file_open: open,
    file_path: opened,
    recording_file: false,
    start_time_ns: open && !legacy ? "1760848200000000000" : "0",
    end_time_ns: "0",
    total_valid: open ? "12" : "0",
    total_invalid: open ? "3" : "0",
    filtered_frames: "0",
    roi_x: 0,
    roi_y: 0,
    roi_w: 512,
    roi_h: 96,
    has_background: false,
    has_core_identity: open && !legacy,
    core_version: "2.1.0",
    core_source: "bundled",
    core_release_tag: "",
    valid_images: dataset(open ? 12 : 0),
    invalid_images: dataset(open ? 3 : 0),
    valid_masks: dataset(open ? 12 : 0),
    invalid_masks: dataset(open ? 3 : 0),
    recorded_images: dataset(0),
    has_series: false,
    series_count: "0",
    multi_image_enabled: false,
    multi_image_count: "0",
    has_accounting: open && !legacy,
    accounting_completion: legacy ? 4 : 0,
    accounting_reconciled: true,
    accounting_empty: "0",
    accounting_rejected: "0",
    accounting_processing_failed: "0",
    accounting_store_loss: "0",
    accounting_persisted: "15",
    accounting_admitted: "15",
    accounting_persistence_failed: "0",
    accounting_summary: legacy ? " · accounting: not recorded (legacy file)" : " · run complete — empty 0",
    pixel_to_micron: legacy ? 0.4886 : 0.25,
    pixel_to_micron_from_file: !legacy,
    kde_analysis_json: "",
    kde_live_json: "",
    has_recorded_config: true,
    ring_ratio_min: 15,
    ring_ratio_max: 25,
    identity: open ? probeOf(opened).identity : absentIdentity(),
  };
}

const ok = (message = "accepted") => ({ ok: true, command: 9, message, operation_id: "0" });

function respond(cmd: string, args: Record<string, unknown>): unknown {
  switch (cmd) {
    case "plugin:app|version":
      return "1.2.0";
    case "plugin:event|listen":
      return ++listener.id;
    case "plugin:event|unlisten":
      return null;
    case "review_abi_version":
      return 2;
    case "is_initialized":
      return false;
    case "init":
      return true;
    case "shell_log":
      return null;
    case "review_launch_path":
      return launchPath;
    case "review_take_open_request": {
      const p = pendingOpen;
      pendingOpen = "";
      return p;
    }
    case "review_check_update":
      return { configured: false, channel: String(args.channel), current: "1.2.0", available: false, version: "", notes: "", date: "" };
    case "review_set_pixel_to_micron":
      return null;
    case "review_list_dir": {
      const dir = String(args.dir);
      const names = dir === "/data/lib" ? ["run_10.h5", "run_2.h5", "legacy.h5", "notes.txt", ".run_3.h5.partial-7"] : (folderFiles[dir] ?? []);
      if (!deferDirs.has(dir)) return names;
      return new Promise<string[]>((resolve) => pendingLists.set(dir, [...(pendingLists.get(dir) ?? []), () => resolve(names)]));
    }
    case "review_probe_runs": {
      const paths = args.paths as string[];
      if (paths.length > 256) return { ok: false, message: `Too many files to probe at once: ${paths.length} (at most 256)`, runs: [] };
      if (probeRefusal) return { ok: false, message: probeRefusal, runs: [] };
      const answer = { ok: true, message: `Probed ${paths.length} file(s)`, runs: paths.map(probeOf) };
      if (!deferProbes) return answer;
      return new Promise((resolve) => pendingProbes.push({ paths, answer: () => resolve(answer) }));
    }
    case "review_open":
      if (refuse.has(String(args.path))) {
        opened = "";
        return { ok: false, command: 9, message: "File not found or not an HDF5 review file: " + String(args.path), operation_id: "0" };
      }
      opened = String(args.path);
      return ok("Review file opened");
    case "review_close":
      opened = "";
      return ok("Review file closed");
    case "fetch_review_info":
      return info();
    case "fetch_review_rows":
      return { valid: true, total: "12", offset: String(args.offset), rows: [] };
    case "fetch_review_frame":
    case "fetch_review_series_packet":
    case "fetch_review_thumbnails_packet":
      return new ArrayBuffer(0);
    case "poll_review_events":
      return { events: [] };
    case "review_jobs_busy":
      return false;
    case "plugin:dialog|open":
      return folderPick;
    default:
      throw new Error(`unexpected command ${cmd}`);
  }
}

class NoResizeObserver {
  observe() {}
  unobserve() {}
  disconnect() {}
}

let root: Root;
let host: HTMLDivElement;

beforeEach(() => {
  vi.useFakeTimers();
  calls.length = 0;
  opened = "";
  launchPath = "";
  pendingOpen = "";
  refuse = new Set();
  folderPick = "/data/lib";
  deferDirs = new Set();
  pendingLists.clear();
  probeFixtures = defaultProbes();
  probeRefusal = "";
  deferProbes = false;
  pendingProbes.length = 0;
  listener.id = 0;
  callbacks.clear();
  const saved = new Map<string, string>();
  vi.stubGlobal("localStorage", {
    getItem: (k: string) => saved.get(k) ?? null,
    setItem: (k: string, v: string) => void saved.set(k, String(v)),
    removeItem: (k: string) => void saved.delete(k),
    clear: () => saved.clear(),
  });
  Object.assign(globalThis, { ResizeObserver: NoResizeObserver });
  HTMLCanvasElement.prototype.getContext = (() => null) as unknown as typeof HTMLCanvasElement.prototype.getContext;
  let nextCallback = 1;
  (window as unknown as { __TAURI_INTERNALS__: unknown }).__TAURI_INTERNALS__ = {
    invoke: async (cmd: string, args: Record<string, unknown>) => {
      calls.push({ cmd, args: args ?? {} });
      return respond(cmd, args ?? {});
    },
    transformCallback: (cb: (event: unknown) => void) => {
      const id = nextCallback++;
      callbacks.set(id, cb);
      return id;
    },
    unregisterCallback: (id: number) => void callbacks.delete(id),
  };
  (window as unknown as { __TAURI_EVENT_PLUGIN_INTERNALS__: unknown }).__TAURI_EVENT_PLUGIN_INTERNALS__ = { unregisterListener: () => undefined };
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});

afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

async function settle(ms = 0) {
  for (let i = 0; i < 5; i++) await act(async () => void (await vi.advanceTimersByTimeAsync(ms)));
}

async function mount() {
  await act(async () => root.render(<ReviewApp />));
  await settle();
}

function button(text: string, scope: ParentNode = host): HTMLButtonElement {
  const found = Array.from(scope.querySelectorAll("button")).filter((b) => b.textContent?.trim() === text || b.getAttribute("aria-label") === text);
  if (found.length === 0) throw new Error(`no button "${text}" in: ${Array.from(scope.querySelectorAll("button")).map((b) => b.textContent?.trim()).join(" | ")}`);
  return found[0];
}

async function click(el: Element) {
  await act(async () => void el.dispatchEvent(new MouseEvent("click", { bubbles: true })));
  await settle();
}

const stageTab = (name: string) => {
  const tab = Array.from(host.querySelectorAll(".mib-stages__tab")).find((b) => b.textContent?.endsWith(name));
  if (!tab) throw new Error(`no stage ${name}`);
  return tab;
};
const activeStage = () => host.querySelector('.mib-stages__tab[aria-current="step"]')?.textContent ?? "";
const visible = (testId: string) => {
  const el = host.querySelector(`[data-testid="${testId}"]`) as HTMLElement | null;
  return !!el && !el.closest("[hidden]");
};
const openCard = () => Array.from(host.querySelectorAll(".mib-card")).find((c) => c.querySelector(".mib-card__title")?.textContent === "Open run")!;
const factValue = (label: string) => {
  const dt = Array.from(openCard().querySelectorAll("dt")).find((d) => d.textContent === label);
  return dt?.nextElementSibling?.textContent ?? null;
};

it("shows the Review app's stages on the kit shell and starts on Open", async () => {
  await mount();
  expect(host.querySelector(".mib-shell__name")?.textContent).toBe("YOFO Review");
  expect(Array.from(host.querySelectorAll(".mib-stages__tab")).map((b) => b.textContent)).toEqual(["01Open", "02Overview", "03Inspect", "04Export"]);
  expect(activeStage()).toBe("01Open");
  expect(visible("review-open")).toBe(true);
  expect(visible("review-inspect")).toBe(false);
  expect(host.textContent).toContain("Backend ready");
});

it("changes stage without reaching the backend, and says honestly what is not available", async () => {
  await mount();
  const before = calls.filter((c) => c.cmd !== "poll_review_events").length;
  await click(stageTab("Overview"));
  expect(visible("review-overview")).toBe(true);
  const overview = host.querySelector('[data-testid="review-overview"]')!;
  expect(overview.textContent).toContain("Overview · not available yet");
  expect(overview.textContent?.replace(/#\d+/g, "")).not.toMatch(/\d/); // no numbers it does not have (issue refs aside)
  await click(stageTab("Export"));
  expect(host.querySelector('[data-testid="review-export"]')?.textContent).toContain("Export · on Inspect's toolbar for now");
  await click(stageTab("Inspect"));
  expect(visible("review-inspect")).toBe(true);
  expect(button("Select HDF File…")).toBeTruthy(); // today's panel, unchanged
  await click(stageTab("Open"));
  expect(calls.filter((c) => c.cmd !== "poll_review_events").length).toBe(before);
});

it("lists a folder's run files and opens one into the review session", async () => {
  await mount();
  await click(button("Open folder…"));
  expect(calls.find((c) => c.cmd === "plugin:dialog|open")?.args).toMatchObject({ options: { directory: true } });
  expect(calls.find((c) => c.cmd === "review_list_dir")?.args).toEqual({ dir: "/data/lib" });
  // Grouped (run_2 and run_10 declare one experiment, by run index), the legacy file apart.
  expect(fileRows()).toEqual(["run_2.h5", "run_10.h5", "legacy.h5"]);
  expect(localStorage.getItem(LIBRARY_FOLDER_KEY)).toBe("/data/lib");

  await click(button("Open run_2.h5"));
  expect(calls.filter((c) => c.cmd === "review_open").map((c) => c.args.path)).toEqual(["/data/lib/run_2.h5"]);
  expect(activeStage()).toBe("01Open"); // the library stays; the identity shows here
  expect(factValue("File")).toBe("run_2.h5");
  expect(factValue("Records")).toBe("12 valid · 3 invalid");
  expect(openCard().textContent).toContain("Complete");
  expect(host.querySelector(".mib-shell__context-value")?.textContent).toBe("run_2.h5");
  expect(button("Open run_2.h5").textContent).toBe("Opened");

  // A legacy file: what it does not record is said, not guessed.
  await click(button("Open legacy.h5"));
  expect(factValue("Run accounting")).toBe("Not recorded · legacy file");
  expect(factValue("Started")).toBe("Not reported");
  expect(factValue("Processing core")).toBe("Not reported");
  expect(openCard().textContent).toContain("Accounting · not recorded");

  await click(button("Inspect legacy.h5"));
  expect(activeStage()).toBe("03Inspect");
  expect(button("Valid Frames (12)")).toBeTruthy();
});

it("shows a refused open in plain words and leaves nothing open", async () => {
  await mount();
  await click(button("Open folder…"));
  await click(button("Open run_2.h5"));
  refuse = new Set(["/data/lib/run_10.h5"]);
  await click(button("Open run_10.h5"));
  const notice = host.querySelector('[data-tone="blocked"]');
  expect(notice?.textContent).toContain("Not opened · run_10.h5");
  expect(notice?.textContent).toContain("File not found or not an HDF5 review file");
  expect(openCard().textContent).toContain("No run is open");
  expect(host.querySelector(".mib-shell__context-value")?.textContent).toBe("None");
});

it("lists the remembered folder at start, and Close file releases the run", async () => {
  localStorage.setItem(LIBRARY_FOLDER_KEY, "/data/lib");
  await mount();
  expect(calls.find((c) => c.cmd === "review_list_dir")?.args).toEqual({ dir: "/data/lib" });
  await click(button("Open run_10.h5"));
  await click(button("Close file"));
  expect(calls.some((c) => c.cmd === "review_close")).toBe(true);
  expect(openCard().textContent).toContain("No run is open");
});

it("an empty folder or a cancelled choice says so", async () => {
  await mount();
  expect(host.textContent).toContain("No folder chosen yet.");
  folderPick = null;
  await click(button("Open folder…"));
  expect(calls.some((c) => c.cmd === "review_list_dir")).toBe(false);
  folderPick = "/data/empty";
  await click(button("Open folder…"));
  expect(host.textContent).toContain("No .h5 or .hdf5 files in this folder.");
});

// #747 review: listings can finish out of order, and a row must never open a
// file of a folder other than the one shown.
const fileRows = (scope: ParentNode = host) =>
  Array.from(scope.querySelectorAll(".mib-table tbody tr:not(.mib-table__empty-row)")).map((r) => r.querySelector("td.mib-table__cell--emphasis")?.textContent ?? "");
const libraryCard = () => Array.from(host.querySelectorAll(".mib-card")).find((c) => c.querySelector(".mib-card__title")?.textContent === "Experiments in this folder")!;
const openButtons = () => Array.from(host.querySelectorAll("button")).filter((b) => b.getAttribute("aria-label")?.startsWith("Open "));
async function chooseFolder(dir: string) {
  folderPick = dir;
  await click(button("Open folder…"));
}
async function answerListing(dir: string) {
  const waiting = pendingLists.get(dir)?.shift();
  if (!waiting) throw new Error(`no pending listing for ${dir}`);
  await act(async () => waiting([]));
  await settle();
}

it("a folder change drops the old folder's rows at once and ignores a superseded listing", async () => {
  await mount();
  await chooseFolder("/data/a");
  expect(fileRows()).toEqual(["a1.h5", "a2.h5"]);

  // B and then C are chosen while both listings are still out.
  deferDirs = new Set(["/data/b", "/data/c"]);
  await chooseFolder("/data/b");
  expect(fileRows()).toEqual([]); // A's rows are gone at once
  expect(openButtons()).toEqual([]);
  expect(libraryCard().textContent).toContain("Listing…");
  expect(libraryCard().textContent).toContain("/data/b");
  await chooseFolder("/data/c");
  expect(libraryCard().textContent).toContain("/data/c");

  // B answers first, but C is the folder shown: B's names never appear.
  await answerListing("/data/b");
  expect(fileRows()).toEqual([]);
  expect(libraryCard().textContent).toContain("Listing…");
  await answerListing("/data/c");
  expect(fileRows()).toEqual(["c1.h5", "c2.h5", "c3.h5"]);
  expect(libraryCard().textContent).toContain("3 run files");

  // The other order: D is chosen after C, D answers first, then C's late answer.
  deferDirs = new Set(["/data/c", "/data/d"]);
  await click(button("Refresh")); // C again, still out
  await chooseFolder("/data/d");
  await answerListing("/data/d");
  expect(fileRows()).toEqual(["d1.h5"]);
  await answerListing("/data/c");
  expect(fileRows()).toEqual(["d1.h5"]);
  expect(libraryCard().textContent).toContain("/data/d");
  expect(libraryCard().textContent).toContain("1 run file");
  expect(calls.some((c) => c.cmd === "review_open")).toBe(false);
});

it("a row shown while its folder is listed again opens nothing", async () => {
  await mount();
  await chooseFolder("/data/c");
  deferDirs = new Set(["/data/c"]);
  await click(button("Refresh"));
  // The rows stay while the refresh is out, disabled.
  expect(fileRows()).toEqual(["c1.h5", "c2.h5", "c3.h5"]);
  expect(openButtons().every((b) => b.disabled)).toBe(true);
  expect(button("Refresh").disabled).toBe(true);
  await click(button("Open c2.h5"));
  expect(calls.some((c) => c.cmd === "review_open")).toBe(false);

  // And a folder change while the old rows are up removes them before any click.
  deferDirs = new Set(["/data/a"]);
  await answerListing("/data/c");
  expect(openButtons().every((b) => !b.disabled)).toBe(true);
  await chooseFolder("/data/a");
  expect(() => button("Open c2.h5")).toThrow();
  expect(calls.some((c) => c.cmd === "review_open")).toBe(false);
  await answerListing("/data/a");
  await click(button("Open a2.h5"));
  expect(calls.filter((c) => c.cmd === "review_open").map((c) => c.args.path)).toEqual(["/data/a/a2.h5"]);
});

// ---- R1 grouping (#717 B1b part 2): run identities from the files' headers ----
const groups = () => Array.from(host.querySelectorAll("table.rv-group")) as HTMLTableElement[];
const groupTitles = () => groups().map((g) => g.querySelector(".rv-group__title")?.textContent ?? "");
const groupOf = (title: string) => {
  const g = groups().find((t) => t.querySelector(".rv-group__title")?.textContent === title);
  if (!g) throw new Error(`no group ${title} in: ${groupTitles().join(" | ")}`);
  return g;
};
const cellOf = (table: HTMLTableElement, file: string, header: string) => {
  const headers = Array.from(table.querySelectorAll("thead th")).map((th) => th.textContent);
  const row = Array.from(table.querySelectorAll("tbody tr")).find((r) => r.querySelector("td.mib-table__cell--emphasis")?.textContent === file);
  return row?.querySelectorAll("td")[headers.indexOf(header)]?.textContent ?? null;
};
const probeCalls = () => calls.filter((c) => c.cmd === "review_probe_runs").map((c) => c.args.paths as string[]);
async function answerProbe() {
  const waiting = pendingProbes.shift();
  if (!waiting) throw new Error("no pending probe");
  await act(async () => waiting.answer());
  await settle();
}

it("groups a folder's runs by experiment and run index, legacy and unreadable files apart", async () => {
  await mount();
  await chooseFolder("/data/exp");
  expect(probeCalls()).toEqual([folderFiles["/data/exp"].slice().sort().map((n) => `/data/exp/${n}`)]);
  expect(groupTitles()).toEqual([
    `Experiment ${EXP_A}`, // newest first
    `Experiment ${EXP_B}`,
    "Runs without an experiment",
    "Legacy files · no run snapshot",
    "Cannot be read",
  ]);
  const a = groupOf(`Experiment ${EXP_A}`);
  expect(fileRows(a)).toEqual(["beta.h5", "gamma.h5", "alpha.h5"]); // run 1, 2, 3, not name order
  expect(cellOf(a, "beta.h5", "Run")).toBe("Run 1");
  expect(cellOf(a, "beta.h5", "Sample")).toBe("S-01 · Donor 4 · Vehicle");
  expect(cellOf(a, "gamma.h5", "Condition")).toBe("CytoD 0.1");
  expect(cellOf(a, "alpha.h5", "Records")).toBe("100 valid · 2 invalid");
  expect(cellOf(a, "alpha.h5", "Core")).toBe("2.1.0");
  expect(cellOf(a, "alpha.h5", "Calibration")).toBe("cal-0042");
  expect(cellOf(a, "alpha.h5", "Status")).toBe("Partial · loss");
  const caption = a.querySelector("caption")?.textContent ?? "";
  expect(caption).toContain("3 runs · 3 samples");
  expect(caption).toContain("conditions: Vehicle · CytoD 0.1 · CytoD 1");
  expect(caption).toContain("core: 2.1.0");
  expect(caption).toContain("1 of 3 runs · Partial · loss");

  const none = groupOf("Runs without an experiment");
  expect(fileRows(none)).toEqual(["nox.h5", "unread.h5"]);
  expect(cellOf(none, "nox.h5", "Snapshot")).toBe("Snapshot v1");
  expect(cellOf(none, "unread.h5", "Snapshot")).toBe("Snapshot unreadable");

  // A legacy file shows what it does not record, and nothing in its place.
  const legacy = groupOf("Legacy files · no run snapshot");
  expect(fileRows(legacy)).toEqual(["old.h5"]);
  expect(cellOf(legacy, "old.h5", "Sample")).toBe("Not recorded");
  expect(cellOf(legacy, "old.h5", "Condition")).toBe("Not recorded");
  expect(cellOf(legacy, "old.h5", "Calibration")).toBe("Not recorded");
  expect(cellOf(legacy, "old.h5", "Status")).toBe("Accounting · not recorded");
  expect(cellOf(legacy, "old.h5", "Records")).toBe("12 valid · 3 invalid");
  expect(legacy.querySelector("caption")?.textContent).toContain("experiment, run, sample, condition and calibration are not recorded");

  const broken = groupOf("Cannot be read");
  expect(cellOf(broken, "broken.h5", "Why it cannot be read")).toContain("Not an HDF5 file, or it cannot be opened");
  expect(libraryCard().textContent).toContain("2 experiments · 8 run files");
  expect(host.textContent).toContain("does not read: an experiment shows the id its files declare");
});

it("opens a grouped run and shows the identity its file declares", async () => {
  await mount();
  await chooseFolder("/data/exp");
  await click(button("Open beta.h5"));
  expect(calls.filter((c) => c.cmd === "review_open").map((c) => c.args.path)).toEqual(["/data/exp/beta.h5"]);
  expect(button("Open beta.h5").textContent).toBe("Opened");
  expect(factValue("Run snapshot")).toBe("Version 3");
  expect(factValue("Experiment")).toBe(EXP_A);
  expect(factValue("Run")).toBe("Run 1");
  expect(factValue("Sample")).toBe("S-01 · Donor 4 · Vehicle");
  expect(factValue("Condition")).toBe("Vehicle");
  expect(factValue("Annotation")).toBe("unit donor-4 · medium PBS");
  expect(factValue("Calibration record")).toBe("cal-0042");
  expect(factValue("Operator (selected at Start)")).toBe("Gavin");

  await click(button("Open old.h5"));
  expect(factValue("Run snapshot")).toBe("Not recorded · legacy file");
  for (const label of ["Experiment", "Run", "Sample", "Condition", "Annotation", "Calibration record", "Operator (selected at Start)"])
    expect(factValue(label)).toBe("Not reported");
});

it("probes a large folder in calls of at most 256 files", async () => {
  await mount();
  await chooseFolder("/data/big");
  const sizes = probeCalls().map((p) => p.length);
  expect(sizes).toEqual([256, 44]);
  expect(probeCalls().flat()).toEqual(folderFiles["/data/big"].map((n) => `/data/big/${n}`));
  expect(fileRows(groupOf("Legacy files · no run snapshot")).length).toBe(300);
  expect(libraryCard().textContent).toContain("0 experiments · 300 run files");
});

it("shows the probe's progress, and a stale probe never fills a newer folder", async () => {
  await mount();
  deferProbes = true;
  await chooseFolder("/data/big");
  expect(libraryCard().textContent).toContain("Reading 0 of 300 files");
  expect(groupTitles()).toEqual(["Reading run identities…"]);
  await answerProbe();
  expect(libraryCard().textContent).toContain("Reading 256 of 300 files");

  // A folder change while the second call is out: its answer is dropped.
  await chooseFolder("/data/exp");
  await answerProbe(); // /data/big's late second call
  expect(pendingProbes.length).toBe(1);
  await answerProbe(); // /data/exp's call
  expect(groupTitles()[0]).toBe(`Experiment ${EXP_A}`);
  expect(fileRows().some((n) => n.startsWith("f"))).toBe(false);

  // A refresh while a probe is out: the newer answer wins over the late older one.
  await click(button("Refresh"));
  probeFixtures["/data/exp/old.h5"] = { identity: v3(EXP_B, 2, "S-02", "CytoD 1"), start_time_ns: OCT09 };
  await click(button("Refresh"));
  expect(pendingProbes.length).toBe(2);
  pendingProbes.reverse(); // the newer call answers first
  await answerProbe();
  expect(fileRows(groupOf(`Experiment ${EXP_B}`))).toEqual(["older.h5", "old.h5"]);
  await answerProbe(); // the older refresh's answer, where old.h5 was legacy
  expect(fileRows(groupOf(`Experiment ${EXP_B}`))).toEqual(["older.h5", "old.h5"]);
  expect(groupTitles()).not.toContain("Legacy files · no run snapshot");
});

it("a refused probe says so and leaves the files listed and openable", async () => {
  probeRefusal = "A review job is running; the files can be read when it finishes";
  await mount();
  await chooseFolder("/data/exp");
  const notice = Array.from(host.querySelectorAll(".mib-notice")).find((n) => n.textContent?.includes("Run identities · not read"));
  expect(notice?.textContent).toContain("A review job is running");
  expect(groupTitles()).toEqual(["Not read"]);
  expect(fileRows(groupOf("Not read")).length).toBe(8);
  await click(button("Open beta.h5"));
  expect(calls.filter((c) => c.cmd === "review_open").map((c) => c.args.path)).toEqual(["/data/exp/beta.h5"]);
});

it("opens the launch file straight into Inspect", async () => {
  launchPath = "/data/run.h5";
  await mount();
  expect(calls.filter((c) => c.cmd === "review_open").map((c) => c.args.path)).toEqual(["/data/run.h5"]);
  expect(activeStage()).toBe("03Inspect");
});

it("opens an OS open request once, into Inspect", async () => {
  await mount();
  const listen = calls.find((c) => c.cmd === "plugin:event|listen");
  expect(listen?.args).toMatchObject({ event: "review-open-file" });
  pendingOpen = "/data/finder.h5";
  const handler = callbacks.get(Number(listen?.args.handler));
  await act(async () => handler?.({ event: "review-open-file", id: 1, payload: "/data/finder.h5" }));
  await settle();
  expect(calls.filter((c) => c.cmd === "review_open").map((c) => c.args.path)).toEqual(["/data/finder.h5"]);
  expect(activeStage()).toBe("03Inspect");
  // The request was taken: a second event opens nothing new.
  await act(async () => handler?.({ event: "review-open-file", id: 1, payload: "/data/finder.h5" }));
  await settle();
  expect(calls.filter((c) => c.cmd === "review_open").length).toBe(1);
});
