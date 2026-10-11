// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { RunRateControl } from "./RunRateControl";
import type { RunFpsStatus } from "../runFps";

let host: HTMLDivElement, root: Root;
const status = (over: Partial<RunFpsStatus> = {}): RunFpsStatus => ({
  available: true, min_fps: 100, max_fps: 5000, default_fps: 5000, qualified_fps: 5000, fps: 5000, exposure_us: 150, hmax: 58, vmax: 256, shs: 64, exposure_lines: 192,
  actual_fps: 5000.673, qualified: true, in_run: true, applied_fps: 5000, pending: false, held: false, ...over,
});
const input = () => host.querySelector("input") as HTMLInputElement;
const apply = () => Array.from(host.querySelectorAll("button")).find((b) => b.textContent === "Apply" || b.textContent === "Applying…")!;
const testid = (id: string) => host.querySelector(`[data-testid=${id}]`);
const flush = async () => { await act(async () => { await Promise.resolve(); await Promise.resolve(); }); };
async function type(text: string) {
  await act(async () => {
    const set = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!;
    set.call(input(), text);
    input().dispatchEvent(new Event("input", { bubbles: true }));
  });
}
function mount(props: Partial<Parameters<typeof RunRateControl>[0]> = {}) {
  const onApply = vi.fn(async () => ({ ok: true, message: "Run rate 1000 fps (exposure 950 µs), applied" }));
  const confirm = vi.fn(async () => true);
  act(() => root.render(<RunRateControl status={status()} ringFrames={0} disabledReason="" onApply={onApply} confirm={confirm} {...props} />));
  return { onApply, confirm };
}

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("Run rate control (ABI 38)", () => {
  it("shows nothing off the PZ7035", () => {
    mount({ status: status({ available: false }) });
    expect(host.textContent).toBe("");
    mount({ status: null });
    expect(host.textContent).toBe("");
  });
  it("shows the configured rate, the derived exposure and the qualification note", async () => {
    mount();
    expect(input().value).toBe("5000");
    expect(testid("run-rate-timing")!.textContent).toContain("exposure 150 µs");
    expect(testid("run-rate-qualification")!.textContent).toContain("5,000 fps is the qualified rate; others are not yet validated on the board.");
    expect(apply().hasAttribute("disabled")).toBe(true); // nothing changed
  });
  it("derives the exposure of the typed rate and applies it through the backend", async () => {
    const { onApply, confirm } = mount();
    await type("1000");
    expect(testid("run-rate-timing")!.textContent).toContain("exposure 950 µs");
    expect(testid("run-rate-note")!.textContent).toContain("re-enters Run");
    await act(async () => { apply().dispatchEvent(new MouseEvent("click", { bubbles: true })); });
    await flush();
    expect(confirm).not.toHaveBeenCalled(); // nothing is held
    expect(onApply).toHaveBeenCalledWith(1000, false);
    expect(testid("run-rate-result")!.textContent).toContain("applied");
  });
  it("refuses an invalid rate before asking the backend", async () => {
    const { onApply } = mount();
    await type("50");
    expect(testid("run-rate-invalid")!.textContent).toBe("the Run rate must be from 100 to 5000 fps");
    expect(apply().hasAttribute("disabled")).toBe(true);
    expect(onApply).not.toHaveBeenCalled();
  });
  it("asks before discarding a stopped Run's frames and sends the acknowledgement", async () => {
    const { onApply, confirm } = mount({ status: status({ held: true }), ringFrames: 5000 });
    await type("2000");
    expect(testid("run-rate-note")!.textContent).toContain("discards them");
    await act(async () => { apply().dispatchEvent(new MouseEvent("click", { bubbles: true })); });
    await flush();
    expect(confirm).toHaveBeenCalledWith("Changing the Run rate discards the 5,000 buffered frames of the stopped Run. Continue?");
    expect(onApply).toHaveBeenCalledWith(2000, true);
  });
  it("sends nothing when the operator declines the discard", async () => {
    const { onApply, confirm } = mount({ status: status({ held: true }), ringFrames: 5000 });
    confirm.mockResolvedValue(false);
    await type("2000");
    await act(async () => { apply().dispatchEvent(new MouseEvent("click", { bubbles: true })); });
    await flush();
    expect(onApply).not.toHaveBeenCalled();
  });
  it("is disabled with the reason while an experiment runs", async () => {
    mount({ disabledReason: "An experiment is running: stop it to change the Run rate." });
    expect(input().disabled).toBe(true);
    expect(apply().hasAttribute("disabled")).toBe(true);
    expect(testid("run-rate-disabled")!.textContent).toContain("An experiment is running");
  });
  it("says so when the camera runs at another rate than the one set, and offers to apply again", async () => {
    mount({ status: status({ fps: 1000, exposure_us: 949.872, vmax: 1280, applied_fps: 5000, pending: true, qualified: false }) });
    expect(testid("run-rate-pending")!.textContent).toContain("The camera runs at 5,000 fps; 1,000 fps is set");
    expect(apply().hasAttribute("disabled")).toBe(false); // unchanged text, but pending: Apply re-enters Run
  });
});
