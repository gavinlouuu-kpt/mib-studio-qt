// Typed client for the Tauri command layer that wraps the Rust ↔ C++ bridge
// (mib-bridge, ADR 0003). Mirrors the DTOs in src-tauri/src/lib.rs.
import { invoke } from "@tauri-apps/api/core";
import { decodeFramePacket, decimalU64 } from "./framePacket";
export type { FrameMeta, FramePacket } from "./framePacket";
import { decodeEvents, decodeCommandResult, decodeExperimentReadiness, decodeExperimentStatus, decodeProcessingStats, wireU64, type CmdResult } from "./eventAdapter";
export type { BridgeEvent, CmdResult, ExperimentReadiness, ExperimentStatus, ProcessingStats, ReadinessGate } from "./eventAdapter";

/** Autofocus/nanopositioner status (schema v11, BE-8). `ring_ratio_age_us`
 *  makes focus-metric staleness explicit (0 = never updated). */
export interface AutofocusStatus {
  valid: boolean;
  connected: boolean;
  enabled: boolean;
  current_voltage: number;
  com_port: number;
  average_ring_ratio: number;
  median_ring_ratio: number;
  last_ring_ratio_update_us: number;
  ring_ratio_age_us: number;
}

/** Autofocus configuration (schema v11, BE-8) — plain values end to end. */
export interface AutofocusConfig {
  valid: boolean;
  focus_setpoint: number;
  focus_range: number;
  voltage_step: number;
  fine_voltage_step: number;
  max_voltage: number;
  min_voltage: number;
  initial_voltage: number;
  manual_voltage_step: number;
  ring_ratio_stale_ms: number;
  require_new_sample_per_step: boolean;
  min_samples_per_step: number;
  safe_shutdown_voltage: number;
  focus_direction: boolean;
}

/** Authoritative per-pump snapshot (schema v10, BE-7). `run_status` /
 *  `direction` are contract PUMP_RUN_STATES / PUMP_DIRECTIONS values. */
export interface PumpStatus {
  valid: boolean;
  connected: boolean;
  run_status: number;
  current_flow_rate: number;
  accumulated_volume: number;
  min_flow_rate: number;
  max_flow_rate: number;
  stalled: boolean;
  com_port: number;
  baud_rate: number;
  modbus_address: number;
  configured_flow_rate: number;
  flow_rate_unit: number;
  direction: number;
}

/** Per-dataset capabilities of the loaded review file (schema v9, BE-6). */
export interface ReviewDatasetInfo {
  present: boolean;
  count: number;
  height: number;
  width: number;
  channels: number;
}

/** Review metadata of the loaded HDF5 file (schema v9, BE-6). */
export interface ReviewMetadata {
  accounting_available: boolean;
  completion_state: number;
  completion_reason: string;
  accounting_reconciled: boolean;
  valid: boolean;
  file_open: boolean;
  recording_file: boolean;
  start_time_ns: number;
  end_time_ns: number;
  total_valid: number;
  total_invalid: number;
  roi_x: number;
  roi_y: number;
  roi_w: number;
  roi_h: number;
  has_background: boolean;
  has_core_identity: boolean;
  core_version: string;
  core_source: string;
  core_release_tag: string;
  valid_images: ReviewDatasetInfo;
  invalid_images: ReviewDatasetInfo;
  valid_masks: ReviewDatasetInfo;
  invalid_masks: ReviewDatasetInfo;
  recorded_images: ReviewDatasetInfo;
  file_path: string;
}

/** One page of review metrics (schema v9, BE-6). */
export interface ReviewMetricsPage {
  valid: boolean;
  total: number;
  offset: number;
  rows: MonitoringRow[];
}

/** Processing-core identity/pin status (bridge schema v8, BE-3). */
export interface ProcessingCoreStatus {
  valid: boolean;
  active_version: string;
  contract_version: number;
  engine_abi_version: number;
  source: string;
  release_tag: string;
  build_id: string;
  artifact_sha256: string;
  required_version: string;
  pin_satisfied: boolean;
}

/** One discovered camera (bridge schema v7, BE-2). `camera_type` is a
 *  contract CAMERA_TYPES value (0 EGrabber, 1 MindVision, 2 Mock). */
export interface DiscoveredCamera {
  camera_type: number;
  camera_index: number;
  interface_index: number;
  device_index: number;
  interface_id: string;
  device_id: string;
  model_name: string;
  firmware_version: string;
  label: string;
}

export interface DiscoveredFramegrabber {
  interface_index: number;
  device_index: number;
  stream_index: number;
  interface_id: string;
  device_id: string;
  stream_id: string;
  model_name: string;
  label: string;
}

export interface CameraDiscovery {
  valid: boolean;
  cameras: DiscoveredCamera[];
  framegrabbers: DiscoveredFramegrabber[];
}

/** Authoritative selected-device snapshot (bridge schema v7, BE-2). `mode`
 *  is a contract CAMERA_SELECTION_MODES value. */
export interface CameraSelection {
  valid: boolean;
  mode: number;
  interface_index: number;
  device_index: number;
  label: string;
  mindvision_index: number;
  mindvision_config_path: string;
  camera_script_path: string;
  mock_frame_dir: string;
  mock_interval_ms: number;
  mock_loop: boolean;
  configured: boolean;
  running: boolean;
}

/** One monitoring metric row (bridge schema v6, BE-5). `(frame_index,
 *  object_id)` is a stable identity for reconciliation. */
export interface MonitoringRow {
  frame_index: number;
  timestamp_ns: number;
  valid: boolean;
  target_group: boolean;
  object_id: number;
  object_count: number;
  track_id: number;
  centroid_x: number;
  centroid_y: number;
  area: number;
  deformability: number;
  area_ratio: number;
  ring_ratio: number;
  youngs_modulus: number;
}

/** Bounded monitoring snapshot (bridge schema v6, BE-5). Evicted rows are
 *  observable as `*_appended - *_held`. */
export interface MonitoringSnapshot {
  valid: boolean;
  monitoring_active: boolean;
  valid_held: number;
  invalid_held: number;
  valid_appended: number;
  invalid_appended: number;
  capacity: number;
  latest_timestamp_ns: number;
  rows: MonitoringRow[];
}

/** Sorter trigger status snapshot (bridge schema v6, BE-5). */
export interface TriggerStatus {
  valid: boolean;
  camera_attached: boolean;
  pulse_duration_us: number;
  trigger_count: number;
  last_onset_us: number;
  last_object_id: number;
  last_track_id: number;
  periodic_active: boolean;
  periodic_interval_ms: number;
}

async function invokeCommand(command: string, args?: Record<string, unknown>): Promise<CmdResult> {
  return decodeCommandResult(await invoke<unknown>(command,args));
}

// Presentation generation guards only; authoritative session identity is still
// an Agent A dependency. Local source mutations make older replies unusable.
let frameGeneration = 0;
let sourceMutations = 0;
async function sourceMutation(command: string, args?: Record<string, unknown>): Promise<CmdResult> {
  frameGeneration++; sourceMutations++;
  try { return await invokeCommand(command, args); }
  finally { sourceMutations--; frameGeneration++; }
}
async function pullFrame(command: string, kind: number, args?: Record<string, unknown>) {
  if (sourceMutations) throw new Error("FRAME_SOURCE_CHANGING");
  const generation = frameGeneration;
  const buffer = await invoke<ArrayBuffer>(command, args);
  if (generation !== frameGeneration) throw new Error("FRAME_REPLY_STALE");
  return decodeFramePacket(buffer, kind);
}

export const bridge = {
  applicationMode: () => invoke<"instrument" | "analysis-only">("application_mode"),
  abiVersion: () => invoke<number>("abi_version"),
  isInitialized: () => invoke<boolean>("is_initialized"),
  init: (dataDir: string) => invoke<boolean>("init", { dataDir }),
  configureMock: (frameDir: string, frameIntervalMs: number, loopFiles: boolean) =>
    sourceMutation("configure_mock", { frameDir, frameIntervalMs, loopFiles }),
  startCapture: () => sourceMutation("start_capture"),
  stopCapture: () => sourceMutation("stop_capture"),
  seekLatest: () => invokeCommand("seek_latest"),
  pollEvents: async () => decodeEvents(await invoke<unknown>("poll_events_exact")),
  fetchFrame: () => pullFrame("fetch_frame_packet", 1),
  // Recording + review (bridge schema v2).
  startRecording: (filePath: string) => invokeCommand("start_recording", { filePath }),
  stopRecording: () => invokeCommand("stop_recording"),
  loadRecording: (filePath: string) => sourceMutation("load_recording", { filePath }),
  seekIndex: (frameIndex: number | string | bigint) => invokeCommand("seek_index", { frameIndex: decimalU64(frameIndex) }),
  fetchFrameByIndex: async (frameIndex: number | string | bigint) =>
    pullFrame("fetch_indexed_frame_packet", 2, { frameIndex: decimalU64(frameIndex) }),
  // Processing (bridge schema v3).
  applyProcessing: (realtimeEnabled: boolean, pixelToMicron: number) =>
    sourceMutation("apply_processing", { realtimeEnabled, pixelToMicron }),
  fetchProcessingStats: async () => decodeProcessingStats(await invoke<unknown>("fetch_processing_stats")),
  // Operation state + bounded-queue observability (bridge schema v4, BE-1).
  cancelOperation: (operationId: number | string | bigint) =>
    invokeCommand("cancel_operation", { operationId: decimalU64(operationId) }),
  queueOverflowTotal: async () => wireU64(await invoke<unknown>("queue_overflow_total")),
  // Experiment lifecycle (bridge schema v5, BE-4) — the backend owns
  // preconditions, accumulation, flush, metadata ordering, and recovery.
  experimentStart: (outputPath: string) =>
    invokeCommand("experiment_start", { outputPath }),
  experimentStop: () => invokeCommand("experiment_stop"),
  experimentCancel: () => invokeCommand("experiment_cancel"),
  fetchExperimentStatus: async () => decodeExperimentStatus(await invoke<unknown>("fetch_experiment_status")),
  // ABI 13: gate list + the generation a Start must present (the bridge's
  // experimentStart evaluates it itself; this is for the preflight UI).
  fetchExperimentReadiness: async (outputPath: string) =>
    decodeExperimentReadiness(await invoke<unknown>("fetch_experiment_readiness", { outputPath })),
  // Platform/shell services (BE-9): stable app paths, persisted shell
  // preferences (survive webview-storage clearing), and shell logging into
  // the app log directory.
  appPaths: () =>
    invoke<{ app_data: string; app_config: string; app_log: string; app_cache: string; documents: string }>(
      "app_paths",
    ),
  getPreferences: () => invoke<Record<string, unknown>>("get_preferences"),
  setPreferences: (preferences: Record<string, unknown>) =>
    invoke<void>("set_preferences", { preferences }),
  shellLog: (level: string, message: string) =>
    invoke<void>("shell_log", { level, message }),
  // Autofocus / nanopositioner (schema v11, BE-8).
  autofocusConnect: (comPort: number, baudRate: number, deviceAddress: number) =>
    invokeCommand("autofocus_connect", { comPort, baudRate, deviceAddress }),
  autofocusDisconnect: () => invokeCommand("autofocus_disconnect"),
  autofocusSetEnabled: (enabled: boolean) =>
    invokeCommand("autofocus_set_enabled", { enabled }),
  autofocusJog: (up: boolean) => invokeCommand("autofocus_jog", { up }),
  autofocusSetConfig: (config: AutofocusConfig) =>
    invokeCommand("autofocus_set_config", { config }),
  fetchAutofocusStatus: () => invoke<AutofocusStatus>("fetch_autofocus_status"),
  fetchAutofocusConfig: () => invoke<AutofocusConfig>("fetch_autofocus_config"),
  // Syringe pumps (schema v10, BE-7): pump 0 = Sample, 1 = Sheath.
  pumpConnect: (pump: number, comPort: number, baudRate: number, modbusAddress: number) =>
    invokeCommand("pump_connect", { pump, comPort, baudRate, modbusAddress }),
  pumpDisconnect: (pump: number) => invokeCommand("pump_disconnect", { pump }),
  pumpSetFlowRate: (pump: number, rate: number, unit: number) =>
    invokeCommand("pump_set_flow_rate", { pump, rate, unit }),
  pumpSetDirection: (pump: number, direction: number) =>
    invokeCommand("pump_set_direction", { pump, direction }),
  pumpStart: (pump: number) => invokeCommand("pump_start", { pump }),
  pumpStop: (pump: number) => invokeCommand("pump_stop", { pump }),
  pumpPurge: (pump: number, direction: number) =>
    invokeCommand("pump_purge", { pump, direction }),
  pumpStopPurge: (pump: number) => invokeCommand("pump_stop_purge", { pump }),
  pumpSetSyringeVolume: (pump: number, volume: number, unit: number) =>
    invokeCommand("pump_set_syringe_volume", { pump, volume, unit }),
  pumpPollStatus: (pump: number) => invokeCommand("pump_poll_status", { pump }),
  fetchPumpStatus: (pump: number) => invoke<PumpStatus>("fetch_pump_status", { pump }),
  pumpScanAddresses: (
    comPort: number,
    baudRate: number,
    startAddress: number,
    endAddress: number,
    timeoutMs: number,
  ) =>
    invokeCommand("pump_scan_addresses", { comPort, baudRate, startAddress, endAddress, timeoutMs }),
  // Paged HDF5 review + export jobs (schema v9, BE-6).
  fetchReviewMetadata: () => invoke<ReviewMetadata>("fetch_review_metadata"),
  fetchReviewMetricsPage: (valid: boolean, offset: number, count: number) =>
    invoke<ReviewMetricsPage>("fetch_review_metrics_page", { valid, offset, count }),
  fetchReviewImage: async (dataset: number, index: number | string | bigint) =>
    pullFrame("fetch_review_frame_packet", 3, { dataset, index: decimalU64(index) }),
  reviewExportCsv: (outputPath: string) =>
    invokeCommand("review_export_csv", { outputPath }),
  // Processing config / ROI / background / core identity (schema v8, BE-3).
  fetchProcessingConfigJson: () =>
    invoke<{ valid: boolean; json: string }>("fetch_processing_config_json"),
  applyProcessingConfigJson: (json: string) =>
    sourceMutation("apply_processing_config_json", { json }),
  setProcessingRoi: (x: number, y: number, w: number, h: number) =>
    sourceMutation("set_processing_roi", { x, y, w, h }),
  fetchBackground: () => pullFrame("fetch_background_packet", 4),
  setBackgroundFromCurrentFrame: () =>
    sourceMutation("set_background_from_current_frame"),
  clearBackgroundImage: () => sourceMutation("clear_background_image"),
  fetchProcessingCoreStatus: () =>
    invoke<ProcessingCoreStatus>("fetch_processing_core_status"),
  // Camera discovery/selection (bridge schema v7, BE-2).
  fetchCameraDiscovery: () => invoke<CameraDiscovery>("fetch_camera_discovery"),
  fetchCameraSelection: () => invoke<CameraSelection>("fetch_camera_selection"),
  selectHardwareCamera: (interfaceIndex: number, deviceIndex: number, label: string) =>
    sourceMutation("select_hardware_camera", { interfaceIndex, deviceIndex, label }),
  selectMindVisionCamera: (cameraIndex: number, label: string, configPath: string) =>
    sourceMutation("select_mindvision_camera", { cameraIndex, label, configPath }),
  applyCameraScript: (scriptPath: string) =>
    sourceMutation("apply_camera_script", { scriptPath }),
  resetHardwareCamera: () => sourceMutation("reset_hardware_camera"),
  // Monitoring + sorter trigger (bridge schema v6, BE-5). Monitoring is
  // visibility-gated: enable only while the Monitoring view is shown.
  monitoringSetActive: (active: boolean) =>
    invokeCommand("monitoring_set_active", { active }),
  monitoringClear: () => invokeCommand("monitoring_clear"),
  fetchMonitoringSnapshot: (maxRows: number) =>
    invoke<MonitoringSnapshot>("fetch_monitoring_snapshot", { maxRows }),
  triggerSetPulseDuration: (pulseUs: number) =>
    invokeCommand("trigger_set_pulse_duration", { pulseUs }),
  triggerManualPulse: () => invokeCommand("trigger_manual_pulse"),
  triggerPeriodicStart: (intervalMs: number) =>
    invokeCommand("trigger_periodic_start", { intervalMs }),
  triggerPeriodicStop: () => invokeCommand("trigger_periodic_stop"),
  fetchTriggerStatus: () => invoke<TriggerStatus>("fetch_trigger_status"),
  /** Deprecated: split-frame access cannot guarantee identity. */
  frameBytes: async (): Promise<Uint8Array> => { throw new Error("FRAME_PROTOCOL_UPGRADE_REQUIRED"); },
};

// Expand a Mono8 buffer (with row stride) into an RGBA ImageData for a canvas.
export function mono8ToImageData(
  bytes: Uint8Array,
  width: number,
  height: number,
  stride: number,
): ImageData {
  const rgba = new Uint8ClampedArray(width * height * 4);
  const rowStride = stride > 0 ? stride : width;
  for (let y = 0; y < height; y++) {
    const src = y * rowStride;
    for (let x = 0; x < width; x++) {
      const g = bytes[src + x] ?? 0;
      const d = (y * width + x) * 4;
      rgba[d] = g;
      rgba[d + 1] = g;
      rgba[d + 2] = g;
      rgba[d + 3] = 255;
    }
  }
  return new ImageData(rgba, width, height);
}
