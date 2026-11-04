// @vitest-environment jsdom
// Where the #866 readouts and locks land in the shell and connect views: the side panel, the status bar, Stop Camera.
import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { EXPERIMENT_STATES } from "../../bridgeContract";
import type { ExperimentStatus } from "../../eventAdapter";
import { StudioContext } from "../../studio/StudioContext";
import type { Studio } from "../../studio/useStudioController";
import { CameraActions } from "../connect/CameraActions";
import { TelemetrySidebar } from "../shell/Sidebar";
import { StatusBar } from "../shell/StatusBar";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement("div"); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });

const status = (over: Partial<ExperimentStatus> = {}) => ({
  valid: true, state: EXPERIMENT_STATES.Idle, valid_buffered: "3", invalid_buffered: "4", valid_saved: "0", invalid_saved: "0",
  persistence_admitted: "0", persistence_committed: "0", persistence_failed: "0", dropped_valid: "0", dropped_invalid: "0",
  flushing: false, terminal: false, ...over,
}) as ExperimentStatus;
const studio = (over: Record<string, unknown> = {}) => ({
  sidebarCollapsed: false, caps: {}, backgroundSet: false, displayFps: 29.6, hostProcessing: true,
  stats: { valid: true, algo_fps1s: 120, valid_fps1s: 1.2, invalid_fps1s: 3.4, pixel_to_micron: 0.5 },
  algoFps: 120, validFps: 1.2, invalidFps: 3.4, running: true, dataRate: 4.56, pz7035: false, instrument: null, afStatus: null,
  expState: EXPERIMENT_STATES.Idle, expActive: false, runOutcome: null, expStatus: status(), elapsedWallSeconds: null, toggleSidebar: vi.fn(),
  ready: true, backgroundRevision: 0, abi: 44, reviewing: false, recording: false, setShowLog: vi.fn(), showLog: false, log: [],
  runLocked: false, runProgress: "", onStartCamera: vi.fn(), onStopCamera: vi.fn(), startCameraReason: undefined, stopCameraBlock: undefined,
  ...over,
}) as unknown as Studio;
const render = async (s: Studio, view: JSX.Element) => act(async () => root.render(<StudioContext.Provider value={s}>{view}</StudioContext.Provider>));
const rowValue = (label: string) => {
  const row = Array.from(host.querySelectorAll<HTMLElement>(".side-row")).find((r) => r.querySelector(".k")!.textContent === label);
  return row ? { text: row.querySelector(".v")!.textContent, cls: row.querySelector(".v")!.className, title: row.title } : null;
};

describe("the side panel (MON-095, 098, 100)", () => {
  it("writes the Camera group as Qt does: Running in green, FPS, Data Rate", async () => {
    await render(studio(), <TelemetrySidebar />);
    expect(rowValue("Status:")).toMatchObject({ text: "Running", cls: expect.stringContaining("ok") });
    expect(rowValue("FPS:")).toMatchObject({ text: "29.6" });
    expect(rowValue("Data Rate:")).toMatchObject({ text: "4.6 MB/s" });
  });
  it("reads Stopped and zero when the camera is stopped", async () => {
    await render(studio({ running: false }), <TelemetrySidebar />);
    expect(Array.from(host.querySelectorAll(".side-row")).filter((r) => r.querySelector(".k")!.textContent === "Status:")[0].textContent).toBe("Status:Stopped");
    expect(rowValue("Data Rate:")!.text).toBe("0.0 MB/s");
  });
  it("shows Algo Time in microseconds, greyed with Qt's tooltip once it is more than a second old", async () => {
    await render(studio({ stats: { valid: true, algo_fps1s: 120, valid_fps1s: 1, invalid_fps1s: 2, pixel_to_micron: 1, algo_avg_us: 85.04, algo_avg_age_ms: 1500 } }), <TelemetrySidebar />);
    expect(rowValue("Algo Time:")).toMatchObject({ text: "85.0 µs", cls: expect.stringContaining("dim"), title: "Stale: last update 1500 ms ago" });
  });
  it("shows the backend's capture rate and data rate as text with their validity, not the rates measured here (MON-098)", async () => {
    const camera = { running: true, frame_rate: { state: "valid", value: 500, age_ms: 40 }, data_rate_mbps: { state: "stale", value: 31, age_ms: 3000 } };
    await render(studio({ stats: { valid: true, algo_fps1s: 1, valid_fps1s: 1, invalid_fps1s: 1, pixel_to_micron: 1, telemetry: { realtime_mode: "inline", camera } } }), <TelemetrySidebar />);
    expect(rowValue("FPS:")).toMatchObject({ text: "29.6" }); // the Display group's own FPS is the first "FPS:" row
    const fps = Array.from(host.querySelectorAll<HTMLElement>(".side-row")).filter((r) => r.querySelector(".k")!.textContent === "FPS:");
    expect(fps.map((r) => r.querySelector(".v")!.textContent)).toEqual(["29.6", "500"]);
    expect(rowValue("Data Rate:")).toMatchObject({ text: "31 (stale 3.0 s) MB/s", title: "" });
    await render(studio({ stats: { valid: true, algo_fps1s: 1, valid_fps1s: 1, invalid_fps1s: 1, pixel_to_micron: 1, telemetry: { realtime_mode: null, camera: { ...camera, frame_rate: { state: "unsupported", value: null, age_ms: null }, data_rate_mbps: { state: "unavailable", value: null, age_ms: null } } } } }), <TelemetrySidebar />);
    expect(rowValue("Data Rate:")!.text).toBe("n/a MB/s");
    expect(Array.from(host.querySelectorAll<HTMLElement>(".side-row")).filter((r) => r.querySelector(".k")!.textContent === "FPS:")[1].querySelector(".v")!.textContent).toBe("unsupported");
  });
  it("falls back to the rate measured here, and says so, only when the backend sends no camera telemetry", async () => {
    await render(studio(), <TelemetrySidebar />);
    expect(rowValue("Data Rate:")).toMatchObject({ text: "4.6 MB/s" });
    expect(rowValue("Data Rate:")!.title).toContain("sent no capture rate");
    await render(studio({ stats: { valid: true, algo_fps1s: 1, valid_fps1s: 1, invalid_fps1s: 1, pixel_to_micron: 1, telemetry_error: "camera.frame_rate.state is not a contract metric state" } }), <TelemetrySidebar />);
    expect(rowValue("Data Rate:")!.title).toContain("could not be read (camera.frame_rate.state is not a contract metric state)");
  });
  it("has no Algo Time row while the backend does not report one", async () => {
    await render(studio(), <TelemetrySidebar />);
    expect(rowValue("Algo Time:")).toBeNull();
    expect(rowValue("Algo FPS:")!.text).toBe("120.0");
  });
  it("shows Runtime as hh:mm:ss while a run is active and 00:00:00 otherwise, and Flushing in orange", async () => {
    await render(studio({ expActive: true, expState: EXPERIMENT_STATES.Active, expStatus: status({ state: EXPERIMENT_STATES.Active, flushing: true }), elapsedWallSeconds: 3725 }), <TelemetrySidebar />);
    expect(rowValue("Runtime:")!.text).toBe("01:02:05");
    expect(rowValue("Flush Status:")).toMatchObject({ text: "Flushing…", cls: expect.stringContaining("warn") });
    await render(studio({ elapsedWallSeconds: 3725 }), <TelemetrySidebar />);
    expect(rowValue("Runtime:")!.text).toBe("00:00:00");
    expect(rowValue("Flush Status:")).toMatchObject({ text: "Idle", cls: expect.stringContaining("dim") });
  });
});

describe("the status bar (MON-104, MON-025)", () => {
  const line = () => host.querySelector<HTMLElement>("[data-tour='monitoring-status-line']")!;
  it("is Qt's one line, elided by CSS and complete in the tooltip", async () => {
    await render(studio(), <StatusBar />);
    expect(line().textContent).toBe("Camera 30 fps · Valid 1.2/s · Invalid 3.4/s · Algo 120/s");
    expect(line().title).toBe(line().textContent);
  });
  it("adds the run time and the buffered count while a run is active", async () => {
    await render(studio({ expActive: true, expStatus: status({ state: EXPERIMENT_STATES.Active, flushing: true }), elapsedWallSeconds: 12 }), <StatusBar />);
    expect(line().textContent).toMatch(/ · Run 12 s · buffered 7 \(flushing\)$/);
  });
  it("shows the stop and save sentence instead while the run is locked", async () => {
    await render(studio({ runLocked: true, runProgress: "Saving experiment data…" }), <StatusBar />);
    expect(line().textContent).toBe("Saving experiment data…");
  });
});

describe("Stop Camera (MON-012)", () => {
  const stop = () => Array.from(host.querySelectorAll<HTMLButtonElement>("button")).find((b) => b.textContent === "Stop Camera")!;
  it("is refused with the reason for an experiment, a save in progress or raw recording", async () => {
    for (const reason of [
      "Cannot stop camera while an experiment is active. Stop the experiment first.",
      "Cannot stop camera while experiment data is being saved. Wait for the save to finish.",
      "Cannot stop camera while frame recording is active. Stop recording first.",
    ]) {
      await render(studio({ stopCameraBlock: reason }), <CameraActions />);
      expect(stop().disabled).toBe(true);
      expect(stop().title).toBe(reason);
    }
  });
  it("is enabled for a running camera with nothing in the way, and disabled with its own reason when stopped", async () => {
    await render(studio(), <CameraActions />);
    expect(stop().disabled).toBe(false);
    await render(studio({ running: false }), <CameraActions />);
    expect(stop()).toMatchObject({ disabled: true, title: "Camera is not running" });
  });
});
