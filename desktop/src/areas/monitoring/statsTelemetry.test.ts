// The statistics side panel's backend figures (MON-095, MON-098): one `fetch_processing_stats` reply per poll gives the
// algorithm time, the realtime mode and the camera telemetry. The contract's names (bridge-contract.json processing_stats)
// must be the ones decodeProcessingStats and decodeProcessingTelemetry read.
import { beforeEach, expect, it, vi } from "vitest";
import contractText from "../../../../crates/mib-bridge/contract/bridge-contract.json?raw";
import { bridge } from "../../bridge";
import { algoTimeReadout, cameraRateReadouts } from "./statsReadouts";

const { invoke } = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
beforeEach(() => { invoke.mockReset(); });

const sample = (state: string, value: number | null, age: number | null) => ({ state, value, age_ms: age });
// Exactly the members the contract lists for the reply (algo_avg_us, algo_avg_age_ms, realtime_mode, camera).
const reply = (extra: Record<string, unknown> = {}) => ({
  transport_version: 1, valid: true, algo_fps1s: 4800, valid_fps1s: 12, invalid_fps1s: 3, pixel_to_micron: 0.5,
  algo_avg_us: 182.5, algo_avg_age_ms: 1500, realtime_mode: "inline",
  camera: { running: true, frame_rate: sample("valid", 5000, 120), data_rate_mbps: sample("stale", 235, 4100) }, ...extra,
});

it("decodes the algorithm time and the camera telemetry from ONE reply (one invoke per poll)", async () => {
  invoke.mockResolvedValueOnce(reply());
  const stats = await bridge.fetchProcessingStats();
  expect(invoke).toHaveBeenCalledTimes(1);
  expect(invoke).toHaveBeenCalledWith("fetch_processing_stats");
  expect(stats.algo_avg_us).toBe(182.5);
  expect(stats.algo_avg_age_ms).toBe(1500);
  expect(stats.telemetry?.realtime_mode).toBe("inline");
  expect(stats.telemetry?.camera?.frame_rate).toEqual(sample("valid", 5000, 120));
  expect(stats.telemetry_error).toBeUndefined();
  // And the readouts the sidebar builds from it.
  expect(algoTimeReadout(stats.algo_avg_us, stats.algo_avg_age_ms)).toEqual({ text: "182.5 µs", cls: "dim", title: "Stale: last update 1500 ms ago" });
  const camera = stats.telemetry!.camera!;
  expect(cameraRateReadouts({ running: true, displayFps: 29.6, dataRateMBs: 4.56, captureFps: camera.frame_rate, captureMBs: camera.data_rate_mbps }))
    .toEqual({ fps: { text: "5000" }, dataRate: { text: "235 (stale 4.1 s) MB/s" } });
});

it("reads the members the contract names, spelled as the contract spells them", () => {
  const contract = JSON.parse(contractText);
  const fields: string = contract.processing_stats.fields;
  for (const name of ["algo_avg_us", "algo_avg_age_ms", "realtime_mode", "camera", "frame_rate", "data_rate_mbps", "running"]) expect(fields).toContain(name);
  expect(contract.processing_stats.metric_states).toEqual(["valid", "stale", "unavailable", "unsupported", "error"]);
});

it("leaves the algorithm time and the camera rows out for a backend that does not send them (the rows fall back)", async () => {
  invoke.mockResolvedValueOnce({ transport_version: 1, valid: true, algo_fps1s: 1, valid_fps1s: 1, invalid_fps1s: 1, pixel_to_micron: 1 });
  const stats = await bridge.fetchProcessingStats();
  expect(stats.algo_avg_us).toBeUndefined();
  expect(stats.telemetry).toEqual({ realtime_mode: null, camera: null });
  expect(algoTimeReadout(stats.algo_avg_us, stats.algo_avg_age_ms)).toBeNull();
  expect(cameraRateReadouts({ running: true, displayFps: 29.6, dataRateMBs: 4.56 }).fps.text).toBe("29.6");
});

it("writes an unavailable or unsupported sample as text and never as a 0", async () => {
  invoke.mockResolvedValueOnce(reply({ camera: { running: true, frame_rate: sample("unsupported", null, null), data_rate_mbps: sample("unavailable", null, null) } }));
  const camera = (await bridge.fetchProcessingStats()).telemetry!.camera!;
  const rows = cameraRateReadouts({ running: true, displayFps: 29.6, dataRateMBs: 4.56, captureFps: camera.frame_rate, captureMBs: camera.data_rate_mbps });
  expect(rows.fps.text).toBe("unsupported");
  expect(rows.dataRate.text).toBe("n/a MB/s");
});

it("keeps the rates when the telemetry breaks the contract, and records why", async () => {
  invoke.mockResolvedValueOnce(reply({ camera: { running: true, frame_rate: sample("fast", 1, 1), data_rate_mbps: sample("valid", 1, 1) } }));
  const stats = await bridge.fetchProcessingStats();
  expect(stats.valid).toBe(true);
  expect(stats.algo_avg_us).toBe(182.5);
  expect(stats.telemetry).toBeUndefined();
  expect(stats.telemetry_error).toContain("metric state");
});
