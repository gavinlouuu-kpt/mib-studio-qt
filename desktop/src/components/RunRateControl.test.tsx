// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { RunRateControl } from "./RunRateControl";
import type { RunFpsStatus } from "../runFps";

let host: HTMLDivElement, root: Root;
const status = (over: Partial<RunFpsStatus> = {}): RunFpsStatus => ({
  available: true, min_fps: 1000, max_fps: 5000, default_fps: 5000, qualified_fps: 5000, fps: 5000, exposure_mode: "auto", exposure_us: 150, hmax: 58, vmax: 256, shs: 64, exposure_lines: 192,
  actual_fps: 5000.673, exposure_start_us: 50, exposure_end_us: 200, strobe_start_us: 7, strobe_end_us: 67, strobe_overlaps: true, strobe_straddles_exposure_start: true, strobe_warning: "",
  qualified: true, in_run: true, applied_fps: 5000, pending: false, held: false, ...over,
});
const testid = (id: string) => host.querySelector(`[data-testid=${id}]`);
const mount = (s: RunFpsStatus | null) => act(() => root.render(<RunRateControl status={s} />));

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("the Run setting on the experiment page (read only)", () => {
  it("shows nothing off the PZ7035", () => {
    mount(status({ available: false }));
    expect(host.textContent).toBe("");
    mount(null);
    expect(host.textContent).toBe("");
  });
  it("shows the setting, the strobe geometry and the qualification note, with no input", () => {
    mount(status());
    expect(testid("run-rate-timing")!.textContent).toContain("5,000 fps · exposure auto 150 µs (SHS 64)");
    expect(testid("run-rate-strobe-window")!.textContent).toContain("exposure 50 to 200 µs after XVS, LED strobe 7 to 67 µs");
    expect(testid("run-rate-qualification")!.textContent).toContain("5,000 fps is the qualified rate; others are not yet validated on the board.");
    expect(host.querySelector("input")).toBeNull();
    expect(host.querySelector("button")).toBeNull();
    expect(host.textContent).toContain("Config → Camera document");
    expect(testid("run-rate-strobe")).toBeNull();
    expect(testid("run-rate-pending")).toBeNull();
  });
  it("shows a derived exposure at 1 kHz and the pending notice", () => {
    mount(status({ fps: 1000, exposure_us: 949.87, hmax: 116, vmax: 640, shs: 32, exposure_lines: 608, applied_fps: 5000, pending: true, qualified: false, actual_fps: 1000.135 }));
    expect(testid("run-rate-timing")!.textContent).toContain("1,000 fps · exposure auto 950 µs (SHS 32)");
    expect(testid("run-rate-pending")!.textContent).toContain("The camera runs at 5,000 fps; 1,000 fps is set and applies after Run is entered again.");
  });
  it("shows a strobe warning if the backend reports one", () => {
    mount(status({ strobe_warning: "strobe outside the exposure at this rate: frames will be unlit" }));
    expect(testid("run-rate-strobe")!.textContent).toContain("frames will be unlit");
  });
});
