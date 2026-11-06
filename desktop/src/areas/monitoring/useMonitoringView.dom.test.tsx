// @vitest-environment jsdom
// The E-modulus table fetch (MON-043, 064, 066, 068, 077): read when the Monitoring sub-tab becomes visible and the backend is
// ready, with the identity already held so the grid comes back only when it differs, never per poll; and Clear Buffer clearing
// the selection (MON-048).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { EmodulusGrid, EmodulusLutInfo } from "../../monitoringDensity";
import { useMonitoringState } from "./useMonitoring";
import { useEmodulusTable, useSelectionReset } from "./useMonitoringView";
import { lookupEmodulus, type EmodulusTableState } from "./emodulusLut";

const bridge = vi.hoisted(() => ({ fetchEmodulusLut: vi.fn() }));
vi.mock("../../bridge", () => ({ bridge }));
Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

const info: EmodulusLutInfo = { path: "/data/lut.txt", sha256: "ab", lut_id: "lut-1", revision: "1", source: "bundled", catalog_sha256: "", checksum_status: "ok" };
// 3 x 2 points: the last cell (area 100..200) has a corner with no value.
const grid = (scale = 1): EmodulusGrid => ({ area_min: 0, area_step: 100, area_bins: 3, deform_min: 0, deform_step: 0.5, deform_bins: 2, values: [1, 2, 3, 4, 5, null].map((v) => (v === null ? v : v * scale)) });
const loadedReply = (identity: string, scale = 1) => ({ valid: true as const, loaded: true as const, unchanged: false as const, identity, info, lut: grid(scale) });
const unchangedReply = (identity: string) => ({ valid: true as const, loaded: true as const, unchanged: true as const, identity, info });

let host: HTMLDivElement, root: Root;
let table: EmodulusTableState;
let notify: () => void;
let state: ReturnType<typeof useMonitoringState>;
const append = vi.fn();

function Harness({ ready, visible }: { ready: boolean; visible: boolean }) {
  const m = useMonitoringState();
  state = m;
  table = m.emodulusTable;
  notify = m.notifyMonitoringCleared;
  useEmodulusTable({ ready, append, monitoringVisible: visible, emodulusTable: m.emodulusTable, setEmodulusTable: m.setEmodulusTable });
  useSelectionReset(m);
  return null;
}
const render = (props: { ready: boolean; visible: boolean }) => act(async () => root.render(<Harness {...props} />));
const settle = () => act(async () => { for (let i = 0; i < 8; i++) await new Promise((r) => setTimeout(r, 0)); });

beforeEach(() => {
  bridge.fetchEmodulusLut.mockReset();
  bridge.fetchEmodulusLut.mockResolvedValue({ valid: false });
  append.mockReset();
  host = document.createElement("div");
  document.body.append(host);
  root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

describe("the E-modulus table fetch", () => {
  it("is not asked for before the backend is ready or while the sub-tab is hidden", async () => {
    await render({ ready: false, visible: true });
    await render({ ready: true, visible: false });
    await settle();
    expect(bridge.fetchEmodulusLut).not.toHaveBeenCalled();
    expect(table.status).toBe("unknown");
  });
  it("reads the table once when the sub-tab becomes visible, and keeps the grid ready for lookups", async () => {
    bridge.fetchEmodulusLut.mockResolvedValue(loadedReply("ab|/data/lut.txt"));
    await render({ ready: true, visible: false });
    await render({ ready: true, visible: true });
    await settle();
    expect(bridge.fetchEmodulusLut).toHaveBeenCalledTimes(1);
    expect(bridge.fetchEmodulusLut).toHaveBeenCalledWith(null);
    expect(table.status).toBe("loaded");
    if (table.status === "loaded") {
      expect(table.table.identity).toBe("ab|/data/lut.txt");
      expect(table.table.info.lut_id).toBe("lut-1");
      expect(lookupEmodulus(table.table.lut, 50, 0)).toBeCloseTo(1.5, 12);
      expect(lookupEmodulus(table.table.lut, 150, 0.25)).toBeNaN(); // the cell with a corner that has no value
    }
  });
  it("never fetches per poll: a visible sub-tab asks once however long it stays", async () => {
    vi.useFakeTimers();
    bridge.fetchEmodulusLut.mockResolvedValue(loadedReply("ab|/data/lut.txt"));
    await render({ ready: true, visible: true });
    await act(async () => { await vi.advanceTimersByTimeAsync(10_000); });
    expect(bridge.fetchEmodulusLut).toHaveBeenCalledTimes(1);
  });
  it("sends the identity it holds on the next visit and keeps the table when the backend says it is unchanged", async () => {
    bridge.fetchEmodulusLut.mockResolvedValue(loadedReply("ab|/data/lut.txt"));
    await render({ ready: true, visible: true });
    await settle();
    const first = table;
    bridge.fetchEmodulusLut.mockResolvedValue(unchangedReply("ab|/data/lut.txt"));
    await render({ ready: true, visible: false });
    await render({ ready: true, visible: true });
    await settle();
    expect(bridge.fetchEmodulusLut).toHaveBeenLastCalledWith("ab|/data/lut.txt");
    expect(table).toBe(first); // no new grid, no new state
  });
  it("takes a changed table", async () => {
    bridge.fetchEmodulusLut.mockResolvedValue(loadedReply("ab|/data/lut.txt"));
    await render({ ready: true, visible: true });
    await settle();
    bridge.fetchEmodulusLut.mockResolvedValue(loadedReply("cd|/data/other.txt", 2));
    await render({ ready: true, visible: false });
    await render({ ready: true, visible: true });
    await settle();
    expect(table.status === "loaded" && table.table.identity).toBe("cd|/data/other.txt");
    expect(table.status === "loaded" && lookupEmodulus(table.table.lut, 0, 0)).toBe(2);
  });
  it("records that there is no table (Qt's no-table case)", async () => {
    bridge.fetchEmodulusLut.mockResolvedValue({ valid: true, loaded: false });
    await render({ ready: true, visible: true });
    await settle();
    expect(table).toEqual({ status: "none" });
  });
  it("leaves the state alone when the backend is not initialised, and asks again at the next visit", async () => {
    bridge.fetchEmodulusLut.mockResolvedValue({ valid: false, message: "not initialised" });
    await render({ ready: true, visible: true });
    await settle();
    expect(table.status).toBe("unknown");
    bridge.fetchEmodulusLut.mockResolvedValue(loadedReply("ab|/data/lut.txt"));
    await render({ ready: true, visible: false });
    await render({ ready: true, visible: true });
    await settle();
    expect(table.status).toBe("loaded");
  });
  it("logs a failed read and says so; a table already held stays usable", async () => {
    bridge.fetchEmodulusLut.mockRejectedValue(new Error("EMODULUS_LUT_INVALID: lut.values is not area_bins x deform_bins"));
    await render({ ready: true, visible: true });
    await settle();
    expect(table).toEqual({ status: "error", message: "EMODULUS_LUT_INVALID: lut.values is not area_bins x deform_bins" });
    expect(append).toHaveBeenCalledWith(expect.stringContaining("E-modulus table could not be read"));
    bridge.fetchEmodulusLut.mockResolvedValue(loadedReply("ab|/data/lut.txt"));
    await render({ ready: true, visible: false });
    await render({ ready: true, visible: true });
    await settle();
    expect(table.status).toBe("loaded");
    const held = table;
    bridge.fetchEmodulusLut.mockRejectedValue(new Error("link down"));
    await render({ ready: true, visible: false });
    await render({ ready: true, visible: true });
    await settle();
    expect(table).toBe(held);
  });
  it("drops a reply that arrives after the sub-tab was hidden again", async () => {
    let finish!: (v: unknown) => void;
    bridge.fetchEmodulusLut.mockImplementation(() => new Promise((r) => { finish = r; }));
    await render({ ready: true, visible: true });
    await render({ ready: true, visible: false });
    await act(async () => finish(loadedReply("ab|/data/lut.txt")));
    expect(table.status).toBe("unknown");
  });
});

describe("Clear Buffer and the selection (MON-048)", () => {
  it("clears the selected cell and closes its window", async () => {
    await render({ ready: true, visible: true });
    await act(async () => { state.setMonSelection({ frame_index: 1 } as never); state.setCellWindowOpen(true); });
    expect(state.monSelection).not.toBeNull();
    await act(async () => notify());
    expect(state.monSelection).toBeNull();
    expect(state.cellWindowOpen).toBe(false);
  });
  it("stops listening when unmounted", async () => {
    await render({ ready: true, visible: true });
    const keep = state;
    await act(async () => root.unmount());
    root = createRoot(host);
    expect(() => keep.notifyMonitoringCleared()).not.toThrow();
  });
});
