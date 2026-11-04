// @vitest-environment jsdom
// Live cell strips (#866 MON-052 .. MON-055) through the component and its hook against a mocked
// bridge: polling gated on visibility, one image pull per new cell, bounded concurrency, the
// reasons under invalid cells, the overlay, the empty and failure states.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import type { MonitoringCell, MonitoringCellReason, MonitoringCellsListing } from "../../monitoringCells";
import { CellStrips } from "./CellStrips";

const { fetchCells, fetchImage } = vi.hoisted(() => ({ fetchCells: vi.fn(), fetchImage: vi.fn() }));
vi.mock("../../bridge", () => ({ bridge: { fetchMonitoringCells: fetchCells, fetchMonitoringCellImage: fetchImage } }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

// A canvas that records what is drawn (jsdom has no 2d context).
const drawn: { canvas: HTMLCanvasElement; data: Uint8ClampedArray }[] = [];
beforeEach(() => {
  vi.useFakeTimers();
  drawn.length = 0;
  fetchCells.mockReset();
  fetchImage.mockReset();
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockImplementation(function (this: HTMLCanvasElement) {
    return {
      createImageData: (w: number, h: number) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }),
      putImageData: (img: { data: Uint8ClampedArray }) => { drawn.push({ canvas: this, data: Uint8ClampedArray.from(img.data) }); },
    } as unknown as CanvasRenderingContext2D;
  } as never);
});
afterEach(() => { vi.useRealTimers(); vi.restoreAllMocks(); });

const W = 6, H = 6;
const reason = (code: MonitoringCellReason["code"], value: number | null, min: number | null, max: number | null): MonitoringCellReason =>
  ({ code, value, min, max });
const cell = (frame: number, extra: Partial<MonitoringCell> = {}): MonitoringCell => ({
  start_generation: "0", capture_epoch: "3", frame_index: String(frame), object_id: 0, timestamp_ns: "0", valid: true,
  target_group: false, object_count: 1, track_id: -1, centroid_x: 3, centroid_y: 3, bbox: { x: 1, y: 1, w: 4, h: 4 },
  area: 100, deformability: 0.1, area_ratio: 1, ring_ratio: 20, youngs_modulus: 0, pixel_to_micron: 0.5, reasons: [],
  image: { width: W, height: H, crop_x: 0, crop_y: 0, source_width: 100, source_height: 50, has_mask: true, bytes: "72" },
  no_image_reason: null, ...extra,
});
const bad = (frame: number, reasons: MonitoringCellReason[], extra: Partial<MonitoringCell> = {}) =>
  cell(frame, { valid: false, reasons, ...extra });
const listing = (over: Partial<MonitoringCellsListing> = {}): MonitoringCellsListing => ({
  valid: true, scope_start_generation: "0", ring_start_generation: "0", ring_capture_epoch: "3", revision: "1",
  strip: [], invalid_strip: [], pins: [], last_capture_change: null, stats: {}, caps: {}, ...over,
});
// A 6 x 6 crop with a 2 x 2 object; its mask marks the object.
const packet = (mask: boolean) => ({
  valid: true, width: W, height: H,
  data: Uint8Array.from({ length: W * H }, (_, i) => {
    const x = i % W, y = Math.floor(i / W), inside = x >= 2 && x <= 3 && y >= 2 && y <= 3;
    return mask ? (inside ? 255 : 0) : 100;
  }),
});
const cellFrame = (args: MonitoringCell) => Number(args.frame_index);

let root: Root | null = null, host: HTMLElement;
async function mount(props: { ready?: boolean; visible?: boolean } = {}) {
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
  await act(async () => { root!.render(<CellStrips ready={props.ready ?? true} visible={props.visible ?? true} />); });
}
const tick = async (ms = 500) => { await act(async () => { await vi.advanceTimersByTimeAsync(ms); }); };
afterEach(async () => { if (root) await act(async () => root!.unmount()); root = null; host?.remove(); });

const thumbs = (kind: "valid" | "invalid") => [...host.querySelectorAll<HTMLElement>(`[data-tour="monitoring-${kind}-cells"] .cell-thumb`)];

it("shows nothing and calls nothing while Monitoring is not visible", async () => {
  fetchCells.mockResolvedValue(listing());
  await mount({ visible: false });
  await tick(2000);
  expect(fetchCells).not.toHaveBeenCalled();
  expect(fetchImage).not.toHaveBeenCalled();
  expect(host.querySelector('[data-tour="monitoring-valid-cells"]')).not.toBeNull();
  expect(host.textContent).toContain("No valid cells yet");
  expect(host.textContent).toContain("No invalid cells yet.");
});

it("does not poll before the backend is ready", async () => {
  await mount({ ready: false });
  await tick(2000);
  expect(fetchCells).not.toHaveBeenCalled();
});

it("lists both strips with their anchors and pulls each new cell's crop and mask once", async () => {
  fetchCells.mockResolvedValue(listing({
    strip: [cell(1), cell(2, { target_group: true })],
    invalid_strip: [bad(11, [reason("area", 154.2, 60, 290), reason("ring", 5, 15, 25)]), bad(12, [reason("no_contour", null, null, null)])],
  }));
  fetchImage.mockImplementation(async (_k: MonitoringCell, layer: string) => packet(layer === "mask"));
  await mount();
  await tick(0);

  expect(host.querySelector('[data-tour="monitoring-cell-strips"]')).not.toBeNull();
  expect(host.querySelector('[data-tour="monitoring-valid-overlay"]')).not.toBeNull();
  expect(host.querySelector('[data-tour="monitoring-invalid-overlay"]')).not.toBeNull();
  expect(thumbs("valid")).toHaveLength(2);
  expect(thumbs("invalid")).toHaveLength(2);
  expect(host.querySelectorAll("canvas")).toHaveLength(4);
  // 4 cells x (image + mask), each once.
  expect(fetchImage).toHaveBeenCalledTimes(8);
  const pulled = fetchImage.mock.calls.map(([k, layer]) => `${String(cellFrame(k)).padStart(2, "0")}:${layer}`).sort();
  expect(pulled).toEqual(["01:image", "01:mask", "02:image", "02:mask", "11:image", "11:mask", "12:image", "12:mask"]);

  // The invalid cells: the short reasons under the image, red, and the tooltip with value and range.
  const [areaRing, noContour] = thumbs("invalid");
  expect(areaRing.querySelector(".cell-thumb-reasons")!.textContent).toBe("Area | Ring");
  expect(areaRing.title).toBe("Area: 154 μm² (range: 60-290)\nRing ratio: 5.0 (range: 15.0-25.0)");
  expect(noContour.querySelector(".cell-thumb-reasons")!.textContent).toBe("No contour");
  expect(noContour.title).toBe("No inner contour found");
  // Valid cells show no reasons.
  expect(thumbs("valid").every(t => t.querySelector(".cell-thumb-reasons") === null)).toBe(true);
  expect(host.textContent).toContain("2 of last 25");
  // The canvas is drawn at the crop's size, scaled to fit the 100 px box.
  const canvas = host.querySelector<HTMLCanvasElement>("canvas")!;
  expect([canvas.width, canvas.height, canvas.style.width, canvas.style.height]).toEqual([W, H, "100px", "100px"]);
});

it("polls again every 500 ms but pulls nothing while the revision stands, and only the new cell after it moves", async () => {
  fetchCells.mockResolvedValue(listing({ revision: "1", strip: [cell(1), cell(2)] }));
  fetchImage.mockImplementation(async (_k: MonitoringCell, layer: string) => packet(layer === "mask"));
  await mount();
  await tick(0);
  expect(fetchImage).toHaveBeenCalledTimes(4);
  const renders = drawn.length;

  await tick(500); await tick(500); await tick(500);
  expect(fetchCells.mock.calls.length).toBeGreaterThanOrEqual(4); // polled
  expect(fetchImage).toHaveBeenCalledTimes(4); // nothing re-pulled
  expect(drawn.length).toBe(renders); // and nothing redrawn

  fetchCells.mockResolvedValue(listing({ revision: "2", strip: [cell(2), cell(3)] })); // 1 left, 3 arrived
  await tick(500);
  expect(fetchImage).toHaveBeenCalledTimes(6);
  expect(fetchImage.mock.calls.slice(4).map(([k]) => cellFrame(k))).toEqual([3, 3]);
  expect(thumbs("valid").map(t => t.dataset.cellKey)).toEqual(["0:3:2:0", "0:3:3:0"]);
});

it("keeps at most four pulls in flight and skips the queued pulls of cells that left the listing", async () => {
  const strip = Array.from({ length: 12 }, (_, i) => cell(100 + i));
  fetchCells.mockResolvedValue(listing({ revision: "1", strip }));
  let inFlight = 0, peak = 0;
  const waiting: (() => void)[] = [];
  fetchImage.mockImplementation((_k: MonitoringCell, layer: string) => new Promise(resolve => {
    inFlight++; peak = Math.max(peak, inFlight);
    waiting.push(() => { inFlight--; resolve(packet(layer === "mask")); });
  }));
  await mount();
  await tick(0);
  expect(peak).toBe(4);
  expect(inFlight).toBe(4);

  // The strip moves on before the queue drained: the newest cells are first in line; the ones
  // that left are never pulled.
  fetchCells.mockResolvedValue(listing({ revision: "2", strip: strip.slice(8) }));
  await tick(500);
  const startedFor = new Set(fetchImage.mock.calls.map(([k]) => cellFrame(k)));
  while (waiting.length) { await act(async () => { waiting.shift()!(); await vi.advanceTimersByTimeAsync(0); }); }
  expect(peak).toBe(4);
  // 4 cells were started before the revision moved (the newest 4 of the first plan: 111..108);
  // afterwards only listed cells (108..111) are pulled, never 100..107.
  for (const f of [100, 101, 102, 103, 104, 105, 106, 107]) expect(startedFor.has(f)).toBe(false);
  expect(thumbs("valid").map(t => t.dataset.cellKey)).toEqual(["0:3:108:0", "0:3:109:0", "0:3:110:0", "0:3:111:0"]);
  expect(host.querySelectorAll("canvas")).toHaveLength(4);
});

it("draws the overlay only when ticked: the tint inside the mask and the contour, per strip", async () => {
  fetchCells.mockResolvedValue(listing({ strip: [cell(1)], invalid_strip: [bad(11, [reason("area", 5, 60, 290)])] }));
  fetchImage.mockImplementation(async (_k: MonitoringCell, layer: string) => packet(layer === "mask"));
  await mount();
  await tick(0);
  const pixel = (frame: "valid" | "invalid", x: number, y: number) => {
    const canvas = host.querySelector(`[data-tour="monitoring-${frame}-cells"] canvas`)!;
    const last = [...drawn].reverse().find(d => d.canvas === canvas)!;
    return Array.from(last.data.slice((y * W + x) * 4, (y * W + x) * 4 + 3));
  };
  expect(pixel("valid", 2, 2)).toEqual([100, 100, 100]); // off by default, as Qt
  const [validBox, invalidBox] = [...host.querySelectorAll<HTMLInputElement>('input[type="checkbox"]')];
  expect([validBox.checked, invalidBox.checked]).toEqual([false, false]);

  await act(async () => { validBox.click(); });
  expect(validBox.checked).toBe(true);
  expect(pixel("valid", 2, 2)).toEqual([0, 255, 0]); // a 2 x 2 object: every pixel of it is on the contour, in the full green
  expect(pixel("valid", 0, 0)).toEqual([100, 100, 100]); // outside the mask: untouched
  expect(pixel("invalid", 2, 2)).toEqual([100, 100, 100]); // the other strip's own switch is still off
  await act(async () => { invalidBox.click(); });
  expect(pixel("invalid", 2, 2)).toEqual([255, 0, 0]); // red for invalid
  await act(async () => { validBox.click(); });
  expect(pixel("valid", 2, 2)).toEqual([100, 100, 100]);
});

it("paints a target-group cell blue", async () => {
  fetchCells.mockResolvedValue(listing({ strip: [cell(1, { target_group: true })] }));
  fetchImage.mockImplementation(async (_k: MonitoringCell, layer: string) => packet(layer === "mask"));
  await mount();
  await tick(0);
  await act(async () => { host.querySelector<HTMLInputElement>('input[type="checkbox"]')!.click(); });
  const last = drawn[drawn.length - 1];
  expect(Array.from(last.data.slice((2 * W + 2) * 4, (2 * W + 2) * 4 + 3))).toEqual([0, 120, 255]);
});

it("says why a cell has no image: the row's own reason, a pull that found it gone, a pull that failed twice", async () => {
  const noImage = cell(1, { image: null, no_image_reason: "no image: this instrument reports cell measurements without images" });
  fetchCells.mockResolvedValue(listing({ strip: [noImage, cell(2), cell(3)] }));
  fetchImage.mockImplementation(async (k: MonitoringCell) => {
    if (cellFrame(k) === 2) throw "CAPTURE_MISMATCH: image refused: frame 2 object 0 is from capture 3, but the current capture is 4";
    throw new Error("socket closed");
  });
  await mount();
  await tick(0);
  await tick(500); // the failed pull of cell 3 is tried once more, by the next poll
  await tick(500);
  const [first, second, third] = thumbs("valid");
  expect(first.querySelector(".cell-thumb-none")!.textContent).toBe("no image");
  expect(first.querySelector<HTMLElement>(".cell-thumb-none")!.title).toContain("reports cell measurements without images");
  expect(second.querySelector<HTMLElement>(".cell-thumb-none")!.title).toContain("the current capture is 4");
  expect(third.querySelector<HTMLElement>(".cell-thumb-none")!.title).toContain("image pull failed: socket closed");
  expect(host.querySelectorAll("canvas")).toHaveLength(0);
  const attemptsOn3 = fetchImage.mock.calls.filter(([k]) => cellFrame(k) === 3).length;
  expect(attemptsOn3).toBe(2); // tried, retried once, then it says why
  await tick(2000);
  expect(fetchImage.mock.calls.filter(([k]) => cellFrame(k) === 3).length).toBe(2);
  expect(fetchImage.mock.calls.filter(([k]) => cellFrame(k) === 2).length).toBe(1); // a gone cell is not retried
});

it("draws the crop without an overlay when the mask is gone or of another size", async () => {
  fetchCells.mockResolvedValue(listing({ strip: [cell(1)] }));
  fetchImage.mockImplementation(async (_k: MonitoringCell, layer: string) =>
    layer === "mask" ? { valid: true, width: 3, height: 3, data: new Uint8Array(9) } : packet(false));
  await mount();
  await tick(0);
  await act(async () => { host.querySelector<HTMLInputElement>('input[type="checkbox"]')!.click(); });
  expect(host.querySelectorAll("canvas")).toHaveLength(1);
  expect(Array.from(drawn[drawn.length - 1].data.slice(0, 3))).toEqual([100, 100, 100]);
});

it("explains an empty capture, an unavailable backend and a failed listing", async () => {
  fetchCells.mockResolvedValue(listing({
    revision: "5", ring_capture_epoch: "4",
    last_capture_change: { capture_epoch: "4", reason: "the camera stopped", pins_ended: 0, pins_dropped: 0, strip_dropped: 7, invalid_strip_dropped: 3 },
  }));
  await mount();
  await tick(0);
  expect(host.querySelector('[role="status"]')!.textContent).toBe("A new capture began (the camera stopped); 10 earlier cells cleared.");
  expect(host.textContent).toContain("No valid cells yet");

  fetchCells.mockResolvedValue({ valid: false });
  await tick(500);
  expect(host.querySelector('[role="status"]')!.textContent).toContain("unavailable: the backend is not initialised");

  fetchCells.mockRejectedValue(new Error("MONITORING_CELLS_INVALID: valid"));
  await tick(500);
  expect(host.querySelector('[role="status"]')!.textContent).toBe("Cell images unavailable: MONITORING_CELLS_INVALID: valid");

  // Recovery: the next good listing clears the notice.
  fetchCells.mockResolvedValue(listing({ revision: "9", strip: [cell(1)] }));
  fetchImage.mockImplementation(async (_k: MonitoringCell, layer: string) => packet(layer === "mask"));
  await tick(500);
  expect(host.querySelector('[role="status"]')).toBeNull();
  expect(thumbs("valid")).toHaveLength(1);
});

it("lays the cells out five to a row, up to 25 per strip, in a scrollable box", async () => {
  fetchCells.mockResolvedValue(listing({ strip: Array.from({ length: 25 }, (_, i) => cell(i + 1)) }));
  fetchImage.mockImplementation(async (_k: MonitoringCell, layer: string) => packet(layer === "mask"));
  await mount();
  await tick(0);
  await tick(0);
  const grid = host.querySelector<HTMLElement>('[data-tour="monitoring-valid-cells"] .cell-grid')!;
  expect(grid.style.gridTemplateColumns).toBe("repeat(5, 100px)");
  expect(grid.children).toHaveLength(25);
  expect(host.querySelector('[data-tour="monitoring-valid-cells"] .cell-strip-scroll')).not.toBeNull();
  expect(host.textContent).toContain("25 of last 25");
  expect(fetchImage).toHaveBeenCalledTimes(50);
});

it("stops polling when it goes away and while it is hidden again", async () => {
  fetchCells.mockResolvedValue(listing());
  await mount();
  await tick(0);
  const calls = fetchCells.mock.calls.length;
  await act(async () => root!.render(<CellStrips ready visible={false} />));
  await tick(3000);
  expect(fetchCells.mock.calls.length).toBe(calls);
});

it("hands the valid strip's Show Overlay to the caller, who keeps the flag (the selected cell's pane follows it)", async () => {
  fetchCells.mockResolvedValue(listing({ strip: [cell(1)], invalid_strip: [bad(11, [reason("no_contour", null, null, null)])] }));
  fetchImage.mockImplementation(async (_k: MonitoringCell, layer: string) => packet(layer === "mask"));
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
  const seen: boolean[] = [];
  const view = (on: boolean) => <CellStrips ready visible validOverlay={on} onValidOverlay={(v) => seen.push(v)} />;
  await act(async () => { root!.render(view(false)); });
  await tick(0);
  const box = (kind: string) => host.querySelector<HTMLInputElement>(`[data-tour="monitoring-${kind}-overlay"]`)!;
  expect(box("valid").checked).toBe(false);
  await act(async () => box("valid").click());
  expect(seen).toEqual([true]);
  expect(box("valid").checked).toBe(false); // controlled: it shows what the caller says
  await act(async () => { root!.render(view(true)); });
  expect(box("valid").checked).toBe(true);
  expect(box("invalid").checked).toBe(false); // the invalid strip keeps its own flag
  await act(async () => box("invalid").click());
  expect(box("invalid").checked).toBe(true);
  expect(seen).toEqual([true]);
});
