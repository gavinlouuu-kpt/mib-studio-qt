// @vitest-environment jsdom
// The classic Hardware setup page (#798): one card per device from one HardwareControls instance
// (one poll), both pumps from one PumpCard, gated actions locked in Operator mode with every Stop
// enabled, forms behind the ViewerGate while Back to preflight stays, and the asked-for card
// scrolled into view.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { bridge } from "../bridge";
import type { AutofocusConfig, PumpStatus } from "../bridge";
import type { OperatingMode } from "../commissioning";
import type { SetupSection } from "../workspace/PrepareSummary";
import { HardwareControls } from "./HardwareControls";
import { HardwareSetupCards, HardwareSetupPage, lockChip, modeNotice } from "./HardwareSetupPage";
import { StageControls } from "./StageControls";
import { ViewerGate } from "./ViewerGate";
import { zeroed } from "./stageTestFixtures";

let sessionState: { clientId?: number; controllerId?: number | null } = {};
const listeners = new Set<(s: typeof sessionState) => void>();
vi.mock("../transport/index", () => ({
  get transport() {
    return { kind: "ws", invoke: vi.fn(), session: { get: () => sessionState, subscribe: (l: (s: typeof sessionState) => void) => { listeners.add(l); return () => listeners.delete(l); } } };
  },
}));
vi.mock("../bridge", () => ({ bridge: {
  fetchPumpStatus: vi.fn(), fetchAutofocusStatus: vi.fn(), fetchAutofocusConfig: vi.fn(),
  pumpStart: vi.fn(), pumpStop: vi.fn(), pumpStopPurge: vi.fn(), pumpPurge: vi.fn(), pumpConnectModel: vi.fn(), pumpPollStatus: vi.fn(),
  autofocusSetEnabled: vi.fn(), autofocusJog: vi.fn(),
  fetchStageStatus: vi.fn(), stageStop: vi.fn(),
  pulseGeneratorStatus: vi.fn(), pulseGeneratorCommand: vi.fn(),
  startupDiscoverySetPreference: vi.fn(), startupDiscoveryStatus: vi.fn(), startupDiscoveryRun: vi.fn(),
} }));

const pump: PumpStatus = { valid: true, connected: true, run_status: 0, current_flow_rate: 1, accumulated_volume: 0, min_flow_rate: 0, max_flow_rate: 10, stalled: false, com_port: 1, baud_rate: 115200, modbus_address: 1, configured_flow_rate: 1, flow_rate_unit: 100, direction: 0 };
const config: AutofocusConfig = { valid: true, focus_setpoint: 1, focus_range: .1, voltage_step: 1, fine_voltage_step: .1, min_voltage: 0, max_voltage: 100, initial_voltage: 50, manual_voltage_step: 1, ring_ratio_stale_ms: 500, min_samples_per_step: 1, safe_shutdown_voltage: 0, require_new_sample_per_step: true, focus_direction: true };
const PZ = {
  instrument: "pz7035" as const, autofocus: false, trigger: false, host_background: false, frame_buffer: false, reanalysis: false, core_updates: false,
  egrabber_script: false, pl_identity: true, led_strobe: true, align_mode: false, run_mode: false,
  pump: { model: "tushui_peristaltic", port: "/dev/ttyPS1", sample_address: 3, sheath_address: 4, microliters_per_rev: 25 },
};

let host: HTMLDivElement, root: Root;
const onBack = vi.fn(), onMode = vi.fn(), append = vi.fn(), onDisarm = vi.fn();
const button = (name: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === name) as HTMLButtonElement;
const buttons = (name: string) => [...host.querySelectorAll("button")].filter((b) => b.textContent === name) as HTMLButtonElement[];
const labels = () => [...host.querySelectorAll("label")].map((l) => l.textContent?.replace(/[\d.]+$/, "").trim());
const card = (id: string) => host.querySelector(`[data-testid=setup-${id}]`);

interface Opts { mode?: OperatingMode; armed?: boolean; section?: SetupSection | null; visit?: number; pz?: boolean; experimentActive?: boolean }
/** App's composition: the page header outside the ViewerGate, one HardwareControls inside with the cards as its layout. */
function Page({ mode = "operator", armed = false, section = null, visit = 0, pz = false, experimentActive = false }: Opts) {
  return (
    <HardwareSetupPage onBack={onBack} mode={mode} armed={armed} onMode={onMode}>
      <ViewerGate>
        <HardwareControls ready experimentActive={experimentActive} append={append} mode={mode} armed={armed} onDisarm={onDisarm} capabilities={pz ? PZ : undefined}
          layout={(parts) => (
            <HardwareSetupCards parts={parts} setupSection={section} visit={visit}
              cameraSources={<div data-testid="camera-sources"><button>Cameras</button>{!pz && <button>MindVision</button>}<button>Configure Mock…</button></div>}
              stage={!pz && <ViewerGate><StageControls ready experimentActive={experimentActive} append={append} mode={mode} armed={armed} onDisarm={onDisarm} /></ViewerGate>} />
          )} />
      </ViewerGate>
    </HardwareSetupPage>
  );
}
const render = (o: Opts = {}) => act(async () => root.render(<Page {...o} />));

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  vi.clearAllMocks();
  sessionState = {}; listeners.clear();
  const saved = new Map<string, string>();
  vi.stubGlobal("localStorage", { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => saved.set(k, v) });
  vi.mocked(bridge.fetchPumpStatus).mockResolvedValue(pump);
  vi.mocked(bridge.fetchAutofocusStatus).mockResolvedValue({ valid: true, connected: true, enabled: false, current_voltage: 50, com_port: 3, average_ring_ratio: 1, median_ring_ratio: 1, last_ring_ratio_update_us: 0, ring_ratio_age_us: 0 });
  vi.mocked(bridge.fetchAutofocusConfig).mockResolvedValue(config);
  vi.mocked(bridge.fetchStageStatus).mockResolvedValue({ ...zeroed });
  vi.mocked(bridge.pulseGeneratorStatus).mockResolvedValue({ valid: true, connected: true, owned: false, error: "None", port: "/dev/test", baud: 9600, address: 1, channels: [{ frequency_hz: 5000, duty_percent: 50, output_enabled: false }] });
  vi.mocked(bridge.startupDiscoverySetPreference).mockImplementation(async (preference) => ({ accepted: true, message: "Applied", preference }));
  vi.mocked(bridge.startupDiscoveryStatus).mockResolvedValue({ valid: true, camera_running: false, nanopositioner_running: false, camera_configured: false, nanopositioner_connected: false, camera: { job_id: "0", errors: [] }, nanopositioner: { job_id: "0", errors: [] } });
  vi.mocked(bridge.pumpStop).mockResolvedValue({ ok: true, command: 10, message: "Stopped", operation_id: "0" });
  vi.mocked(bridge.pumpStart).mockResolvedValue({ ok: true, command: 10, message: "Started", operation_id: "0" });
  vi.mocked(bridge.pumpConnectModel).mockResolvedValue({ ok: false, command: 10, message: "No such port: /dev/ttyNOPE", operation_id: "0" });
  vi.mocked(bridge.stageStop).mockResolvedValue({ ok: true, command: 13, message: "Stage stopped", operation_id: "1" });
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.unstubAllGlobals(); });

describe("the page", () => {
  it("renders the seven cards with today's fields and buttons, from one HardwareControls instance (one poll)", async () => {
    await render();
    expect(host.querySelector("h2")!.textContent).toBe("Hardware setup");
    expect(host.textContent).toContain("Reached from a preflight row's Set up… and from Settings · nothing here is required for the mock camera");
    expect(host.querySelector("[data-testid=hardware-setup-back]")!.textContent).toBe("Back to preflight");
    const titles = [...host.querySelectorAll(".mib-card__title")].map((t) => t.textContent);
    expect(titles).toEqual(["Camera sources and startup selection", "Sample pump", "Sheath pump", "Focus · autofocus / nanopositioner", "Z stage", "Acquisition pulse generator (RS485)"]);
    // The camera card holds App's sources and the startup selection.
    expect(card("camera")!.querySelector("[data-testid=camera-sources]")).not.toBeNull();
    expect(card("camera")!.textContent).toContain("Startup hardware selection");
    for (const b of ["Auto-select hardware now", "Retry camera selection", "Retry nanopositioner selection", "Configure Mock…"]) expect(card("camera")!.contains(button(b)), b).toBe(true);
    // Today's fields, every one of them, and today's buttons.
    for (const l of ["Pump model", "System serial port (COM3 or /dev/ttyUSB0)", "Baud rate", "Device address", "Flow rate", "Rate unit", "Direction", "Syringe volume (1–9999)", "Volume unit",
      "Nanopositioner backend", "COM port number", "Focus setpoint", "Jog step (V)", "Positive focus direction", "System serial port", "USB serial number", "Modbus address", "Go to position (µm)", "Step", "Channel", "Frequency (Hz)", "Duty (%)", "Auto-select hardware on next startup"]) {
      expect(labels().some((t) => t?.startsWith(l)), l).toBe(true);
    }
    for (const b of ["Connect", "Disconnect", "Read device status", "Apply rate", "Apply direction", "Apply volume", "Run configured pump", "Purge infuse", "Stop pump", "Stop purge",
      "Connect autofocus", "Disconnect autofocus", "Read status", "Enable autofocus", "Disable autofocus", "Jog voltage −", "Jog voltage +", "Apply autofocus configuration",
      "Connect stage", "Disconnect stage", "Stop stage", "Set zero here…", "Go to position", "Connect pulse generator", "Disconnect pulse generator", "Set frequency", "Set duty", "Enable pulse output", "Disable pulse output"]) {
      expect(button(b), b).toBeDefined();
    }
    // One instance, one poll: both slots read once on mount.
    expect(vi.mocked(bridge.fetchPumpStatus).mock.calls.map(([id]) => id).sort()).toEqual([0, 1]);
    expect(vi.mocked(bridge.fetchAutofocusStatus)).toHaveBeenCalledTimes(1);
    expect(host.querySelectorAll(".hardware-controls:not(.stage-controls)").length).toBe(0); // the parts are placed, not the panel
  });

  it("renders both pumps from one PumpCard, with the connection as a chip", async () => {
    vi.mocked(bridge.fetchPumpStatus).mockImplementation(async (id: number) => ({ ...pump, connected: id === 0 }));
    await render();
    const pumps = [...host.querySelectorAll("[data-testid=setup-pump]")];
    expect(pumps.map((p) => p.querySelector(".mib-card__title")!.textContent)).toEqual(["Sample pump", "Sheath pump"]);
    expect(pumps.map((p) => p.querySelector(".mib-card__status .mib-chip")!.textContent)).toEqual(["Connected", "Not connected"]);
    expect(pumps.every((p) => p.querySelector("[data-testid^=setup-pump-gate-]"))).toBe(true);
    expect(buttons("Run configured pump").length).toBe(2);
    expect(buttons("Stop purge").length).toBe(2);
  });

  it("the Mode notice is the existing Mode switch: Operator locks, Service unlocks, its button switches", async () => {
    await render();
    const notice = host.querySelector("[data-testid=hardware-setup-mode]")!;
    expect(notice.getAttribute("data-tone")).toBe("unknown");
    expect(notice.textContent).toContain("Operator mode · run, purge, jog and move are locked");
    expect(notice.textContent).toContain("Connecting devices and reading their status is always allowed. Dangerous actions need Service / Commissioning mode and arming.");
    await act(async () => button("Enter Service mode…").click());
    expect(onMode).toHaveBeenCalledWith("service");
    await render({ mode: "service", armed: true });
    expect(host.querySelector("[data-testid=hardware-setup-mode]")!.getAttribute("data-tone")).toBe("warn");
    await act(async () => button("Exit to Operator").click());
    expect(onMode).toHaveBeenLastCalledWith("operator");
    expect(modeNotice("service", false).title).toBe("Service / Commissioning mode · not armed");
  });
});

describe("gated actions", () => {
  it("Operator mode: the gated tiles are locked, every gated action disabled, every Stop enabled and working", async () => {
    await render();
    for (const id of ["setup-pump-gate-0", "setup-pump-gate-1", "setup-focus-gate", "setup-pulse-gate"]) {
      const tile = host.querySelector(`[data-testid=${id}]`)!;
      expect(tile.getAttribute("data-locked"), id).toBe("locked");
      expect(tile.querySelector(".mib-chip")!.textContent, id).toBe("Locked · Service / Commissioning mode");
      expect(tile.textContent).toContain("Enter Service / Commissioning mode to actuate hardware.");
    }
    for (const b of [...buttons("Run configured pump"), ...buttons("Purge infuse"), button("Enable autofocus"), button("Jog voltage −"), button("Jog voltage +"), button("Enable pulse output")]) expect(b.disabled).toBe(true);
    for (const b of [...buttons("Stop pump"), ...buttons("Stop purge"), button("Disable autofocus"), button("Disable pulse output"), button("Stop stage")]) {
      expect(b.disabled, b.textContent!).toBe(false);
      expect(b.closest(".hw-destructive, .stage-stop"), b.textContent!).not.toBeNull();
    }
    await act(async () => buttons("Stop pump")[1].click());
    expect(bridge.pumpStop).toHaveBeenCalledWith(1);
    await act(async () => button("Stop stage").click());
    expect(bridge.stageStop).toHaveBeenCalled();
    expect(lockChip("Arm the control before firing.")).toEqual({ tone: "unknown", text: "Locked · not armed" });
    expect(lockChip("Device is not connected.")).toEqual({ tone: "unknown", text: "Locked · not connected" });
    expect(lockChip("")).toEqual({ tone: "confirmed", text: "Unlocked · armed" });
  });

  it("Service mode and arming enable them; one action consumes the arming", async () => {
    await render({ mode: "service", armed: true });
    expect(host.querySelector("[data-testid=setup-pump-gate-0]")!.getAttribute("data-locked")).toBe("unlocked");
    expect(buttons("Run configured pump")[0].disabled).toBe(false);
    expect(button("Jog voltage +").disabled).toBe(false);
    await act(async () => buttons("Run configured pump")[0].click());
    expect(bridge.pumpStart).toHaveBeenCalledWith(0);
    expect(onDisarm).toHaveBeenCalledOnce();
  });

  it("a locked experiment: every tile says so, Stop still works", async () => {
    await render({ mode: "service", armed: true, experimentActive: true });
    expect(host.querySelector("[data-testid=setup-pump-gate-0] .mib-chip")!.textContent).toBe("Locked · experiment active");
    expect(buttons("Stop pump")[0].disabled).toBe(false);
  });
});

describe("errors and viewers", () => {
  it("a refused pump Connect shows the error in the page, never a success", async () => {
    vi.mocked(bridge.fetchPumpStatus).mockResolvedValue({ ...pump, connected: false });
    await render();
    const port = [...host.querySelectorAll("label")].find((l) => l.textContent?.startsWith("System serial port (COM3"))!.querySelector("input")!;
    await act(async () => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(port, "/dev/ttyNOPE"); port.dispatchEvent(new Event("input", { bubbles: true })); });
    await act(async () => buttons("Connect")[0].click());
    expect(bridge.pumpConnectModel).toHaveBeenCalledWith(0, 0, "/dev/ttyNOPE", 115200, 1, 25);
    expect(host.querySelector("[data-testid=setup-error]")!.textContent).toContain("No such port: /dev/ttyNOPE");
    expect(append).toHaveBeenCalledWith(expect.stringContaining("No such port"));
  });

  it("a viewer's forms are disabled; Back to preflight and the Mode switch are not", async () => {
    await render();
    sessionState = { clientId: 7, controllerId: 3 };
    await act(async () => { listeners.forEach((l) => l(sessionState)); });
    for (const b of [...buttons("Connect"), ...buttons("Stop pump"), button("Connect autofocus"), button("Connect stage"), button("Stop stage"), button("Connect pulse generator")]) expect(b.matches(":disabled"), b.textContent!).toBe(true);
    expect([...host.querySelectorAll(".hw-setup__cards input")].every((i) => i.matches(":disabled"))).toBe(true);
    const back = host.querySelector<HTMLButtonElement>("[data-testid=hardware-setup-back]")!;
    expect(back.matches(":disabled")).toBe(false);
    await act(async () => back.click());
    expect(onBack).toHaveBeenCalledOnce();
    expect(button("Enter Service mode…").matches(":disabled")).toBe(false);
  });
});

describe("sections and instruments", () => {
  it("setupSection scrolls the right card into view, again on a later visit", async () => {
    const scrolled: string[] = [];
    Element.prototype.scrollIntoView = function () { scrolled.push((this as HTMLElement).getAttribute("data-setup-card")!); };
    await render({ section: "fluidics", visit: 1 });
    expect(scrolled).toEqual(["pumps"]);
    await render({ section: "focus", visit: 2 });
    await render({ section: "camera", visit: 3 });
    await render({ section: "camera", visit: 4 });
    expect(scrolled).toEqual(["pumps", "focus", "camera", "camera"]);
    await render({ section: null, visit: 5 });
    expect(scrolled.length).toBe(4);
  });

  it("the PZ7035 hides the Z stage, the focus card, the pulse generator and the startup selection, and keeps the peristaltic pumps", async () => {
    vi.mocked(bridge.fetchPumpStatus).mockResolvedValue({ ...pump, connected: false });
    await render({ pz: true });
    const titles = [...host.querySelectorAll(".mib-card__title")].map((t) => t.textContent);
    expect(titles).toEqual(["Camera sources and startup selection", "Sample pump", "Sheath pump"]);
    expect(card("stage")).toBeNull();
    expect(host.textContent).not.toContain("MindVision");
    expect(host.textContent).not.toContain("nanopositioner");
    expect(host.textContent).toContain("This instrument's peristaltic pumps share /dev/ttyPS1: Sample at Modbus address 3, Sheath at 4.");
    expect([...host.querySelectorAll("label")].filter((l) => l.textContent?.startsWith("Device address")).map((l) => l.querySelector("input")!.value)).toEqual(["3", "4"]);
  });
});
