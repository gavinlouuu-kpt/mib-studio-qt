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
import { save } from "@tauri-apps/plugin-dialog";
import App from "../App";
import { BRIDGE_ABI_VERSION, CALIBRATION_APPLY_OUTCOMES, COMMAND_TYPES, EXPERIMENT_START_OUTCOMES, EXPERIMENT_STATES, EXPERIMENT_STOP_OUTCOMES as S, JSON_TRANSPORT,
  READINESS_GATE_STATUSES as G, RUN_COMPLETION_STATES } from "../bridgeContract";
import { DESKTOP_CAPABILITIES } from "../platformCapabilities";
import { applyReply, calList as calListOf, calRecord } from "./calibrationFixtures";
import { LANDING_STORAGE_KEY } from "./landing";
import quickLookGolden from "../../../crates/mib-bridge/contract/fixtures/run-quick-look-v1.json";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/app", () => ({ getVersion: vi.fn(async () => "1.2.0") }));
vi.mock("@tauri-apps/api/window", () => ({ getCurrentWindow: () => ({ onCloseRequested: async () => () => {}, close: async () => {} }) }));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(async () => null), save: vi.fn(async () => null), confirm: vi.fn(async () => false), message: vi.fn(async () => {}) }));
vi.mock("@tauri-apps/plugin-opener", () => ({ openUrl: vi.fn(), revealItemInDir: vi.fn() }));
// Whether this client controls the instrument: a store the test flips (a browser's lost and
// regained control; the desktop always holds it, so the default is held).
const control = vi.hoisted(() => ({ held: true, listeners: new Set<() => void>() }));
vi.mock("../transport/useControlRole", async (importOriginal) => {
  const actual = await importOriginal<typeof import("../transport/useControlRole")>();
  const { useSyncExternalStore } = await import("react");
  return { ...actual, useHoldsControl: () => useSyncExternalStore(
    (l: () => void) => { control.listeners.add(l); return () => { control.listeners.delete(l); }; }, () => control.held) };
});
const setControl = (held: boolean) => act(async () => { control.held = held; for (const l of control.listeners) l(); });

/** Commands that act on hardware, files or the session, or write settings: none may follow from navigation. */
const ACTUATING = [
  "save_camera_roi", "set_processing_roi", "apply_processing_config_json", "apply_camera_script", "apply_pz_camera_document",
  "select_hardware_camera", "select_mindvision_camera",
  "start_capture", "stop_capture", "set_camera_overview", "set_instrument_mode", "set_instrument_led", "set_service_mode",
  "stage_connect", "stage_move_to", "stage_move_by", "stage_set_zero", "stage_stop", "autofocus_connect", "autofocus_jog", "autofocus_set_enabled",
  "pump_connect", "pump_start", "pump_stop", "pump_purge", "pump_set_flow_rate", "trigger_manual_pulse", "trigger_periodic_start",
  "experiment_start", "experiment_stop", "start_recording", "stop_recording", "ring_freeze", "ring_resume",
  "processing_core_command", "install_application_update", "apply_processing", "configure_mock",
  "calibration_apply",
];

/** The reads Prepare (and the Plan, which takes its verdict and declares its calibration record)
 *  add: the path-less readiness and the calibration records list (#716 A4b). Neither acts. */
const PREPARE_READS = ["calibration_records_list", "fetch_experiment_readiness"];

/** What showing Experiment · Live (O4, A6b) adds: monitoring on while it is shown and off when it is
 *  left with no run in progress (the classic Monitoring tab's visibility gate, BE-5), and reads. None
 *  of them acts on hardware. */
const LIVE_COMMANDS = ["monitoring_set_active", "fetch_monitoring_snapshot", "fetch_monitoring_cells", "fetch_monitoring_cell_packet"];
/** The last monitoring switch sent: Live turns monitoring off again when it is left. */
const lastMonitoring = (entries: { cmd: string; args: unknown }[]) => entries.filter((e) => e.cmd === "monitoring_set_active").pop()?.args;

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
/** Set to hold the next camera geometry read back until the test releases it. */
let holdNextGeometry = false;
/** A host that can start a run (A5): the camera streams and the pinned core is active. */
let canStart = false;
/** The backend's answer to experiment_start (ABI 40: with its start outcome code). */
let startReply: { ok: boolean; message: string; start_outcome?: number } = { ok: true, message: "experiment started", start_outcome: EXPERIMENT_START_OUTCOMES.Started };
/** Set to answer as a backend without the ABI 40 outcome codes: no start_outcome or stop_outcome in
 *  the replies, and experiment_stop ignores expectedStartGeneration (stops whatever run is active). */
let noOutcomeCodes = false;
/** Set to report the last run Failed (the classic startup landing then selects Experiment). */
let failedRun = false;
/** Set to hold every camera selection read (the classic startup landing waits for the first). */
let holdSelection = false;
const heldSelection: (() => void)[] = [];
/** Set to make every experiment status read fail once experiment_start was sent. */
let statusFailsAfterStart = false;
/** Set to make the mock backend run experiments (A5 queue): a start makes a run Active (a new
 *  generation, its file), a stop makes it Stopping; `finalizeRun` makes it terminal and closed. */
let statefulRuns = false;
let run = { generation: 0, state: EXPERIMENT_STATES.Idle as number, flushing: false, terminal: false, ok: false, path: "" };
/** The recording open in the classic Review ("" when none), and whether load_recording opens it. */
let reviewFile = "";
const NO_DATASET = { present: false, count: 0, width: 0, height: 0, channels: 0 };
let reviewLoads = false;
const finalizeRun = () => Object.assign(run, { state: EXPERIMENT_STATES.Idle, flushing: false, terminal: true, ok: true });
let startSent = false;
/** The calibration records the mock backend lists (#716 A4b), and its answer to calibration_apply. */
const emptyCalList = () => ({ valid: true, store: { revision: 0, max_versions: 2048, error: null }, applied_record_id: null, in_effect_record_id: null,
  current_factor: 0.5, current_identity: { camera: "mock:Camera 01", roi: "full", chip: "not tracked", objective: "not tracked", known: true }, records: [] as unknown[] });
let calList: unknown = emptyCalList();
/** The calibration.record gate of the path-less readiness read, when set. */
let calGate: { id: string; status: number; reason: string; remediation: string } | null = null;
let calApply: (a: Record<string, unknown>) => unknown = () => { throw new Error("calibration_apply is not expected here"); };
/** Set to hold the next readiness read with a file (the Start flow's), until the test releases it. */
let holdStartReadiness = false;
const heldReadiness: (() => void)[] = [];
/** Set to hold the checklist's core status reads; the test answers each one. */
let holdCore = false;
const heldCore: ((reply: unknown) => void)[] = [];
const CORE_OK = { valid: true, active_version: "2.1.0", contract_version: 1, engine_abi_version: 1, source: "bundled",
  release_tag: "", build_id: "", artifact_sha256: "", required_version: "2.1.0", pin_satisfied: true };
const heldGeometry: (() => void)[] = [];

function respond(variant: Variant, cmd: string, args?: unknown): unknown {
  const pz = variant === "pz7035";
  const a = (args ?? {}) as Record<string, unknown>;
  switch (cmd) {
    case "abi_version": return BRIDGE_ABI_VERSION;
    case "is_initialized": return true;
    case "fetch_preview_buffer": return { capture_running: canStart, recording: false, available: false };
    case "fetch_processing_core_status":
      if (holdCore) return new Promise((resolve) => heldCore.push(resolve));
      return canStart ? CORE_OK : null;
    case "fetch_experiment_status": if (statusFailsAfterStart && startSent) throw new Error("TRANSPORT_LOST");
      return statefulRuns ? { ...status, state: run.state, flushing: run.flushing, terminal: run.terminal, finalization_ok: run.ok, output_path: run.path,
        start_generation: String(run.generation) } : failedRun ? { ...status, state: EXPERIMENT_STATES.Failed, terminal: true } : status;
    case "fetch_review_metadata": return { valid: reviewFile !== "", file_open: reviewFile !== "", file_path: reviewFile, recording_file: false,
      valid_images: NO_DATASET, invalid_images: NO_DATASET, valid_masks: NO_DATASET, invalid_masks: NO_DATASET, recorded_images: NO_DATASET,
      start_time_ns: 0, end_time_ns: 0, total_valid: 0, total_invalid: 0, roi_x: 0, roi_y: 0, roi_w: 0, roi_h: 0, has_background: false,
      has_core_identity: false, core_version: "", core_source: "", core_release_tag: "" };
    case "fetch_platform_info": return pz ? { science: "pl", host_processing: false, aravis: true, capabilities: PZ_CAPS } : { science: "host", host_processing: true, aravis: false };
    case "fetch_camera_selection": if (holdSelection) {
      holdSelection = false;
      const reply = respond(variant, cmd, args);
      holdSelection = true;
      return new Promise((resolve) => heldSelection.push(() => resolve(reply)));
    } return { valid: true, configured: true, mode: 0, label: pz ? "PZ7035 IMX426" : "Camera 01", interface_index: 0, device_index: 0, mindvision_index: 0,
      mindvision_config_path: "", camera_script_path: "", mock_frame_dir: "", mock_interval_ms: 33, mock_loop: true };
    case "discover_cameras": return { cameras: [], framegrabbers: [] };
    case "fetch_camera_geometry": if (holdNextGeometry) {
      holdNextGeometry = false;
      const reply = respond(variant, cmd, args);
      return new Promise((resolve) => heldGeometry.push(() => resolve(reply)));
    } return { supported: true, overview: device.overview, camera: "cam", sensor_width: 2048, sensor_height: 512, roi: { x: 0, y: 0, width: 512, height: 96 },
      width_increment: 16, height_increment: 4, offset_x_increment: 16, offset_y_increment: 4, min_width: 64, min_height: 4, session: {} };
    case "fetch_instrument_status": return pz ? { available: true, mode: { name: device.mode, run_x: device.runX, run_y: device.runY, run_set: device.runSet, service: false } }
      : { available: false, error: "not a PZ7035" };
    case "fetch_experiment_readiness": if (a.outputPath && holdStartReadiness) {
      holdStartReadiness = false;
      const reply = respond(variant, cmd, args);
      return new Promise((resolve) => heldReadiness.push(() => resolve(reply)));
    } return a.outputPath
      ? { transport_version: JSON_TRANSPORT.version, valid: true, ready: true, generation: "2", gates: [{ id: "storage.output", status: G.Pass, reason: "", remediation: "" }] }
      : { transport_version: JSON_TRANSPORT.version, valid: true, ready: false, generation: "1",
        gates: [...(calGate ? [calGate] : []), { id: "storage.output", status: G.Unavailable, reason: "no output path chosen yet", remediation: "choose an HDF5 destination" }] };
    case "experiment_start": startSent = true;
      if (statefulRuns && startReply.ok) run = { generation: run.generation + 1, state: EXPERIMENT_STATES.Active, flushing: false, terminal: false, ok: false, path: String(a.outputPath) };
      if (noOutcomeCodes) return { ...cmdResult(COMMAND_TYPES.Experiment), ok: startReply.ok, message: startReply.message };
      return { ...cmdResult(COMMAND_TYPES.Experiment), ...startReply };
    // With running experiments the shell's 500 ms status poll must get through (it reads the
    // processing stats first).
    case "fetch_processing_stats": return statefulRuns
      ? { transport_version: JSON_TRANSPORT.version, valid: false, algo_fps1s: null, valid_fps1s: null, invalid_fps1s: null, pixel_to_micron: null } : null;
    case "experiment_stop": {
      if (!statefulRuns) return cmdResult(COMMAND_TYPES.Experiment);
      // As ExperimentCoordinator::requestStop: a named run is compared with the run the backend holds
      // (Active, or Stopping until its finalization ends) before anything else.
      const reply = (outcome: number, message: string) => ({ ...cmdResult(COMMAND_TYPES.Experiment), ok: outcome === S.Accepted, message,
        ...(noOutcomeCodes ? {} : { stop_outcome: outcome }) });
      const held = run.state === EXPERIMENT_STATES.Active || run.state === EXPERIMENT_STATES.Stopping;
      const expected = noOutcomeCodes ? undefined : a.expectedStartGeneration;
      if (expected !== undefined && (!held || String(run.generation) !== expected)) {
        return reply(S.RunMismatch, `stop refused: expected run ${String(expected)}, but ${held
          ? `run ${run.generation} is ${run.state === EXPERIMENT_STATES.Active ? "active" : "stopping"}` : "no run is active"}; nothing was stopped`);
      }
      if (run.state === EXPERIMENT_STATES.Stopping) return reply(S.Busy, "Busy");
      if (run.state !== EXPERIMENT_STATES.Active) return reply(S.NotActive, "NotActive");
      Object.assign(run, { state: EXPERIMENT_STATES.Stopping, flushing: true });
      return reply(S.Accepted, "Accepted");
    }
    case "poll_events_exact": return { transport_version: JSON_TRANSPORT.version, events: [] };
    case "set_camera_overview": device.overview = !!a.overview; return cmdResult(COMMAND_TYPES.Camera);
    case "save_camera_roi": return cmdResult(COMMAND_TYPES.Camera);
    case "set_instrument_mode":
      device.mode = String(a.mode);
      if (a.mode === "run") Object.assign(device, { runX: a.x, runY: a.y, runSet: true });
      return cmdResult(COMMAND_TYPES.Camera);
    case "calibration_records_list": return calList;
    // Opening a recording in the classic Review (the Summary's Open in Review, A7b): refused here, so
    // the classic Review stays empty.
    case "load_recording":
      if (!reviewLoads) return { ...cmdResult(COMMAND_TYPES.RecordingLoad), ok: false, message: "not in this mock" };
      reviewFile = String(a.filePath);
      return cmdResult(COMMAND_TYPES.RecordingLoad);
    case "close_review": reviewFile = ""; return cmdResult(COMMAND_TYPES.Review);
    // The run quick look (ABI 44, #716 A7b): each path answered with the golden v3 run, no reference.
    case "run_quick_look": return { ...quickLookGolden, reference: null, message: "",
      runs: (a.paths as string[]).map((p) => ({ ...quickLookGolden.runs[0], path: p, delta: null })) };
    case "calibration_apply": return calApply(a);
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
  calList = emptyCalList();
  calGate = null;
  calApply = () => { throw new Error("calibration_apply is not expected here"); };
  device = { overview: false, mode: "unknown", runX: 0, runY: 0, runSet: false };
  holdNextGeometry = false;
  heldGeometry.length = 0;
  canStart = false;
  reviewFile = "";
  reviewLoads = false;
  startReply = { ok: true, message: "experiment started", start_outcome: EXPERIMENT_START_OUTCOMES.Started };
  noOutcomeCodes = false;
  statusFailsAfterStart = false;
  statefulRuns = false;
  holdStartReadiness = false;
  heldReadiness.length = 0;
  control.held = true;
  run = { generation: 0, state: EXPERIMENT_STATES.Idle, flushing: false, terminal: false, ok: false, path: "" };
  startSent = false;
  holdCore = false;
  heldCore.length = 0;
  failedRun = false;
  holdSelection = false;
  heldSelection.length = 0;
  const saved = new Map<string, string>();
  const store = { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v), removeItem: (k: string) => void saved.delete(k) };
  vi.stubGlobal("localStorage", store);
  vi.stubGlobal("sessionStorage", store);
  // These gates start from the classic view, as an operator who chose it last (landing.ts); the
  // workspace landing itself is tested in "the workspace branch opens in the workspace" below.
  saved.set(LANDING_STORAGE_KEY, "classic");
  vi.useFakeTimers();
  vi.mocked(invoke).mockReset();
  vi.mocked(save).mockReset(); // no file left over from an earlier test's unused answer
  vi.mocked(save).mockResolvedValue(null);
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
    expect([...namesIn(navigation)].filter((n) => !namesIn(idle).has(n)).sort()).toEqual(["calibration_records_list", "fetch_camera_geometry", "fetch_experiment_readiness"]);
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
    for (const n of added) expect(n.startsWith("fetch_") || PREPARE_READS.includes(n)).toBe(true);
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
    for (const n of [...namesIn(visit)].filter((n) => !namesIn(idle).has(n))) expect(["fetch_camera_geometry", ...PREPARE_READS]).toContain(n);
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

  it("a classic tab change still awaiting its read is cancelled by opening the workspace: nothing acts, the classic tab shows the mismatch", async () => {
    await boot(variant);
    log.splice(0);
    // The classic Camera & Alignment tab starts its switch, which first reads the geometry: held.
    holdNextGeometry = true;
    await act(async () => (host.querySelector('button[aria-label^="Camera & Alignment:"]') as HTMLElement).click());
    expect(heldGeometry).toHaveLength(1);
    // The operator opens the workspace before the read answers; then it answers.
    await act(async () => buttonByText("Workspace preview")!.click());
    await act(async () => heldGeometry.shift()!());
    await advance(6000);
    await click(host.querySelectorAll(".mib-stages__tab")[1], "Align");
    await advance(3000);
    expect(log.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    // Back in the classic view: still nothing acts, and the tab says the camera is not in its mode.
    await click(buttonByText("Classic view"), "Classic view");
    await advance(6000);
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    const action = variant === "pz7035" ? "Switch to Align" : "Show the full sensor";
    expect(host.querySelector("[data-testid=classic-mode-notice]")!.textContent).toContain(action);
    await click(buttonByText(action), action);
    await advance(3000);
    expect(log.filter((e) => ACTUATING.includes(e.cmd)).map((e) => e.cmd)).toEqual([variant === "pz7035" ? "set_instrument_mode" : "set_camera_overview"]);
  });

  it("control: the classic Camera & Alignment tab does switch the camera mode, so the harness sees actuation", async () => {
    await boot(variant);
    log.splice(0);
    await click(host.querySelector('button[aria-label^="Camera & Alignment:"]'), "classic Camera & Alignment tab");
    await advance(3000);
    expect(log.map((e) => e.cmd)).toContain(variant === "pz7035" ? "set_instrument_mode" : "set_camera_overview");
  });
});

/** Type into a field of the workspace (React's controlled input). */
async function typeInto(selector: string, value: string) {
  const el = host.querySelector<HTMLInputElement>(selector);
  if (!el) throw new Error(`no field ${selector}`);
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(el, value);
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });
}
const openPlan = async () => {
  await click(buttonByText("Workspace preview"), "workspace toggle");
  await click(host.querySelectorAll(".mib-stages__tab")[2], "Experiment");
  expect(host.querySelector("[data-testid=workspace-plan]")).not.toBeNull();
};

/** Unmount the app and start it again over the same storage (a reload, or the next start). */
async function restart(variant: Variant) {
  await act(async () => root.unmount());
  root = createRoot(host);
  log = [];
  await boot(variant);
}
/** What acts, among the commands sent: the start-up core `info` request is a read the classic
 *  start-up sends either way (coreManagement's restore), never an action. */
const acting = (entries: { cmd: string; args: unknown }[]) => entries.filter((e) => ACTUATING.includes(e.cmd)
  && !(e.cmd === "processing_core_command" && JSON.parse((e.args as { request: string }).request).operation === "info"));
const storedLanding = () => localStorage.getItem(LANDING_STORAGE_KEY);
const workspaceShown = () => host.querySelector("[data-testid=workspace]") !== null;

describe.each<Variant>(["desktop", "pz7035"])("the workspace branch opens in the workspace (%s)", (variant) => {
  beforeEach(() => localStorage.removeItem(LANDING_STORAGE_KEY)); // no choice made in this browser yet

  it("with no choice remembered, the app opens in Prepare; nothing acts at start-up, and the classic view is one click away", async () => {
    await boot(variant);
    expect(host.querySelector("[data-testid=workspace-prepare]")).not.toBeNull();
    // The classic view is hidden, not unmounted, and stays on Connect.
    expect((host.querySelector("nav.menubar") as HTMLElement).style.display).toBe("none");
    const band = host.querySelector(".ws-shell")!.textContent!;
    expect(band).toContain("Navigation never acts: only an explicit button switches or writes.");
    expect(band).toContain("The classic view is one click away");
    expect(band).not.toContain("stays the default");
    await advance(6000);
    // Opening in the workspace switches no camera mode and lights nothing: the classic startup landing is skipped.
    expect(acting(log)).toEqual([]);
    for (const r of readinessReads(log)) expect(r.args).toEqual({ outputPath: "" });
    expect(storedLanding()).toBeNull(); // opening by default records no choice

    await click(buttonByText("Classic view"), "Classic view");
    expect(workspaceShown()).toBe(false);
    expect((host.querySelector("nav.menubar") as HTMLElement).style.display).toBe("");
    // The classic tab is the first one (Connect, which only reads): the startup landing did not move it.
    expect(host.querySelector('.tabbar button[aria-selected="true"]')).toBe(host.querySelector(".tabbar button"));
    await advance(6000);
    expect(acting(log)).toEqual([]);
  });

  it("remembers the last explicit choice per browser: Classic view opens the classic view next time, Workspace preview the workspace", async () => {
    await boot(variant);
    expect(workspaceShown()).toBe(true);
    await click(buttonByText("Classic view"), "Classic view");
    expect(storedLanding()).toBe("classic");

    await restart(variant);
    expect(workspaceShown()).toBe(false);
    await click(buttonByText("Workspace preview"), "workspace toggle");
    expect(workspaceShown()).toBe(true);
    expect(storedLanding()).toBe("workspace");

    await restart(variant);
    expect(host.querySelector("[data-testid=workspace-prepare]")).not.toBeNull();
    expect(acting(log)).toEqual([]);
  });

  it.each(["Classic view", "Camera setup"])("leaving through %s before the first camera snapshot: the classic startup landing never runs, so nothing acts", async (exit) => {
    // A failed last run and a camera on the full sensor: the classic landing would select Experiment
    // and switch the desktop camera to the experiment window (the control below shows it does).
    failedRun = true;
    device.overview = true;
    holdSelection = true;
    await act(async () => { vi.mocked(invoke).mockImplementation(async (cmd: string, args?: unknown) => { log.push({ cmd, args }); return respond(variant, cmd, args); });
      root.render(<App />); });
    await advance(3000);
    expect(heldSelection.length).toBeGreaterThan(0);
    await click(buttonByText(exit), exit);
    expect(workspaceShown()).toBe(false);
    holdSelection = false;
    await act(async () => { for (const r of heldSelection.splice(0)) r(); });
    await advance(6000);
    expect(acting(log)).toEqual([]);
    expect(host.querySelector('.tabbar button[aria-selected="true"]')).toBe(host.querySelector(".tabbar button")); // still Connect
  });

  it("control: with a classic choice remembered, the same start-up lands on Experiment and the desktop camera is switched", async () => {
    localStorage.setItem(LANDING_STORAGE_KEY, "classic");
    failedRun = true;
    device.overview = true;
    await boot(variant);
    expect(host.querySelector('.tabbar button[aria-selected="true"]')!.getAttribute("aria-label")).toMatch(/^Experiment/);
    if (variant === "desktop") expect(acting(log).map((e) => e.cmd)).toContain("set_camera_overview");
  });

  it("a task jump to the classic view (Prepare's Camera setup) is not a choice: the next start opens the workspace again", async () => {
    await boot(variant);
    await click(buttonByText("Camera setup"), "Camera setup");
    expect(workspaceShown()).toBe(false);
    expect(storedLanding()).toBeNull();
    await restart(variant);
    expect(workspaceShown()).toBe(true);
  });
});

describe.each<Variant>(["desktop", "pz7035"])("Experiment · Plan never acts on its own (%s)", (variant) => {
  it("visiting the Plan, filling the sheet and switching sub-views sends nothing that acts; Start run is locked with the reason", async () => {
    await boot(variant);
    const idle = await steadyState();
    await openPlan();
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    await typeInto("[data-testid=plan-operator-name]", "A. Operator");
    for (const sub of ["Live", "Summary", "Plan"]) await click(buttonByText(sub), sub);
    await advance(6000);
    const visit = log.splice(0);
    expect(visit.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    // Only Prepare's path-less readiness read is added (the Plan takes Prepare's verdict), and Live's
    // reads and monitoring switch while it was shown; monitoring is off again after it.
    for (const n of [...namesIn(visit)].filter((n) => !namesIn(idle).has(n))) expect([...PREPARE_READS, ...LIVE_COMMANDS]).toContain(n);
    expect(lastMonitoring(visit)).toEqual({ active: false });
    for (const r of readinessReads(visit)) expect(r.args).toEqual({ outputPath: "" });
    // In the mock nothing is ready to run (no core status, no stream or Run mode): Start run says why and sends nothing.
    const start = buttonByText("Start run")!;
    expect(start.disabled).toBe(true);
    expect(host.querySelector("[data-testid=plan-blocked-prepare]")!.textContent).toContain("Start is unavailable · Prepare: Not ready");
    await click(start, "Start run");
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
  });
});

describe("Experiment · Plan Start run (desktop)", () => {
  it("sends experiment_start once, with the plan's startRequestJson; the classic Start still sends none", async () => {
    canStart = true;
    await boot("desktop");
    await openPlan();
    const id = host.querySelector("[data-testid=plan-experiment-id]")!.textContent!;
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    await typeInto("[data-testid=plan-temperatureC]", "22.5");
    await typeInto("[data-testid=plan-operator-name]", "A. Operator");
    await advance(3000);
    log.splice(0);
    vi.mocked(save).mockResolvedValueOnce("/data/run.h5");
    await click(buttonByText("Start run"), "Start run");
    const acted = log.splice(0).filter((e) => ACTUATING.includes(e.cmd));
    expect(acted).toEqual([{ cmd: "experiment_start", args: {
      outputPath: "/data/run.h5", acknowledgeDiscardRing: false,
      startRequestJson: `{"experiment_id":"${id}","run_index":1,"sample_ref":"S1","operator":{"name":"A. Operator"},"annotation":{"condition":"Vehicle","temperature_c":22.5}}`,
    } }]);
    expect(host.querySelector("[data-testid=plan-outcome]")!.textContent).toContain("Run 1 started · /data/run.h5");

    // The classic Start button is unchanged: no start request.
    await click(buttonByText("Classic view"), "Classic view");
    await click(host.querySelector('button[aria-label^="Experiment:"]'), "classic Experiment tab");
    await advance(3000);
    log.splice(0);
    vi.mocked(save).mockResolvedValueOnce("/data/classic.h5");
    await click(buttonByText("Start Experiment"), "classic Start Experiment");
    const classic = log.splice(0).filter((e) => e.cmd === "experiment_start");
    expect(classic).toEqual([{ cmd: "experiment_start", args: { outputPath: "/data/classic.h5", acknowledgeDiscardRing: false } }]);
  });

  it.each([
    ["by its start outcome code (ABI 40)", false],
    ["by the message prefix, from a reply without a code", true],
  ])("the backend's InvalidRequest, %s, is shown next to the sheet, no run is claimed, and the plan is kept", async (_how, withoutCode) => {
    canStart = true;
    noOutcomeCodes = withoutCode;
    startReply = { ok: false, message: "invalid start request: annotation.condition: 300 bytes, at most 256", start_outcome: EXPERIMENT_START_OUTCOMES.InvalidRequest };
    await boot("desktop");
    await openPlan();
    const id = host.querySelector("[data-testid=plan-experiment-id]")!.textContent!;
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    vi.mocked(save).mockResolvedValueOnce("/data/run.h5");
    await click(buttonByText("Start run"), "Start run");
    expect(log.filter((e) => e.cmd === "experiment_start")).toHaveLength(1);
    const refusal = host.querySelector("[data-testid=plan-sheet] [data-testid=plan-refusal]")!;
    expect(refusal.textContent).toContain("Condition: 300 bytes, at most 256. No file was created.");
    expect(host.querySelector("[data-testid=workspace-plan]")!.textContent).not.toMatch(/Run 1 started|run has started/);
    expect(host.querySelector("[data-testid=plan-outcome]")).toBeNull();
    expect(host.querySelector("[data-testid=plan-experiment-id]")!.textContent).toBe(id);
    expect(host.querySelector<HTMLInputElement>("[data-testid=plan-condition]")!.value).toBe("Vehicle");
    expect(buttonByText("Start run")!.disabled).toBe(false);
  });

  it("a StorageFailed reply (the backend may have opened the file) locks the plan; it is not sent again", async () => {
    canStart = true;
    startReply = { ok: false, message: "failed to initialize datasets", start_outcome: EXPERIMENT_START_OUTCOMES.StorageFailed };
    await boot("desktop");
    await openPlan();
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    vi.mocked(save).mockResolvedValueOnce("/data/run.h5");
    await click(buttonByText("Start run"), "Start run");
    await advance(3000);
    expect(log.filter((e) => e.cmd === "experiment_start")).toHaveLength(1);
    expect(byTestId("plan-outcome")!.textContent).toContain("Not started · its file may exist · /data/run.h5");
    expect(byTestId("plan-outcome")!.textContent).toContain("failed to initialize datasets");
    expect(host.querySelector("[data-testid=plan-sheet] [data-testid=plan-refusal]")).toBeNull();
    expect(buttonByText("Start run")!.disabled).toBe(true);
    expect(buttonByText("New plan")).toBeDefined();
  });
});

// #736 review: the start outcome is the start reply's alone; the checklist's reads do not stack;
// the Plan's placeholders read nothing.
describe("Experiment · Plan after review (desktop)", () => {
  it("a successful start followed by a failed status read leaves the plan locked", async () => {
    canStart = true;
    statusFailsAfterStart = true;
    await boot("desktop");
    await openPlan();
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    vi.mocked(save).mockResolvedValueOnce("/data/run.h5");
    await click(buttonByText("Start run"), "Start run");
    expect(log.filter((e) => e.cmd === "experiment_start")).toHaveLength(1);
    expect(host.querySelector("[data-testid=plan-outcome]")!.textContent).toContain("Run 1 started · /data/run.h5");
    expect(host.querySelector("[data-testid=plan-refresh]")!.textContent).toContain("TRANSPORT_LOST");
    const start = buttonByText("Start run")!;
    expect(start.disabled).toBe(true);
    // The run stops (the status reads recover): the same identity is still not offered again.
    statusFailsAfterStart = false;
    await advance(6000);
    expect(buttonByText("Start run")!.disabled).toBe(true);
    vi.mocked(save).mockResolvedValueOnce("/data/again.h5");
    await click(buttonByText("Start run"), "Start run");
    expect(log.filter((e) => e.cmd === "experiment_start")).toHaveLength(1);
  });

  it("the checklist reads one at a time: a stalled read is not stacked, and an older reply never overwrites a newer one", async () => {
    canStart = true;
    await boot("desktop");
    await openPlan();
    await advance(3000);
    expect(host.querySelector("[data-testid=plan-blocked-prepare]")).toBeNull(); // the core is valid: Prepare does not block
    log.splice(0);
    holdCore = true;
    await advance(1600); // the next tick: its read stalls on the core status
    const coreReads = () => log.filter((e) => e.cmd === "fetch_processing_core_status").length;
    const triggerReads = () => log.filter((e) => e.cmd === "fetch_trigger_status").length;
    expect(coreReads()).toBe(1);
    for (let i = 0; i < 5; i++) await advance(1500); // several periods while it is held
    expect(coreReads()).toBe(1);
    expect(triggerReads()).toBe(1);
    await advance(2000); // past the stale bound (10 s): the next tick starts one more read, not a pile
    expect(coreReads()).toBe(2);
    expect(heldCore).toHaveLength(2);
    holdCore = false;
    // The newer read answers first (core valid), then the stalled older one (core missing).
    await act(async () => heldCore[1](CORE_OK));
    await act(async () => heldCore[0]({ ...CORE_OK, valid: false }));
    expect(host.querySelector("[data-testid=plan-blocked-prepare]")).toBeNull(); // the older reply was dropped
    // Later reads apply again.
    await advance(3000);
    expect(host.querySelector("[data-testid=plan-blocked-prepare]")).toBeNull();
  });

  it("Live and Summary read neither the gates nor the checklist; Plan resumes both", async () => {
    await boot("desktop");
    // The classic Connect tab polls the checklist itself: leave it for Camera & Alignment first.
    await click(host.querySelector('button[aria-label^="Camera & Alignment:"]'), "classic Camera & Alignment tab");
    await advance(3000);
    const prepareReads = (entries: { cmd: string }[]) => entries.filter((e) => e.cmd === "fetch_experiment_readiness" || e.cmd === "fetch_trigger_status");
    await openPlan();
    await advance(3000);
    expect(prepareReads(log.splice(0)).length).toBeGreaterThan(0);
    for (const sub of ["Live", "Summary"]) {
      await click(buttonByText(sub), sub);
      log.splice(0);
      await advance(6000);
      expect(prepareReads(log.splice(0))).toEqual([]);
    }
    await click(buttonByText("Plan"), "Plan");
    await advance(3000);
    expect(prepareReads(log.splice(0)).length).toBeGreaterThan(0);
  });
});

// #716 A5 (second PR): the guided sample queue and the PZ7035's explicit switch to Run.
const chooseQueue = async () => {
  await click(host.querySelectorAll<HTMLInputElement>("[data-testid=workspace-plan] .mib-option input")[1], "Sample queue");
  expect(host.querySelector("[data-testid=queue-card]")).not.toBeNull();
};
const byTestId = (id: string) => host.querySelector<HTMLElement>(`[data-testid=${id}]`);

describe.each<Variant>(["desktop", "pz7035"])("the sample queue never acts on its own (%s)", (variant) => {
  it("choosing the queue, adding, typing, moving and removing samples, and switching sub-views send nothing that acts", async () => {
    await boot(variant);
    const idle = await steadyState();
    await openPlan();
    await chooseQueue();
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    await click(byTestId("queue-add"), "+ Sample");
    await typeInto("[data-testid=plan-condition]", "Cytochalasin D");
    await click(byTestId("queue-add"), "+ Sample");
    await click(host.querySelector('[aria-label="Move S3 up"]'), "Move S3 up");
    await click(host.querySelector('[aria-label="Remove S2"]'), "Remove S2");
    for (const sub of ["Live", "Summary", "Plan"]) await click(buttonByText(sub), sub);
    await advance(10_000);
    const visit = log.splice(0);
    expect(visit.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    for (const n of [...namesIn(visit)].filter((n) => !namesIn(idle).has(n))) expect([...PREPARE_READS, ...LIVE_COMMANDS]).toContain(n);
    expect(lastMonitoring(visit)).toEqual({ active: false });
    for (const r of readinessReads(visit)) expect(r.args).toEqual({ outputPath: "" });
    // The queue is still there after leaving the stage and the workspace.
    await click(host.querySelectorAll(".mib-stages__tab")[0], "Prepare");
    await click(buttonByText("Classic view"), "Classic view");
    await click(buttonByText("Workspace preview"), "workspace toggle");
    await click(host.querySelectorAll(".mib-stages__tab")[2], "Experiment");
    expect([...host.querySelectorAll("[data-testid=plan-sheet-table] tbody tr")].map((tr) => tr.children[1].textContent)).toEqual(["S1", "S3"]);
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
  });
});

describe("the sample queue over the mocked transport (desktop)", () => {
  it("two samples send exactly start S1, stop, start S2, stop, each on its button, and nothing in between", async () => {
    canStart = true;
    statefulRuns = true;
    await boot("desktop");
    await openPlan();
    await chooseQueue();
    const id = host.querySelector("[data-testid=plan-experiment-id]")!.textContent!;
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    await click(byTestId("queue-add"), "+ Sample");
    await typeInto("[data-testid=plan-condition]", "Cytochalasin D");
    await advance(3000);
    log.splice(0);
    const acts = () => log.filter((e) => ACTUATING.includes(e.cmd));

    vi.mocked(save).mockResolvedValueOnce("/data/s1.h5");
    await click(byTestId("queue-start"), "Start S1");
    await advance(10_000);
    expect(acts()).toEqual([{ cmd: "experiment_start", args: { outputPath: "/data/s1.h5", acknowledgeDiscardRing: false,
      startRequestJson: `{"experiment_id":"${id}","run_index":1,"sample_ref":"S1","annotation":{"condition":"Vehicle"}}` } }]);
    // Readiness was evaluated with the chosen file before the start.
    expect(log.some((e) => e.cmd === "fetch_experiment_readiness" && (e.args as { outputPath: string }).outputPath === "/data/s1.h5")).toBe(true);
    expect(byTestId("queue-step")!.textContent).toContain("S1 running · run 1 · /data/s1.h5");

    await click(buttonByText("Next sample"), "Next sample");
    await advance(3000);
    expect(acts().map((e) => e.cmd)).toEqual(["experiment_start", "experiment_stop"]);
    expect(byTestId("queue-step")!.textContent).toContain("Stopping S1");
    finalizeRun();
    await advance(10_000);
    expect(byTestId("queue-step")!.textContent).toContain("S1 finalized · the backend closed /data/s1.h5");
    expect(acts()).toHaveLength(2); // the close waits for the operator

    await click(buttonByText("Confirm closed"), "Confirm closed");
    await advance(10_000);
    expect(byTestId("queue-step")!.textContent).toContain("Swap the tube for S2");
    expect(acts()).toHaveLength(2); // S2 waits for its Start

    vi.mocked(save).mockResolvedValueOnce("/data/s2.h5");
    await click(buttonByText("Start S2"), "Start S2");
    await advance(3000);
    await click(buttonByText("Stop last sample"), "Stop last sample");
    finalizeRun();
    await advance(3000);
    await click(buttonByText("Confirm closed"), "Confirm closed");
    await advance(10_000);
    expect(byTestId("queue-finished")!.textContent).toContain("Queue complete · 2 of 2 closed");
    expect(acts()).toEqual([
      { cmd: "experiment_start", args: { outputPath: "/data/s1.h5", acknowledgeDiscardRing: false,
        startRequestJson: `{"experiment_id":"${id}","run_index":1,"sample_ref":"S1","annotation":{"condition":"Vehicle"}}` } },
      { cmd: "experiment_stop", args: { expectedStartGeneration: "1" } }, // only S1's run (ABI 40)
      { cmd: "experiment_start", args: { outputPath: "/data/s2.h5", acknowledgeDiscardRing: false,
        startRequestJson: `{"experiment_id":"${id}","run_index":2,"sample_ref":"S2","annotation":{"condition":"Cytochalasin D"}}` } },
      { cmd: "experiment_stop", args: { expectedStartGeneration: "2" } },
    ]);
  });

  async function runningS1() {
    canStart = true;
    statefulRuns = true;
    await boot("desktop");
    await openPlan();
    await chooseQueue();
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    await click(byTestId("queue-add"), "+ Sample");
    await advance(3000);
    log.splice(0);
    vi.mocked(save).mockResolvedValueOnce("/data/s1.h5");
    await click(byTestId("queue-start"), "Start S1");
    await advance(3000);
    expect(byTestId("queue-step")!.textContent).toContain("S1 running · run 1 · /data/s1.h5");
  }
  const acts = () => log.filter((e) => ACTUATING.includes(e.cmd));
  const stops = () => log.filter((e) => e.cmd === "experiment_stop");

  it("Summary (O5/O6, A7b): reads the quick look on S1's close; Repeat sample re-queues S1 as run 2 (same sample_ref, a new file); Open in Review loads the file in the classic Review", async () => {
    canStart = true;
    statefulRuns = true;
    await boot("desktop");
    await openPlan();
    await chooseQueue();
    const id = host.querySelector("[data-testid=plan-experiment-id]")!.textContent!;
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    await click(byTestId("queue-add"), "+ Sample");
    await advance(3000);
    vi.mocked(save).mockResolvedValueOnce("/data/s1.h5");
    await click(byTestId("queue-start"), "Start S1");
    await advance(3000);
    await click(buttonByText("Summary"), "Summary");
    expect(byTestId("summary-empty")).not.toBeNull();
    log.splice(0);
    await advance(5000);
    expect(log.some((e) => e.cmd === "run_quick_look")).toBe(false); // nothing closed yet, nothing read
    finalizeRun();
    await advance(3000);
    const quick = log.filter((e) => e.cmd === "run_quick_look");
    expect(quick).toEqual([{ cmd: "run_quick_look", args: { paths: ["/data/s1.h5"] } }]);
    expect(byTestId("summary-run")!.textContent).toContain("Run summary · Run 1 · S1");
    await advance(5000);
    expect(log.filter((e) => e.cmd === "run_quick_look")).toHaveLength(1); // the status polls read nothing more

    log.splice(0);
    await click(byTestId("decision-repeat"), "Repeat sample");
    expect(log.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]); // sends nothing
    await click(buttonByText("Plan"), "Plan");
    expect(byTestId("queue-step")!.textContent).toContain("Repeat S1 · Vehicle · load the tube again");
    vi.mocked(save).mockResolvedValueOnce("/data/s1-repeat.h5");
    await click(byTestId("queue-start"), "Start S1");
    await advance(3000);
    expect(log.filter((e) => ACTUATING.includes(e.cmd))).toEqual([{ cmd: "experiment_start", args: { outputPath: "/data/s1-repeat.h5", acknowledgeDiscardRing: false,
      startRequestJson: `{"experiment_id":"${id}","run_index":2,"sample_ref":"S1","annotation":{"condition":"Vehicle"}}` } }]);
    await click(buttonByText("Next sample"), "Next sample");
    finalizeRun();
    await advance(3000);

    await click(buttonByText("Summary"), "Summary");
    await advance(1000);
    expect(byTestId("summary-runs")!.textContent).toContain("Run 1 · S1");
    expect(byTestId("summary-runs")!.textContent).toContain("Run 2 · S1");
    log.splice(0);
    await click(byTestId("decision-review"), "Open in Review");
    await advance(1000);
    expect(log.filter((e) => e.cmd === "load_recording")).toEqual([{ cmd: "load_recording", args: { filePath: "/data/s1-repeat.h5" } }]);
    expect(host.querySelector("[data-testid=workspace]")).toBeNull(); // the classic Review tab
  });

  it("A7b ruling: a recording opened in Review (Summary's Open in Review) blocks the Plan's Start, which sends nothing; closing it in Review lifts the block", async () => {
    canStart = true;
    statefulRuns = true;
    reviewLoads = true;
    await boot("desktop");
    await openPlan();
    await chooseQueue();
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    await click(byTestId("queue-add"), "+ Sample");
    await advance(3000);
    vi.mocked(save).mockResolvedValueOnce("/data/s1.h5");
    await click(byTestId("queue-start"), "Start S1");
    await advance(3000);
    finalizeRun();
    await advance(3000);
    await click(buttonByText("Confirm closed"), "Confirm closed");
    await click(buttonByText("Summary"), "Summary");
    await advance(1000);
    await click(byTestId("decision-review"), "Open in Review");
    await advance(2000);
    expect(reviewFile).toBe("/data/s1.h5");
    expect(host.querySelector("[data-testid=workspace]")).toBeNull(); // the classic Review tab

    // Back in the workspace's Plan: Start S2 is unavailable with the block, and pressing it sends nothing.
    await click(buttonByText("Workspace preview"), "workspace toggle");
    await click(host.querySelectorAll(".mib-stages__tab")[2], "Experiment");
    await click(buttonByText("Plan"), "Plan");
    expect(byTestId("plan-blocked-review")!.textContent).toContain("A recording is open in Review · close it there (Review → Close File) to start");
    const start = byTestId("queue-start") as HTMLButtonElement;
    expect(start.disabled).toBe(true);
    log.splice(0);
    await act(async () => start.click());
    await advance(3000);
    expect(log.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);

    // Closed in the classic Review: the block goes.
    await click(buttonByText("Classic view"), "Classic view");
    await click(buttonByText("Close File"), "Close File");
    expect(reviewFile).toBe("");
    await click(buttonByText("Workspace preview"), "workspace toggle");
    await click(host.querySelectorAll(".mib-stages__tab")[2], "Experiment");
    await advance(3000);
    expect(byTestId("plan-blocked-review")).toBeNull();
    expect((byTestId("queue-start") as HTMLButtonElement).disabled).toBe(false);
  });

  it("Next sample names S1's run: when another run is active by then, the backend stops nothing and S1 is interrupted; nothing more is sent", async () => {
    await runningS1();
    // Between the last status read and the press, another client stops S1 and starts its own run.
    run = { generation: 2, state: EXPERIMENT_STATES.Active, flushing: false, terminal: false, ok: false, path: "/data/theirs.h5" };
    await click(buttonByText("Next sample"), "Next sample");
    await advance(3000);
    expect(stops()).toEqual([{ cmd: "experiment_stop", args: { expectedStartGeneration: "1" } }]);
    expect(run).toMatchObject({ generation: 2, state: EXPERIMENT_STATES.Active }); // their run was not stopped
    expect(byTestId("queue-state-S1")!.textContent).toBe("Interrupted · not stopped");
    const step = byTestId("queue-step")!.textContent!;
    expect(step).toContain("S1 · not stopped · run 1 is not the backend's active run");
    expect(step).toContain("stop refused: expected run 1, but run 2 is active; nothing was stopped");
    expect(buttonByText("Next sample")).toBeUndefined();
    // Nothing more: no stop without the generation, no stop of their run, no start.
    await advance(10_000);
    finalizeRun(); // their run ends: not S1's to confirm
    await advance(10_000);
    expect(acts().map((e) => e.cmd)).toEqual(["experiment_start", "experiment_stop"]);
    expect(buttonByText("Confirm closed")).toBeUndefined();
    expect(byTestId("queue-state-S1")!.textContent).toBe("Interrupted · not stopped");
  });

  it("S1's run that ended by itself before Next sample: the backend refuses the stop (no run is active) and the queue follows S1 to its close", async () => {
    await runningS1();
    Object.assign(run, { state: EXPERIMENT_STATES.Idle, flushing: false, terminal: false, ok: false }); // ended, not yet reported terminal
    await click(buttonByText("Next sample"), "Next sample");
    expect(stops()).toEqual([{ cmd: "experiment_stop", args: { expectedStartGeneration: "1" } }]);
    expect(byTestId("queue-step")!.textContent).toContain("stop refused: expected run 1, but no run is active");
    finalizeRun();
    await advance(3000);
    expect(byTestId("queue-step")!.textContent).toContain("S1 finalized · the backend closed /data/s1.h5");
    expect(stops()).toHaveLength(1);
  });

  it("Live (O4, A6b): a control loss pauses the queue and Live sends nothing; nothing resumes on its own; once resumed, Stop & finalize names S1's run", async () => {
    await runningS1();
    await click(buttonByText("Live"), "Live");
    await advance(1000);
    expect(byTestId("workspace-live")).not.toBeNull();
    expect(byTestId("live-sample-S1")!.textContent).toContain("Running");
    expect(byTestId("live-lifecycle")!.textContent).toContain("S1 · Vehicle · run 1");
    expect((byTestId("live-stop-run") as HTMLButtonElement).disabled).toBe(false);
    log.splice(0);
    await setControl(false);
    await advance(3000);
    expect(byTestId("live-paused")!.textContent).toContain("Paused because this client lost control");
    expect(byTestId("live-viewer")).not.toBeNull();
    expect((byTestId("live-stop-run") as HTMLButtonElement).disabled).toBe(true);
    await click(byTestId("live-stop-run"), "Stop & finalize run");
    await click(byTestId("live-stop-acquisition"), "Stop acquisition");
    expect(acts()).toEqual([]);
    // Control comes back: the queue stays paused, and Live's stop stays unavailable until Resume.
    await setControl(true);
    await advance(3000);
    expect(byTestId("live-paused")).not.toBeNull();
    expect((byTestId("live-stop-run") as HTMLButtonElement).disabled).toBe(true);
    await click(byTestId("live-resume"), "Resume queue");
    expect(byTestId("live-paused")).toBeNull();
    expect(acts()).toEqual([]); // resuming sends nothing
    await click(byTestId("live-stop-run"), "Stop & finalize run");
    await advance(1000);
    expect(acts()).toEqual([{ cmd: "experiment_stop", args: { expectedStartGeneration: "1" } }]); // the queue's own row, as Next sample
    finalizeRun();
    await advance(3000);
    await click(byTestId("live-confirm-closed"), "Confirm closed");
    expect(acts()).toHaveLength(1); // confirming sends nothing
    await click(buttonByText("Plan"), "Plan");
    expect(byTestId("queue-step")!.textContent).toContain("Swap the tube for S2");
    expect(acts()).toHaveLength(1); // S2 waits for its Start in the Plan
  });

  const switches = () => log.filter((e) => e.cmd === "monitoring_set_active").map((e) => e.args);

  it("Live (A6b ruling): leaving Live during a run sends no monitoring_set_active(false), whether to Plan, Prepare or the classic view; once the run ends off Live it is turned off", async () => {
    await runningS1();
    log.splice(0);
    await click(buttonByText("Live"), "Live");
    await advance(1000);
    expect(switches()).toEqual([{ active: true }]);
    // The controller moves to Plan, then Prepare, then the classic view: the run is still active.
    await click(buttonByText("Plan"), "Plan");
    await advance(3000);
    await click(host.querySelectorAll(".mib-stages__tab")[0], "Prepare");
    await advance(3000);
    await click(buttonByText("Classic view"), "Classic view");
    await advance(3000);
    expect(switches()).toEqual([{ active: true }]); // never turned off under the run
    // Stopping is still the run: nothing until it is terminal.
    Object.assign(run, { state: EXPERIMENT_STATES.Stopping, flushing: true });
    await advance(3000);
    expect(switches()).toEqual([{ active: true }]);
    finalizeRun();
    await advance(3000);
    expect(switches()).toEqual([{ active: true }, { active: false }]); // the run ended off Live: it follows the view again
    await advance(6000);
    expect(switches()).toHaveLength(2); // once
    // Back on Live it is on again; leaving with no run turns it off at once.
    await click(buttonByText("Workspace preview"), "workspace toggle");
    await click(host.querySelectorAll(".mib-stages__tab")[2], "Experiment");
    await click(buttonByText("Live"), "Live");
    await click(buttonByText("Plan"), "Plan");
    expect(switches()).toEqual([{ active: true }, { active: false }, { active: true }, { active: false }]);
  });

  it("the classic Monitoring tab follows the same rule: leaving it during a run sends no off; the run's end does", async () => {
    await runningS1();
    await click(buttonByText("Classic view"), "Classic view");
    await click(host.querySelector('button[aria-label^="Experiment:"]'), "classic Experiment tab");
    log.splice(0);
    await click(buttonByText("Monitoring"), "classic Monitoring tab");
    await click(buttonByText("Preview"), "classic Preview tab");
    await advance(3000);
    expect(switches()).toEqual([{ active: true }]);
    finalizeRun();
    await advance(3000);
    expect(switches()).toEqual([{ active: true }, { active: false }]);
  });

  it("Live (A6b ruling): a viewer client never sends the monitoring switch: not at start-up, on Live, leaving it, nor when a run ends; holding control again, it follows the view", async () => {
    control.held = false;
    statefulRuns = true;
    await boot("desktop");
    // Another client's run.
    run = { generation: 1, state: EXPERIMENT_STATES.Active, flushing: false, terminal: false, ok: false, path: "/data/theirs.h5" };
    await openPlan();
    await click(buttonByText("Live"), "Live");
    await advance(3000);
    expect(byTestId("workspace-live")).not.toBeNull();
    expect(log.some((e) => e.cmd === "fetch_monitoring_snapshot")).toBe(true); // the viewer reads the feed
    await click(buttonByText("Plan"), "Plan");
    await advance(3000);
    finalizeRun();
    await advance(3000);
    await click(buttonByText("Live"), "Live");
    await click(buttonByText("Plan"), "Plan");
    await advance(3000);
    expect(switches()).toEqual([]);
    // Holding control again (off Live, no run): it follows the view.
    await setControl(true);
    await advance(1000);
    expect(switches()).toEqual([{ active: false }]);
  });

  it("the classic Stop Experiment is not tied to a row: it sends no generation; the queue follows S1 to its close", async () => {
    await runningS1();
    await click(buttonByText("Classic view"), "Classic view");
    await click(host.querySelector('button[aria-label^="Experiment:"]'), "classic Experiment tab");
    await advance(1000);
    await click(buttonByText("Stop Experiment"), "classic Stop Experiment");
    expect(stops()).toEqual([{ cmd: "experiment_stop", args: undefined }]);
    finalizeRun();
    await advance(3000);
    await click(buttonByText("Workspace preview"), "workspace toggle");
    await click(host.querySelectorAll(".mib-stages__tab")[2], "Experiment");
    expect(byTestId("queue-step")!.textContent).toContain("S1 finalized · the backend closed /data/s1.h5");
    expect(stops()).toHaveLength(1);
  });

  it("a stop reply without stop_outcome (a backend before ABI 40) is handled as before: accepted, then S1 is followed to its close", async () => {
    noOutcomeCodes = true;
    startReply = { ok: true, message: "experiment started" };
    await runningS1();
    await click(buttonByText("Next sample"), "Next sample");
    await advance(3000);
    expect(stops()).toEqual([{ cmd: "experiment_stop", args: { expectedStartGeneration: "1" } }]);
    expect(byTestId("queue-step")!.textContent).toContain("Stopping S1");
    finalizeRun();
    await advance(3000);
    expect(byTestId("queue-step")!.textContent).toContain("S1 finalized · the backend closed /data/s1.h5");
    expect(stops()).toHaveLength(1);
  });
});

describe("the Plan's switch to Run (pz7035)", () => {
  it("visiting the Plan sends no switch; with the Run window placed in Align, the button sends exactly the classic Run switch", async () => {
    await boot("pz7035");
    log.splice(0);
    await openPlan();
    await advance(3000);
    const notice = byTestId("plan-camera-mode")!;
    expect(notice.textContent).toContain("Camera · Mode · unknown");
    expect(buttonByText("Switch to Run")!.disabled).toBe(true); // no Run window placed yet
    expect(notice.textContent).toContain("place the 512×96 Run window in Align first");
    expect(byTestId("plan-blocked-locked")!.textContent).toContain("Place the 512×96 run window in Camera & Alignment first");
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    // Place the Run window in Align (no switch: it is applied by the switch to Run).
    await click(host.querySelectorAll(".mib-stages__tab")[1], "Align");
    await advance(3000);
    await typeInto('input[aria-label$=" X"]', "40");
    await click(buttonByText("Write Run window"), "Write Run window");
    await click(host.querySelectorAll(".mib-stages__tab")[2], "Experiment");
    await advance(3000);
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    expect(byTestId("plan-blocked-locked")!.textContent).toContain("The camera is not in Run mode yet");
    await click(buttonByText("Switch to Run"), "Switch to Run");
    await advance(3000);
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([{ cmd: "set_instrument_mode", args: { mode: "run", x: 40, y: expect.any(Number) } }]);
    expect(byTestId("plan-camera-mode")).toBeNull(); // in Run now
    expect(byTestId("plan-blocked-locked")?.textContent ?? "").not.toContain("Run mode");
    // And nothing more on its own.
    await advance(10_000);
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
  });
});

// #739 review: a control loss withdraws a pending Start; a run is the row's only on its own file;
// a desktop camera left on the full sensor blocks Start until the explicit switch.
describe("the sample queue after review (desktop)", () => {
  async function queueOfTwo() {
    canStart = true;
    statefulRuns = true;
    await boot("desktop");
    await openPlan();
    await chooseQueue();
    await typeInto("[data-testid=plan-condition]", "Vehicle");
    await click(byTestId("queue-add"), "+ Sample");
    await advance(3000);
    log.splice(0);
  }
  const starts = () => log.filter((e) => e.cmd === "experiment_start");

  it("losing and regaining control while readiness is pending sends no experiment_start; Start again after Resume sends one", async () => {
    await queueOfTwo();
    holdStartReadiness = true;
    vi.mocked(save).mockResolvedValueOnce("/data/s1.h5");
    await click(byTestId("queue-start"), "Start S1");
    expect(heldReadiness).toHaveLength(1); // the Start flow waits on readiness with the chosen file
    await setControl(false);
    await advance(1000);
    await setControl(true);
    await advance(1000);
    await act(async () => heldReadiness.shift()!());
    await advance(10_000);
    expect(starts()).toEqual([]);
    expect(log.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    expect(byTestId("queue-step")!.textContent).toContain("Not started · control was lost while the Start of S1 was pending");
    expect(byTestId("queue-paused")).not.toBeNull();
    await click(buttonByText("Resume queue"), "Resume queue");
    await advance(5000);
    expect(starts()).toEqual([]); // resuming sends nothing
    vi.mocked(save).mockResolvedValueOnce("/data/s1.h5");
    await click(byTestId("queue-start"), "Start S1");
    await advance(3000);
    expect(starts()).toHaveLength(1);
    expect(JSON.parse((starts()[0].args as { startRequestJson: string }).startRequestJson)).toMatchObject({ run_index: 1, sample_ref: "S1" });
  });

  it("S1's refresh fails and another client's run starts before the next good read: S1 is interrupted, never adopted; nothing is stopped", async () => {
    await queueOfTwo();
    statusFailsAfterStart = true;
    vi.mocked(save).mockResolvedValueOnce("/data/s1.h5");
    await click(byTestId("queue-start"), "Start S1");
    expect(starts()).toHaveLength(1);
    // Meanwhile another client stops S1 and starts its own run on its own file.
    run = { generation: run.generation + 1, state: EXPERIMENT_STATES.Active, flushing: false, terminal: false, ok: false, path: "/data/theirs.h5" };
    statusFailsAfterStart = false;
    await advance(3000);
    expect(byTestId("queue-state-S1")!.textContent).toBe("Interrupted · another run");
    expect(byTestId("queue-step")!.textContent).toContain("another run started on this instrument");
    expect(buttonByText("Next sample")).toBeUndefined();
    finalizeRun(); // their run ends: not S1's to confirm
    await advance(5000);
    expect(buttonByText("Confirm closed")).toBeUndefined();
    expect(log.filter((e) => e.cmd === "experiment_stop")).toEqual([]);
    expect(starts()).toHaveLength(1);
  });

  it("Align's full sensor blocks Start in the Plan with the reason; only Show the experiment window switches back", async () => {
    await queueOfTwo();
    await click(host.querySelectorAll(".mib-stages__tab")[1], "Align");
    await advance(3000);
    await click(buttonByText("Show the full sensor"), "Show the full sensor");
    await advance(3000);
    await click(host.querySelectorAll(".mib-stages__tab")[2], "Experiment");
    await advance(3000);
    log.splice(0);
    // The queue and the single plan both say why Start is unavailable.
    expect(byTestId("plan-camera-mode")!.textContent).toContain("Camera · Full sensor 2048 × 512");
    expect((byTestId("queue-start") as HTMLButtonElement).disabled).toBe(true);
    expect(byTestId("plan-blocked-camera")!.textContent).toContain("The camera shows the full sensor, not the experiment window");
    await click(byTestId("queue-start"), "Start S1");
    for (const sub of ["Live", "Summary", "Plan"]) await click(buttonByText(sub), sub);
    await advance(6000);
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
    await click(buttonByText("Show the experiment window"), "Show the experiment window");
    await advance(3000);
    expect(log.splice(0).filter((e) => ACTUATING.includes(e.cmd))).toEqual([{ cmd: "set_camera_overview", args: { overview: false } }]);
    expect(byTestId("plan-camera-mode")).toBeNull();
    expect(byTestId("plan-blocked-camera")).toBeNull();
    expect((byTestId("queue-start") as HTMLButtonElement).disabled).toBe(false);
  });

  it("the single plan is blocked the same way on the full sensor", async () => {
    canStart = true;
    await boot("desktop");
    await click(buttonByText("Workspace preview"), "workspace toggle");
    await click(host.querySelectorAll(".mib-stages__tab")[1], "Align");
    await advance(3000);
    await click(buttonByText("Show the full sensor"), "Show the full sensor");
    await click(host.querySelectorAll(".mib-stages__tab")[2], "Experiment");
    await advance(3000);
    log.splice(0);
    expect(buttonByText("Start run")!.disabled).toBe(true);
    expect(byTestId("plan-blocked-camera")!.textContent).toContain("The camera shows the full sensor, not the experiment window");
    await click(buttonByText("Start run"), "Start run");
    expect(log.filter((e) => ACTUATING.includes(e.cmd))).toEqual([]);
  });
});

// #716 A4b: Prepare's Calibration card. The whole App over the mocked transport, every command
// logged: listing, polling and navigation never send calibration_apply, even while a re-apply is
// offered; only the operator's press sends exactly one. A Start declares the record in effect.
describe("Prepare's Calibration card (A4b)", () => {
  const APPLIED = calRecord("cal-a", "applied", { factor: 0.52 });
  const PROPOSED = calRecord("cal-p", "proposed", { factor: 0.53 });
  const RESTART_GATE = { id: "calibration.record", status: G.Fail, reason: "the applied record's factor 0.52 is not the current pixel-to-micron factor 0.5",
    remediation: "re-apply record cal-a (calibration_apply), or run the calibration method" };

  it.each<Variant>(["desktop", "pz7035"])("navigation, listing and polling never send calibration_apply; the Fix's Re-apply sends exactly one (%s)", async (variant) => {
    calList = calListOf([PROPOSED, APPLIED]);
    calGate = RESTART_GATE;
    calApply = (a) => applyReply(String(a.recordId), CALIBRATION_APPLY_OUTCOMES.Applied, { previous_factor: 0.5, new_factor: 0.52 });
    await boot(variant);
    const navigation = await navigateWorkspace();
    expect(navigation.filter((e) => e.cmd === "calibration_apply")).toEqual([]);
    expect(navigation.filter((e) => e.cmd === "calibration_records_list").length).toBeGreaterThan(0);
    await click(buttonByText("Workspace preview"), "workspace toggle");
    await advance(10000); // polling
    for (const tab of host.querySelectorAll(".mib-stages__tab")) await click(tab, "stage tab");
    await click(host.querySelectorAll(".mib-stages__tab")[0], "Prepare");
    await advance(4000);
    expect(log.filter((e) => e.cmd === "calibration_apply")).toEqual([]);
    expect(host.querySelector("[data-testid=calibration-fix]")!.textContent).toContain("re-apply record cal-a");
    log.splice(0);
    await click(host.querySelector("[data-testid=calibration-facts] [data-testid=calibration-reapply-cal-a]"), "Re-apply cal-a");
    expect(log.filter((e) => ACTUATING.includes(e.cmd))).toEqual([{ cmd: "calibration_apply", args: { recordId: "cal-a", expectedCurrentFactor: 0.5 } }]);
    expect(host.querySelector("[data-testid=calibration-outcome]")!.textContent).toContain("Pixel-to-micron 0.5 → 0.52 µm/px");
    await advance(6000);
    expect(log.filter((e) => e.cmd === "calibration_apply")).toHaveLength(1);
  });

  it("Start declares the record in effect; the backend's mismatch says the applied calibration changed (desktop)", async () => {
    canStart = true;
    calList = calListOf([APPLIED], { in_effect_record_id: "cal-a", current_factor: 0.52 });
    startReply = { ok: false, start_outcome: EXPERIMENT_START_OUTCOMES.InvalidRequest,
      message: 'invalid start request: calibration_record_id: declared "cal-a" but no calibration record is applied and in effect (mismatch)' };
    await boot("desktop");
    await openPlan();
    const id = host.querySelector("[data-testid=plan-experiment-id]")!.textContent!;
    await advance(3000);
    log.splice(0);
    vi.mocked(save).mockResolvedValueOnce("/data/run.h5");
    await click(buttonByText("Start run"), "Start run");
    expect(log.filter((e) => e.cmd === "experiment_start")).toEqual([{ cmd: "experiment_start", args: {
      outputPath: "/data/run.h5", acknowledgeDiscardRing: false,
      startRequestJson: `{"experiment_id":"${id}","run_index":1,"sample_ref":"S1","calibration_record_id":"cal-a"}` } }]);
    expect(log.filter((e) => e.cmd === "calibration_apply")).toEqual([]);
    expect(byTestId("plan-calibration-refusal")!.textContent).toContain("Not started · the applied calibration changed since this page read it");
    expect(host.querySelector("[data-testid=plan-experiment-id]")!.textContent).toBe(id);
  });
});
