import { decodeRunPreview } from "./runPreview";
import { decodeRingFrame, type RingStatus } from "./ringPlayback";
import type { SsdBrief, SsdRuns, SsdStatus } from "./ssdView";
import type { StartupPreference } from './startupPreference';
import type { ReviewExportRequest, ReviewExportStatus } from "./reviewExport";
// Typed client for the Tauri command layer that wraps the Rust ↔ C++ bridge
// (mib-bridge, ADR 0003). Mirrors the DTOs in src-tauri/src/lib.rs.
import {invoke} from "./transport";
import { decodeFramePacket, decimalU64 } from "./framePacket";
import { discoverCameras, type PollOptions } from "./discovery";
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
  backend_name?: string;
  endpoint_id?: string;
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

/** Z stage snapshot (#464, ADR 0013 Amendment 1; ABI 30 = no homing).
 *  `move_state` is a contract STAGE_MOVE_STATES value; positions are
 *  micrometres in the operator's frame once `zero_set`, otherwise the raw
 *  controller counter, which means nothing. */
export interface StageStatus {
  valid: boolean;
  enabled: boolean;
  connected: boolean;
  /** Controller matches the stage profile; otherwise motion is refused. */
  configured: boolean;
  /** The operator set zero since the controller powered up; moves need it. */
  zero_set: boolean;
  /** ... and declared the stage was at mid-travel (widens the envelope). */
  mid_travel_declared: boolean;
  /** Power-up token off (hardware-acceptance mode): a power cycle is NOT detected. Show a warning. */
  session_only_zero: boolean;
  /** Supervised limit-switch check passed for this controller. A badge only. */
  limits_verified: boolean;
  /** A move is queued or running. */
  busy: boolean;
  model: string;
  serial: string;
  firmware: string;
  port_name: string;
  move_state: number;
  position_um: number;
  limit_positive: boolean;
  limit_negative: boolean;
  home: boolean;
  emergency_stop: boolean;
  driver_alarm: boolean;
  span_um: number;
  /** Allowed travel around the zero (0/0 until zero is set). */
  envelope_min_um: number;
  envelope_max_um: number;
  last_error: string;
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
  port_name?: string;
  configured_flow_rate: number;
  flow_rate_unit: number;
  direction: number;
  /** Contract PUMP_MODELS value (v22). */
  model?: number;
  /** Peristaltic flow calibration, µL per head revolution (v22). */
  microliters_per_rev?: number;
  /** Peristaltic head speed setpoint, rpm (v22). */
  speed_rpm?: number;
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

export interface ReviewChartSnapshot {
  valid:boolean;error?:string;source_path:string;pixel_to_micron:number;
  rows:string;finite_points:string;excluded_nonfinite:string;histogram_samples:string;
  area_range:[number,number];deform_range:[number,number];ring_range:[number,number];
  resolution:number;density:[number,number,string][];histogram:string[];
  curves:{modulus:number;points:[number,number][]}[];curve_source:string;
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

/** Camera/framegrabber lists the UI renders, projected from a discovery
 *  snapshot (schema v14): `valid` is false for a cancelled/failed job and
 *  `complete` mirrors the backend's identity-coverage flag. */
export interface CameraDiscovery {
  valid: boolean;
  complete: boolean;
  job_id: string;
  cameras: DiscoveredCamera[];
  framegrabbers: DiscoveredFramegrabber[];
}

/** Central profile registry (bridge schema v25, #398). Integers are contract
 *  values: `session` REGISTRY_SESSION_STATES, `connectivity`
 *  REGISTRY_CONNECTIVITY, job `kind`/`state` REGISTRY_JOB_KINDS /
 *  REGISTRY_JOB_STATES, `central_state` REGISTRY_CENTRAL_STATES. u64 values
 *  arrive as decimal strings. No token or password is ever part of these. */
export interface RegistryJob {
  job_id: string;
  kind: number;
  state: number;
  message: string;
}

export interface RegistryProject {
  project_id: string;
  display_name: string;
  roles: string[];
}

export interface RegistryRevision {
  revision_id: string;
  method_id: string;
  project_id: string;
  display_name: string;
  author_id: string;
  content_hash: string;
  revision_number: string;
  metadata_version: string;
  central_state: number;
  /** #398 M2b: "" = not materialized; `local_validation` is
   *  REGISTRY_LOCAL_VALIDATION for this instrument + current method context. */
  materialized_dir: string;
  local_validation: number;
  validated_by: string;
  validated_at_utc: string;
  /** #398 M3b: lineage, author's notes, newer published revision ("" = none). */
  parent_revision_id: string;
  release_notes: string;
  newer_revision_id: string;
}

/** #398 M3b authoring mirrors. Drafts are local until submitted. */
export interface RegistryDraft {
  draft_id: string;
  project_id: string;
  method_id: string;
  new_method: boolean;
  method_display_name: string;
  base_revision_id: string;
  release_notes: string;
  submitted_revision_id: string;
  updated_at_utc: string;
}

export interface RegistryMethod {
  method_id: string;
  project_id: string;
  display_name: string;
  head_revision_id: string;
}

export interface RegistryHistoryEntry {
  who: string;
  what: string;
  reason: string;
  created_at: string;
  review: boolean;
}

export interface RegistryConflict {
  present: boolean;
  draft_id: string;
  base_revision_id: string;
  head_revision_id: string;
  compared: boolean;
  upstream_changes: string[];
  draft_vs_head: string[];
}

/** #398 M2c Apply preview (ok false: why not) and outcome. */
export interface MethodApplyPlan {
  ok: boolean;
  error: string;
  revision_id: string;
  display_name: string;
  revision_number: string;
  central_state: string;
  changed_keys: string[];
  camera_script_path: string;
}

export interface MethodApplyResult {
  ok: boolean;
  error: string;
  applied: string[];
  not_applied: string[];
}

/** Authoring command outcome: job_id "0" = refused, `error` why. */
export interface RegistryCommand {
  job_id: string;
  error: string;
}

/** "Mark validated" outcome (#398 M2b): job_id "0" = refused, `error` why. */
export interface RegistryValidationRequest {
  job_id: string;
  error: string;
}

export interface RegistrySnapshot {
  valid: boolean;
  configured: boolean;
  generation: string;
  origin: string;
  session: number;
  subject_id: string;
  email: string;
  connectivity: number;
  health_message: string;
  successful_requests: string;
  failed_requests: string;
  rejected_revisions: string;
  projects: RegistryProject[];
  revisions: RegistryRevision[];
  corrupt_revision_ids: string[];
  cache_error: string;
  has_last_successful_refresh: boolean;
  last_successful_refresh_unix_ms: number;
  last_job: RegistryJob;
  queued_jobs: string;
  busy: boolean;
  instrument_id: string;
  instrument_name: string;
  drafts: RegistryDraft[];
  methods: RegistryMethod[];
  history_revision_id: string;
  history: RegistryHistoryEntry[];
  submit_conflict: RegistryConflict;
}

/** Device-discovery request (schema v14, #419). Kinds are
 *  DISCOVERY_DEVICE_KINDS; a pulse-generator scan needs an explicit serial
 *  scope (the backend refuses broad sweeps). */
export interface DiscoveryRequest {
  kinds: number[];
  providers?: string[];
  hasSerialScope?: boolean;
  serialPortName?: string;
  baudRate?: number;
  dataBits?: number;
  parity?: string;
  stopBits?: number;
  addressFrom?: number;
  addressTo?: number;
  perAddressTimeoutMs?: number;
  initialDelayMs?: number;
  deadlineMs?: number;
  maxRetries?: number;
  retryDelayMs?: number;
  origin?: string;
}

export interface DiscoveryStart {
  accepted: boolean;
  coalesced: boolean;
  /** u64 as a decimal string on the wire. */
  job_id: string;
  /** DISCOVERY_ERROR_KINDS value when not accepted. */
  rejection: number;
  reason: string;
}

/** One discovered device (schema v14). `kind` is DISCOVERY_DEVICE_KINDS,
 *  `identity_strength` DISCOVERY_IDENTITY_STRENGTHS, `identification`
 *  DISCOVERY_IDENTIFICATION_STATUSES; `camera_type` keeps CAMERA_TYPES
 *  (-1 when not a camera). SDK indices are session-local, never identity. */
export interface DiscoveredDevice {
  kind: number;
  provider_id: string;
  display_name: string;
  system_path: string;
  persistent_id: string;
  sdk_index: number;
  interface_index: number;
  device_index: number;
  stream_index: number;
  bus_address: number;
  stable_identity: string;
  identity_strength: number;
  identification: number;
  claimed_by: string[];
  capabilities: string[];
  synthetic: boolean;
  camera_type: number;
  interface_id: string;
  device_id: string;
  stream_id: string;
  model_name: string;
  firmware_version: string;
  label: string;
}

export interface DiscoveryError {
  provider_id: string;
  kind: number;
  message: string;
  endpoint: string;
}

/** Bounded discovery snapshot (schema v14). `state` is DISCOVERY_JOB_STATES. */
export interface DiscoverySnapshot {
  valid: boolean;
  job_id: string;
  generation: string;
  state: number;
  complete: boolean;
  overflow: boolean;
  attempt: number;
  max_attempts: number;
  kinds: number[];
  candidates: DiscoveredDevice[];
  errors: DiscoveryError[];
  providers_run: string[];
  origin: string;
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
  pixel_to_micron?: number; // exact analysis-time factor; absent/0 = unknown
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

/** Where the science runs (ABI 21). `host_processing` false = the PL processes every frame
 *  and the host pipeline's controls do not apply. */
export interface PlatformInfo {
  science: "host" | "pl";
  host_processing: boolean;
  aravis: boolean;
  /** Which surfaces exist on this instrument (#501). Absent from older servers. */
  capabilities?: PlatformCapabilities;
}

/** #501: the backend says what the instrument has; the UI hides the rest. */
export interface PlatformCapabilities {
  instrument: "desktop" | "pz7035";
  autofocus: boolean;
  trigger: boolean;
  host_background: boolean;
  frame_buffer: boolean;
  reanalysis: boolean;
  core_updates: boolean;
  egrabber_script: boolean;
  pl_identity: boolean;
  led_strobe: boolean;
  align_mode: boolean;
  run_mode: boolean;
  /** Raw LED limits per mode (µs) for Service mode (ABI 27); present with the camera modes. */
  led_limits?: { run: LedLimits; align: LedLimits };
  /** The instrument's pumps: one RS485 port, a Modbus address per slot. */
  pump: null | { model: string; port: string; sample_address: number; sheath_address: number; microliters_per_rev: number };
}

export type IdMatch = "match" | "mismatch" | "unknown";

/** PZ7035 identity and health (#501, `fetch_instrument_status`). Read-only. */
export interface LedLimits { delay_min_us: number; delay_max_us: number; width_min_us: number; width_max_us: number }

/** Camera mode the backend applied last (ABI 27, #501 P1). */
export interface InstrumentModeState {
  name: "align" | "run" | "unknown"; run_x: number; run_y: number; service: boolean;
  /** True once a Run switch succeeded in the backend process: run_x/run_y are the operator's window, not the (0, 0) default. */
  run_set?: boolean;
  /** True after the unattended safe state (no client for the grace time): LED off, cell path off, camera released. Cleared by the next mode switch. */
  idle?: boolean;
  /** How often Align's ingress recovery fired in this process (#629). */
  align_lock?: {receiver_clears: number; failures: number; last_stuck_p13?: number; pl_gave_ups?: number; last_pl_gave_up_ms?: number};
  /** Align live view: whole frames from the PL bridge (results8 on) or the producer's bands. */
  align_source?: "bridge" | "bands" | "";
}

/** Where recordings land (#501): `ram` on today's JTAG RAM root; `warning` is the operator text,
 *  "" once the target is persistent (SATA). */
export interface RecordingTargetState { path: string; writable: boolean; ram: boolean; free_bytes: number; filesystem: string; warning: string }

/** The PL result stream while an experiment runs (#501): the provider's cumulative counters. */
export interface ResultsStreamState {
  available: boolean;
  running?: boolean;
  frames?: number;
  results?: number;
  empty_frames?: number;
  invalid_frames?: number;
  truncated_frames?: number;
  incomplete_frames?: number;
  decode_errors?: number;
  sequence_gaps?: number;
  overruns?: number;
  last_error?: string;
}

/** The reconciled accounting of a run (ABI 31, `fetch_run_accounting`, #549). `source` "review" is the
 *  file loaded for review, "last_run" the run that finished last in this session. */
export interface RunAccounting {
  available: boolean;
  error?: string;
  source: string;
  /** false for a raw recording or a legacy file without accounting (review only). */
  recorded?: boolean;
  start_generation?: number;
  /** PL receiver auto-resets during the run (results9 self-heal; each loses frames); null on an image without it. */
  receiver_auto_resets?: number | null;
  /** Frames the PL dropped because no FrameStart was seen during the run (results12): lost data; null on an image without the counters. */
  nofs_frames?: number | null;
  file_path?: string;
  /** Contract experiment_completion value. */
  completion?: number;
  completion_name?: string;
  completion_reason?: string;
  reconciled?: boolean;
  /** Frames the run claimed: the true denominator of the loss fractions. */
  admitted?: number;
  empty?: number;
  processed?: number;
  scientifically_rejected?: number;
  processing_failed?: number;
  store_overwritten?: number;
  store_not_committed?: number;
  store_malformed?: number;
  cancelled_by_policy?: number;
  pending_at_stop?: number;
  sequence_gaps?: number;
  persistence_admitted?: number;
  persistence_committed?: number;
  persistence_failed?: number;
  fatal_error?: boolean;
  fatal_message?: string;
  malformed_warn_fraction?: number;
}

export interface InstrumentStatus {
  available: boolean;
  results?: ResultsStreamState;
  mode?: InstrumentModeState;
  storage?: RecordingTargetState;
  /** The every-frame ring (ABI 34): capacity, the buffered range and whether playback is offered. */
  ring?: RingStatus;
  /** The SATA SSD record store in brief (ABI 35, #667 S1); `fetchSsdStatus` has the rest. */
  ssd?: SsdBrief;
  /** Where saved files' wall-clock times come from (G14): the board has no RTC. */
  wall_clock?: { synced: boolean; source: "client_sync" | "board_clock_unsynced" | "system_clock"; offset_ns: number; last_sync_unix_ms: number };
  error?: string;
  pinned_profile_id?: string;
  core?: {
    build_id: string;
    profile_id: string;
    abi_version: number;
    science_profile: number;
    profile_version: number;
    expected: null | { build_id: string; profile_id: string; commit: string; image: string; abi_major: number; abi_minor: number };
    pinned_profile_id: string;
    build_match: IdMatch;
    profile_match: IdMatch;
  };
  led?: { on: boolean; preset: "run" | "align" | "custom" | "off"; delay_us: number; width_us: number; guard_fault: boolean; guard_trips: number };
  link?: {
    rates_valid: boolean;
    ingress_errors_per_s: number;
    /** Ingress errors averaged over the last 5 s, and the warning from it (above ingress_errors_warn_per_s). */
    ingress_errors_avg_per_s?: number;
    ingress_errors_warn?: boolean;
    resyncs_per_s: number;
    bad_frames_per_s: number;
    dropped_per_s: number;
    ingress_errors_warn_per_s: number;
    resyncs_warn_per_s: number;
    /** Sustained loss against the sensor's frame rate (bad above 1%, dropped above 0.1% for 5 s). */
    bad_frames_warn?: boolean;
    dropped_warn?: boolean;
    /** The results bridge is ARMED/RUNNING; dropped frames are judged only then (P[6] counts every frame otherwise). */
    bridge_active?: boolean;
    /** The PL receiver self-heal block (results9): present, gave up after its tries, auto-resets since the PL reset. */
    rx_heal?: {present: boolean; gave_up: boolean; tries: number; auto_resets: number; v2?: boolean; fs_present?: boolean; fs_seen?: number; nofs_frames?: number; nofs_lines?: number; flag_clears?: number; episodes?: number; failed_episodes?: number};
    bad_frames_warn_per_s?: number;
    dropped_warn_per_s?: number;
  };
  /** The sensor as the PL sees it: XVS period in 100 MHz clocks (0 while the sensor is closed), the
   *  frame rate it gives, and the ingress geometry latched at the last receiver reset. */
  sensor?: { xvs_period_clocks: number; fps: number; width: number; height: number };
  latency?: { last_us: number; max_us: number; over_budget: number; frames: number };
}

/** Camera & Alignment geometry (ABI 20, `fetch_camera_geometry`). Sensor coordinates. */
export interface CameraGeometry {
  supported: boolean;
  overview: boolean;
  camera: string;
  sensor_width: number;
  sensor_height: number;
  roi: {x: number; y: number; width: number; height: number};
  width_increment: number;
  height_increment: number;
  offset_x_increment: number;
  offset_y_increment: number;
  min_width: number;
  min_height: number;
  /** Last camera read-back (Aravis): applied window, sensor rate and the delivered rate. */
  session: {
    overview?: boolean;
    region?: {x: number; y: number; width: number; height: number};
    frame_rate_hz?: number;
    frame_rate_max_hz?: number;
    frame_rate_clamped?: boolean;
    frame_rate_limit?: string;
    exposure_us?: number;
    band_count?: number;
    delivered_frame_rate_hz?: number;
    delivered_limit?: string;
    preview_rate_hz?: number;
  };
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
  closeReview: () => sourceMutation("close_review"),
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
  fetchCaptureLifecycle: () => invoke<CaptureLifecycle>("fetch_capture_lifecycle"),
  experimentAcknowledgeFault: (expectedRun: string, faultRevision: string, code: string, message: string, confirmed: boolean) => invokeCommand("experiment_acknowledge_fault", {expectedRun, faultRevision, code, message, confirmed}),
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
  setProcessedPreviewEnabled: (enabled: boolean) => invoke<void>("set_processed_preview_enabled", {enabled}),
  fetchProcessedPreview: () => invoke<ArrayBuffer>("fetch_processed_preview"),
  backgroundCalibrationCommand: (request: Record<string, unknown>) => invokeCommand("background_calibration_command", {json: JSON.stringify(request)}),
  backgroundCalibrationStatus: () => invoke<BackgroundCalibrationStatus>("background_calibration_status"),
  startupDiscoverySetPreference: (preference: StartupPreference) => invoke<{accepted: boolean; message: string; preference?: StartupPreference}>("startup_discovery_set_preference", {json: JSON.stringify(preference)}),
  startupDiscoveryRun: (action: string) => invoke<{accepted: boolean; message: string}>("startup_discovery_run", {action}),
  startupDiscoveryStatus: () => invoke<StartupDiscoveryStatus>("startup_discovery_status"),
  pulseGeneratorCommand: (request: Record<string, unknown>) => invokeCommand("pulse_generator_command", {json: JSON.stringify(request)}),
  pulseGeneratorStatus: () => invoke<PulseGeneratorStatus>("pulse_generator_status"),
  autofocusConnectEndpoint: (backend: string, endpoint: string, comPort: number, baudRate: number, deviceAddress: number) =>
    invokeCommand("autofocus_connect_endpoint", { backend, endpoint, comPort, baudRate, deviceAddress }),
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
  pumpConnectEndpoint: (pump: number, portName: string, baudRate: number, modbusAddress: number) => invokeCommand("pump_connect_endpoint", {pump, portName, baudRate, modbusAddress}),
  // v22: either pump model in either slot; microlitersPerRev calibrates peristaltic flow.
  pumpConnectModel: (pump: number, model: number, portName: string, baudRate: number, modbusAddress: number, microlitersPerRev: number) =>
    invokeCommand("pump_connect_model", { pump, model, portName, baudRate, modbusAddress, microlitersPerRev }),
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
  // Z stage (#464, ADR 0013 Amendment 1). The stage is never homed. The backend
  // refuses moves before stageSetZero and outside the travel envelope around it;
  // stageStop is always accepted.
  stageConnect: (portName = "", usbSerial = "", modbusAddress = 0) =>
    invokeCommand("stage_connect", { portName, usbSerial, modbusAddress }),
  stageDisconnect: () => invokeCommand("stage_disconnect"),
  stageMoveTo: (targetUm: number) => invokeCommand("stage_move_to", { targetUm }),
  stageMoveBy: (deltaUm: number) => invokeCommand("stage_move_by", { deltaUm }),
  stageSetZero: (midTravel = false) => invokeCommand("stage_set_zero", { midTravel }),
  stageStop: () => invokeCommand("stage_stop"),
  stageApplyProfile: () => invokeCommand("stage_apply_profile"),
  fetchStageStatus: () => invoke<StageStatus>("fetch_stage_status"),
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
  renderReviewOverlay: async (request:{source_path:string;valid:boolean;index:number;mode:number;roi:boolean}) => new Uint8Array(await invoke<ArrayBuffer>("render_review_overlay",{json:JSON.stringify(request)})),
  fetchReanalysisPreview: (request:{source_kind:string;source_path:string;dataset:string;index:number}) => pullFrame("fetch_review_reanalysis_preview",3,{json:JSON.stringify(request)}),
  fetchReviewCharts: async (): Promise<ReviewChartSnapshot> => JSON.parse(await invoke<string>("fetch_review_charts_json")),
  reviewReanalysis: (request: {source_path:string;output_path:string;dataset:string;start:number;count:number;source_kind?:"hdf"|"folder"|"avi";synthetic_background?:boolean;roi?:{x:number;y:number;w:number;h:number};image_processing?:unknown;max_frames?:number;max_input_mib?:number;background_index?:number;background_dataset?:string;clear_background?:boolean}) => invokeCommand("review_reanalysis_json", {json:JSON.stringify(request)}),
  reviewReanalysisStatus: async (): Promise<ReviewExportStatus> => JSON.parse(await invoke<string>("review_reanalysis_status_json")),
  reviewExport: (request: ReviewExportRequest) => invokeCommand("review_export_json", { json: JSON.stringify(request) }),
  reviewExportStatus: async (): Promise<ReviewExportStatus> => JSON.parse(await invoke<string>("review_export_status_json")),
  reviewExportCsv: (outputPath: string) =>
    invokeCommand("review_export_csv", { outputPath }),
  // Processing config / ROI / background / core identity (schema v8, BE-3).
  fetchProcessingConfigJson: () =>
    invoke<{ valid: boolean; json: string }>("fetch_processing_config_json"),
  applyProcessingConfigJson: (json: string) =>
    sourceMutation("apply_processing_config_json", { json }),
  setProcessingRoi: (x: number, y: number, w: number, h: number) =>
    sourceMutation("set_processing_roi", { x, y, w, h }),
  // Camera & Alignment (ABI 20): the overview changes the frame geometry, so it is a source
  // mutation; saving the window only persists it for the next experiment-mode start.
  setCameraOverview: (overview: boolean) => sourceMutation("set_camera_overview", {overview}),
  saveCameraRoi: (x: number, y: number, w: number, h: number) => invokeCommand("save_camera_roi", {x, y, w, h}),
  fetchCameraGeometry: () => invoke<CameraGeometry>("fetch_camera_geometry"),
  fetchPlatformInfo: () => invoke<PlatformInfo>("fetch_platform_info"),
  fetchRunAccounting: (source: "review" | "last_run") => invoke<RunAccounting>("fetch_run_accounting", { source }),
  fetchInstrumentStatus: () => invoke<InstrumentStatus>("fetch_instrument_status"),
  // PZ7035 camera modes (ABI 27, #501 P1). A mode switch restarts or stops the camera, so it
  // is a source mutation like the overview switch.
  setInstrumentMode: (mode: "align" | "run", x = 0, y = 0) => sourceMutation("set_instrument_mode", {mode, x, y}),
  setServiceMode: (on: boolean) => invokeCommand("set_service_mode", {on}),
  setInstrumentLed: (delayUs: number, widthUs: number) => invokeCommand("set_instrument_led", {delayUs, widthUs}),
  fetchRunPreview: async () => decodeRunPreview(await invoke<ArrayBuffer>("fetch_run_preview")),
  // The every-frame ring (ABI 34, #649 v1): Stop in Run holds it for playback, Resume re-arms it (a new ring).
  fetchRingStatus: () => invoke<RingStatus>("fetch_ring_status"),
  fetchRingFrame: async (seq: number) => decodeRingFrame(await invoke<ArrayBuffer>("fetch_ring_frame", { seq })),
  ringFreeze: () => invokeCommand("ring_freeze"),
  ringResume: () => invokeCommand("ring_resume"),
  // The SATA SSD record store, read only (ABI 35, #667 S1).
  fetchSsdStatus: () => invoke<SsdStatus>("fetch_ssd_status"),
  fetchSsdRuns: () => invoke<SsdRuns>("fetch_ssd_runs"),
  fetchBackground: () => pullFrame("fetch_background_packet", 4),
  setBackgroundFromCurrentFrame: () =>
    sourceMutation("set_background_from_current_frame"),
  clearBackgroundImage: () => sourceMutation("clear_background_image"),
  fetchProcessingCoreStatus: () =>
    invoke<ProcessingCoreStatus>("fetch_processing_core_status"),
  // Device discovery jobs (bridge schema v14, #419): start / poll / cancel.
  startDeviceDiscovery: (request: DiscoveryRequest) =>
    invoke<DiscoveryStart>("start_device_discovery", { request }),
  startCameraDiscovery: () => invoke<DiscoveryStart>("start_camera_discovery"),
  fetchDeviceDiscovery: (jobId: string) =>
    invoke<DiscoverySnapshot>("fetch_device_discovery", { jobId: decimalU64(jobId) }),
  cancelDeviceDiscovery: (jobId: string) =>
    invoke<boolean>("cancel_device_discovery", { jobId: decimalU64(jobId) }),
  /** Camera + framegrabber discovery as one awaited job (replaces the
   *  synchronous fetch_camera_discovery of schema v7). */
  discoverCameras: (opts?: PollOptions) =>
    discoverCameras(
      {
        start: () => invoke<DiscoveryStart>("start_camera_discovery"),
        fetch: (jobId) => invoke<DiscoverySnapshot>("fetch_device_discovery", { jobId }),
        cancel: (jobId) => invoke<boolean>("cancel_device_discovery", { jobId }),
      },
      opts,
    ),
  // Central profile registry (bridge schema v25, #398). Commands return a job
  // ID as a decimal string; "0" means refused (not configured / not ready).
  registrySignIn: (email: string, password: string) =>
    invoke<string>("registry_sign_in", { email, password }),
  registrySignOut: () => invoke<string>("registry_sign_out"),
  registryRefresh: () => invoke<string>("registry_refresh"),
  registryDownload: (revisionId: string) => invoke<string>("registry_download", { revisionId }),
  registryCancelAll: () => invoke<boolean>("registry_cancel_all"),
  registryMaterialize: (revisionId: string) => invoke<string>("registry_materialize", { revisionId }),
  registryRecordValidation: (revisionId: string, evidenceFile: string, passed: boolean) =>
    invoke<RegistryValidationRequest>("registry_record_validation", { revisionId, evidenceFile, passed }),
  // #398 M3b authoring.
  registryNewDraftFromRevision: (revisionId: string, useCurrentConfig: boolean) =>
    invoke<RegistryCommand>("registry_new_draft_from_revision", { revisionId, useCurrentConfig }),
  registryNewMethodDraft: (projectId: string, name: string, releaseNotes: string) =>
    invoke<RegistryCommand>("registry_new_method_draft", { projectId, name, releaseNotes }),
  registrySetDraftNotes: (draftId: string, notes: string) =>
    invoke<RegistryCommand>("registry_set_draft_notes", { draftId, notes }),
  registryDraftFromHead: (draftId: string, keepDraftConfig: boolean) =>
    invoke<RegistryCommand>("registry_draft_from_head", { draftId, keepDraftConfig }),
  registrySubmitDraft: (draftId: string, asBranch: boolean) =>
    invoke<RegistryCommand>("registry_submit_draft", { draftId, asBranch }),
  registryDeleteDraft: (draftId: string) => invoke<RegistryCommand>("registry_delete_draft", { draftId }),
  registryTransition: (revisionId: string, target: number, reason: string) =>
    invoke<RegistryCommand>("registry_transition", { revisionId, target, reason }),
  registryFetchHistory: (revisionId: string) => invoke<RegistryCommand>("registry_fetch_history", { revisionId }),
  registryPlanApply: (revisionId: string) => invoke<MethodApplyPlan>("registry_plan_apply", { revisionId }),
  registryApplyMethod: (revisionId: string) => invoke<MethodApplyResult>("registry_apply_method", { revisionId }),
  fetchRegistrySnapshot: () => invoke<RegistrySnapshot>("fetch_registry_snapshot"),
  fetchRegistryJob: (jobId: string) =>
    invoke<RegistryJob>("fetch_registry_job", { jobId: decimalU64(jobId) }),
  // Camera selection (bridge schema v7, BE-2).
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

export interface PulseGeneratorStatus {valid: boolean; connected: boolean; owned: boolean; error: string; port: string; baud: number; address: number; channels: Array<{frequency_hz: number; duty_percent: number; output_enabled: boolean}>}

export interface StartupDiscoveryStatus {preference?: StartupPreference; valid: boolean; camera_running: boolean; nanopositioner_running: boolean; camera_configured: boolean; nanopositioner_connected: boolean; camera: StartupJob; nanopositioner: StartupJob}
interface StartupJob {job_id: string; state?: number; complete?: boolean; candidate_count?: number; errors: string[]}

export interface BackgroundCalibrationStatus {valid: boolean; state: string; operation_generation: string; frozen_config_version: string; attempted: number; accepted: number; rejected_non_empty: number; rejected_processing_failed: number; published_background_generation: string; published_sha256: string; message: string}

export interface CaptureLifecycle {valid: boolean; state: string; generation: string; camera_ready: boolean; failure: string; message: string; failure_generation: string}
