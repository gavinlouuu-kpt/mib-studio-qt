// @vitest-environment jsdom
// O4 Experiment · Live (#716 A6b) over a mocked transport: the detected-cell strip on the kit's
// CellImage with crops pulled as frame packets (once per cell, two at a time), pin and unpin on a
// click only, the outcomes said in plain words (CaptureMismatch, LimitReached), ended pins with
// their metadata and reason and never an image, "Clear ended pins", scatter click-to-pin, previous /
// next and the viewer, the viewer gating (no control: nothing is sent), Freeze display, and the two
// separate stops. The control loss of a running queue is in navigation.test.tsx (the whole App).
import { act, StrictMode } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import { EXPERIMENT_STATES, EXPERIMENT_STOP_OUTCOMES, FRAME_PACKET, JSON_TRANSPORT, MONITORING_CELL_PIN_OUTCOMES as P } from "../bridgeContract";
import type { MonitoringRow, MonitoringSnapshot } from "../bridge";
import type { ExperimentStatus } from "../eventAdapter";
import { chartDomain, chartPosition, liveScatter, STRIP_SHOWN } from "./liveModel";
import { LiveView } from "./LiveView";
import { usePlan, type StopReply } from "./usePlan";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));

// ---- the mock backend ----
const cell = (frame: string, extra: Record<string, unknown> = {}) => ({
  start_generation: "0", capture_epoch: "3", frame_index: frame, object_id: 1, timestamp_ns: "1",
  valid: true, target_group: false, object_count: 1, track_id: -1, centroid_x: 50, centroid_y: 38,
  bbox: { x: 40, y: 30, w: 20, h: 16 }, area: 600, deformability: 0.05, area_ratio: 1.02, ring_ratio: null,
  youngs_modulus: 1.2, pixel_to_micron: 0.5,
  image: { width: 4, height: 3, crop_x: 30, crop_y: 20, source_width: 512, source_height: 96, has_mask: true, bytes: "12" },
  no_image_reason: null, ...extra,
});
const ENDED = "no image: capture 3 ended (the camera stopped); the pin keeps its measurements";
let listing: Record<string, unknown>;
const setListing = (extra: Record<string, unknown>) => {
  listing = { ...listing, ...extra, revision: String(Number(listing.revision) + 1) };
};
let pinReply: Record<string, unknown> = {};
/** Frames whose crop pulls are refused as another capture's. */
let mismatched = new Set<string>();
/** Set to hold every crop pull until the test releases them. */
let holdPulls = false;
const heldPulls: (() => void)[] = [];
const cmd = (message = "ok") => ({ transport_version: JSON_TRANSPORT.version, ok: true, command: 10, message, operation_id: "0" });

function packet(kind: number, width = 4, height = 3): ArrayBuffer {
  const h = FRAME_PACKET.header_bytes, len = width * height;
  const buf = new ArrayBuffer(h + len), d = new DataView(buf);
  d.setUint32(0, 0x4642494d, true); d.setUint16(4, FRAME_PACKET.version, true); d.setUint16(6, h, true);
  d.setUint32(8, 1, true); d.setUint32(12, kind, true);
  d.setBigUint64(32, BigInt(width), true); d.setBigUint64(40, BigInt(height), true);
  d.setBigUint64(56, BigInt(width), true); d.setBigUint64(64, BigInt(len), true);
  new Uint8Array(buf, h).fill(128);
  return buf;
}

function respond(command: string, args: Record<string, unknown> = {}): unknown {
  switch (command) {
    case "fetch_monitoring_cells": return listing;
    case "fetch_monitoring_cell_packet": {
      const frame = String(args.frameIndex);
      const answer = () => {
        if (mismatched.has(frame)) throw `CAPTURE_MISMATCH: image refused: frame ${frame} object 1 is from capture 3, but the current capture is 4 (the camera stopped)`;
        return packet(args.layer === "mask" ? FRAME_PACKET.pull_kinds.monitoring_cell_mask : FRAME_PACKET.pull_kinds.monitoring_cell_image);
      };
      if (holdPulls) return new Promise((resolve, reject) => heldPulls.push(() => { try { resolve(answer()); } catch (e) { reject(e); } }));
      return answer();
    }
    case "monitoring_cell_pin": return { ok: true, outcome: P.Pinned, message: "pinned", revision: "9",
      cell: cell(String(args.frameIndex), { start_generation: String(args.expectedStartGeneration), capture_epoch: String(args.captureEpoch) }), ...pinReply };
    case "monitoring_cell_unpin": return cmd("unpinned");
    case "monitoring_cell_clear_pins": return cmd("cleared 2 pin(s)");
    default: throw new Error(`unexpected command ${command}`);
  }
}

const calls = () => vi.mocked(invoke).mock.calls.map(([c, a]) => ({ cmd: c, args: a as Record<string, unknown> }));
const sent = (name: string) => calls().filter((c) => c.cmd === name);
const CONTROL = ["monitoring_cell_pin", "monitoring_cell_unpin", "monitoring_cell_clear_pins"];

// ---- the view ----
const status = (over: Partial<ExperimentStatus> = {}): ExperimentStatus => ({
  transport_version: 1, valid: true, state: EXPERIMENT_STATES.Idle, start_time_ns: "0", end_time_ns: "0", valid_buffered: "0", invalid_buffered: "0",
  valid_saved: "0", invalid_saved: "0", dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false, output_path: "", message: "",
  start_generation: "0", readiness_generation: "1", capture_generation: "1", persistence_admitted: "0", persistence_committed: "0", persistence_failed: "0",
  terminal: false, finalization_ok: false, completion: 0, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "", ...over,
} as ExperimentStatus);
const row = (frame: number, over: Partial<MonitoringRow> = {}): MonitoringRow => ({
  frame_index: frame, timestamp_ns: 0, valid: true, target_group: false, object_id: 1, object_count: 1, track_id: -1, centroid_x: 1, centroid_y: 1,
  area: 400 + frame, deformability: 0.02 + frame / 1000, area_ratio: 1, ring_ratio: 0.5, youngs_modulus: 1, pixel_to_micron: 0.5,
  start_generation: "0", capture_epoch: "3", ...over,
});
const snapshotOf = (rows: MonitoringRow[]): MonitoringSnapshot => ({
  valid: true, monitoring_active: true, valid_held: rows.length, invalid_held: 0, valid_appended: rows.length, invalid_appended: 0, capacity: 200, latest_timestamp_ns: 0, rows,
});

interface Setup {
  controlHeld?: boolean; status?: ExperimentStatus | null; snapshot?: MonitoringSnapshot | null; running?: boolean; experimentActive?: boolean; hostProcessing?: boolean;
}
let setup: Setup = {};
let stopReply: StopReply = { ok: true, message: "Accepted", stop_outcome: EXPERIMENT_STOP_OUTCOMES.Accepted };
const onStopRun = vi.fn(async (_generation: string) => stopReply);
const onStopAcquisition = vi.fn(async () => {});
const onView = vi.fn();
const subscribeFrames = vi.fn();
const toUrl = (p: { width: number; height: number }, layer: string) => `data:test/${layer}-${p.width}x${p.height}`;

function Harness(s: Setup) {
  const plan = usePlan(async () => ({ kind: "busy" }), { status: s.status ?? null, controlHeld: s.controlHeld ?? true });
  return <LiveView tabs={null} plan={plan} snapshot={s.snapshot ?? null} running={s.running ?? true} experimentActive={s.experimentActive ?? false}
    hostProcessing={s.hostProcessing ?? true} subscribeFrames={subscribeFrames} onStopAcquisition={onStopAcquisition} onStopRun={onStopRun} onView={onView} toUrl={toUrl} />;
}

let host: HTMLDivElement, root: Root;
const render = (s: Setup = setup) => { setup = s; return act(async () => root.render(<StrictMode><Harness {...s} /></StrictMode>)); };
const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const $ = (id: string) => host.querySelector<HTMLElement>(`[data-testid=${id}]`);
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text) as HTMLButtonElement | undefined;
const click = async (el: Element | null | undefined) => {
  if (!el) throw new Error("nothing to click");
  await act(async () => (el as HTMLElement).click());
  await advance(50);
};
const stripCells = () => [...host.querySelectorAll("[data-testid=live-strip] .mib-cell")];
const stripNames = () => stripCells().map((c) => c.querySelector(".mib-cell__id")!.textContent);

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.useFakeTimers();
  vi.mocked(invoke).mockReset();
  vi.mocked(invoke).mockImplementation(async (c: string, a?: unknown) => respond(c, a as Record<string, unknown>));
  onStopRun.mockClear(); onStopAcquisition.mockClear(); onView.mockClear(); subscribeFrames.mockClear();
  listing = { valid: true, scope_start_generation: "0", ring_start_generation: "0", ring_capture_epoch: "3", revision: "1",
    strip: [cell("101"), cell("102"), cell("103")], pins: [], last_capture_change: null, stats: {}, caps: { strip_max_cells: 24, max_pins: 16, strip_max_bytes: 4194304 } };
  pinReply = {};
  mismatched = new Set();
  holdPulls = false;
  heldPulls.length = 0;
  stopReply = { ok: true, message: "Accepted", stop_outcome: EXPERIMENT_STOP_OUTCOMES.Accepted };
  setup = {};
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
  vi.restoreAllMocks();
});

describe("the detected-cell strip", () => {
  it("shows the newest cells first on the kit's CellImage, each crop pulled once as a frame packet", async () => {
    await render();
    await advance(100);
    expect(stripNames()).toEqual(["F103·O1", "F102·O1", "F101·O1"]);
    expect(stripCells().map((c) => c.querySelector("img")?.getAttribute("src"))).toEqual(["data:test/image-4x3", "data:test/image-4x3", "data:test/image-4x3"]);
    const pulls = sent("fetch_monitoring_cell_packet");
    expect(pulls.map((p) => [p.args.frameIndex, p.args.layer])).toEqual([["103", "image"], ["102", "image"], ["101", "image"]]);
    expect(pulls[0].args).toEqual({ startGeneration: "0", captureEpoch: "3", frameIndex: "103", objectId: 1, layer: "image" });
    // Later reads of the same listing pull nothing again; a new cell is pulled once.
    await advance(3000);
    expect(sent("fetch_monitoring_cells").length).toBeGreaterThanOrEqual(3);
    expect(sent("fetch_monitoring_cell_packet")).toHaveLength(3);
    setListing({ strip: [cell("101"), cell("102"), cell("103"), cell("104")] });
    await advance(1100);
    expect(stripNames()[0]).toBe("F104·O1");
    expect(sent("fetch_monitoring_cell_packet").map((p) => p.args.frameIndex)).toEqual(["103", "102", "101", "104"]);
    // Nothing that acts was sent: the strip only reads.
    expect(calls().filter((c) => CONTROL.includes(c.cmd))).toEqual([]);
  });

  it("pulls at most two crops at a time, and only for the cells shown", async () => {
    setListing({ strip: Array.from({ length: 20 }, (_, i) => cell(String(200 + i))) });
    holdPulls = true;
    await render();
    await advance(100);
    expect(stripCells()).toHaveLength(STRIP_SHOWN);
    expect(sent("fetch_monitoring_cell_packet")).toHaveLength(2);
    // A crop still loading is an empty box, never "Image not recorded".
    expect(stripCells()[0].textContent).not.toContain("Image not recorded");
    while (heldPulls.length) { heldPulls.shift()!(); await advance(10); }
    expect(sent("fetch_monitoring_cell_packet")).toHaveLength(STRIP_SHOWN);
  });

  it("pulls the masks only for the Mask overlay, and draws the crop with its mask", async () => {
    await render();
    await advance(100);
    expect(sent("fetch_monitoring_cell_packet").every((p) => p.args.layer === "image")).toBe(true);
    await click(button("Mask"));
    await advance(100);
    expect(sent("fetch_monitoring_cell_packet").filter((p) => p.args.layer === "mask").map((p) => p.args.frameIndex)).toEqual(["103", "102", "101"]);
    expect(stripCells()[0].querySelector(".mib-cell__mask")?.getAttribute("src")).toBe("data:test/mask-4x3");
  });

  it("says why a crop of an earlier capture is not shown, and reads the listing again", async () => {
    mismatched = new Set(["102"]);
    await render();
    await advance(100);
    const refused = stripCells()[1];
    expect(refused.querySelector("img")).toBeNull();
    expect(refused.getAttribute("title")).toContain("Not served: image refused: frame 102 object 1 is from capture 3, but the current capture is 4 (the camera stopped)");
    const reads = sent("fetch_monitoring_cells").length;
    expect(reads).toBeGreaterThanOrEqual(2); // the first read, and one after the refusal
    await advance(2000);
    expect(sent("fetch_monitoring_cell_packet").filter((p) => p.args.frameIndex === "102")).toHaveLength(1); // never retried
  });
});

describe("pins", () => {
  it("pins a strip cell on a click: one command with the cell's identity, the outcome said, the pinned card shows it", async () => {
    await render();
    await advance(100);
    expect(sent("monitoring_cell_pin")).toEqual([]);
    await click(host.querySelector('[aria-label="Cell F102·O1"]'));
    expect(sent("monitoring_cell_pin")).toEqual([{ cmd: "monitoring_cell_pin", args: { expectedStartGeneration: "0", captureEpoch: "3", frameIndex: "102", objectId: 1 } }]);
    expect($("live-pin-notice")!.textContent).toContain("Pinned · F102·O1");
    setListing({ pins: [cell("102")] });
    await advance(1100);
    expect($("live-pinned")!.textContent).toContain("frame 102 · object 1 · pin 1 of 1");
    expect($("live-pinned-facts")!.textContent).toContain("150.0 µm²"); // 600 px² at 0.5 µm/px
    expect($("live-pinned-image")!.querySelector("img")!.getAttribute("src")).toBe("data:test/image-4x3");
    expect($("live-pin-count")!.textContent).toBe("1 of 16 pins");
    // Unpin: one command naming the pin.
    await click($("live-unpin"));
    expect(sent("monitoring_cell_unpin")).toEqual([{ cmd: "monitoring_cell_unpin", args: { startGeneration: "0", captureEpoch: "3", frameIndex: "102", objectId: 1 } }]);
  });

  it("says the backend's CaptureMismatch and LimitReached in plain words", async () => {
    await render();
    await advance(100);
    pinReply = { ok: false, outcome: P.CaptureMismatch, cell: null, message: "pin refused: frame 101 object 1 is from capture 3, but the current capture is 4 (the camera stopped)" };
    await click(host.querySelector('[aria-label="Cell F101·O1"]'));
    expect($("live-pin-notice")!.textContent).toContain("Not pinned · F101·O1 is of an earlier capture");
    expect($("live-pin-notice")!.textContent).toContain("the current capture is 4 (the camera stopped)");
    pinReply = { ok: false, outcome: P.LimitReached, cell: null, message: "pin refused: 16 cells are pinned (the limit); unpin one first" };
    await click(host.querySelector('[aria-label="Cell F103·O1"]'));
    expect($("live-pin-notice")!.textContent).toContain("Not pinned · the pin limit is reached");
    expect($("live-pin-notice")!.textContent).toContain("Ended pins count toward the limit too");
    expect(sent("monitoring_cell_pin")).toHaveLength(2);
  });

  it("shows an ended pin's metadata and reason, never an image, and Clear ended pins unpins only those", async () => {
    setListing({ ring_capture_epoch: "4", strip: [], pins: [
      cell("1", { image: null, no_image_reason: ENDED }),
      cell("2", { image: null, no_image_reason: ENDED }),
      cell("3", { capture_epoch: "4" }),
    ] });
    await render();
    await advance(100);
    expect($("live-pin-count")!.textContent).toBe("3 of 16 pins");
    expect($("live-ended-count")!.textContent).toBe("2 ended");
    // No pull for an ended pin: it has no image.
    expect(sent("fetch_monitoring_cell_packet").map((p) => p.args.frameIndex)).toEqual(["3"]);
    await click(host.querySelector('[aria-label="Previous pin"]'));
    expect($("live-pinned")!.textContent).toContain("frame 2 · object 1 · pin 2 of 3");
    expect($("live-noimage")!.textContent).toContain(ENDED);
    expect($("live-pinned")!.querySelector("img")).toBeNull();
    expect($("live-pinned-facts")!.textContent).toContain("3 · ended");
    expect($("live-pinned-facts")!.textContent).toContain("150.0 µm²"); // its measurements are kept
    await click($("live-clear-ended"));
    expect(sent("monitoring_cell_unpin").map((c) => c.args)).toEqual([
      { startGeneration: "0", captureEpoch: "3", frameIndex: "1", objectId: 1 },
      { startGeneration: "0", captureEpoch: "3", frameIndex: "2", objectId: 1 },
    ]);
    expect($("live-pin-notice")!.textContent).toContain("Cleared 2 ended pins");
    expect(sent("monitoring_cell_clear_pins")).toEqual([]);
  });

  it("steps through the pins and opens the larger viewer (a modal); Escape closes it", async () => {
    setListing({ pins: [cell("11"), cell("12")] });
    await render();
    await advance(100);
    expect($("live-pinned")!.textContent).toContain("frame 12 · object 1 · pin 2 of 2");
    await click(host.querySelector('[aria-label="Next pin"]'));
    expect($("live-pinned")!.textContent).toContain("frame 11 · object 1 · pin 1 of 2");
    await click($("live-open-viewer"));
    const dialog = $("live-viewer-dialog")!;
    expect(dialog.getAttribute("role")).toBe("dialog");
    expect(dialog.textContent).toContain("Pinned cell · F11·O1");
    expect(dialog.querySelector("img")!.getAttribute("src")).toBe("data:test/image-4x3");
    await act(async () => dialog.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowRight", bubbles: true })));
    expect($("live-viewer-dialog")!.textContent).toContain("Pinned cell · F12·O1");
    await act(async () => $("live-viewer-dialog")!.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true })));
    expect($("live-viewer-dialog")).toBeNull();
    expect(calls().filter((c) => CONTROL.includes(c.cmd))).toEqual([]);
  });
});

describe("scatter click-to-pin", () => {
  const rows = [row(10), row(20), row(30, { start_generation: undefined, capture_epoch: undefined })];
  const at = (i: number) => {
    const points = liveScatter(snapshotOf(rows)).points;
    return chartPosition(points[i], chartDomain(points), 560, 300);
  };
  const clickChart = (x: number, y: number) => act(async () => {
    $("live-chart")!.dispatchEvent(new MouseEvent("click", { bubbles: true, clientX: x, clientY: y }));
  });

  it("pins the row nearest the click by its identity; a click on no point sends nothing", async () => {
    await render({ snapshot: snapshotOf(rows) });
    await advance(100);
    const p = at(1);
    await clickChart(p.x + 3, p.y - 2);
    await advance(50);
    expect(sent("monitoring_cell_pin")).toEqual([{ cmd: "monitoring_cell_pin", args: { expectedStartGeneration: "0", captureEpoch: "3", frameIndex: "20", objectId: 1 } }]);
    await clickChart(5, 5);
    await advance(50);
    expect(sent("monitoring_cell_pin")).toHaveLength(1);
    expect($("live-scatter-note")!.textContent).toContain("3 valid rows");
  });

  it("says a row without an identity (an older backend) cannot be pinned, and sends nothing", async () => {
    await render({ snapshot: snapshotOf(rows) });
    await advance(100);
    const p = at(2);
    await clickChart(p.x, p.y);
    await advance(50);
    expect(sent("monitoring_cell_pin")).toEqual([]);
    expect($("live-pin-notice")!.textContent).toContain("Not pinned · F30·O1");
    expect($("live-pin-notice")!.textContent).toContain("Nothing was sent");
  });
});

describe("a client without control only watches", () => {
  it("sends no pin, unpin or clear; says why; still reads", async () => {
    setListing({ ring_capture_epoch: "4", pins: [cell("1", { image: null, no_image_reason: ENDED }), cell("2", { capture_epoch: "4" })] , strip: [cell("5", { capture_epoch: "4" })] });
    await render({ controlHeld: false, snapshot: snapshotOf([row(10, { capture_epoch: "4" })]) });
    await advance(100);
    expect($("live-viewer")!.textContent).toContain("only the controlling client pins, unpins or clears them");
    // The strip's crops are not buttons; the scatter takes no click; the pin buttons are disabled with the reason.
    expect(host.querySelector('[aria-label="Cell F5·O1"]')).toBeNull();
    expect(stripCells()).toHaveLength(1);
    const points = liveScatter(snapshotOf([row(10, { capture_epoch: "4" })])).points;
    const p = chartPosition(points[0], chartDomain(points), 560, 300);
    await act(async () => { $("live-chart")!.dispatchEvent(new MouseEvent("click", { bubbles: true, clientX: p.x, clientY: p.y })); });
    for (const id of ["live-unpin", "live-clear-ended", "live-clear-pins"]) {
      const b = $(id) as HTMLButtonElement;
      expect(b.disabled).toBe(true);
      expect(b.title).toContain("only the controlling client");
    }
    await click($("live-clear-ended"));
    await advance(3000);
    expect(calls().filter((c) => CONTROL.includes(c.cmd))).toEqual([]);
    expect(sent("fetch_monitoring_cells").length).toBeGreaterThan(1);
    // The stops need control too.
    expect(($("live-stop-acquisition") as HTMLButtonElement).title).toContain("does not control the instrument");
    expect(($("live-stop-run") as HTMLButtonElement).disabled).toBe(true);
  });
});

describe("Freeze display", () => {
  it("holds the strip and the scatter shown; pins keep updating; nothing is sent", async () => {
    await render({ snapshot: snapshotOf([row(10)]) });
    await advance(100);
    await click($("live-freeze"));
    expect($("live-frozen")).not.toBeNull();
    setListing({ strip: [cell("101"), cell("102"), cell("103"), cell("104")], pins: [cell("102")] });
    await render({ ...setup, snapshot: snapshotOf([row(10), row(20)]) });
    await advance(1100);
    expect(stripNames()).toEqual(["F103·O1", "F102·O1", "F101·O1"]);
    expect($("live-scatter-note")!.textContent).toContain("1 valid rows");
    expect($("live-pin-count")!.textContent).toBe("1 of 16 pins");
    await click($("live-freeze"));
    expect(stripNames()[0]).toBe("F104·O1");
    expect($("live-scatter-note")!.textContent).toContain("2 valid rows");
    expect(calls().filter((c) => CONTROL.includes(c.cmd))).toEqual([]);
  });

  it("stops showing live frames while frozen", async () => {
    vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null); // jsdom has no canvas
    await render();
    const sink = subscribeFrames.mock.calls.filter(([s]) => s !== null).pop()![0] as (p: unknown) => boolean;
    const frame = { valid: true, frame_index: "7", width: 4, height: 3, stride_bytes: 4, data: new Uint8Array(12) };
    expect(sink(frame)).toBe(true);
    await click($("live-freeze"));
    expect(sink(frame)).toBe(false);
  });
});

describe("the two stops", () => {
  it("Stop & finalize run names the active run's generation; a RunMismatch reply is said, nothing else is sent", async () => {
    await render({ status: status({ state: EXPERIMENT_STATES.Active, start_generation: "5", output_path: "/data/run.h5" }), experimentActive: true });
    await advance(100);
    const stop = $("live-stop-run") as HTMLButtonElement;
    expect(stop.disabled).toBe(false);
    stopReply = { ok: false, message: "stop refused: expected run 5, but run 6 is active; nothing was stopped", stop_outcome: EXPERIMENT_STOP_OUTCOMES.RunMismatch };
    await click(stop);
    expect(onStopRun.mock.calls).toEqual([["5"]]);
    expect($("live-stop-notice")!.textContent).toContain("Not stopped · that run is no longer the active one");
    expect($("live-run-chip")!.textContent).toBe("Recording");
    expect($("live-run-file")!.textContent).toBe("run.h5");
    // The camera never stops under a run.
    const acquisition = $("live-stop-acquisition") as HTMLButtonElement;
    expect(acquisition.disabled).toBe(true);
    expect(acquisition.title).toContain("A run is active");
    expect(onStopAcquisition).not.toHaveBeenCalled();
  });

  it("Stop acquisition stops the camera when no run is active; Stop & finalize run has nothing to stop", async () => {
    await render({ status: status(), running: true });
    await advance(100);
    expect(($("live-stop-run") as HTMLButtonElement).disabled).toBe(true);
    expect(($("live-stop-run") as HTMLButtonElement).title).toBe("No run is active.");
    await click($("live-stop-acquisition"));
    expect(onStopAcquisition).toHaveBeenCalledTimes(1);
    expect(onStopRun).not.toHaveBeenCalled();
  });

  it("follows a run's lifecycle from the status alone", async () => {
    await render({ status: status({ state: EXPERIMENT_STATES.Stopping, flushing: true, start_generation: "5", output_path: "/data/run.h5" }) });
    await advance(100);
    const steps = [...host.querySelectorAll("[data-testid=live-stepper] li")].map((li) => li.getAttribute("data-state"));
    expect(steps).toEqual(["done", "done", "current", "pending"]);
    expect($("live-lifecycle")!.textContent).toContain("A run started outside this plan · run.h5");
  });
});

it("on the PZ7035 says the cells carry measurements, not images", async () => {
  setListing({ strip: [cell("5", { image: null, no_image_reason: "no image: the PL reports results only" })] });
  await render({ hostProcessing: false });
  await advance(100);
  expect($("live-pl")).not.toBeNull();
  expect(stripCells()[0].textContent).toContain("Image not recorded");
  expect(sent("fetch_monitoring_cell_packet")).toEqual([]);
});
