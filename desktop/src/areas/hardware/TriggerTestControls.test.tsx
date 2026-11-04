// @vitest-environment jsdom
import { act, useState } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { bridge, type TriggerStatus } from "../../bridge";
import { canActuate, canStartPeriodic, canStopPeriodic, type OperatingMode } from "../../commissioning";
import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";
import { TriggerTestControls } from "./TriggerTestControls";

vi.mock("../../bridge", () => ({
  bridge: {
    triggerSetPulseDuration: vi.fn(), triggerManualPulse: vi.fn(), triggerPeriodicStart: vi.fn(), triggerPeriodicStop: vi.fn(), fetchTriggerStatus: vi.fn(),
  },
}));
let session: { get: () => object; subscribe: () => () => void } | undefined;
vi.mock("../../transport/index", () => ({ get transport() { return { kind: session ? "ws" : "tauri", invoke: vi.fn(), session }; } }));

// A backend that remembers the applied duration and whether the periodic test runs.
const backend = { pulse_duration_us: 1, periodic_active: false, periodic_interval_ms: 0, camera_attached: true };
const status = (): TriggerStatus => ({ valid: true, trigger_count: 0, last_onset_us: 0, last_object_id: 0, last_track_id: 0, ...backend });
const ok = (message = "done") => ({ ok: true, command: 8, operation_id: "0", message });
const append = vi.fn();

let host: HTMLDivElement, root: Root;
// The real Studio value is built by useStudioController; this provides the fields the trigger controls read, with real state.
function Harness({ mode = "service", mounted = true }: { mode?: OperatingMode; mounted?: boolean }) {
  const [pulseUs, setPulseUs] = useState("1"), [periodicMs, setPeriodicMs] = useState("1000");
  const [trigStatus, setTrigStatus] = useState<TriggerStatus | null>(status());
  const [triggerArmed, setTriggerArmed] = useState(true);
  const input = { mode, experimentActive: false, triggerAttached: trigStatus?.camera_attached ?? false, armed: triggerArmed };
  const value = {
    caps: { trigger: true }, operatingMode: mode, triggerArmed, setTriggerArmed, expActive: false, trigStatus, setTrigStatus, append, pulseUs, setPulseUs,
    periodicMs, setPeriodicMs, ready: true, actuateCheck: canActuate(input), startPeriodicCheck: canStartPeriodic(input),
    stopPeriodicCheck: canStopPeriodic({ periodicActive: trigStatus?.periodic_active ?? false }),
  } as unknown as Studio;
  return <StudioContext.Provider value={value}>{mounted && <TriggerTestControls />}<output data-testid="armed">{String(triggerArmed)}</output></StudioContext.Provider>;
}
const render = (props = {}) => act(async () => root.render(<Harness {...props} />));
const button = (name: string) => [...host.querySelectorAll("button")].find((x) => x.textContent === name)!;
const pulseInput = () => host.querySelector<HTMLInputElement>('input[aria-label="Pulse duration (µs)"]')!;
const intervalInput = () => host.querySelector<HTMLInputElement>('input[aria-label="Periodic interval (ms)"]')!;
const alerts = () => [...host.querySelectorAll("[role=alert]")].map((x) => x.textContent);
const typeInto = (element: HTMLInputElement, text: string) => act(async () => {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(element, text);
  element.dispatchEvent(new Event("input", { bubbles: true }));
});
const calledOnceWith = (mock: unknown, ...args: unknown[]) => { expect(mock).toHaveBeenCalledOnce(); expect(mock).toHaveBeenCalledWith(...args); };
const wait = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.useFakeTimers();
  vi.clearAllMocks();
  session = undefined;
  Object.assign(backend, { pulse_duration_us: 1, periodic_active: false, periodic_interval_ms: 0, camera_attached: true });
  vi.mocked(bridge.fetchTriggerStatus).mockImplementation(async () => status());
  vi.mocked(bridge.triggerSetPulseDuration).mockImplementation(async (us) => { backend.pulse_duration_us = us; return ok(); });
  vi.mocked(bridge.triggerManualPulse).mockResolvedValue(ok());
  vi.mocked(bridge.triggerPeriodicStart).mockImplementation(async (ms) => { backend.periodic_active = true; backend.periodic_interval_ms = ms; return ok(); });
  vi.mocked(bridge.triggerPeriodicStop).mockImplementation(async () => { backend.periodic_active = false; return ok(); });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

describe("Qt wording and ranges (HW-043..HW-045)", () => {
  it("uses the Qt tooltips", async () => {
    await render();
    expect(button("Sort Trigger").title).toBe("Manually fire a sort trigger pulse for testing");
    expect(pulseInput().closest("label")!.title).toBe("Trigger pulse duration in microseconds");
    expect(button("Periodic Test").title).toBe("Toggle periodic sort trigger pulses at the interval below");
    expect(intervalInput().closest("label")!.title).toBe("Interval between periodic test trigger pulses");
  });
  it("has the Qt defaults and ranges and no Set Pulse button", async () => {
    await render();
    expect(pulseInput().value).toBe("1"); expect(pulseInput().min).toBe("1"); expect(pulseInput().max).toBe("1000000");
    expect(intervalInput().value).toBe("1000"); expect(intervalInput().min).toBe("10"); expect(intervalInput().max).toBe("60000");
    expect(button("Set Pulse")).toBeUndefined();
  });
  it("keeps the Service mode and arm gates on Sort Trigger", async () => {
    await render({ mode: "operator" });
    expect(host.textContent).toContain("Trigger tests are in Service / Commissioning mode");
    expect(button("Sort Trigger")).toBeUndefined();
  });
});

describe("pulse duration is applied as it is typed (HW-044)", () => {
  it("applies after a short pause, once, and logs the applied value", async () => {
    await render();
    await typeInto(pulseInput(), "2"); await typeInto(pulseInput(), "20");
    await wait(299);
    expect(bridge.triggerSetPulseDuration).not.toHaveBeenCalled();
    await wait(1);
    calledOnceWith(bridge.triggerSetPulseDuration, 20);
    expect(append).toHaveBeenCalledWith("pulse duration 20 µs");
    expect(host.textContent).toContain("applied 20 µs");
  });
  it("applies at once on Enter and on leaving the field", async () => {
    await render();
    await typeInto(pulseInput(), "30");
    await act(async () => { pulseInput().dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true })); });
    expect(bridge.triggerSetPulseDuration).toHaveBeenLastCalledWith(30);
    await typeInto(pulseInput(), "40");
    await act(async () => { pulseInput().dispatchEvent(new FocusEvent("focusout", { bubbles: true })); });
    expect(bridge.triggerSetPulseDuration).toHaveBeenLastCalledWith(40);
    await wait(500);
    expect(bridge.triggerSetPulseDuration).toHaveBeenCalledTimes(2); // the pause timers were cancelled
  });
  it.each([["", "blank"], ["0", "zero"], ["1000001", "above the range"], ["1.5", "a fraction"], ["-3", "negative"]])("refuses %j (%s) visibly and sends nothing", async (text) => {
    await render();
    await typeInto(pulseInput(), text);
    await wait(500);
    expect(bridge.triggerSetPulseDuration).not.toHaveBeenCalled();
    expect(alerts().join(" ")).toContain("Pulse duration must be a whole number of µs between 1 and 1,000,000.");
    expect(pulseInput().getAttribute("aria-invalid")).toBe("true");
    expect(button("Sort Trigger").disabled).toBe(true);
    expect(button("Periodic Test").disabled).toBe(true);
  });
  it("accepts the range ends", async () => {
    await render();
    await typeInto(pulseInput(), "1000000"); await wait(300);
    expect(bridge.triggerSetPulseDuration).toHaveBeenLastCalledWith(1000000);
  });
  it("shows a refusal from the backend and does not log success", async () => {
    vi.mocked(bridge.triggerSetPulseDuration).mockResolvedValue({ ...ok("Pulse duration must be >= 1 us"), ok: false });
    await render();
    await typeInto(pulseInput(), "5"); await wait(300);
    expect(alerts().join(" ")).toContain("Not applied: Pulse duration must be >= 1 us");
    expect(append).toHaveBeenCalledWith("pulse duration failed: Pulse duration must be >= 1 us");
    expect(append).not.toHaveBeenCalledWith("pulse duration 5 µs");
  });
  it("shows what the backend already has, and does not overwrite it by opening the tab", async () => {
    backend.pulse_duration_us = 20;
    await render();
    expect(pulseInput().value).toBe("20");
    expect(bridge.triggerSetPulseDuration).not.toHaveBeenCalled();
  });
  it("applies a value still waiting when the tab is left", async () => {
    await render();
    await typeInto(pulseInput(), "75");
    await render({ mounted: false });
    await wait(0);
    calledOnceWith(bridge.triggerSetPulseDuration, 75);
  });
});

describe("Sort Trigger fires with the applied duration (HW-043)", () => {
  it("applies the pending value first and logs it", async () => {
    await render();
    await typeInto(pulseInput(), "50"); // typed, pause not over
    await act(async () => button("Sort Trigger").click());
    calledOnceWith(bridge.triggerSetPulseDuration, 50);
    expect(vi.mocked(bridge.triggerSetPulseDuration).mock.invocationCallOrder[0]).toBeLessThan(vi.mocked(bridge.triggerManualPulse).mock.invocationCallOrder[0]);
    expect(append).toHaveBeenCalledWith("manual sort pulse fired (50 µs)");
    expect(host.querySelector("[data-testid=armed]")!.textContent).toBe("false");
    await wait(500);
    expect(bridge.triggerSetPulseDuration).toHaveBeenCalledOnce();
  });
  it("does not send the duration again when it is already applied", async () => {
    backend.pulse_duration_us = 20;
    await render();
    await act(async () => button("Sort Trigger").click());
    expect(bridge.triggerSetPulseDuration).not.toHaveBeenCalled();
    expect(append).toHaveBeenCalledWith("manual sort pulse fired (20 µs)");
  });
  it("logs the duration the backend reports, not the typed text", async () => {
    vi.mocked(bridge.triggerSetPulseDuration).mockImplementation(async () => { backend.pulse_duration_us = 99; return ok(); });
    await render();
    await typeInto(pulseInput(), "50");
    await act(async () => button("Sort Trigger").click());
    expect(append).toHaveBeenCalledWith("manual sort pulse fired (99 µs)");
  });
  it("does not fire, and keeps the arm, when the duration cannot be applied", async () => {
    vi.mocked(bridge.triggerSetPulseDuration).mockResolvedValue({ ...ok("refused"), ok: false });
    await render();
    await typeInto(pulseInput(), "50");
    await act(async () => button("Sort Trigger").click());
    expect(bridge.triggerManualPulse).not.toHaveBeenCalled();
    expect(host.querySelector("[data-testid=armed]")!.textContent).toBe("true");
  });
  it("reports a failed pulse", async () => {
    vi.mocked(bridge.triggerManualPulse).mockResolvedValue({ ...ok("No camera attached for trigger output"), ok: false });
    await render();
    await act(async () => button("Sort Trigger").click());
    expect(append).toHaveBeenCalledWith("sort trigger failed: No camera attached for trigger output");
  });
});

describe("periodic test (HW-045)", () => {
  it("starts with the default 1000 ms, after applying the pulse duration", async () => {
    await render();
    await typeInto(pulseInput(), "20");
    await act(async () => button("Periodic Test").click());
    calledOnceWith(bridge.triggerSetPulseDuration, 20);
    calledOnceWith(bridge.triggerPeriodicStart, 1000);
    expect(append).toHaveBeenCalledWith("periodic test started (1000 ms)");
    expect(button("Stop Periodic Test")).toBeDefined();
  });
  it("locks the interval while running and stops on request", async () => {
    await render();
    await act(async () => button("Periodic Test").click());
    expect(intervalInput().disabled).toBe(true);
    await act(async () => button("Stop Periodic Test").click());
    expect(bridge.triggerPeriodicStop).toHaveBeenCalledOnce();
    expect(append).toHaveBeenCalledWith("periodic test stopped");
    expect(intervalInput().disabled).toBe(false);
  });
  it.each([["9"], ["60001"], [""], ["0"], ["12.5"]])("refuses an interval of %j visibly and starts nothing", async (text) => {
    await render();
    await typeInto(intervalInput(), text);
    expect(button("Periodic Test").disabled).toBe(true);
    expect(alerts().join(" ")).toContain("Interval must be a whole number of ms between 10 and 60,000.");
    await act(async () => button("Periodic Test").click());
    expect(bridge.triggerPeriodicStart).not.toHaveBeenCalled();
  });
  it("accepts the range ends", async () => {
    await render();
    await typeInto(intervalInput(), "10");
    await act(async () => button("Periodic Test").click());
    expect(bridge.triggerPeriodicStart).toHaveBeenLastCalledWith(10);
  });
});

describe("a running periodic test stops when the Monitoring tab is left (HW-045)", () => {
  it("stops it and logs it", async () => {
    await render();
    await act(async () => button("Periodic Test").click());
    expect(backend.periodic_active).toBe(true);
    await render({ mounted: false });
    await wait(0);
    expect(bridge.triggerPeriodicStop).toHaveBeenCalledOnce();
    expect(append).toHaveBeenCalledWith("periodic test stopped (Monitoring tab left)");
    expect(backend.periodic_active).toBe(false);
  });
  it("also stops one that was started from Operator mode's view, and one another client started", async () => {
    backend.periodic_active = true;
    await render({ mode: "operator" });
    expect(button("Stop Periodic Test")).toBeDefined();
    await render({ mode: "operator", mounted: false });
    await wait(0);
    expect(bridge.triggerPeriodicStop).toHaveBeenCalledOnce();
  });
  it("leaves nothing to stop when no test runs", async () => {
    await render();
    await render({ mounted: false });
    await wait(0);
    expect(bridge.triggerPeriodicStop).not.toHaveBeenCalled();
    expect(append).not.toHaveBeenCalled();
  });
  it("does not stop on a mode switch, only when the panel goes away", async () => {
    await render();
    await act(async () => button("Periodic Test").click());
    await render({ mode: "operator" });
    await wait(0);
    expect(bridge.triggerPeriodicStop).not.toHaveBeenCalled();
    expect(backend.periodic_active).toBe(true);
  });
  it("does not try for a viewer (the server would refuse it)", async () => {
    const state = { clientId: 7, controllerId: 3 }; // a stable snapshot: useSyncExternalStore re-renders forever on a new object each read
    session = { get: () => state, subscribe: () => () => undefined };
    backend.periodic_active = true;
    await render();
    await render({ mounted: false });
    await wait(0);
    expect(bridge.triggerPeriodicStop).not.toHaveBeenCalled();
  });
});
