// @vitest-environment jsdom
// The pin / fetch / unpin lifecycle behind the selected-cell pane (MON-044, MON-048) against a mocked bridge: pin before
// fetching, release when the selection moves, on close and on unmount, no leaked pin when the selection moves while a pin
// is in flight, every outcome of the pin, and a client that cannot control the instrument.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { MonitoringRow } from "../../../bridge";
import { MONITORING_CELL_PIN_OUTCOMES as O } from "../../../bridgeContract";
import type { MonitoringCell, MonitoringCellPinReply } from "../../../monitoringCells";
import { useCellView, type CellViewState } from "./useCellView";

const bridge = vi.hoisted(() => ({ monitoringCellPin: vi.fn(), monitoringCellUnpin: vi.fn(), fetchMonitoringCellImage: vi.fn() }));
vi.mock("../../../bridge", () => ({ bridge }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const row = (frame: number, over: Partial<MonitoringRow> = {}): MonitoringRow => ({
  frame_index: frame, timestamp_ns: frame, valid: true, target_group: false, object_id: 0, object_count: 1, track_id: 0, centroid_x: 0, centroid_y: 0,
  area: 100, deformability: 0.2, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 1, start_generation: "1", capture_epoch: "2", ...over,
});
const W = 4, H = 3;
const cellOf = (r: MonitoringRow, over: Partial<MonitoringCell> = {}): MonitoringCell => ({
  start_generation: "1", capture_epoch: "2", frame_index: String(r.frame_index), object_id: r.object_id, timestamp_ns: "0", valid: true, target_group: false,
  object_count: 1, track_id: 0, centroid_x: 1, centroid_y: 1, bbox: { x: 0, y: 0, w: 2, h: 2 }, area: 100, deformability: 0.2, area_ratio: 1, ring_ratio: 20,
  youngs_modulus: 0, pixel_to_micron: 1, reasons: [],
  image: { width: W, height: H, crop_x: 0, crop_y: 0, source_width: 100, source_height: 50, has_mask: true, bytes: "12" }, no_image_reason: null, ...over,
});
const pinReply = (r: MonitoringRow, over: Partial<MonitoringCellPinReply> = {}): MonitoringCellPinReply =>
  ({ ok: true, outcome: O.Pinned, message: "pinned", cell: cellOf(r), ...over });
const packet = (fill: number) => ({ valid: true, width: W, height: H, data: new Uint8Array(W * H).fill(fill) });

let calls: string[];
let host: HTMLDivElement, root: Root;
let latest: CellViewState;
const log = vi.fn();

function Probe(props: { selection: MonitoringRow | null; ready?: boolean; canPin?: boolean }) {
  latest = useCellView({ selection: props.selection, ready: props.ready ?? true, canPin: props.canPin ?? true, log });
  return <div data-phase={latest.phase} />;
}
const show = (props: Parameters<typeof Probe>[0]) => act(async () => root.render(<Probe {...props} />));
/** Let the queued pin / unpin calls and the pulls that follow them run. */
const settle = () => act(async () => { for (let i = 0; i < 12; i++) await new Promise((r) => setTimeout(r, 0)); });
const sent = () => calls.filter((c) => c.startsWith("pin") || c.startsWith("unpin"));

beforeEach(() => {
  calls = [];
  log.mockReset();
  for (const f of Object.values(bridge)) f.mockReset();
  bridge.monitoringCellPin.mockImplementation(async (k) => { calls.push(`pin ${k.frame_index}`); return pinReply(row(Number(k.frame_index))); });
  bridge.monitoringCellUnpin.mockImplementation(async (k) => { calls.push(`unpin ${k.frame_index}`); return { ok: true, message: "unpinned" }; });
  bridge.fetchMonitoringCellImage.mockImplementation(async (k, layer) => { calls.push(`fetch ${layer} ${k.frame_index}`); return packet(layer === "mask" ? 255 : 90); });
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("showing a cell", () => {
  it("pins before it fetches, then pulls the crop and its mask by identity", async () => {
    let finish!: (r: MonitoringCellPinReply) => void;
    bridge.monitoringCellPin.mockImplementationOnce(async (k) => { calls.push(`pin ${k.frame_index}`); return new Promise<MonitoringCellPinReply>((r) => { finish = r; }); });
    await show({ selection: row(10) });
    await settle();
    expect(latest.phase).toBe("loading"); // the pin is with the backend; nothing is pulled before it answers
    expect(calls).toEqual(["pin 10"]);
    await act(async () => finish(pinReply(row(10))));
    await settle();
    expect(calls).toEqual(["pin 10", "fetch image 10", "fetch mask 10"]);
    expect(bridge.monitoringCellPin).toHaveBeenCalledWith(expect.objectContaining({ start_generation: "1", capture_epoch: "2", frame_index: 10, object_id: 0 }));
    expect(latest).toMatchObject({ phase: "ready", pinned: true });
    if (latest.phase === "ready") {
      expect([latest.pixels.width, latest.pixels.height, latest.pixels.image[0], latest.pixels.mask?.[0]]).toEqual([W, H, 90, 255]);
    }
  });
  it("does not fetch a mask the pinned cell does not have", async () => {
    bridge.monitoringCellPin.mockImplementation(async (k) => { calls.push(`pin ${k.frame_index}`); const r = row(10); return pinReply(r, { cell: cellOf(r, { image: { ...cellOf(r).image!, has_mask: false } }) }); });
    await show({ selection: row(10) });
    await settle();
    expect(calls).toEqual(["pin 10", "fetch image 10"]);
    expect(latest).toMatchObject({ phase: "ready" });
  });
  it("fetches the image once per selection, not per render", async () => {
    await show({ selection: row(10) });
    await settle();
    await show({ selection: row(10, { deformability: 0.9 }) }); // the same cell again (a new snapshot hands over a copy of the row)
    await settle();
    expect(calls).toEqual(["pin 10", "fetch image 10", "fetch mask 10"]);
  });
  it("is idle with nothing selected or before the backend is ready, and sends nothing", async () => {
    await show({ selection: null });
    await settle();
    expect(latest.phase).toBe("idle");
    await show({ selection: row(10), ready: false });
    await settle();
    expect(latest.phase).toBe("idle");
    expect(calls).toEqual([]);
  });
});

describe("never leaking a pin", () => {
  it("releases the previous pin when the selection moves, before it pins the next", async () => {
    await show({ selection: row(10) });
    await settle();
    await show({ selection: row(11) });
    await settle();
    expect(sent()).toEqual(["pin 10", "unpin 10", "pin 11"]);
    expect(latest.phase).toBe("ready");
  });
  it("releases the pin on close (the selection goes to null) and on unmount", async () => {
    await show({ selection: row(10) });
    await settle();
    await show({ selection: null });
    await settle();
    expect(sent()).toEqual(["pin 10", "unpin 10"]);
    expect(latest.phase).toBe("idle");
    await show({ selection: row(12) });
    await settle();
    await act(async () => root.unmount());
    await settle();
    expect(sent()).toEqual(["pin 10", "unpin 10", "pin 12", "unpin 12"]);
    root = createRoot(host);
  });
  it("unpins a stale pin when the selection moved while the pin was in flight, and never shows the stale cell", async () => {
    let finish!: (r: MonitoringCellPinReply) => void;
    bridge.monitoringCellPin.mockImplementationOnce(async (k) => { calls.push(`pin ${k.frame_index}`); return new Promise<MonitoringCellPinReply>((r) => { finish = r; }); });
    await show({ selection: row(10) });
    await settle();
    await show({ selection: row(11) }); // moves on while the backend is still pinning 10
    await settle();
    expect(sent()).toEqual(["pin 10"]); // 11 waits its turn: the calls reach the backend one after the other
    await act(async () => finish(pinReply(row(10))));
    await settle();
    expect(sent()).toEqual(["pin 10", "unpin 10", "pin 11"]);
    expect(bridge.monitoringCellUnpin).toHaveBeenCalledTimes(1);
    expect(calls.filter((c) => c.startsWith("fetch"))).toEqual(["fetch image 11", "fetch mask 11"]); // 10's pixels were never pulled
    expect(latest).toMatchObject({ phase: "ready" });
  });
  it("never sends the pin of a selection that was replaced before its turn", async () => {
    let finish!: (r: MonitoringCellPinReply) => void;
    bridge.monitoringCellPin.mockImplementationOnce(async (k) => { calls.push(`pin ${k.frame_index}`); return new Promise<MonitoringCellPinReply>((r) => { finish = r; }); });
    await show({ selection: row(10) });
    await settle();
    await show({ selection: row(11) });
    await show({ selection: row(12) });
    await act(async () => finish(pinReply(row(10))));
    await settle();
    expect(sent()).toEqual(["pin 10", "unpin 10", "pin 12"]);
  });
  it("does not release a pin that was already held (it is not this view's to give back)", async () => {
    bridge.monitoringCellPin.mockImplementation(async (k) => { calls.push(`pin ${k.frame_index}`); return pinReply(row(Number(k.frame_index)), { outcome: O.AlreadyPinned, message: "already pinned" }); });
    await show({ selection: row(10) });
    await settle();
    await show({ selection: null });
    await settle();
    expect(sent()).toEqual(["pin 10"]);
    expect(latest.phase).toBe("idle");
  });
  it("logs a pin that could not be released", async () => {
    bridge.monitoringCellUnpin.mockRejectedValue(new Error("VIEWER_ONLY: another client controls the instrument"));
    await show({ selection: row(10) });
    await settle();
    await show({ selection: null });
    await settle();
    expect(log).toHaveBeenCalledWith(expect.stringContaining("the pin of frame 10 could not be released: VIEWER_ONLY"));
  });
});

describe("what the pin says", () => {
  it("PinnedWithoutImage shows the measurements with the reason, fetches nothing, and still releases the pin", async () => {
    bridge.monitoringCellPin.mockImplementation(async (k) => {
      calls.push(`pin ${k.frame_index}`);
      const r = row(10);
      return pinReply(r, { outcome: O.PinnedWithoutImage, message: "the frame left the ring before the pin", cell: cellOf(r, { image: null, no_image_reason: "the frame left the ring" }) });
    });
    await show({ selection: row(10) });
    await settle();
    expect(latest).toMatchObject({ phase: "measurements", reason: "the frame left the ring before the pin" });
    expect(calls.filter((c) => c.startsWith("fetch"))).toEqual([]);
    await show({ selection: null });
    await settle();
    expect(sent()).toEqual(["pin 10", "unpin 10"]);
  });
  it.each([
    [O.CaptureMismatch, "capture 2 ended: the camera stopped", "capture 2 ended: the camera stopped"],
    [O.RunMismatch, "", "another run"],
    [O.NotFound, "", "no longer holds"],
    [O.LimitReached, "", "pinned cells"],
  ])("outcome %s is a plain message and nothing to release", async (outcome, message, expected) => {
    bridge.monitoringCellPin.mockImplementation(async (k) => { calls.push(`pin ${k.frame_index}`); return { ok: false, outcome, message, cell: null }; });
    await show({ selection: row(10) });
    await settle();
    expect(latest.phase).toBe("refused");
    expect((latest as { message: string }).message).toContain(expected);
    await show({ selection: null });
    await settle();
    expect(sent()).toEqual(["pin 10"]);
  });
  it("a pin that throws is a message", async () => {
    bridge.monitoringCellPin.mockRejectedValue(new Error("INVALID_U64: unsafe number"));
    await show({ selection: row(10) });
    await settle();
    expect(latest).toMatchObject({ phase: "refused", message: "The cell could not be pinned: INVALID_U64: unsafe number" });
  });
  it("an image the capture change took is the backend's reason, with the measurements", async () => {
    bridge.fetchMonitoringCellImage.mockRejectedValue("CAPTURE_MISMATCH: image refused: the camera stopped");
    await show({ selection: row(10) });
    await settle();
    expect(latest).toMatchObject({ phase: "measurements", reason: "image refused: the camera stopped" });
  });
  it("an image the backend no longer holds says so", async () => {
    bridge.fetchMonitoringCellImage.mockResolvedValue({ valid: false, width: 0, height: 0, data: new Uint8Array(0) });
    await show({ selection: row(10) });
    await settle();
    expect(latest).toMatchObject({ phase: "measurements", reason: "the backend no longer holds this cell's image" });
  });
  it("a mask of another size is no overlay", async () => {
    bridge.fetchMonitoringCellImage.mockImplementation(async (_k, layer) => (layer === "mask" ? { valid: true, width: 2, height: 2, data: new Uint8Array(4) } : packet(90)));
    await show({ selection: row(10) });
    await settle();
    expect(latest).toMatchObject({ phase: "ready" });
    if (latest.phase === "ready") expect(latest.pixels.mask).toBeNull();
  });
});

describe("a client that does not control the instrument", () => {
  it("does not pin (a control command): it pulls what the strips still hold, and releases nothing", async () => {
    await show({ selection: row(10), canPin: false });
    await settle();
    expect(calls).toEqual(["fetch image 10", "fetch mask 10"]);
    expect(latest).toMatchObject({ phase: "ready", pinned: false });
    await show({ selection: null, canPin: false });
    await settle();
    expect(bridge.monitoringCellUnpin).not.toHaveBeenCalled();
  });
  it("says why when the backend has let go of the cell", async () => {
    bridge.fetchMonitoringCellImage.mockRejectedValue("CAPTURE_MISMATCH: image refused: the camera stopped");
    await show({ selection: row(10), canPin: false });
    await settle();
    expect(latest).toMatchObject({ phase: "measurements" });
  });
  it("falls back to the pull when the server refuses the pin as VIEWER_ONLY", async () => {
    bridge.monitoringCellPin.mockRejectedValue(new Error("VIEWER_ONLY: another client controls the instrument (take_control to claim it)"));
    await show({ selection: row(10) });
    await settle();
    expect(latest).toMatchObject({ phase: "ready", pinned: false });
    await show({ selection: null });
    await settle();
    expect(bridge.monitoringCellUnpin).not.toHaveBeenCalled();
  });
});
