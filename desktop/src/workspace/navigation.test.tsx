// @vitest-environment jsdom
// P1 gate (#713, exec plan P1/P2), extended for O2 Align (A3): pure navigation in the opt-in
// workspace never actuates. The whole App runs over a mocked transport that records every backend
// command. Opening the workspace, visiting every stage, going back to the classic view and opening
// the classic Connect tab from Prepare must leave the backend command log as it was: no command
// that the idle app does not already send in its steady state, except two reads (Prepare's
// readiness read, always without a path, and Align's camera geometry read-back), and nothing that
// acts or writes. Align's camera mode switch, which the classic Camera & Alignment tab does on
// entry, happens only when the operator presses its button.
// A control proves the harness sees actuation: the classic Camera & Alignment tab does switch
// the camera mode, on the desktop (set_camera_overview) and on the PZ7035 (set_instrument_mode).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import App from "../App";
import { BRIDGE_ABI_VERSION, COMMAND_TYPES, EXPERIMENT_STATES, JSON_TRANSPORT, READINESS_GATE_STATUSES as G, RUN_COMPLETION_STATES } from "../bridgeContract";
import { DESKTOP_CAPABILITIES } from "../platformCapabilities";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/app", () => ({ getVersion: vi.fn(async () => "1.2.0") }));
vi.mock("@tauri-apps/api/window", () => ({ getCurrentWindow: () => ({ onCloseRequested: async () => () => {}, close: async () => {} }) }));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(async () => null), save: vi.fn(async () => null), confirm: vi.fn(async () => false), message: vi.fn(async () => {}) }));
vi.mock("@tauri-apps/plugin-opener", () => ({ openUrl: vi.fn(), revealItemInDir: vi.fn() }));

/** Commands that act on hardware, files or the session, or write settings: none may follow from navigation. */
const ACTUATING = [
  "save_camera_roi", "set_processing_roi", "apply_processing_config_json", "apply_camera_script", "apply_pz_camera_document",
  "select_hardware_camera", "select_mindvision_camera",
  "start_capture", "stop_capture", "set_camera_overview", "set_instrument_mode", "set_instrument_led", "set_service_mode",
  "stage_connect", "stage_move_to", "stage_move_by", "stage_set_zero", "stage_stop", "autofocus_connect", "autofocus_jog", "autofocus_set_enabled",
  "pump_connect", "pump_start", "pump_stop", "pump_purge", "pump_set_flow_rate", "trigger_manual_pulse", "trigger_periodic_start",
  "experiment_start", "experiment_stop", "start_recording", "stop_recording", "ring_freeze", "ring_resume",
  "processing_core_command", "install_application_update", "apply_processing", "configure_mock",
];

type Variant = "desktop" | "pz7035";
const PZ_CAPS = { ...DESKTOP_CAPABILITIES, instrument: "pz7035", autofocus: false, trigger: false, host_background: false, frame_buffer: false, reanalysis: false,
  core_updates: false, egrabber_script: false, pl_identity: true, led_strobe: true, align_mode: true, run_mode: true, pump: null };
const status = { transport_version: JSON_TRANSPORT.version, valid: true, state: EXPERIMENT_STATES.Idle, start_time_ns: "0", end_time_ns: "0",
  valid_buffered: "0", invalid_buffered: "0", valid_saved: "0", invalid_saved: "0", dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false,
  output_path: "", message: "", start_generation: "0", readiness_generation: "1", capture_generation: "1", persistence_admitted: "0", persistence_committed: "0",
  persistence_failed: "0", terminal: false, finalization_ok: false, completion: RUN_COMPLETION_STATES.Complete, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "" };
const cmdResult = (command: number) => ({ transport_version: JSON_TRANSPORT.version, ok: true, command, message: "ok", operation_id: "0" });

/** The camera as the mock backend holds it: the mode switches change it, the reads report it. */
let device = { overview: false, mode: "unknown", runX: 0, runY: 0, runSet: false };

function respond(variant: Variant, cmd: string, args?: unknown): unknown {
  const pz = variant === "pz7035";
  const a = (args ?? {}) as Record<string, unknown>;
  switch (cmd) {
    case "abi_version": return BRIDGE_ABI_VERSION;
    case "is_initialized": return true;
    case "fetch_preview_buffer": return { capture_running: false, recording: false, available: false };
    case "fetch_experiment_status": return status;
    case "fetch_review_metadata": return { valid: false, file_open: false, file_path: "", recording_file: false };
    case "fetch_platform_info": return pz ? { science: "pl", host_processing: false, aravis: true, capabilities: PZ_CAPS } : { science: "host", host_processing: true, aravis: false };
    case "fetch_camera_selection": return { valid: true, configured: true, mode: 0, label: pz ? "PZ7035 IMX426" : "Camera 01", interface_index: 0, device_index: 0, mindvision_index: 0,
      mindvision_config_path: "", camera_script_path: "", mock_frame_dir: "", mock_interval_ms: 33, mock_loop: true };
    case "discover_cameras": return { cameras: [], framegrabbers: [] };
    case "fetch_camera_geometry": return { supported: true, overview: device.overview, camera: "cam", sensor_width: 2048, sensor_height: 512, roi: { x: 0, y: 0, width: 512, height: 96 },
      width_increment: 16, height_increment: 4, offset_x_increment: 16, offset_y_increment: 4, min_width: 64, min_height: 4, session: {} };
    case "fetch_instrument_status": return pz ? { available: true, mode: { name: device.mode, run_x: device.runX, run_y: device.runY, run_set: device.runSet, service: false } }
      : { available: false, error: "not a PZ7035" };
    case "fetch_experiment_readiness": return { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "1",
      gates: [{ id: "storage.output", status: G.Unavailable, reason: "no output path chosen yet", remediation: "choose an HDF5 destination" }] };
    case "poll_events_exact": return { transport_version: JSON_TRANSPORT.version, events: [] };
    case "set_camera_overview": device.overview = !!a.overview; return cmdResult(COMMAND_TYPES.Camera);
    case "save_camera_roi": return cmdResult(COMMAND_TYPES.Camera);
    case "set_instrument_mode":
      device.mode = String(a.mode);
      if (a.mode === "run") Object.assign(device, { runX: a.x, runY: a.y, runSet: true });
      return cmdResult(COMMAND_TYPES.Camera);
    case "review_export_status_json":
    case "review_reanalysis_status_json": return JSON.stringify({ state: "idle" });
    default: return null; // reads the shell tolerates as "not reported"
  }
}

let host: HTMLDivElement, root: Root;
let log: { cmd: string; args: unknown }[] = [];
const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const buttonByText = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text);
const click = async (el: Element | null | undefined, what: string) => {
  if (!el) throw new Error(`no ${what}`);
  await act(async () => (el as HTMLElement).click());
  await advance(500);
};
const namesIn = (entries: { cmd: string }[]) => new Set(entries.map((e) => e.cmd));

async function boot(variant: Variant) {
  vi.mocked(invoke).mockImplementation(async (cmd: string, args?: unknown) => {
    log.push({ cmd, args });
    return respond(variant, cmd, args);
  });
  await act(async () => root.render(<App />));
  await advance(6000); // init, the startup landing, the first polls
  expect(host.textContent).toContain("backend: ready");
}

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  device = { overview: false, mode: "unknown", runX: 0, runY: 0, runSet: false };
  const saved = new Map<string, string>();
  const store = { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v), removeItem: (k: string) => void saved.delete(k) };
  vi.stubGlobal("localStorage", store);
  vi.stubGlobal("sessionStorage", store);
  vi.useFakeTimers();
  vi.mocked(invoke).mockReset();
  log = [];
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

/** Open the workspace, visit every stage, leave, come back, and open the classic Connect tab
 *  from Prepare's Camera setup. Returns every command sent meanwhile. */
async function navigateWorkspace() {
  await click(buttonByText("Workspace preview"), "workspace toggle");
  expect(host.querySelector("[data-testid=workspace-prepare]")).not.toBeNull();
  await advance(5000);
  for (const tab of host.querySelectorAll(".mib-stages__tab")) await click(tab, "stage tab");
  await click(host.querySelectorAll(".mib-stages__tab")[0], "Prepare");
  await click(buttonByText("Classic view"), "Classic view");
  await click(buttonByText("Workspace preview"), "workspace toggle");
  await click(buttonByText("Camera setup"), "Camera setup");
  expect(host.querySelector("[data-testid=workspace]")).toBeNull();
  await advance(6000);
  return log.splice(0);
}

/** The steady state: what the app sends by itself over a quiet window after start-up. */
async function steadyState() {
  log.splice(0);
  await advance(6000);
  return log.splice(0);
}

const readinessReads = (entries: { cmd: string; args: unknown }[]) => entries.filter((e) => e.cmd === "fetch_experiment_readiness");

describe.each<Variant>(["desktop", "pz7035"])("workspace navigation never actuates (%s)", (variant) => {
  it("leaves the backend command log as it was, apart from Prepare's path-less readiness read", async () => {
    await boot(variant);
    const idle = await steadyState();
    expect(idle.length).toBeGreaterThan(0);
    expect(readinessReads(idle)).toEqual([]); // the classic view never polls readiness

    const navigation = await navigateWorkspace();
    expect(navigation.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    // Two reads are added: Prepare's gates and Align's camera read-back (the classic tab's poll).
    expect([...namesIn(navigation)].filter((n) => !namesIn(idle).has(n)).sort()).toEqual(["fetch_camera_geometry", "fetch_experiment_readiness"]);
    expect(readinessReads(navigation).length).toBeGreaterThan(0);
    for (const r of readinessReads(navigation)) expect(r.args).toEqual({ outputPath: "" });

    // Back on the classic Connect tab: the steady state is what it was before.
    const after = await steadyState();
    expect([...namesIn(after)].sort()).toEqual([...namesIn(idle)].sort());
  });

  it("from the classic Camera & Alignment tab too: only reads are added, the mode is not switched again", async () => {
    await boot(variant);
    await click(host.querySelector('button[aria-label^="Camera & Alignment:"]'), "classic Camera & Alignment tab");
    await advance(3000);
    const idle = await steadyState();
    const navigation = await navigateWorkspace();
    expect(navigation.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    // Prepare (and then the classic Connect tab) poll the checklist devices: reads only.
    const added = [...namesIn(navigation)].filter((n) => !namesIn(idle).has(n));
    expect(added.length).toBeGreaterThan(0);
    for (const n of added) expect(n).toMatch(/^fetch_/);
    for (const r of readinessReads(navigation)) expect(r.args).toEqual({ outputPath: "" });
  });

  it("Align offers the classic tab's mode switch as a button: a visit sends no switch, a press sends exactly the classic command", async () => {
    await boot(variant);
    const idle = await steadyState();
    await click(buttonByText("Workspace preview"), "workspace toggle");
    await click(host.querySelectorAll(".mib-stages__tab")[1], "Align");
    await advance(6000);
    const visit = log.splice(0);
    expect(visit.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    for (const n of [...namesIn(visit)].filter((n) => !namesIn(idle).has(n))) expect(["fetch_camera_geometry", "fetch_experiment_readiness"]).toContain(n);
    const notice = host.querySelector("[data-testid=align-mode]")!;
    expect(notice.textContent).toContain(variant === "pz7035" ? "Mode · unknown" : "Experiment window");
    const action = variant === "pz7035" ? "Switch to Align" : "Show the full sensor";
    await click(buttonByText(action), action);
    const pressed = log.splice(0).filter((e) => ACTUATING.includes(e.cmd));
    expect(pressed).toEqual(variant === "pz7035"
      ? [{ cmd: "set_instrument_mode", args: { mode: "align", x: expect.any(Number), y: expect.any(Number) } }]
      : [{ cmd: "set_camera_overview", args: { overview: true } }]);
  });

  it("the ROI quick control writes only on Write, through the classic tab's command", async () => {
    await boot(variant);
    await click(buttonByText("Workspace preview"), "workspace toggle");
    await click(host.querySelectorAll(".mib-stages__tab")[1], "Align");
    await advance(3000);
    log.splice(0);
    const field = host.querySelector<HTMLInputElement>('input[aria-label$=" X"]')!;
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(field, "40");
      field.dispatchEvent(new Event("input", { bubbles: true }));
    });
    await advance(3000);
    expect(log.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]); // typing writes nothing
    await click(buttonByText(variant === "pz7035" ? "Write Run window" : "Write ROI"), "Write");
    const writes = log.splice(0).filter((e) => ACTUATING.includes(e.cmd));
    if (variant === "pz7035") {
      // The Run window is placed here and applied by the switch to Run, as in the classic tab.
      expect(writes).toEqual([]);
      expect(host.querySelector("[data-testid=align-state-roi]")!.textContent).toBe("Written · applied when Experiment opens (the switch to Run)");
    } else {
      expect(writes).toEqual([{ cmd: "save_camera_roi", args: { x: 48, y: 0, w: 512, h: 96 } }]); // 40 snapped to the x step of 16
    }
  });

  it("leaving the workspace after Align's switch acts on nothing; the classic tab shows the mismatch with an explicit switch", async () => {
    await boot(variant);
    // The classic Experiment tab, then the workspace (the reviewed repro).
    await click(host.querySelector('button[aria-label^="Experiment:"]'), "classic Experiment tab");
    await advance(3000);
    log.splice(0);
    await click(buttonByText("Workspace preview"), "workspace toggle");
    await click(host.querySelectorAll(".mib-stages__tab")[1], "Align");
    await advance(3000);
    if (variant === "pz7035") {
      // Placing the Run window while the classic Experiment tab is hidden must not switch to Run.
      const x = host.querySelector<HTMLInputElement>('input[aria-label$=" X"]')!;
      await act(async () => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(x, "40");
        x.dispatchEvent(new Event("input", { bubbles: true }));
      });
      await click(buttonByText("Write Run window"), "Write Run window");
      await advance(3000);
    }
    expect(log.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    await click(buttonByText(variant === "pz7035" ? "Switch to Align" : "Show the full sensor"), "Align switch");
    await advance(3000);
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd)).map((e) => e.cmd)).toEqual([variant === "pz7035" ? "set_instrument_mode" : "set_camera_overview"]);
    // Leave: nothing acts, now or in the steady state after.
    await click(buttonByText("Classic view"), "Classic view");
    await advance(6000);
    expect(host.querySelector("[data-testid=workspace]")).toBeNull();
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    // The classic Experiment tab says what changed and offers its own mode as a button.
    const notice = host.querySelector("[data-testid=classic-mode-notice]")!;
    expect(notice.textContent).toContain(variant === "pz7035" ? "Camera mode · Align, this tab uses Run" : "Camera mode · the full sensor, this tab uses the experiment window");
    const action = variant === "pz7035" ? "Switch to Run" : "Show the experiment window";
    await advance(6000);
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    await click(buttonByText(action), action);
    await advance(3000);
    expect(log.filter((e) => ACTUATING.includes(e.cmd))).toEqual(variant === "pz7035"
      ? [{ cmd: "set_instrument_mode", args: { mode: "run", x: 40, y: expect.any(Number) } }]
      : [{ cmd: "set_camera_overview", args: { overview: false } }]);
    expect(host.querySelector("[data-testid=classic-mode-notice]")).toBeNull();
  });

  it("control: the classic Camera & Alignment tab does switch the camera mode, so the harness sees actuation", async () => {
    await boot(variant);
    log.splice(0);
    await click(host.querySelector('button[aria-label^="Camera & Alignment:"]'), "classic Camera & Alignment tab");
    await advance(3000);
    expect(log.map((e) => e.cmd)).toContain(variant === "pz7035" ? "set_instrument_mode" : "set_camera_overview");
  });
});
