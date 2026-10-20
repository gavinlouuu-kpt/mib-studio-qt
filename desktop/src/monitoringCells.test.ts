// Bridge ABI 41 (#716 A6a): the live cell client against a mocked transport. Identities travel as
// canonical decimal strings, malformed ones never reach the transport, the listing and pin replies
// are checked against the contract, and the pixels come back as frame packets of kind 5 / 6.
import { beforeEach, expect, it, vi } from "vitest";
import cellGolden from "../../crates/mib-bridge/contract/fixtures/monitoring-cell-v2.json";
import { bridge } from "./bridge";
import { MONITORING_CELLS, MONITORING_CELL_PIN_OUTCOMES } from "./bridgeContract";
import { cellKeyArgs, cellKeyString, decodeMonitoringCellPinReply, decodeMonitoringCells, pinOutcomeName } from "./monitoringCells";

const { invoke } = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
beforeEach(() => { invoke.mockReset(); });

const cell = (frame: string, extra: Record<string, unknown> = {}) => ({
  start_generation: "18446744073709551615", capture_epoch: "3", frame_index: frame, object_id: 0, timestamp_ns: "9007199254740993",
  valid: true, target_group: false, object_count: 1, track_id: -1, centroid_x: 50, centroid_y: 38,
  bbox: { x: 40, y: 30, w: 20, h: 16 }, area: 250, deformability: 0.05, area_ratio: 1.02, ring_ratio: null,
  youngs_modulus: 0, pixel_to_micron: 0.5,
  image: { width: 40, height: 36, crop_x: 30, crop_y: 20, source_width: 512, source_height: 96, has_mask: true, bytes: "2880" },
  no_image_reason: null, ...extra,
});
const listing = (extra: Record<string, unknown> = {}) => ({
  valid: true, scope_start_generation: "18446744073709551615", ring_start_generation: "0", ring_capture_epoch: "3", revision: "17",
  strip: [cell("9007199254740993")],
  pins: [cell("12", { image: null, no_image_reason: "no image: the frame left the monitoring buffer before it was pinned" })],
  stats: { strip_cells: 1, strip_bytes: "2880", pins: 1, pin_bytes: "0" },
  caps: { strip_max_cells: 24, max_pins: 16 }, ...extra,
});
const golden = () => Uint8Array.from(cellGolden.hex.match(/../g)!, h => parseInt(h, 16)).buffer;

it("pins the contract's outcomes and caps", () => {
  expect(MONITORING_CELL_PIN_OUTCOMES).toEqual({ Pinned: 0, PinnedWithoutImage: 1, AlreadyPinned: 2, RunMismatch: 3, NotFound: 4, LimitReached: 5, CaptureMismatch: 6 });
  expect(MONITORING_CELLS).toMatchObject({ strip_max_cells: 24, max_pins: 16, crop_max_side: 256, strip_max_bytes: 4194304, pin_max_bytes: 4194304 });
  expect(pinOutcomeName(3)).toBe("RunMismatch");
  expect(pinOutcomeName(6)).toBe("CaptureMismatch");
  expect(pinOutcomeName(undefined)).toBe("unknown");
});

it("fetches and checks the listing: exact identities, image xor reason", async () => {
  invoke.mockResolvedValueOnce(listing());
  const l = await bridge.fetchMonitoringCells();
  expect(invoke).toHaveBeenLastCalledWith("fetch_monitoring_cells");
  if (!l.valid) throw new Error("valid listing expected");
  expect(l.scope_start_generation).toBe("18446744073709551615");
  expect([l.ring_capture_epoch, l.strip[0].capture_epoch]).toEqual(["3", "3"]);
  expect(l.strip[0].frame_index).toBe("9007199254740993");
  expect(l.strip[0].image?.has_mask).toBe(true);
  expect(l.pins[0].image).toBeNull();
  expect(l.pins[0].no_image_reason).toContain("left the monitoring buffer");
  expect(l.strip[0].ring_ratio).toBeNull();
  invoke.mockResolvedValueOnce({ valid: false });
  expect(await bridge.fetchMonitoringCells()).toEqual({ valid: false });
});

it.each([
  ["a rounded identity", { strip: [cell("9007199254740993", { frame_index: 9007199254740993 })] }],
  ["image and reason together", { strip: [cell("1", { no_image_reason: "x" })] }],
  ["neither image nor reason", { strip: [cell("1", { image: null })] }],
  ["a crop larger than crop_max_side", { strip: [cell("1", { image: { ...cell("1").image, width: 257 } })] }],
  ["a strip over its cap", { strip: Array.from({ length: 25 }, (_, i) => cell(String(i))) }],
  ["pins over their cap", { pins: Array.from({ length: 17 }, (_, i) => cell(String(i))) }],
  ["a non-canonical generation", { scope_start_generation: "07" }],
  ["a cell without its capture epoch", { strip: [cell("1", { capture_epoch: undefined })] }],
  ["a listing without the ring's capture epoch", { ring_capture_epoch: 3 }],
])("rejects %s", async (_why, extra) => {
  invoke.mockResolvedValueOnce(listing(extra));
  await expect(bridge.fetchMonitoringCells()).rejects.toThrow("MONITORING_CELLS_INVALID");
});

it("pins by identity with decimal strings; refusals keep their message", async () => {
  invoke.mockResolvedValueOnce({ ok: true, outcome: 0, message: "pinned", cell: cell("9007199254740993"), revision: "18" });
  const r = await bridge.monitoringCellPin({ start_generation: "18446744073709551615", capture_epoch: "3", frame_index: "9007199254740993", object_id: 2 });
  expect(invoke).toHaveBeenLastCalledWith("monitoring_cell_pin",
    { expectedStartGeneration: "18446744073709551615", captureEpoch: "3", frameIndex: "9007199254740993", objectId: 2 });
  expect([r.ok, r.outcome, r.cell?.frame_index, r.revision]).toEqual([true, 0, "9007199254740993", "18"]);

  // A snapshot row (frame_index is a JSON number there) works as the key.
  invoke.mockResolvedValueOnce({ ok: false, outcome: 3, message: "pin refused: run 4 is not the latest run started (run 5)", cell: null, revision: "18" });
  const refused = await bridge.monitoringCellPin({ start_generation: "4", capture_epoch: "2", frame_index: 1234, object_id: 0 });
  expect(invoke).toHaveBeenLastCalledWith("monitoring_cell_pin", { expectedStartGeneration: "4", captureEpoch: "2", frameIndex: "1234", objectId: 0 });
  expect([refused.ok, refused.outcome, refused.cell]).toEqual([false, 3, null]);
  expect(refused.message).toContain("not the latest run");

  // Before init the backend never evaluated it: no outcome.
  invoke.mockResolvedValueOnce({ ok: false, message: "Backend not initialized", cell: null });
  expect((await bridge.monitoringCellPin({ start_generation: "0", capture_epoch: "1", frame_index: "1", object_id: 0 })).outcome).toBeUndefined();
  // An earlier capture's identity: refused with its own outcome.
  invoke.mockResolvedValueOnce({ ok: false, outcome: 6, message: "pin refused: frame 1 object 0 is from capture 1, but the current capture is 2", cell: null, revision: "18" });
  const stale = await bridge.monitoringCellPin({ start_generation: "0", capture_epoch: "1", frame_index: "1", object_id: 0 });
  expect([stale.ok, stale.outcome, pinOutcomeName(stale.outcome)]).toEqual([false, 6, "CaptureMismatch"]);
  // An outcome this client does not know is rejected, not guessed.
  expect(() => decodeMonitoringCellPinReply({ ok: true, outcome: 9, message: "", cell: null })).toThrow("unknown pin outcome");
});

it("never sends a malformed identity", async () => {
  for (const bad of [
    { start_generation: "07", capture_epoch: "1", frame_index: "1", object_id: 0 },
    { start_generation: "1", capture_epoch: "1", frame_index: 9007199254740992, object_id: 0 },
    { start_generation: "1", capture_epoch: "1", frame_index: "-1", object_id: 0 },
    { start_generation: "1", capture_epoch: "1", frame_index: "1", object_id: 1.5 },
    { start_generation: "1", capture_epoch: "1", frame_index: "1", object_id: 2147483648 },
    { start_generation: "1", capture_epoch: "01", frame_index: "1", object_id: 0 },
    { start_generation: "1", capture_epoch: -1, frame_index: "1", object_id: 0 },
  ]) {
    await expect(bridge.monitoringCellPin(bad)).rejects.toThrow(/INVALID_U64|INVALID_OBJECT_ID/);
    expect(() => bridge.monitoringCellUnpin(bad)).toThrow(/INVALID_U64|INVALID_OBJECT_ID/);
    await expect(bridge.fetchMonitoringCellImage(bad)).rejects.toThrow(/INVALID_U64|INVALID_OBJECT_ID/);
  }
  expect(invoke).not.toHaveBeenCalled();
  expect(cellKeyArgs({ start_generation: 0n, capture_epoch: 4n, frame_index: 5n, object_id: -1 }))
    .toEqual({ startGeneration: "0", captureEpoch: "4", frameIndex: "5", objectId: -1 });
  expect(cellKeyString({ start_generation: "3", capture_epoch: 2, frame_index: 9, object_id: 1 })).toBe("3:2:9:1");
});

it("unpins and clears through command results", async () => {
  invoke.mockResolvedValueOnce({ transport_version: 1, ok: true, command: 10, message: "unpinned", operation_id: "0" });
  expect((await bridge.monitoringCellUnpin({ start_generation: "5", capture_epoch: "2", frame_index: "12", object_id: 0 })).message).toBe("unpinned");
  expect(invoke).toHaveBeenLastCalledWith("monitoring_cell_unpin", { startGeneration: "5", captureEpoch: "2", frameIndex: "12", objectId: 0 });
  invoke.mockResolvedValueOnce({ transport_version: 1, ok: true, command: 10, message: "cleared 3 pin(s)", operation_id: "0" });
  expect((await bridge.monitoringCellClearPins()).message).toBe("cleared 3 pin(s)");
  expect(invoke.mock.lastCall?.[0]).toBe("monitoring_cell_clear_pins");
});

it("pulls the crop and the mask as frame packets of their own kinds", async () => {
  invoke.mockResolvedValueOnce(golden());
  const image = await bridge.fetchMonitoringCellImage({ start_generation: "1", capture_epoch: "2", frame_index: cellGolden.frame_index, object_id: 0 });
  expect(invoke).toHaveBeenLastCalledWith("fetch_monitoring_cell_packet",
    { startGeneration: "1", captureEpoch: "2", frameIndex: cellGolden.frame_index, objectId: 0, layer: "image" });
  expect([image.valid, image.pull_kind, image.width, image.height]).toEqual([true, 5, 3, 2]);
  expect(Array.from(image.data)).toEqual(cellGolden.pixels);

  const mask = golden();
  new DataView(mask).setUint32(12, 6, true);
  invoke.mockResolvedValueOnce(mask);
  const m = await bridge.fetchMonitoringCellImage({ start_generation: "1", capture_epoch: "2", frame_index: "1", object_id: 0 }, "mask");
  expect(invoke.mock.lastCall?.[1]).toMatchObject({ layer: "mask" });
  expect(m.pull_kind).toBe(6);

  // An image reply where a mask was asked for is refused.
  invoke.mockResolvedValueOnce(golden());
  await expect(bridge.fetchMonitoringCellImage({ start_generation: "1", capture_epoch: "2", frame_index: "1", object_id: 0 }, "mask"))
    .rejects.toThrow("FRAME_PACKET_INVALID_FLAGS_OR_SOURCE");

  // Not held: an empty packet, never a substitute.
  const empty = new ArrayBuffer(96), d = new DataView(empty);
  d.setUint32(0, 0x4642494d, true); d.setUint16(4, 2, true); d.setUint16(6, 96, true); d.setUint32(12, 5, true);
  invoke.mockResolvedValueOnce(empty);
  const none = await bridge.fetchMonitoringCellImage({ start_generation: "1", capture_epoch: "2", frame_index: "1", object_id: 0 });
  expect([none.valid, none.data.length]).toEqual([false, 0]);
});

it("decodes a listing straight from the decoder too", () => {
  const l = decodeMonitoringCells(listing({ strip: [], pins: [] }));
  expect(l.valid && l.strip.length === 0 && l.pins.length === 0).toBe(true);
});
