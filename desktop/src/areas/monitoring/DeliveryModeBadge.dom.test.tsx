// @vitest-environment jsdom
// #858 (SHELL-055): the badge in the status bar reads the backend's delivery mode on every stage.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";

const bridge = vi.hoisted(() => ({ fetchFrameDeliveryMode: vi.fn() }));
vi.mock("../../bridge", () => ({ bridge }));

import { DELIVERY_MODE_POLL_MS, DeliveryModeBadge } from "./DeliveryModeBadge";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => {
  vi.useFakeTimers();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
  bridge.fetchFrameDeliveryMode.mockReset();
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

const studio = (over: Record<string, unknown> = {}) => ({ ready: true, running: false, instrumentModes: false, ...over }) as unknown as Studio;
const render = async (s: Studio) => act(async () => root.render(<StudioContext.Provider value={s}><DeliveryModeBadge /></StudioContext.Provider>));
const tick = async (ms: number) => { await act(async () => { await vi.advanceTimersByTimeAsync(ms); }); };
const badge = () => host.querySelector<HTMLElement>(".delivery-mode-badge");

describe("DeliveryModeBadge", () => {
  it("shows the backend's mode with Qt's text and tooltip, and follows a change within one read", async () => {
    bridge.fetchFrameDeliveryMode.mockResolvedValue({ valid: true, mode: "everyFrame", active_mode: "everyFrame", confirmed: true, capturing: true });
    await render(studio({ running: true }));
    await tick(0);
    expect(badge()?.textContent).toBe("▶ EVERY FRAME · sequence preserved");
    expect(badge()?.className).toBe("delivery-mode-badge every");
    expect(badge()?.title).toContain("Latest Frame minimizes latency");
    bridge.fetchFrameDeliveryMode.mockResolvedValue({ valid: true, mode: "latestFrame", active_mode: "latestFrame", confirmed: false, capturing: false });
    await tick(DELIVERY_MODE_POLL_MS);
    expect(badge()?.textContent).toBe("⏩ LATEST FRAME · drops stale frames (requested)");
    expect(badge()?.className).toBe("delivery-mode-badge latest");
  });
  it("reads again at once when the camera starts (the start confirms the mode)", async () => {
    bridge.fetchFrameDeliveryMode.mockResolvedValue({ valid: true, mode: "latestFrame", active_mode: "latestFrame", confirmed: false, capturing: false });
    await render(studio());
    await tick(0);
    bridge.fetchFrameDeliveryMode.mockResolvedValue({ valid: true, mode: "latestFrame", active_mode: "latestFrame", confirmed: true, capturing: true });
    await render(studio({ running: true }));
    await tick(0);
    expect(badge()?.textContent).toBe("⏩ LATEST FRAME · drops stale frames");
  });
  it("shows nothing on the PZ7035, before the backend is ready, or when the read fails", async () => {
    bridge.fetchFrameDeliveryMode.mockResolvedValue({ valid: true, mode: "everyFrame", active_mode: "everyFrame", confirmed: true, capturing: true });
    await render(studio({ instrumentModes: true }));
    await tick(0);
    expect(badge()).toBeNull();
    expect(bridge.fetchFrameDeliveryMode).not.toHaveBeenCalled();
    await render(studio({ ready: false }));
    await tick(0);
    expect(badge()).toBeNull();
    bridge.fetchFrameDeliveryMode.mockRejectedValue(new Error("unknown command fetch_frame_delivery_mode"));
    await render(studio());
    await tick(0);
    expect(badge()).toBeNull();
  });
});
