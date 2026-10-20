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
};
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
  const rows = Array.from(host.querySelectorAll(".mib-table tbody tr")).map((r) => r.querySelector("td")?.textContent);
  expect(rows).toEqual(["legacy.h5", "run_2.h5", "run_10.h5"]);
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
const fileRows = () => Array.from(host.querySelectorAll(".mib-table tbody tr:not(.mib-table__empty-row)")).map((r) => r.querySelector("td")?.textContent ?? "");
const libraryCard = () => Array.from(host.querySelectorAll(".mib-card")).find((c) => c.querySelector(".mib-card__title")?.textContent === "Runs in this folder")!;
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
