// @vitest-environment jsdom
// O2 Align (#716 A3), whole App over a mocked transport: a processing-config readback that
// answers late (after its write timed out and a newer write completed) never replaces the newer
// readback. The desktop camera here has no geometry, so the ROI quick control is the processing
// ROI (set_processing_roi, readback fetch_processing_config_json).
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { invoke } from "@tauri-apps/api/core";
import App from "../App";
import { BRIDGE_ABI_VERSION, COMMAND_TYPES, EXPERIMENT_STATES, JSON_TRANSPORT, RUN_COMPLETION_STATES } from "../bridgeContract";
import { WRITE_TIMEOUT_MS } from "./quickControlModel";

vi.mock("@tauri-apps/api/core", () => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/app", () => ({ getVersion: vi.fn(async () => "1.2.0") }));
vi.mock("@tauri-apps/api/window", () => ({ getCurrentWindow: () => ({ onCloseRequested: async () => () => {}, close: async () => {} }) }));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(async () => null), save: vi.fn(async () => null), confirm: vi.fn(async () => false), message: vi.fn(async () => {}) }));
vi.mock("@tauri-apps/plugin-opener", () => ({ openUrl: vi.fn(), revealItemInDir: vi.fn() }));

const status = { transport_version: JSON_TRANSPORT.version, valid: true, state: EXPERIMENT_STATES.Idle, start_time_ns: "0", end_time_ns: "0",
  valid_buffered: "0", invalid_buffered: "0", valid_saved: "0", invalid_saved: "0", dropped_valid: "0", dropped_invalid: "0", flushing: false, cancelled: false,
  output_path: "", message: "", start_generation: "0", readiness_generation: "1", capture_generation: "1", persistence_admitted: "0", persistence_committed: "0",
  persistence_failed: "0", terminal: false, finalization_ok: false, completion: RUN_COMPLETION_STATES.Complete, completion_reason: "", fault_revision: "0", fault_code: "", fault_message: "" };

/** The processing ROI the mock backend holds, and config reads that are held back on request. */
let roi = { x: 0, y: 0, w: 0, h: 0 };
let holdNextConfig = false;
const held: (() => void)[] = [];
const configDoc = () => ({ valid: true, json: JSON.stringify({ roi: { ...roi }, realtime_processing: { enabled: false }, pixel_to_micron: 1 }) });

function respond(cmd: string, args?: Record<string, unknown>): unknown {
  switch (cmd) {
    case "abi_version": return BRIDGE_ABI_VERSION;
    case "is_initialized": return true;
    case "fetch_preview_buffer": return { capture_running: false, recording: false, available: false };
    case "fetch_experiment_status": return status;
    case "fetch_review_metadata": return { valid: false, file_open: false, file_path: "", recording_file: false };
    case "fetch_platform_info": return { science: "host", host_processing: true, aravis: false };
    case "fetch_camera_selection": return { valid: true, configured: true, mode: 0, label: "Camera 01", interface_index: 0, device_index: 0, mindvision_index: 0,
      mindvision_config_path: "", camera_script_path: "", mock_frame_dir: "", mock_interval_ms: 33, mock_loop: true };
    case "fetch_camera_geometry": return { supported: false, overview: false, camera: "", sensor_width: 0, sensor_height: 0, roi: { x: 0, y: 0, width: 0, height: 0 },
      width_increment: 0, height_increment: 0, offset_x_increment: 0, offset_y_increment: 0, min_width: 0, min_height: 0, session: {} };
    case "fetch_instrument_status": return { available: false, error: "not a PZ7035" };
    case "poll_events_exact": return { transport_version: JSON_TRANSPORT.version, events: [] };
    case "set_processing_roi":
      roi = { x: Number(args!.x), y: Number(args!.y), w: Number(args!.w), h: Number(args!.h) };
      return { transport_version: JSON_TRANSPORT.version, ok: true, command: COMMAND_TYPES.ProcessingSettings, message: "ok", operation_id: "0" };
    case "fetch_processing_config_json": {
      const doc = configDoc(); // what the backend held when the read was made
      if (!holdNextConfig) return doc;
      holdNextConfig = false;
      return new Promise((resolve) => held.push(() => resolve(doc)));
    }
    case "review_export_status_json":
    case "review_reanalysis_status_json": return JSON.stringify({ state: "idle" });
    default: return null;
  }
}

let host: HTMLDivElement, root: Root;
const advance = (ms: number) => act(async () => { await vi.advanceTimersByTimeAsync(ms); });
const button = (text: string) => [...host.querySelectorAll("button")].find((b) => b.textContent === text);
const click = async (el: Element | null | undefined) => { await act(async () => (el as HTMLElement).click()); await advance(300); };
const chip = () => host.querySelector("[data-testid=align-state-roi]")!.textContent;
const field = () => host.querySelector<HTMLInputElement>('input[aria-label="Processing ROI (frame) X"]')!;
async function typeX(value: string) {
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(field(), value);
    field().dispatchEvent(new Event("input", { bubbles: true }));
  });
}

beforeEach(() => {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
  const saved = new Map<string, string>();
  const store = { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v), removeItem: (k: string) => void saved.delete(k) };
  vi.stubGlobal("localStorage", store);
  vi.stubGlobal("sessionStorage", store);
  vi.useFakeTimers();
  vi.mocked(invoke).mockReset();
  vi.mocked(invoke).mockImplementation(async (cmd: string, args?: unknown) => respond(cmd, args as Record<string, unknown>));
  roi = { x: 0, y: 0, w: 0, h: 0 };
  holdNextConfig = false;
  held.length = 0;
  host = document.createElement("div"); document.body.append(host); root = createRoot(host);
});
afterEach(async () => {
  await act(async () => root.unmount());
  host.remove();
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

it("a late config readback from a timed-out write never replaces the newer write's readback", async () => {
  await act(async () => root.render(<App />));
  await advance(6000);
  await click(button("Workspace preview"));
  await click(host.querySelectorAll(".mib-stages__tab")[1]);
  expect(chip()).toBe("Current");

  // Write A: the backend applies it, but its config readback is held back past the timeout.
  holdNextConfig = true;
  await typeX("10");
  await click(button("Write ROI"));
  expect(held).toHaveLength(1);
  await advance(WRITE_TIMEOUT_MS);
  expect(chip()).toMatch(/^Timed out/);

  // Write B completes with its own readback.
  await typeX("20");
  await click(button("Write ROI"));
  expect(chip()).toBe("Readback matches the request");
  expect(field().value).toBe("20");

  // A's readback (x = 10) finally answers: it is older than B's and is dropped.
  await act(async () => held.shift()!());
  await advance(3000);
  expect(chip()).toBe("Readback matches the request");
  expect(field().value).toBe("20");
  expect(host.querySelector("[data-testid=align-quick-controls]")!.textContent).toContain("0 × 0 @ 20, 0");
});
