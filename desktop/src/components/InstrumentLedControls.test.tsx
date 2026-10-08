// @vitest-environment jsdom
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { InstrumentLedControls } from "./InstrumentLedControls";

let host: HTMLDivElement, root: Root;
const ok = { ok: true, command: 0, message: "LED set", operation_id: "0" };
const limits = { delay_min_us: 0, delay_max_us: 100, width_min_us: 20, width_max_us: 80 };
const button = (name: string) => Array.from(host.querySelectorAll("button")).find((b) => b.textContent === name)!;

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

describe("PZ7035 LED in Service mode (#501 P1)", () => {
  it("starts at the mode preset, nudges the width in 0.5 µs steps within the limits", async () => {
    const apply = vi.fn().mockResolvedValue(ok);
    await act(async () => root.render(<InstrumentLedControls mode="run" limits={limits} current={undefined} disabled={false} apply={apply} append={() => {}} />));
    await act(async () => button("Width + 0.5 µs").click());
    expect(apply).toHaveBeenLastCalledWith(7, 60.5);
    await act(async () => button("Restore preset").click());
    expect(apply).toHaveBeenLastCalledWith(7, 60);
  });

  it("clamps a nudge to the backend's limits", async () => {
    const apply = vi.fn().mockResolvedValue(ok);
    await act(async () => root.render(<InstrumentLedControls mode="run" limits={{ ...limits, width_max_us: 60 }} current={undefined} disabled={false} apply={apply} append={() => {}} />));
    await act(async () => button("Width + 0.5 µs").click());
    expect(apply).toHaveBeenLastCalledWith(7, 60);
  });

  it("follows the Align preset: whole frames 100/135, banded fallback 0/125", async () => {
    const apply = vi.fn().mockResolvedValue(ok);
    await act(async () => root.render(<InstrumentLedControls mode="align" limits={undefined} current={{ on: true, delay_us: 100, width_us: 135 }} disabled={false} apply={apply} append={() => {}} />));
    expect(host.textContent).toContain("preset 100 / 135 µs");
    await act(async () => button("Apply").click());
    expect(apply).toHaveBeenLastCalledWith(100, 135);
    await act(async () => root.render(<InstrumentLedControls mode="align" alignBands limits={undefined} current={undefined} disabled={false} apply={apply} append={() => {}} />));
    expect(host.textContent).toContain("preset 0 / 125 µs");
  });

  it("nudges the delay in 0.5 µs steps within the limits too", async () => {
    const apply = vi.fn().mockResolvedValue(ok);
    await act(async () => root.render(<InstrumentLedControls mode="run" limits={{ ...limits, delay_max_us: 7.5 }} current={undefined} disabled={false} apply={apply} append={() => {}} />));
    await act(async () => button("Delay + 0.5 µs").click());
    expect(apply).toHaveBeenLastCalledWith(7.5, 60);
    await act(async () => button("Delay + 0.5 µs").click()); // 7.5 is the limit
    expect(apply).toHaveBeenLastCalledWith(7.5, 60);
    await act(async () => button("Delay − 0.5 µs").click());
    expect(apply).toHaveBeenLastCalledWith(7, 60);
  });

  it("reads out the strobe state, the guard and its trips, and alerts on a fault", async () => {
    const render = (current: Parameters<typeof InstrumentLedControls>[0]["current"]) =>
      act(async () => root.render(<InstrumentLedControls mode="run" limits={limits} current={current} disabled={false} apply={vi.fn()} append={() => {}} />));
    await render({ on: true, delay_us: 7, width_us: 60, guard_fault: false, guard_trips: 0 });
    expect(host.querySelector('[aria-label="LED readout"]')!.textContent).toBe("Strobe (S[0]): on (1) · guard: ok · trips: 0");
    expect(host.querySelector('[role="alert"]')).toBeNull();
    await render({ on: false, delay_us: 0, width_us: 0, guard_fault: true, guard_trips: 3 });
    expect(host.querySelector('[aria-label="LED readout"]')!.textContent).toBe("Strobe (S[0]): off (0) · guard: FAULT · trips: 3");
    expect(host.querySelector('[role="alert"]')!.textContent).toContain("guard tripped");
    await render(undefined);
    expect(host.querySelector('[aria-label="LED readout"]')!.textContent).toBe("Strobe (S[0]): — · guard: — · trips: —");
  });
});
