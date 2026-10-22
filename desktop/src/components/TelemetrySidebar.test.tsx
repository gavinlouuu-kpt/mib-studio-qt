// @vitest-environment jsdom
// The classic left column (#797): one summary per subsystem, "—" for absent values, one dim line
// for optional hardware that is not connected, and one pump poll however many consumers read it.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { bridge } from "../bridge";
import type { AutofocusStatus, InstrumentStatus, PumpStatus } from "../bridge";
import { EXPERIMENT_STATES } from "../bridgeContract";
import type { ExperimentStatus } from "../eventAdapter";
import { HardwareControls } from "./HardwareControls";
import { TelemetrySidebar, type TelemetrySidebarProps } from "./TelemetrySidebar";
import { PUMP_STATUS_POLL_MS, usePumpStatus } from "./usePumpStatus";
import { contextLine, runName } from "./telemetrySidebarModel";

vi.mock("../bridge", () => ({ bridge: { fetchPumpStatus: vi.fn(), fetchAutofocusStatus: vi.fn(), fetchAutofocusConfig: vi.fn() } }));

const pump = (over: Partial<PumpStatus> = {}): PumpStatus => ({
  valid: true, connected: true, run_status: 1, current_flow_rate: 2.4, accumulated_volume: 0, min_flow_rate: 0, max_flow_rate: 10, stalled: false,
  com_port: 1, baud_rate: 115200, modbus_address: 1, configured_flow_rate: 2.4, flow_rate_unit: 100, direction: 0, ...over,
});
const focus = (over: Partial<AutofocusStatus> = {}): AutofocusStatus => ({
  valid: true, connected: true, enabled: true, current_voltage: 52.3, com_port: 3, average_ring_ratio: 0.91, median_ring_ratio: 0.912,
  last_ring_ratio_update_us: 1000, ring_ratio_age_us: 12000, ...over,
});
const status = (over: Partial<ExperimentStatus> = {}): ExperimentStatus => ({
  valid: true, state: EXPERIMENT_STATES.Active, start_time_ns: "1", end_time_ns: "0", valid_buffered: "12", invalid_buffered: "3", valid_saved: "398", invalid_saved: "14",
  dropped_valid: "0", dropped_invalid: "1", flushing: false, cancelled: false, output_path: "/data/runs/Exp-0142_03.h5", message: "",
  start_generation: "3", readiness_generation: "3", capture_generation: "3", persistence_admitted: "420", persistence_committed: "412", persistence_failed: "0",
  terminal: false, finalization_ok: false, completion: 0, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "", ...over,
});

const IDLE: TelemetrySidebarProps = {
  stage: { index: 0, title: "Hardware Preflight", status: "ready", statusLabel: "Ready" },
  source: "Mock camera", run: null,
  camera: { running: false, status: "idle", displayFps: 0, dataRateMBs: 0 },
  processing: { hostProcessing: true, statsValid: true, algoFps: 0, validFps: 0, invalidFps: 0, pixelToMicron: 0.5, background: { supported: true, set: false } },
  focus: { supported: true, status: focus({ connected: false }) },
  fluidics: { pumps: [null, null] },
  experiment: { status: status({ state: EXPERIMENT_STATES.Idle, start_time_ns: "0", output_path: "" }), state: EXPERIMENT_STATES.Idle, active: false, outcome: null, elapsedWallSeconds: null },
};
const RUNNING: TelemetrySidebarProps = {
  ...IDLE,
  stage: { index: 2, title: "Experiment", status: "running", statusLabel: "Running" },
  run: { name: "Exp-0142_03", sample: "sample 3 of 7" },
  camera: { running: true, status: "running", displayFps: 30.2, dataRateMBs: 1.5 },
  processing: { ...IDLE.processing, algoFps: 412, validFps: 398, invalidFps: 14, background: { supported: true, set: true } },
  focus: { supported: true, status: focus() },
  fluidics: { pumps: [pump(), pump({ run_status: 0, current_flow_rate: 0 })] },
  experiment: { status: status(), state: EXPERIMENT_STATES.Active, active: true, outcome: null, elapsedWallSeconds: 83 },
};

let host: HTMLDivElement, root: Root;
const render = (p: TelemetrySidebarProps) => act(async () => root.render(<TelemetrySidebar {...p} />));
const section = (id: string) => host.querySelector(`[data-testid=telemetry-${id}]`)!;
const headings = () => [...host.querySelectorAll("h4")].map((h) => h.textContent);
const rows = (id: string) => Object.fromEntries([...section(id).querySelectorAll(".tsb-row")].map((r) => [r.querySelector(".tsb-row__k")!.textContent, r.querySelector(".tsb-row__v")!]));
const text = (id: string) => Object.fromEntries(Object.entries(rows(id)).map(([k, v]) => [k, v.textContent]));

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.clearAllMocks();
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers(); });

describe("the board's sections (idle mock camera)", () => {
  it("renders exactly the sections drawn, Focus and Fluidics as one dim line each, the counters dim", async () => {
    await render(IDLE);
    expect(headings()).toEqual(["Camera", "Processing", "Experiment"]);
    expect(section("stage").textContent).toBe("01 Hardware Preflight · Ready");
    expect(section("stage").getAttribute("data-tone")).toBe("confirmed");
    expect(section("context").textContent).toBe("Mock camera · idle · nothing runs");
    expect(section("focus").textContent).toBe("Focus · not connected");
    expect(section("focus").className).toBe("tsb-quiet");
    expect(section("fluidics").textContent).toBe("Fluidics · not connected");
    expect(text("camera")).toEqual({ Status: "idle", "Display rate": "0.0 fps", "Data rate": "0.0 MB/s" });
    expect(text("processing")).toEqual({ "Algo / valid / invalid": "0 / 0 / 0 fps", "px → µm": "0.5", Background: "Not set" });
    expect(Object.keys(text("experiment"))).toEqual(["Status", "Elapsed", "Buffered", "Saved", "Pending save", "Dropped", "Failed to save", "Flush"]);
    for (const [label, cell] of Object.entries(rows("experiment"))) expect(cell.className, label).toContain("tsb-row__v--mute");
    expect(text("experiment").Buffered).toBe("12 valid · 3 invalid");
    expect(text("experiment").Elapsed).toBe("—");
    // No duplicate sections, no bare "Unavailable" in a value column.
    expect(host.textContent).not.toContain("Nanopositioner Autofocus");
    expect(headings()).not.toContain("Display");
    expect(host.textContent).not.toContain("Unavailable");
    expect(host.querySelector(".mib-root")).not.toBeNull();
  });

  it("colours only backend states: a zero rate is mute, never confirmed", async () => {
    await render(IDLE);
    expect(rows("camera")["Display rate"].className).toContain("tsb-row__v--mute");
    expect(host.querySelectorAll(".tsb-row__v--confirmed").length).toBe(0);
  });
});

describe("a running experiment", () => {
  it("shows the stage chip, the run and sample in the context line, one merged Focus section, pump rates and live counters", async () => {
    await render(RUNNING);
    expect(headings()).toEqual(["Camera", "Processing", "Focus", "Fluidics", "Experiment"]);
    expect(section("stage").textContent).toBe("03 Experiment · Running");
    expect(section("context").textContent).toBe("Mock camera · running · Exp-0142_03 · sample 3 of 7");
    expect(text("camera")).toEqual({ Status: "Running", "Display rate": "30.2 fps", "Data rate": "1.5 MB/s" });
    expect(rows("camera").Status.className).toContain("tsb-row__v--confirmed");
    expect(text("processing")).toEqual({ "Algo / valid / invalid": "412 / 398 / 14 fps", "px → µm": "0.5", Background: "Set" });
    expect(text("focus")).toEqual({ Controller: "connected (auto)", "Ring width": "0.912", Voltage: "52.3 V", "Metric age": "12 ms" });
    expect(text("fluidics")).toEqual({ "Sample pump": "0.040 µL/s · running", "Sheath pump": "0.000 µL/s · stopped" });
    expect(rows("fluidics")["Sample pump"].className).toContain("tsb-row__v--confirmed");
    expect(rows("fluidics")["Sheath pump"].className).toContain("tsb-row__v--mute");
    expect(text("experiment")).toEqual({
      Status: "Active", Elapsed: "83s", Buffered: "12 valid · 3 invalid", Saved: "398 valid · 14 invalid", "Pending save": "7", Dropped: "0 valid · 1 invalid",
      "Failed to save": "0", Flush: "Idle",
    });
    expect(rows("experiment").Saved.className).not.toContain("tsb-row__v--mute");
    expect(host.textContent).not.toContain("Nanopositioner Autofocus");
  });

  it("Last run appears after a run with its outcome, never during one", async () => {
    await render({ ...IDLE, experiment: { ...IDLE.experiment, outcome: "ok" } });
    expect(text("experiment")["Last run"]).toBe("Complete");
    await render({ ...RUNNING, experiment: { ...RUNNING.experiment, outcome: "ok" } });
    expect(text("experiment")["Last run"]).toBeUndefined();
  });

  it("the run's name is the file without its extension; the context line without a queue names no sample", () => {
    expect(runName(status())).toBe("Exp-0142_03");
    expect(runName(status({ output_path: "C:\\runs\\exp 7.h5" }))).toBe("exp 7");
    expect(runName(null)).toBe("");
    expect(contextLine("Mock camera", true, { name: "Exp-0142_03", sample: "" })).toBe("Mock camera · running · Exp-0142_03");
    expect(contextLine("", false, null)).toBe("No camera · idle · nothing runs");
  });
});

describe("absent values", () => {
  it("render as — (never Unavailable): no stats, no experiment status, a pump without a sample, an unknown unit", async () => {
    await render({
      ...RUNNING,
      camera: { running: false, status: "unconfigured", displayFps: null, dataRateMBs: null },
      processing: { ...RUNNING.processing, statsValid: false },
      focus: { supported: true, status: focus({ last_ring_ratio_update_us: 0 }) },
      fluidics: { pumps: [pump({ flow_rate_unit: 7, current_flow_rate: 3 }), null] },
      experiment: { status: null, state: EXPERIMENT_STATES.Idle, active: false, outcome: null, elapsedWallSeconds: -1 },
    });
    expect(text("camera")).toEqual({ Status: "unconfigured", "Display rate": "—", "Data rate": "—" });
    expect(text("processing")).toEqual({ "Algo / valid / invalid": "—", "px → µm": "—", Background: "Set" });
    expect(text("focus")).toEqual({ Controller: "connected (auto)", "Ring width": "—", Voltage: "52.3 V", "Metric age": "—" });
    expect(text("fluidics")).toEqual({ "Sample pump": "3 (unit 7) · running", "Sheath pump": "—" });
    expect(text("experiment")).toEqual({ Status: "Inactive", Elapsed: "—", Buffered: "—", Saved: "—", "Pending save": "—", Dropped: "—", "Failed to save": "—", Flush: "—" });
    expect(host.textContent).not.toContain("Unavailable");
  });

  it("a stalled pump reads blocked; a paused one is mute", async () => {
    await render({ ...RUNNING, fluidics: { pumps: [pump({ stalled: true }), pump({ run_status: 3 })] } });
    expect(text("fluidics")).toEqual({ "Sample pump": "0.040 µL/s · stalled", "Sheath pump": "0.040 µL/s · paused" });
    expect(rows("fluidics")["Sample pump"].className).toContain("tsb-row__v--blocked");
  });
});

describe("the PZ7035 variant", () => {
  const instrument = {
    available: true,
    core: { build_id: "1a2b3c4d5e6f", profile_id: "9f8e7d6c5b4a", abi_version: 1, science_profile: 1, profile_version: 1, expected: null, pinned_profile_id: "", build_match: "match", profile_match: "mismatch" },
    led: { on: true, preset: "run", delay_us: 7, width_us: 60, guard_fault: false, guard_trips: 0 },
  } as unknown as InstrumentStatus;

  it("shows PL core as before and Runs on: PL (every frame); no Focus section", async () => {
    await render({ ...IDLE, processing: { ...IDLE.processing, hostProcessing: false, background: { supported: false, set: false } }, plCore: { instrument }, focus: { supported: false, status: null } });
    expect(headings()).toEqual(["Camera", "Processing", "PL core", "Experiment"]);
    expect(text("processing")).toEqual({ "Runs on": "PL (every frame)", "px → µm": "0.5" });
    expect(text("pl-core")).toEqual({ Build: "1a2b3c4d", Weights: "9f8e7d6c", LED: "run 7/60 µs", Sensor: "—", Link: "—", Latency: "—" });
    expect(rows("pl-core").Build.className).toContain("tsb-row__v--confirmed");
    expect(rows("pl-core").Weights.className).toContain("tsb-row__v--mute");
    expect(host.querySelector("[data-testid=telemetry-focus]")).toBeNull();
    expect(section("fluidics").textContent).toBe("Fluidics · not connected");
  });

  it("no PL device reads as such, not —", async () => {
    await render({ ...IDLE, plCore: { instrument: { available: false, error: "no PL device" } as unknown as InstrumentStatus } });
    expect(text("pl-core").Build).toBe("no PL device");
  });
});

describe("one pump poll (usePumpStatus)", () => {
  /** App's host: the sidebar reads the shared poll, and the hardware panel is mounted beside it. */
  function Host({ panel }: { panel: boolean }) {
    const hw = usePumpStatus(true);
    return <>
      <TelemetrySidebar {...IDLE} fluidics={{ pumps: hw.pumps }} />
      {panel && <HardwareControls ready experimentActive={false} append={() => {}} mode="operator" armed={false} onDisarm={() => {}} />}
    </>;
  }

  it("stays one interval with both consumers mounted, and both see the same pumps", async () => {
    vi.useFakeTimers();
    vi.mocked(bridge.fetchPumpStatus).mockImplementation(async (id: number) => pump({ modbus_address: id + 1 }));
    vi.mocked(bridge.fetchAutofocusStatus).mockResolvedValue(focus());
    vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue({ valid: false } as never);
    await act(async () => root.render(<Host panel />));
    await act(async () => { await vi.advanceTimersByTimeAsync(PUMP_STATUS_POLL_MS * 3); });
    // One read per period (two slots each), whatever the number of consumers.
    expect(vi.mocked(bridge.fetchPumpStatus).mock.calls.length).toBe(2 * 4);
    expect(vi.mocked(bridge.fetchAutofocusStatus).mock.calls.length).toBe(4);
    expect(text("fluidics")["Sample pump"]).toBe("0.040 µL/s · running");
    expect(host.querySelector(".hardware-controls")!.textContent).toContain("Connected · Forward");
    // The panel unmounts: the sidebar keeps the poll, still one interval.
    await act(async () => root.render(<Host panel={false} />));
    await act(async () => { await vi.advanceTimersByTimeAsync(PUMP_STATUS_POLL_MS * 2); });
    expect(vi.mocked(bridge.fetchPumpStatus).mock.calls.length).toBe(2 * 6);
    expect(text("fluidics")["Sample pump"]).toBe("0.040 µL/s · running");
  });

  it("a failed read clears the pumps (the column says not connected), and the poll stops with the last consumer", async () => {
    vi.useFakeTimers();
    vi.mocked(bridge.fetchPumpStatus).mockRejectedValue(new Error("Transport lost"));
    vi.mocked(bridge.fetchAutofocusStatus).mockResolvedValue(focus());
    await act(async () => root.render(<Host panel={false} />));
    expect(section("fluidics").textContent).toBe("Fluidics · not connected");
    await act(async () => root.render(<div />));
    const before = vi.mocked(bridge.fetchPumpStatus).mock.calls.length;
    await act(async () => { await vi.advanceTimersByTimeAsync(PUMP_STATUS_POLL_MS * 3); });
    expect(vi.mocked(bridge.fetchPumpStatus).mock.calls.length).toBe(before);
  });
});
