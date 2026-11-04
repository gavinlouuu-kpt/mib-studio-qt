// Bridge ABI 46 (#866 PR E): the live Monitoring density, the reference contour from a file, the
// E-modulus table and the processing / camera telemetry against a mocked transport. Arguments are
// checked before anything is sent, replies are checked against the contract, and a point is matched
// to a snapshot row by its whole identity (start generation, capture epoch, frame index, object id),
// never by a frame index alone.
import { beforeEach, expect, it, vi } from "vitest";
import { bridge } from "./bridge";
import { BRIDGE_ABI_VERSION, EMODULUS_LUT, KDE_REFERENCE_FROM_FILE, MONITORING_DENSITY, PROCESSING_STATS } from "./bridgeContract";
import {
  decodeEmodulusLut, decodeKdeReference, decodeMonitoringDensity, decodeMonitoringDensitySettingsReply, decodeProcessingTelemetry,
  densityByRow, densityRowKey, emodulusLutArgs, formatMetricSample, kdeReferenceArgs, kdeReferenceLabel, monitoringDensityArgs,
  monitoringDensitySettingsArgs,
} from "./monitoringDensity";

const { invoke } = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
beforeEach(() => { invoke.mockReset(); });

const settings = (extra: Record<string, unknown> = {}) => ({
  enabled: true, interval_ms: 2000, bandwidth_factor: 1, core_fraction: 0.9, x0: 0, x1: 1000, y0: 0, y1: 1, ...extra,
});
const stats = (extra: Record<string, unknown> = {}) => ({
  next_interval_ms: 1800, refresh_ms: 2000, estimates: "4", skipped_unchanged: "9", skipped_under_load: "0", busy: false,
  priority_lowered: true, ...extra,
});
// Two captures in one ring: the frame numbering restarted (frames 1 and 2 exist twice), and frame 2 holds two objects.
const points = (extra: Record<string, unknown> = {}) => ({
  count: 4,
  contexts: [{ start_generation: "0", capture_epoch: "3" }, { start_generation: "7", capture_epoch: "5" }],
  context: [0, 0, 1, 1],
  frame_index: ["1", "2", "1", "2"],
  object_id: [0, 0, 0, 1],
  density: [0.25, 1, 0.5, null],
  ...extra,
});
const result = (extra: Record<string, unknown> = {}) => ({
  points: points(), bandwidth: { x: 31.5, y: 0.0042 }, bandwidth_factor: 1, core_fraction: 0.9, core_level: 0.2, core_count: 3,
  contours: [[[100, 0.01], [200, 0.01], [200, 0.06], [100, 0.01]]], pixel_to_micron: 0.5,
  grid: { x0: 0, x1: 1000, y0: 0, y1: 1, nx: 128, ny: 64 }, compute_ms: 12, compute_us: 11500, wall_ms: 14,
  computed_at_ns: "1730000000000000000", ...extra,
});
const reply = (extra: Record<string, unknown> = {}) => ({
  valid: true, generation: "5", unchanged: false, settings: settings(), stats: stats(), result: result(), ...extra,
});

it("pins the contract: the ABI, the density bounds, the metric states and the realtime modes", () => {
  expect(BRIDGE_ABI_VERSION).toBe(46);
  expect(MONITORING_DENSITY).toMatchObject({
    interval_ms_min: 500, interval_ms_max: 60000, interval_ms_default: 2000, bandwidth_factor_min: 0.2, bandwidth_factor_max: 5,
    bandwidth_factor_default: 1, core_fraction_min: 0.05, core_fraction_max: 1, core_fraction_default: 0.9, grid_nx: 128, grid_ny: 64,
    compute_budget_factor: 20, max_points: 1000,
  });
  expect(KDE_REFERENCE_FROM_FILE).toMatchObject({ path_max_bytes: 4096, max_vertices: 20000 });
  expect(EMODULUS_LUT).toMatchObject({ known_identity_max_bytes: 8192 });
  expect(PROCESSING_STATS.metric_states).toEqual(["valid", "stale", "unavailable", "unsupported", "error"]);
  expect(PROCESSING_STATS.realtime_modes).toEqual(["inline", "async_batch"]);
});

it("pushes the settings: omitted members take the backend's defaults, bad ones never reach the transport", async () => {
  expect(monitoringDensitySettingsArgs({ enabled: false })).toEqual({ enabled: false });
  expect(monitoringDensitySettingsArgs({ enabled: true, intervalMs: 500, bandwidthFactor: 1.5, coreFraction: 0.8, x0: 0, x1: 1000, y0: 0, y1: 1 }))
    .toEqual({ enabled: true, intervalMs: 500, bandwidthFactor: 1.5, coreFraction: 0.8, x0: 0, x1: 1000, y0: 0, y1: 1 });
  expect(() => monitoringDensitySettingsArgs({ enabled: true, intervalMs: 1500.5 })).toThrow("INVALID_ARGUMENTS");
  expect(() => monitoringDensitySettingsArgs({ enabled: true, bandwidthFactor: Number.NaN })).toThrow("INVALID_ARGUMENTS");
  expect(() => monitoringDensitySettingsArgs({ enabled: true, x1: Infinity })).toThrow("INVALID_ARGUMENTS");
  expect(() => monitoringDensitySettingsArgs({ enabled: "yes" as unknown as boolean })).toThrow("INVALID_ARGUMENTS");

  invoke.mockResolvedValueOnce({ ok: true, message: "Monitoring density enabled", settings: settings({ interval_ms: 500, bandwidth_factor: 5 }) });
  const r = await bridge.monitoringDensitySetSettings({ enabled: true, intervalMs: 100, bandwidthFactor: 9 });
  expect(invoke).toHaveBeenLastCalledWith("monitoring_density_set_settings", { enabled: true, intervalMs: 100, bandwidthFactor: 9 });
  // The service clamps: the reply is what applies (the frontend sent 100 ms and 9, the backend says 500 and 5).
  expect(r.settings).toMatchObject({ interval_ms: 500, bandwidth_factor: 5 });
  await expect(bridge.monitoringDensitySetSettings({ enabled: true, intervalMs: 2.5 })).rejects.toThrow("INVALID_ARGUMENTS");
  expect(invoke).toHaveBeenCalledTimes(1);
});

it("checks the settings reply: bounds, the applied settings of an accepted reply, and the not-initialized form", () => {
  expect(decodeMonitoringDensitySettingsReply({ ok: false, message: "Backend not initialized", settings: null }))
    .toEqual({ ok: false, message: "Backend not initialized", settings: null });
  expect(() => decodeMonitoringDensitySettingsReply({ ok: true, message: "x", settings: null })).toThrow("MONITORING_DENSITY_INVALID");
  expect(() => decodeMonitoringDensitySettingsReply({ ok: true, message: "x", settings: settings({ interval_ms: 100 }) })).toThrow("interval_ms");
  expect(() => decodeMonitoringDensitySettingsReply({ ok: true, message: "x", settings: settings({ bandwidth_factor: 6 }) })).toThrow("bandwidth_factor");
  expect(() => decodeMonitoringDensitySettingsReply({ ok: true, message: "x", settings: settings({ core_fraction: 0.01 }) })).toThrow("core_fraction");
});

it("requests an update through the command layer", async () => {
  invoke.mockResolvedValueOnce({ transport_version: 1, ok: true, command: 7, message: "Monitoring density update requested", operation_id: "0" });
  expect((await bridge.monitoringDensityRequestUpdate()).ok).toBe(true);
  expect(invoke).toHaveBeenLastCalledWith("monitoring_density_request_update");
});

it("polls by generation: an unchanged reply changes nothing, a new one carries the result", async () => {
  expect(monitoringDensityArgs()).toEqual({});
  expect(monitoringDensityArgs(null)).toEqual({});
  expect(monitoringDensityArgs("18446744073709551615")).toEqual({ sinceGeneration: "18446744073709551615" });
  expect(monitoringDensityArgs(7)).toEqual({ sinceGeneration: "7" });
  expect(() => monitoringDensityArgs(-1)).toThrow();
  expect(() => monitoringDensityArgs(1.5)).toThrow();

  invoke.mockResolvedValueOnce({ valid: true, generation: "5", unchanged: true });
  expect(await bridge.fetchMonitoringDensity("5")).toEqual({ valid: true, unchanged: true, generation: "5" });
  expect(invoke).toHaveBeenLastCalledWith("fetch_monitoring_density", { sinceGeneration: "5" });

  invoke.mockResolvedValueOnce(reply());
  const r = await bridge.fetchMonitoringDensity("4");
  if (!r.valid || r.unchanged) throw new Error("a new result expected");
  expect(r.generation).toBe("5");
  expect(r.stats.refresh_ms).toBe(2000);
  expect(r.result?.core_count).toBe(3);
  expect(r.result?.contours[0][2]).toEqual([200, 0.06]);

  invoke.mockResolvedValueOnce({ valid: false });
  expect(await bridge.fetchMonitoringDensity()).toEqual({ valid: false });
  expect(invoke).toHaveBeenLastCalledWith("fetch_monitoring_density", {});
});

it("an empty ring is a reply without a result (the shell clears its densities and contours)", () => {
  const r = decodeMonitoringDensity(reply({ result: null }));
  if (!r.valid || r.unchanged) throw new Error("a new generation expected");
  expect(r.result).toBeNull();
});

it("matches a point to a snapshot row by the whole identity, so a restarted frame numbering never collides", () => {
  const r = decodeMonitoringDensity(reply());
  if (!r.valid || r.unchanged || !r.result) throw new Error("a result expected");
  const byRow = densityByRow(r.result);
  // Frames 1 and 2 exist in two captures and frame 2 of the second holds two objects: three finite densities, one null (left out).
  expect(byRow.size).toBe(3);
  const row = (start: string, epoch: string, frame: string, object: number) => byRow.get(densityRowKey({ start_generation: start, capture_epoch: epoch, frame_index: frame, object_id: object }));
  expect(row("0", "3", "1", 0)).toBe(0.25);
  expect(row("0", "3", "2", 0)).toBe(1);
  expect(row("7", "5", "1", 0)).toBe(0.5);
  expect(row("7", "5", "2", 1)).toBeUndefined(); // density null
  expect(row("7", "5", "2", 0)).toBeUndefined(); // another object of that frame: not in the estimate
  expect(row("0", "4", "1", 0)).toBeUndefined(); // the same frame number of another capture
  // A snapshot row's frame_index is a JSON number: it matches the same key.
  expect(byRow.get(densityRowKey({ start_generation: "0", capture_epoch: "3", frame_index: 2, object_id: 0 }))).toBe(1);
});

it("refuses a density reply that breaks the contract", () => {
  const bad = (extra: Record<string, unknown>) => () => decodeMonitoringDensity(reply({ result: result(extra) }));
  expect(bad({ points: points({ density: [0.25, 1, 0.5] }) })).toThrow("not parallel");
  expect(bad({ points: points({ context: [0, 0, 1, 2] }) })).toThrow("names no context");
  expect(bad({ points: points({ density: [0.25, 1.5, 0.5, null] }) })).toThrow("outside [0, 1]");
  expect(bad({ points: points({ frame_index: ["1", "02", "1", "2"] }) })).toThrow("canonical decimal");
  expect(bad({ points: points({ count: MONITORING_DENSITY.max_points + 1 }) })).toThrow("max_points");
  expect(bad({ points: points({ object_id: [0, 0, 0, 1.5] }) })).toThrow("object_id");
  expect(bad({ core_level: null })).toThrow("without a core level"); // contours listed, no level
  expect(bad({ contours: [[[1, 2, 3]]] })).toThrow("[x, y]");
  expect(bad({ contours: [[[1, "2"]]] })).toThrow("finite");
  expect(bad({ computed_at_ns: 12 })).toThrow("decimal");
  expect(() => decodeMonitoringDensity(reply({ generation: 5 }))).toThrow("decimal");
  expect(() => decodeMonitoringDensity(reply({ stats: stats({ busy: "no" }) }))).toThrow("busy");
  expect(() => decodeMonitoringDensity("nope")).toThrow("MONITORING_DENSITY_INVALID");
  expect(() => decodeMonitoringDensity({ valid: "maybe" })).toThrow("valid");
  // No core below three cells: a null level with no contours is fine.
  const few = decodeMonitoringDensity(reply({ result: result({ core_level: null, core_count: 0, contours: [], points: points({ count: 0, contexts: [], context: [], frame_index: [], object_id: [], density: [] }) }) }));
  if (!few.valid || few.unchanged || !few.result) throw new Error("a result expected");
  expect(few.result.core_level).toBeNull();
  expect(few.result.points.count).toBe(0);
});

const reference = (extra: Record<string, unknown> = {}) => ({
  ok: true, path: "/data/run7.h5", file_name: "run7.h5", error: null, reason: null, source: "full-run", core_fraction: 0.9, core_percent: 90,
  loops: 1, contours: [[[100, 0.01], [200, 0.01], [200, 0.06], [100, 0.01]]], ...extra,
});

it("reads a reference contour from a file: the label is Qt's, a refusal carries the plain reason", async () => {
  expect(kdeReferenceArgs("/data/run7.h5")).toEqual({ path: "/data/run7.h5" });
  expect(() => kdeReferenceArgs("")).toThrow("INVALID_ARGUMENTS");
  expect(() => kdeReferenceArgs("x".repeat(KDE_REFERENCE_FROM_FILE.path_max_bytes + 1))).toThrow("INVALID_ARGUMENTS");
  expect(() => kdeReferenceArgs("é".repeat(KDE_REFERENCE_FROM_FILE.path_max_bytes / 2 + 1))).toThrow("INVALID_ARGUMENTS"); // bytes, not characters
  invoke.mockResolvedValueOnce(reference());
  const ok = await bridge.fetchKdeReferenceFromFile("/data/run7.h5");
  expect(invoke).toHaveBeenLastCalledWith("fetch_kde_reference_from_file", { path: "/data/run7.h5" });
  expect(ok.ok && ok.loops === 1 && ok.contours[0].length === 4).toBe(true);
  expect(kdeReferenceLabel(ok)).toBe("run7.h5, full-run 90%");
  expect(kdeReferenceLabel(decodeKdeReference(reference({ source: "live", core_percent: 80, core_fraction: 0.8 })))).toBe("run7.h5, live estimate 80%");

  const refused = decodeKdeReference({
    ok: false, path: "/data/run8.h5", file_name: "run8.h5", error: "No reference contour loaded from /data/run8.h5: the experiment has no stored core contour",
    reason: "the experiment has no stored core contour", source: null, core_fraction: null, core_percent: null, loops: 0, contours: [],
  });
  expect(refused.ok).toBe(false);
  expect(refused.error).toBe("No reference contour loaded from /data/run8.h5: the experiment has no stored core contour");
  expect(kdeReferenceLabel(refused)).toBeNull();
  await expect(bridge.fetchKdeReferenceFromFile("")).rejects.toThrow("INVALID_ARGUMENTS");
});

it("refuses a reference reply that breaks the contract", () => {
  expect(() => decodeKdeReference(reference({ error: "x" }))).toThrow("error is set exactly");
  expect(() => decodeKdeReference(reference({ contours: [] , loops: 0 }))).toThrow("exactly when");
  expect(() => decodeKdeReference(reference({ loops: 2 }))).toThrow("loops differs");
  expect(() => decodeKdeReference(reference({ source: "stored" }))).toThrow("source");
  const big = [Array.from({ length: KDE_REFERENCE_FROM_FILE.max_vertices + 1 }, (_, i) => [i, i])];
  expect(() => decodeKdeReference(reference({ contours: big }))).toThrow("max_vertices");
});

const info = { path: "/cache/lut.txt", sha256: "ab".repeat(32), lut_id: "lut-1", revision: "r2", source: "catalog", catalog_sha256: "cd".repeat(32), checksum_status: "verified" };
const grid = (extra: Record<string, unknown> = {}) => ({
  area_min: 100, area_step: 10, area_bins: 3, deform_min: 0, deform_step: 0.05, deform_bins: 2, values: [1.5, 2, null, 1.8, null, 3.25], ...extra,
});

it("reads the E-modulus table: the grid only when the identity differs, null for no value", async () => {
  expect(emodulusLutArgs()).toEqual({});
  expect(emodulusLutArgs("")).toEqual({});
  expect(emodulusLutArgs("abc|/p")).toEqual({ knownIdentity: "abc|/p" });
  expect(() => emodulusLutArgs("x".repeat(EMODULUS_LUT.known_identity_max_bytes + 1))).toThrow("INVALID_ARGUMENTS");

  const identity = `${info.sha256}|${info.path}`;
  invoke.mockResolvedValueOnce({ valid: true, loaded: true, identity, unchanged: false, info, lut: grid() });
  const first = await bridge.fetchEmodulusLut();
  expect(invoke).toHaveBeenLastCalledWith("fetch_emodulus_lut", {});
  if (!first.valid || !first.loaded || first.unchanged) throw new Error("a grid expected");
  expect(first.identity).toBe(identity);
  expect(first.lut.values).toHaveLength(first.lut.area_bins * first.lut.deform_bins);
  // Row-major [deform][area]: deform index 1, area index 2 is the last value.
  expect(first.lut.values[1 * first.lut.area_bins + 2]).toBe(3.25);
  expect(first.lut.values[2]).toBeNull();

  invoke.mockResolvedValueOnce({ valid: true, loaded: true, identity, unchanged: true, info, lut: null });
  const same = await bridge.fetchEmodulusLut(identity);
  expect(invoke).toHaveBeenLastCalledWith("fetch_emodulus_lut", { knownIdentity: identity });
  expect(same).toMatchObject({ valid: true, loaded: true, unchanged: true, identity });
  expect("lut" in same).toBe(false);

  expect(decodeEmodulusLut({ valid: true, loaded: false, identity: "", unchanged: false, info: null, lut: null })).toEqual({ valid: true, loaded: false });
  expect(decodeEmodulusLut({ valid: false, message: "try again" })).toEqual({ valid: false, message: "try again" });
  expect(decodeEmodulusLut({ valid: false })).toEqual({ valid: false });
});

it("refuses a table reply that breaks the contract", () => {
  const identity = `${info.sha256}|${info.path}`;
  const full = (lut: unknown, extra: Record<string, unknown> = {}) => () =>
    decodeEmodulusLut({ valid: true, loaded: true, identity, unchanged: false, info, lut, ...extra });
  expect(full(grid({ values: [1, 2, 3] }))).toThrow("area_bins x deform_bins");
  expect(full(grid({ area_bins: 1, values: [1, 2] }))).toThrow("two bins");
  expect(full(grid({ area_step: 0 }))).toThrow("positive");
  expect(full(grid({ values: [1.5, 2, "n", 1.8, null, 3.25] }))).toThrow("finite");
  expect(full(null)).toThrow("not an object");
  expect(() => decodeEmodulusLut({ valid: true, loaded: true, identity: "", unchanged: false, info, lut: grid() })).toThrow("identity");
  expect(() => decodeEmodulusLut({ valid: true, loaded: true, identity, unchanged: true, info, lut: grid() })).toThrow("no grid");
});

const sample = (state: string, value: number | null, age: number | null) => ({ state, value, age_ms: age });
const statsReply = (extra: Record<string, unknown> = {}) => ({
  transport_version: 1, valid: true, algo_fps1s: 4800, valid_fps1s: 12, invalid_fps1s: 3, pixel_to_micron: 0.5,
  algo_avg_us: 182.5, algo_avg_age_ms: 40, realtime_mode: "async_batch",
  camera: { running: true, frame_rate: sample("valid", 5000, 120), data_rate_mbps: sample("stale", 235, 4100) }, ...extra,
});

it("decodes the telemetry of the processing stats, under the names PR #877's decoder reads", async () => {
  const t = decodeProcessingTelemetry(statsReply());
  expect(t).toEqual({
    algo_avg_us: 182.5, algo_avg_age_ms: 40, realtime_mode: "async_batch",
    camera: { running: true, frame_rate: sample("valid", 5000, 120), data_rate_mbps: sample("stale", 235, 4100) },
  });
  invoke.mockResolvedValueOnce(statsReply({ realtime_mode: "inline" }));
  expect((await bridge.fetchProcessingTelemetry()).realtime_mode).toBe("inline");
  expect(invoke).toHaveBeenLastCalledWith("fetch_processing_stats");
  // Not published yet, or not initialized: null, never a 0.
  expect(decodeProcessingTelemetry(statsReply({ algo_avg_us: null, algo_avg_age_ms: null, realtime_mode: null, camera: null })))
    .toEqual({ algo_avg_us: null, algo_avg_age_ms: null, realtime_mode: null, camera: null });
  // An older backend sends none of it.
  expect(decodeProcessingTelemetry({ transport_version: 1, valid: true, algo_fps1s: 1 }))
    .toEqual({ algo_avg_us: null, algo_avg_age_ms: null, realtime_mode: null, camera: null });
  // A stopped session: unavailable samples with no value.
  const stopped = decodeProcessingTelemetry(statsReply({ camera: { running: false, frame_rate: sample("unavailable", null, null), data_rate_mbps: sample("unavailable", null, null) } }));
  expect(stopped.camera?.running).toBe(false);
});

it("refuses telemetry that breaks the contract", () => {
  expect(() => decodeProcessingTelemetry(statsReply({ realtime_mode: "batch" }))).toThrow("realtime_mode");
  expect(() => decodeProcessingTelemetry(statsReply({ camera: { running: true, frame_rate: sample("fast", 1, 1), data_rate_mbps: sample("valid", 1, 1) } }))).toThrow("metric state");
  // An unknown rate is never a 0: a value exists exactly for a valid or stale sample.
  expect(() => decodeProcessingTelemetry(statsReply({ camera: { running: true, frame_rate: sample("unsupported", 0, 0), data_rate_mbps: sample("valid", 1, 1) } }))).toThrow("exactly");
  expect(() => decodeProcessingTelemetry(statsReply({ camera: { running: true, frame_rate: sample("valid", null, null), data_rate_mbps: sample("valid", 1, 1) } }))).toThrow("exactly");
  expect(() => decodeProcessingTelemetry(statsReply({ algo_avg_us: "182" }))).toThrow("algo_avg_us");
});

it("formats a sample as Qt's Statistics panel does", () => {
  expect(formatMetricSample({ state: "valid", value: 5000.7, age_ms: 10 })).toBe("5000");
  expect(formatMetricSample({ state: "stale", value: 235, age_ms: 4100 })).toBe("235 (stale 4.1 s)");
  expect(formatMetricSample({ state: "unavailable", value: null, age_ms: null })).toBe("n/a");
  expect(formatMetricSample({ state: "unsupported", value: null, age_ms: null })).toBe("unsupported");
  expect(formatMetricSample({ state: "error", value: null, age_ms: null })).toBe("error");
});
