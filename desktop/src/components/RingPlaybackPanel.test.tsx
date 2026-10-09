// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { RingPlaybackPanel } from "./RingPlaybackPanel";
import type { RingFrame, RingStatus } from "../ringPlayback";

let host: HTMLDivElement, root: Root;
const ok = { ok: true, command: 0, message: "Run resumed", operation_id: "0" };
const status = (over: Partial<RingStatus> = {}): RingStatus => ({
  available: true, frozen: true, invalid: false, stop_incomplete: false, restore_needed: false, run_frozen: true, capacity_frames: 5000, first_seq: 100, last_seq: 5099,
  count: 5000, sensor_fps: 5000, ...over,
});

function frameAt(seq: number, withMask = true): RingFrame {
  const payload = new Array(15).fill(0);
  payload[0] = 1; payload[2] = Math.round(130 * 65536); payload[5] = Math.round(0.05 * 65536) | (1 << 24); payload[4] = Math.round(0.9 * 65536);
  return {
    seq, frameId: 1000 + seq, timestampTicks: 100_000 * seq, tickHz: 100_000_000, flags: 0, width: 16, height: 4, maskPresent: withMask,
    resultsTruncated: false, gray: new Uint8Array(64).fill(50), mask: new Uint8Array(8),
    cells: [
      { x: 1, y: 1, width: 4, height: 2, valid: true, index: 0, payloadValidity: 0x7fff, payload },
      { x: 8, y: 0, width: 3, height: 3, valid: false, index: 1, payloadValidity: 0b1, payload: [(2) | (3 << 16), ...new Array(14).fill(0)] },
    ],
  };
}

const button = (label: string) => Array.from(host.querySelectorAll("button")).find((b) => b.textContent === label || b.getAttribute("aria-label") === label)!;
const flush = async () => { await act(async () => { await Promise.resolve(); await Promise.resolve(); }); };

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  // jsdom has no canvas 2d context: the panel's drawing is exercised through its own pure module (ringPlayback.test.ts).
  HTMLCanvasElement.prototype.getContext = (() => null) as never;
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("ring playback panel (#649 v1)", () => {
  it("shows why there is no playback instead of controls", async () => {
    const fetchFrame = vi.fn();
    await act(async () => root.render(<RingPlaybackPanel status={status({ invalid: true, reason: "the ring stalled (RING_STALLED): re-arm" })}
      fetchFrame={fetchFrame} onResume={async () => ok} append={() => {}} />));
    expect(host.querySelector("[data-testid=ring-unavailable]")?.textContent).toMatch(/RING_STALLED/);
    expect(host.querySelector("input[type=range]")).toBeNull();
    expect(fetchFrame).not.toHaveBeenCalled();
    expect(button("Resume Run (discards these frames)")).toBeTruthy();
  });

  it("shows the capacity in frames and seconds, loads the newest frame and lists its cells", async () => {
    const fetchFrame = vi.fn(async (seq: number) => frameAt(seq));
    await act(async () => root.render(<RingPlaybackPanel status={status()} fetchFrame={fetchFrame} onResume={async () => ok} append={() => {}} />));
    await flush();
    expect(host.textContent).toContain("5,000 frames · 1.00 s at 5,000 fps");
    expect(host.textContent).toContain("Frames 100 to 5,099");
    expect(fetchFrame).toHaveBeenCalledWith(5099);
    expect(host.textContent).toContain("Frame 5,099");
    const rows = host.querySelectorAll("table.ring-cells tbody tr");
    expect(rows).toHaveLength(2);
    expect(rows[0].textContent).toContain("valid");
    expect(rows[0].textContent).toContain("130.0");
    expect(rows[0].textContent).toContain("0.050");
    expect(rows[1].textContent).toContain("area outside the gate");
    expect(rows[1].textContent).toContain("—"); // the PL left the measurements out
  });

  it("steps, jumps and scrubs inside the buffered range", async () => {
    const fetchFrame = vi.fn(async (seq: number) => frameAt(seq));
    await act(async () => root.render(<RingPlaybackPanel status={status()} fetchFrame={fetchFrame} onResume={async () => ok} append={() => {}} />));
    await flush();
    await act(async () => button("Previous frame").click());
    await flush();
    expect(fetchFrame).toHaveBeenLastCalledWith(5098);
    await act(async () => button("First buffered frame").click());
    await flush();
    expect(fetchFrame).toHaveBeenLastCalledWith(100);
    await act(async () => button("Previous frame").click());
    await flush();
    expect(fetchFrame).toHaveBeenLastCalledWith(100); // clamped at the oldest
    const slider = host.querySelector("input[type=range]") as HTMLInputElement;
    expect(slider.min).toBe("100");
    expect(slider.max).toBe("5099");
  });

  it("cycles the overlay and says when a frame has no mask", async () => {
    const fetchFrame = vi.fn(async (seq: number) => frameAt(seq, false));
    await act(async () => root.render(<RingPlaybackPanel status={status()} fetchFrame={fetchFrame} onResume={async () => ok} append={() => {}} />));
    await flush();
    expect(button("Overlay").textContent).toBe("Overlay: Both");
    await act(async () => button("Overlay").click());
    expect(button("Overlay").textContent).toBe("Overlay: Off");
    await act(async () => button("Overlay").click());
    expect(button("Overlay").textContent).toBe("Overlay: Mask");
    expect(host.textContent).toContain("No result for this frame");
  });

  it("keeps Save clip disabled with the reason, and Resume calls the backend", async () => {
    const onResume = vi.fn(async () => ok);
    const append = vi.fn();
    await act(async () => root.render(<RingPlaybackPanel status={status()} fetchFrame={async (s) => frameAt(s)} onResume={onResume} append={append} />));
    await flush();
    const save = button("Save clip") as HTMLButtonElement;
    expect(save.disabled).toBe(true);
    expect(host.textContent).toContain("Saving needs the SSD (not available yet).");
    await act(async () => button("Resume Run (discards these frames)").click());
    await flush();
    expect(onResume).toHaveBeenCalledTimes(1);
    expect(append).toHaveBeenCalledWith("Run resumed");
  });

  it("plays the frozen part under a stop that did not complete, with the flag shown", async () => {
    const fetchFrame = vi.fn(async (seq: number) => frameAt(seq));
    await act(async () => root.render(<RingPlaybackPanel status={status({ frozen: false, stop_incomplete: true })} fetchFrame={fetchFrame} onResume={async () => ok} append={() => {}} />));
    await flush();
    expect(host.querySelector("[data-testid=ring-stop-incomplete]")?.textContent).toMatch(/Stop incomplete/);
    expect(fetchFrame).toHaveBeenCalledWith(5099);
    expect(host.querySelector("input[type=range]")).not.toBeNull();
  });

  it("says restore needed instead of offering playback", async () => {
    const fetchFrame = vi.fn();
    await act(async () => root.render(<RingPlaybackPanel status={status({ frozen: false, restore_needed: true })} fetchFrame={fetchFrame} onResume={async () => ok} append={() => {}} />));
    expect(host.querySelector("[data-testid=ring-unavailable]")?.textContent).toMatch(/Restore needed/);
    expect(fetchFrame).not.toHaveBeenCalled();
  });

  it("reports a frame that could not be fetched (overwritten while copied)", async () => {
    const fetchFrame = vi.fn(async () => { throw new Error("RING_FRAME_UNAVAILABLE: overwritten: frame 5099 was overwritten while it was copied"); });
    await act(async () => root.render(<RingPlaybackPanel status={status()} fetchFrame={fetchFrame} onResume={async () => ok} append={() => {}} />));
    await flush();
    expect(host.querySelector("[role=alert]")?.textContent).toMatch(/overwritten/);
  });
});
