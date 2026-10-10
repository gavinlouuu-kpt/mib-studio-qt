//! Rust <-> C++ bridge over the Qt-free `mib_backend` `BackendFacade`.
//!
//! Epic #246, ADR 0003. This crate wraps `backend::bridge::BackendFacade` behind
//! a `cxx` FFI boundary so the Tauri/Rust shell can drive the C++ backend without
//! Qt. The Rust side never sees `AppBackend`, OpenCV, or HDF5 — only flat command
//! submitters, a poll-drained event queue, and an on-demand frame pull.
//!
//! Threading: the backend emits events from its own capture/processing threads.
//! The C++ shim enqueues each event onto an internal mutex-guarded queue and
//! returns immediately (non-blocking sink, per ADR 0003); Rust drains it with
//! [`ffi::BackendBridge::poll_events`]. Frame pixels are pulled with
//! [`ffi::BackendBridge::fetch_latest_frame`] — never pushed through the event
//! channel and never base64-encoded per frame.

// The review bridge (ReviewSession, ADR 0014) is always compiled; the backend
// bridge below is not under the `review-only` feature.
pub mod review_bridge;
pub use review_bridge::review_ffi;

#[cfg(not(feature = "review-only"))]
#[cxx::bridge(namespace = "mib_bridge")]
pub mod ffi {
    /// Flattened result of a dispatched command. `command` mirrors
    /// `backend::bridge::BackendCommandType` as a small integer.
    #[derive(Debug, Clone)]
    pub struct BridgeCommandResult {
        pub ok: bool,
        pub command: u32,
        pub message: String,
        /// Non-zero when this command started (or targeted) a tracked
        /// long-running operation; correlates with `OperationStatus` events
        /// (schema v4, ADR 0004).
        pub operation_id: u64,
    }

    /// Discriminant for [`BridgeEvent::kind`], mirroring the
    /// `backend::bridge::BackendEvent` variant order.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    #[repr(u32)]
    pub enum BridgeEventKind {
        FrameReady = 0,
        CameraStatus = 1,
        RecordingStatus = 2,
        ProcessingResult = 3,
        PlaybackPosition = 4,
        BackendError = 5,
        /// Operation lifecycle (schema v4): u0 operationId, u1 kind, u2 state,
        /// u3 progress, u4 total; text message.
        OperationStatus = 6,
        /// Synthetic bounded-queue marker (schema v4): u0 events dropped since
        /// the last poll, u1 dropped total. Emitted first in a poll batch.
        QueueOverflow = 7,
        /// Experiment lifecycle snapshot (schema v5; shared backend since
        /// #372): u0 state, u1 validBuffered, u2 invalidBuffered,
        /// u3 validSaved, u4 invalidSaved, u5 startWallClockNs;
        /// f0/experiment_end_time_ns endWallClockNs,
        /// f1/experiment_dropped_valid valid policy drops,
        /// f2/experiment_dropped_invalid invalid policy drops; b0 flushing,
        /// b1 cancelled; text message. Full status (incl. output path) via
        /// `fetch_experiment_status`.
        ExperimentStatus = 8,
    }

    /// A flattened `BackendEvent`. Rather than mirror every variant field, the
    /// bridge carries a small pool of typed slots whose meaning depends on
    /// `kind`. This keeps the FFI schema stable and additive: new fields append
    /// slots, never repurpose them. See `event_to_bridge` in `shim.cpp` for the
    /// per-kind field mapping.
    #[derive(Debug, Clone)]
    pub struct BridgeEvent {
        pub kind: BridgeEventKind,
        /// Unsigned slots (indices, dims, counts, timestamps).
        pub u0: u64,
        pub u1: u64,
        pub u2: u64,
        pub u3: u64,
        pub u4: u64,
        pub u5: u64,
        /// Floating slots (rates, metrics).
        pub f0: f64,
        pub f1: f64,
        pub f2: f64,
        /// Boolean slots (configured/running, playing/hasFrame).
        pub b0: bool,
        pub b1: bool,
        /// Text slot (labels, error/file messages).
        pub text: String,
        // ABI 12: exact companions for legacy floating integer slots. Legacy
        // slots keep their original meanings for compatibility.
        pub experiment_end_time_ns: u64,
        pub experiment_dropped_valid: u64,
        pub experiment_dropped_invalid: u64,
        pub frame_byte_size: u64,
        // ABI 13 (shared backend, #372): exact ExperimentStatus companions.
        // Zero/false for every other kind. `experiment_completion` is a
        // contract `run_completion_states` value (Unknown until terminal).
        pub experiment_start_generation: u64,
        pub experiment_persistence_admitted: u64,
        pub experiment_persistence_committed: u64,
        pub experiment_persistence_failed: u64,
        pub experiment_completion: u32,
        pub experiment_terminal: bool,
        pub experiment_finalization_ok: bool,
    }

    /// Pollable snapshot of the realtime processing pipeline. `valid` is false
    /// when the backend is not initialized.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeProcessingStats {
        pub valid: bool,
        pub algo_fps1s: f64,
        pub valid_fps1s: f64,
        pub invalid_fps1s: f64,
        pub pixel_to_micron: f64,
    }

    /// Pollable experiment lifecycle snapshot (schema v5, BE-4; shared
    /// backend since #372). `valid` is false when the backend is not
    /// initialized. Saved fields count successful writes by class; dropped
    /// fields count buffer policy drops by class. Pending saves are admitted
    /// minus committed, failed, and policy drops (clamped at zero).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeExperimentStatus {
        pub valid: bool,
        /// Contract `experiment_states` value.
        pub state: u32,
        pub start_time_ns: u64,
        pub end_time_ns: u64,
        pub valid_buffered: u64,
        pub invalid_buffered: u64,
        pub valid_saved: u64,
        pub invalid_saved: u64,
        pub dropped_valid: u64,
        pub dropped_invalid: u64,
        pub flushing: bool,
        pub cancelled: bool,
        pub output_path: String,
        pub message: String,
        // ABI 13: the shared coordinator's full status.
        pub start_generation: u64,
        pub readiness_generation: u64,
        pub capture_generation: u64,
        pub persistence_admitted: u64,
        pub persistence_committed: u64,
        pub persistence_failed: u64,
        /// Finalization finished (Idle or Failed); the outcome below is final.
        pub terminal: bool,
        pub finalization_ok: bool,
        /// Contract `run_completion_states` value (Unknown until terminal).
        pub completion: u32,
        pub completion_reason: String,
        pub fault_revision: u64,
        pub fault_code: String,
        pub fault_message: String,
    }

    /// One readiness gate (ABI 13). `status` is a contract
    /// `readiness_gate_statuses` value; Fail and Unavailable block Start.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeReadinessGate {
        pub id: String,
        pub status: u32,
        pub reason: String,
        pub remediation: String,
    }

    /// Experiment readiness evaluation (ABI 13): the generation is what a
    /// Start must present; the backend refuses a stale one.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeExperimentReadiness {
        pub valid: bool,
        pub ready: bool,
        pub generation: u64,
        pub gates: Vec<BridgeReadinessGate>,
    }

    /// Device-discovery request (schema v14, issue #419). `kinds` are
    /// contract `discovery_device_kinds`; a pulse-generator scan must carry
    /// an explicit serial scope (port, settings, address range) — the
    /// backend refuses broad sweeps. Zero `deadline_ms` means the backend
    /// default (60 s).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeDiscoveryRequest {
        pub kinds: Vec<u32>,
        pub providers: Vec<String>,
        pub has_serial_scope: bool,
        pub serial_port_name: String,
        pub baud_rate: i32,
        pub data_bits: i32,
        pub parity: u8,
        pub stop_bits: i32,
        pub address_from: i32,
        pub address_to: i32,
        pub per_address_timeout_ms: i32,
        pub initial_delay_ms: i32,
        pub deadline_ms: i32,
        pub max_retries: i32,
        pub retry_delay_ms: i32,
        pub origin: String,
    }

    /// Outcome of starting a discovery job (schema v14). `rejection` is a
    /// contract `discovery_error_kinds` value when `accepted` is false.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeDiscoveryStart {
        pub accepted: bool,
        pub coalesced: bool,
        pub job_id: u64,
        pub rejection: u32,
        pub reason: String,
    }

    /// One discovered device (schema v14). `kind`, `identity_strength` and
    /// `identification` are contract values; `camera_type` keeps the
    /// `camera_types` meaning (0 EGrabber, 1 MindVision, 2 Mock, -1 n/a).
    /// Transient SDK indices are session-local and never identity.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeDiscoveredDevice {
        pub kind: u32,
        pub provider_id: String,
        pub display_name: String,
        pub system_path: String,
        pub persistent_id: String,
        pub sdk_index: i32,
        pub interface_index: i32,
        pub device_index: i32,
        pub stream_index: i32,
        pub bus_address: i32,
        pub stable_identity: String,
        pub identity_strength: u32,
        pub identification: u32,
        pub claimed_by: Vec<String>,
        pub capabilities: Vec<String>,
        pub synthetic: bool,
        pub camera_type: i32,
        pub interface_id: String,
        pub device_id: String,
        pub stream_id: String,
        pub model_name: String,
        pub firmware_version: String,
        pub label: String,
    }

    /// Structured discovery error (schema v14); `kind` is a contract
    /// `discovery_error_kinds` value.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeDiscoveryError {
        pub provider_id: String,
        pub kind: u32,
        pub message: String,
        pub endpoint: String,
    }

    /// Bounded discovery snapshot (schema v14). `state` is a contract
    /// `discovery_job_states` value; `complete` is false whenever identity
    /// coverage has a gap (error, busy, timeout, overflow). Camera jobs carry
    /// the synthetic mock entry (`synthetic`, camera_type 2).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeDiscoverySnapshot {
        pub valid: bool,
        pub job_id: u64,
        pub generation: u64,
        pub state: u32,
        pub complete: bool,
        pub overflow: bool,
        pub attempt: i32,
        pub max_attempts: i32,
        pub kinds: Vec<u32>,
        pub candidates: Vec<BridgeDiscoveredDevice>,
        pub errors: Vec<BridgeDiscoveryError>,
        pub providers_run: Vec<String>,
        pub origin: String,
    }

    /// One HTTP header of a registry request (schema v25, #398).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeHttpHeader {
        pub name: String,
        pub value: String,
    }

    /// HTTPS POST the backend registry worker asks the shell to perform
    /// (schema v25, #398; ADR 0002 seam). The transport must refuse non-HTTPS
    /// URLs and redirects, verify TLS, honour `timeout_ms`, stop reading past
    /// `max_response_bytes`, never log headers or bodies, and abort promptly
    /// (status 0) once `registry_request_cancelled(cancel_handle)` is true.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeHttpRequest {
        pub url: String,
        pub body: String,
        pub headers: Vec<BridgeHttpHeader>,
        pub timeout_ms: u32,
        pub max_response_bytes: u64,
        pub cancel_handle: u64,
    }

    /// Transport result: `status` 0 means transport failure/timeout/cancel.
    /// `body` is raw bytes (the backend validates and parses it).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeHttpResponse {
        pub status: u32,
        pub body: Vec<u8>,
    }

    /// A registry project the signed-in user belongs to (schema v25).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeRegistryProject {
        pub project_id: String,
        pub display_name: String,
        pub roles: Vec<String>,
    }

    /// A cached central revision (schema v25); `central_state` is a contract
    /// `registry_central_states` value.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeRegistryRevision {
        pub revision_id: String,
        pub method_id: String,
        pub project_id: String,
        pub display_name: String,
        pub author_id: String,
        pub content_hash: String,
        pub revision_number: u64,
        pub metadata_version: u64,
        pub central_state: u32,
        /// Verified materialized files ("" = not materialized; #398 M2b).
        pub materialized_dir: String,
        /// `registry_local_validation` on this instrument under the current
        /// method context, with who/when (empty when none).
        pub local_validation: u32,
        pub validated_by: String,
        pub validated_at_utc: String,
        /// #398 M3b: lineage, author's notes, and a newer published revision
        /// of the same method ("" = none).
        pub parent_revision_id: String,
        pub release_notes: String,
        pub newer_revision_id: String,
    }

    /// A local method draft (#398 M3b); never sent until submitted.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeRegistryDraft {
        pub draft_id: String,
        pub project_id: String,
        pub method_id: String,
        pub new_method: bool,
        pub method_display_name: String,
        pub base_revision_id: String,
        pub release_notes: String,
        pub submitted_revision_id: String,
        pub updated_at_utc: String,
    }

    /// A registry method with its published head (#398 M3b).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeRegistryMethod {
        pub method_id: String,
        pub project_id: String,
        pub display_name: String,
        pub head_revision_id: String,
    }

    /// One review decision (`review` true) or audit event of a revision.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeRegistryHistoryEntry {
        pub who: String,
        pub what: String,
        pub reason: String,
        pub created_at: String,
        pub review: bool,
    }

    /// The last submit stopped because the draft's base is no longer the
    /// method head (#398 M3b); nothing was sent.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeRegistryConflict {
        pub present: bool,
        pub draft_id: String,
        pub base_revision_id: String,
        pub head_revision_id: String,
        pub compared: bool,
        pub upstream_changes: Vec<String>,
        pub draft_vs_head: Vec<String>,
    }

    /// #398 M2c: what applying a revision would change (`ok` false: why not).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeMethodApplyPlan {
        pub ok: bool,
        pub error: String,
        pub revision_id: String,
        pub display_name: String,
        pub revision_number: u64,
        pub central_state: String,
        pub changed_keys: Vec<String>,
        pub camera_script_path: String,
    }

    /// #398 M2c: outcome of applying a revision's config.json exactly.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeMethodApplyResult {
        pub ok: bool,
        pub error: String,
        pub applied: Vec<String>,
        pub not_applied: Vec<String>,
    }

    /// Outcome of an authoring command: `job_id` 0 = refused, `error` why.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeRegistryCommand {
        pub job_id: u64,
        pub error: String,
    }

    /// Outcome of `registry_record_validation` (#398 M2b): `job_id` 0 means
    /// refused and `error` says why (e.g. the test run was not recorded with
    /// this revision applied on this instrument).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeRegistryValidationRequest {
        pub job_id: u64,
        pub error: String,
    }

    /// Registry job status (schema v25); `kind`/`state` are contract
    /// `registry_job_kinds` / `registry_job_states` values. `job_id` 0 means
    /// unknown, evicted or refused.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeRegistryJob {
        pub job_id: u64,
        pub kind: u32,
        pub state: u32,
        pub message: String,
    }

    /// Value snapshot of the backend registry worker (schema v25, #398).
    /// `session` = `registry_session_states`, `connectivity` =
    /// `registry_connectivity`. Never carries a token or password.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeRegistrySnapshot {
        pub valid: bool,
        pub configured: bool,
        pub generation: u64,
        pub origin: String,
        pub session: u32,
        pub subject_id: String,
        pub email: String,
        pub connectivity: u32,
        pub health_message: String,
        pub successful_requests: u64,
        pub failed_requests: u64,
        pub rejected_revisions: u64,
        pub projects: Vec<BridgeRegistryProject>,
        pub revisions: Vec<BridgeRegistryRevision>,
        pub corrupt_revision_ids: Vec<String>,
        pub cache_error: String,
        pub has_last_successful_refresh: bool,
        pub last_successful_refresh_unix_ms: i64,
        pub last_job: BridgeRegistryJob,
        pub queued_jobs: u64,
        pub busy: bool,
        /// This instrument's identity (UUID + optional name; #398 M2b).
        pub instrument_id: String,
        pub instrument_name: String,
        /// #398 M3b authoring state.
        pub drafts: Vec<BridgeRegistryDraft>,
        pub methods: Vec<BridgeRegistryMethod>,
        pub history_revision_id: String,
        pub history: Vec<BridgeRegistryHistoryEntry>,
        pub submit_conflict: BridgeRegistryConflict,
    }

    /// Authoritative selected-device snapshot (schema v7, BE-2). `mode` is a
    /// contract `camera_selection_modes` value.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeCameraSelection {
        pub valid: bool,
        pub mode: u32,
        pub interface_index: i32,
        pub device_index: i32,
        pub label: String,
        pub mindvision_index: i32,
        pub mindvision_config_path: String,
        pub camera_script_path: String,
        pub mock_frame_dir: String,
        pub mock_interval_ms: i32,
        pub mock_loop: bool,
        pub configured: bool,
        pub running: bool,
    }

    /// Autofocus / nanopositioner status (schema v11, BE-8). Freshness of
    /// the focus metric is explicit (`ring_ratio_age_us`) so stale metrics
    /// are observable and never silently drive a move.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeAutofocusStatus {
        pub valid: bool,
        pub connected: bool,
        pub enabled: bool,
        pub current_voltage: f64,
        pub com_port: i32,
        pub backend_name: String,
        pub endpoint_id: String,
        pub average_ring_ratio: f64,
        pub median_ring_ratio: f64,
        pub last_ring_ratio_update_us: u64,
        pub ring_ratio_age_us: u64,
    }

    /// Autofocus configuration (schema v11, BE-8) — plain values, no
    /// QSettings types anywhere in the round-trip.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeAutofocusConfig {
        pub valid: bool,
        pub focus_setpoint: f64,
        pub focus_range: f64,
        pub voltage_step: f64,
        pub fine_voltage_step: f64,
        pub max_voltage: f64,
        pub min_voltage: f64,
        pub initial_voltage: f64,
        pub manual_voltage_step: f64,
        pub ring_ratio_stale_ms: i32,
        pub require_new_sample_per_step: bool,
        pub min_samples_per_step: i32,
        pub safe_shutdown_voltage: f64,
        pub focus_direction: bool,
    }

    /// Z stage snapshot (#464, ADR 0013 Amendment 1; ABI 30 = no homing).
    /// `move_state` is a contract `stage_move_states` value; positions are
    /// micrometres in the operator's frame once `zero_set`, otherwise the raw
    /// controller counter, which means nothing.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeStageStatus {
        pub valid: bool,
        pub enabled: bool,
        pub connected: bool,
        /// Controller matches the stage profile; otherwise motion is refused.
        pub configured: bool,
        /// The operator set zero since the controller powered up; moves need it.
        pub zero_set: bool,
        /// ... and declared the stage was at mid-travel (widens the envelope).
        pub mid_travel_declared: bool,
        /// Power-up token off (hardware-acceptance mode): a controller power cycle
        /// is NOT detected and the zero is not persisted. The shell must warn.
        pub session_only_zero: bool,
        /// The supervised limit-switch check passed for this controller
        /// (`zc300ctl verify-limits`). It clears a "wiring unverified" badge and
        /// gates nothing.
        pub limits_verified: bool,
        /// A move is queued or running.
        pub busy: bool,
        pub model: String,
        pub serial: String,
        pub firmware: String,
        pub port_name: String,
        pub move_state: u32,
        pub position_um: f64,
        pub limit_positive: bool,
        pub limit_negative: bool,
        pub home: bool,
        pub emergency_stop: bool,
        pub driver_alarm: bool,
        pub span_um: f64,
        /// Allowed travel around the zero (0/0 until zero is set).
        pub envelope_min_um: f64,
        pub envelope_max_um: f64,
        pub last_error: String,
    }

    /// Authoritative per-pump snapshot (schema v10, BE-7). `run_status` /
    /// `direction` are contract `pump_run_states` / `pump_directions` values.
    #[derive(Debug, Clone, Default)]
    pub struct BridgePumpStatus {
        pub valid: bool,
        pub connected: bool,
        pub run_status: u32,
        pub current_flow_rate: f64,
        pub accumulated_volume: f64,
        pub min_flow_rate: f64,
        pub max_flow_rate: f64,
        pub stalled: bool,
        pub com_port: i32,
        pub baud_rate: i32,
        pub modbus_address: i32,
        pub port_name: String,
        pub configured_flow_rate: f64,
        pub flow_rate_unit: i32,
        pub direction: u32,
        /// Contract `pump_models` value (v22).
        pub model: u32,
        /// Peristaltic flow calibration, µL per head revolution (v22).
        pub microliters_per_rev: f64,
        /// Peristaltic head speed setpoint in rpm (v22).
        pub speed_rpm: f64,
    }

    /// Per-dataset capabilities of the loaded review file (schema v9, BE-6).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeReviewDatasetInfo {
        pub present: bool,
        pub count: u64,
        pub height: i32,
        pub width: i32,
        pub channels: i32,
    }

    /// Review metadata for the loaded HDF5 file (schema v9, BE-6).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeReviewMetadata {
        pub valid: bool,
        pub file_open: bool,
        pub recording_file: bool,
        pub start_time_ns: u64,
        pub end_time_ns: u64,
        pub total_valid: u64,
        pub total_invalid: u64,
        pub roi_x: i32,
        pub roi_y: i32,
        pub roi_w: i32,
        pub roi_h: i32,
        pub has_background: bool,
        pub has_core_identity: bool,
        pub core_version: String,
        pub core_source: String,
        pub core_release_tag: String,
        pub valid_images: BridgeReviewDatasetInfo,
        pub invalid_images: BridgeReviewDatasetInfo,
        pub valid_masks: BridgeReviewDatasetInfo,
        pub invalid_masks: BridgeReviewDatasetInfo,
        pub recorded_images: BridgeReviewDatasetInfo,
        pub file_path: String,
    }

    /// One page of review frame/object metrics (schema v9, BE-6): bounded
    /// rows served from a metadata-only cache — never image payloads.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeReviewMetricsPage {
        pub valid: bool,
        pub total: u64,
        pub offset: u64,
        pub rows: Vec<BridgeMonitoringRow>,
    }

    /// Full processing configuration document (schema v8, BE-3): a lossless
    /// JSON string — `image_processing` (exact config.json schema),
    /// `realtime_processing`, `flush_interval`, `pixel_to_micron`, `roi`,
    /// `background_set`, and the monotonic `config_version` for
    /// external-change detection. `valid` is false when uninitialized.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeCheckedConfigDocument {
        pub ok: bool,
        pub path: String,
        pub revision: String,
        pub document_json: String,
        pub error: String,
    }
    #[derive(Debug, Clone, Default)]
    pub struct BridgeConfigTransactionResult {
        pub saved: bool,
        pub applied: bool,
        pub verified: bool,
        pub conflict: bool,
        pub revision: String,
        pub error: String,
    }

    #[derive(Debug, Clone, Default)]
    pub struct BridgeConfigDocument {
        pub valid: bool,
        pub json: String,
    }

    /// Processing-core identity/trust status (schema v8, BE-3). Trust
    /// verification stays backend-owned; this is observability only.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeProcessingCoreStatus {
        pub valid: bool,
        pub active_version: String,
        pub contract_version: u32,
        pub engine_abi_version: u32,
        pub source: String,
        pub release_tag: String,
        pub build_id: String,
        pub artifact_sha256: String,
        pub required_version: String,
        pub pin_satisfied: bool,
    }

    /// One monitoring metric row (schema v6, BE-5): the per-object
    /// measurements that feed the Monitoring charts. `(frame_index,
    /// object_id)` is a stable identity for frontend reconciliation. Never
    /// carries image/mask payloads.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeMonitoringRow {
        pub frame_index: u64,
        pub timestamp_ns: u64,
        pub valid: bool,
        pub target_group: bool,
        pub object_id: i32,
        pub object_count: i32,
        pub track_id: i32,
        pub centroid_x: f64,
        pub centroid_y: f64,
        pub area: f64,
        pub deformability: f64,
        pub area_ratio: f64,
        pub ring_ratio: f64,
        pub youngs_modulus: f64,
        pub pixel_to_micron: f64,
    }

    /// Bounded monitoring snapshot (schema v6, BE-5). Evictions are
    /// observable: `*_appended - *_held`; freshness via `latest_timestamp_ns`.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeMonitoringSnapshot {
        pub valid: bool,
        pub monitoring_active: bool,
        pub valid_held: u64,
        pub invalid_held: u64,
        pub valid_appended: u64,
        pub invalid_appended: u64,
        pub capacity: u64,
        pub latest_timestamp_ns: u64,
        pub rows: Vec<BridgeMonitoringRow>,
    }

    /// Sorter trigger status snapshot (schema v6, BE-5).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeTriggerStatus {
        pub valid: bool,
        pub camera_attached: bool,
        pub pulse_duration_us: i32,
        pub trigger_count: u64,
        pub last_onset_us: f64,
        pub last_object_id: i32,
        pub last_track_id: i32,
        pub periodic_active: bool,
        pub periodic_interval_ms: i32,
    }

    /// A frame pulled on demand: metadata plus a single owned copy of the pixel
    /// bytes. `valid` is false when no frame is available.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeFrame {
        pub capture_session: u64,
        pub store_generation: u64,
        pub valid: bool,
        pub frame_index: u64,
        pub timestamp_ns: u64,
        pub width: u64,
        pub height: u64,
        pub pixel_format: u64,
        pub stride_bytes: u64,
        pub data: Vec<u8>,
    }

    extern "Rust" {
        /// One bulk copy of C++ bytes into a Rust `Vec<u8>`. cxx's `rust::Vec::push_back`
        /// is an FFI call per element: filling a 509 KB full-field frame that way cost ~75 ms
        /// on the PZ7035's Cortex-A9 and held the browser Overview at ~10 fps.
        fn bytes_to_vec(bytes: &[u8]) -> Vec<u8>;
    }

    unsafe extern "C++" {
        include!("mib-bridge/src/shim.h");

        /// Opaque owner of an `AppBackend` + `BackendFacade`. Dropping it calls
        /// `shutdown()` then destroys the backend.
        type BackendBridge;

        // Declared unconditionally: cxx-build does not emit the C++ wrappers
        // for `#[cfg(feature = ...)]` bridge functions while rustc still
        // compiles the Rust side under the feature, which left the desktop
        // test binaries with unresolved `contract_fixture_*` externals
        // (Linux CI for PR #375 and the Windows bench). The producers are
        // test fixtures in shim.cpp; nothing outside the feature-gated Rust
        // callers below can reach them and no Tauri command exposes them.
        fn contract_fixture_events() -> Vec<BridgeEvent>;
        fn contract_fixture_frame() -> BridgeFrame;

        /// Construct a fresh, uninitialized bridge.
        fn new_backend_bridge() -> UniquePtr<BackendBridge>;

        /// Schema version of the command/event contract (ADR 0003). Additive
        /// changes bump this.
        fn profile_fetch_url(url: &str) -> String;
        fn bridge_abi_version() -> u32;

        fn initialize(self: Pin<&mut BackendBridge>, data_dir: &str) -> bool;
        fn initialize_with_resources(self: Pin<&mut BackendBridge>, data_dir: &str, resource_root: &str) -> bool;
        fn shutdown(self: Pin<&mut BackendBridge>);
        fn is_initialized(&self) -> bool;

        fn configure_mock_camera(
            self: Pin<&mut BackendBridge>,
            frame_dir: &str,
            frame_interval_ms: i32,
            loop_files: bool,
        ) -> BridgeCommandResult;
        fn start_capture(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;
        fn stop_capture(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;
        fn start_frame_recording(self: Pin<&mut BackendBridge>, file_path: &str)
            -> BridgeCommandResult;
        fn stop_frame_recording(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;

        /// Resolve the latest live frame through the playback service. Emits a
        /// FrameReady + PlaybackPosition event pair (the push path the webview
        /// subscribes to).
        fn playback_seek_latest(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;

        /// Load a recorded HDF5 file for review through the playback service.
        fn load_recording(self: Pin<&mut BackendBridge>, file_path: &str)
            -> BridgeCommandResult;

        /// Seek playback to an absolute frame index (review scrubbing). Emits a
        /// FrameReady + PlaybackPosition event pair.
        fn playback_seek_index(self: Pin<&mut BackendBridge>, frame_index: u64)
            -> BridgeCommandResult;

        /// Apply realtime processing settings (enable/disable + pixel→micron
        /// scale). Additive schema v3.
        fn apply_processing(
            self: Pin<&mut BackendBridge>,
            realtime_enabled: bool,
            pixel_to_micron: f64,
        ) -> BridgeCommandResult;

        /// Request cancellation of a tracked operation by ID (schema v4). Fails
        /// safely (`ok == false`) for unknown or already-finished IDs.
        fn cancel_operation(self: Pin<&mut BackendBridge>, operation_id: u64)
            -> BridgeCommandResult;

        /// Start an experiment (schema v5, BE-4): atomic precondition
        /// validation (processing-core pin, camera running, HDF5 openable) and
        /// backend-owned accumulation/flush. The result's `operation_id`
        /// tracks the experiment's operation lifecycle.
        fn experiment_start(self: Pin<&mut BackendBridge>, output_path: &str)
            -> BridgeCommandResult;

        /// Request an asynchronous experiment stop: final flush, metadata/
        /// provenance write (only after data is flushed), close. Never blocks
        /// on the flush.
        fn experiment_stop(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;
        fn fetch_capture_lifecycle(self: Pin<&mut BackendBridge>) -> String;
        fn experiment_acknowledge_fault(self: Pin<&mut BackendBridge>, expected_run: u64, fault_revision: u64, code: &str, message: &str, confirmed: bool) -> BridgeCommandResult;

        /// Like `experiment_stop`, but the terminal status is marked cancelled.
        /// The HDF5 file is still finalized so it remains readable.
        fn experiment_cancel(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;

        /// Pull the current experiment lifecycle snapshot (schema v5; full
        /// shared-coordinator status since ABI 13).
        fn fetch_experiment_status(self: Pin<&mut BackendBridge>) -> BridgeExperimentStatus;

        /// Evaluate experiment readiness for `output_path` (ABI 13): gate
        /// list with statuses/reasons/remediation and the generation a
        /// Start must present. `experiment_start` performs this itself.
        fn fetch_experiment_readiness(self: Pin<&mut BackendBridge>, output_path: &str)
            -> BridgeExperimentReadiness;

        /// Autofocus / nanopositioner commands (schema v11, BE-8). On
        /// platforms without the Coremor SDK, connect fails with a structured
        /// message and every other command stays safe.
        fn autofocus_connect_endpoint(self: Pin<&mut BackendBridge>, backend: &str, endpoint: &str, com_port: i32, baud_rate: i32, device_address: i32) -> BridgeCommandResult;
        fn autofocus_connect(
            self: Pin<&mut BackendBridge>,
            com_port: i32,
            baud_rate: i32,
            device_address: i32,
        ) -> BridgeCommandResult;
        /// Disconnect disables control first so no motion outlives it.
        fn autofocus_disconnect(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;
        fn autofocus_set_enabled(self: Pin<&mut BackendBridge>, enabled: bool)
            -> BridgeCommandResult;
        /// Manual voltage jog: `up == true` increases, else decreases.
        fn autofocus_jog(self: Pin<&mut BackendBridge>, up: bool) -> BridgeCommandResult;
        fn autofocus_set_config(
            self: Pin<&mut BackendBridge>,
            config: BridgeAutofocusConfig,
        ) -> BridgeCommandResult;
        fn fetch_autofocus_status(self: Pin<&mut BackendBridge>) -> BridgeAutofocusStatus;
        fn fetch_autofocus_config(self: Pin<&mut BackendBridge>) -> BridgeAutofocusConfig;

        /// Syringe pump commands (schema v10, BE-7). `pump` is a contract
        /// `pump_ids` value (0 Sample, 1 Sheath). Serial-port conflicts with
        /// the other pump or the autofocus controller are structured errors.
        fn pump_connect_endpoint(self: Pin<&mut BackendBridge>, pump: u32, port_name: &str, baud_rate: i32, modbus_address: i32) -> BridgeCommandResult;
        fn pump_connect(
            self: Pin<&mut BackendBridge>,
            pump: u32,
            com_port: i32,
            baud_rate: i32,
            modbus_address: i32,
        ) -> BridgeCommandResult;
        /// Connect a pump slot to either model (v22): `model` is a contract
        /// `pump_models` value; `microliters_per_rev` calibrates peristaltic
        /// flow. A peristaltic connect only reads the pump.
        fn pump_connect_model(
            self: Pin<&mut BackendBridge>,
            pump: u32,
            model: u32,
            port_name: &str,
            baud_rate: i32,
            modbus_address: i32,
            microliters_per_rev: f64,
        ) -> BridgeCommandResult;
        /// Disconnect stops an active run/purge first.
        fn pump_disconnect(self: Pin<&mut BackendBridge>, pump: u32) -> BridgeCommandResult;
        fn pump_set_flow_rate(
            self: Pin<&mut BackendBridge>,
            pump: u32,
            rate: f64,
            unit: i32,
        ) -> BridgeCommandResult;
        fn pump_set_direction(self: Pin<&mut BackendBridge>, pump: u32, direction: u32)
            -> BridgeCommandResult;
        fn pump_start(self: Pin<&mut BackendBridge>, pump: u32) -> BridgeCommandResult;
        fn pump_stop(self: Pin<&mut BackendBridge>, pump: u32) -> BridgeCommandResult;
        fn pump_purge(self: Pin<&mut BackendBridge>, pump: u32, direction: u32)
            -> BridgeCommandResult;
        fn pump_stop_purge(self: Pin<&mut BackendBridge>, pump: u32) -> BridgeCommandResult;
        fn pump_set_syringe_volume(
            self: Pin<&mut BackendBridge>,
            pump: u32,
            volume: i32,
            unit: i32,
        ) -> BridgeCommandResult;
        /// Poll the pump hardware, then read the snapshot via
        /// `fetch_pump_status`. Polling runs on the command thread and never
        /// blocks the event queue.
        fn pump_poll_status(self: Pin<&mut BackendBridge>, pump: u32) -> BridgeCommandResult;
        fn fetch_pump_status(self: Pin<&mut BackendBridge>, pump: u32) -> BridgePumpStatus;
        /// Probe a COM port for responsive Modbus addresses as a tracked
        /// operation (the terminal Completed event's text carries the
        /// comma-separated addresses).
        fn pump_scan_addresses(
            self: Pin<&mut BackendBridge>,
            com_port: i32,
            baud_rate: i32,
            start_address: i32,
            end_address: i32,
            timeout_ms: i32,
        ) -> BridgeCommandResult;

        /// Z stage (#464, ADR 0013 Amendment 1). The stage is never homed.
        /// Safety lives in the backend: moves are refused until the operator
        /// set zero this power-up (`stage_set_zero`: one register write, no
        /// motion) and outside the travel envelope around it; `stage_connect`
        /// is observe-only; `stage_stop` is always accepted (also during an
        /// experiment); everything else needs an idle experiment. Moves return
        /// a tracked operation id (kind StageMove); cancelling it stops the
        /// axis.
        fn stage_connect(
            self: Pin<&mut BackendBridge>,
            port_name: &str,
            usb_serial: &str,
            modbus_address: i32,
        ) -> BridgeCommandResult;
        fn stage_disconnect(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;
        fn stage_move_to(self: Pin<&mut BackendBridge>, target_um: f64) -> BridgeCommandResult;
        fn stage_move_by(self: Pin<&mut BackendBridge>, delta_um: f64) -> BridgeCommandResult;
        fn stage_set_zero(self: Pin<&mut BackendBridge>, mid_travel: bool) -> BridgeCommandResult;
        fn stage_stop(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;
        fn stage_apply_profile(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;
        fn fetch_stage_status(self: Pin<&mut BackendBridge>) -> BridgeStageStatus;

        /// Pull the review metadata of the loaded HDF5 file (schema v9, BE-6).
        fn fetch_review_metadata(self: Pin<&mut BackendBridge>) -> BridgeReviewMetadata;

        /// Pull one bounded page of frame/object metrics from the loaded file
        /// (schema v9, BE-6). `valid` selects the valid/invalid table.
        fn fetch_review_metrics_page(
            self: Pin<&mut BackendBridge>,
            valid: bool,
            offset: u64,
            count: u64,
        ) -> BridgeReviewMetricsPage;

        /// Pull one review image/mask by index from a dataset (contract
        /// `review_image_datasets` value) — bounded hyperslab read.
        fn fetch_review_image(
            self: Pin<&mut BackendBridge>,
            dataset: u32,
            index: u64,
        ) -> BridgeFrame;

        /// Start a cancellable metrics CSV export job for the loaded file
        /// (schema v9, BE-6). Returns the job's operation_id; progress and the
        /// terminal state arrive as OperationStatus events. Partial outputs
        /// are removed on cancel/failure; the source file is opened read-only.
        fn set_processed_preview_enabled(self: Pin<&mut BackendBridge>, enabled: bool);
        fn fetch_processed_preview(self: Pin<&mut BackendBridge>) -> Vec<u8>;
        fn background_calibration_command(self: Pin<&mut BackendBridge>, json: &str) -> BridgeCommandResult;
        fn background_calibration_status(self: Pin<&mut BackendBridge>) -> String;
        fn startup_discovery_set_preference(self: Pin<&mut BackendBridge>, json: &str) -> String;
        fn startup_discovery_run(self: Pin<&mut BackendBridge>, action: &str) -> String;
        fn startup_discovery_status(self: Pin<&mut BackendBridge>) -> String;
        fn pulse_generator_command(self: Pin<&mut BackendBridge>, json: &str) -> BridgeCommandResult;
        fn pulse_generator_status(self: Pin<&mut BackendBridge>) -> String;

        fn render_review_overlay(self: Pin<&mut BackendBridge>, json: &str) -> Vec<u8>;
        fn fetch_review_reanalysis_preview(self: Pin<&mut BackendBridge>, json: &str) -> BridgeFrame;
        fn fetch_review_charts_json(self: Pin<&mut BackendBridge>) -> String;
        fn fetch_monitoring_chart_reference(self: Pin<&mut BackendBridge>) -> String;
        fn review_reanalysis_json(self: Pin<&mut BackendBridge>, json: &str) -> BridgeCommandResult;
        fn review_reanalysis_status_json(self: Pin<&mut BackendBridge>) -> String;
        fn review_export_json(self: Pin<&mut BackendBridge>, json: &str) -> BridgeCommandResult;
        fn review_export_status_json(self: Pin<&mut BackendBridge>) -> String;

        fn review_export_csv(self: Pin<&mut BackendBridge>, output_path: &str)
            -> BridgeCommandResult;

        /// Pull the full processing configuration document (schema v8, BE-3).
        fn fetch_processing_config_json(self: Pin<&mut BackendBridge>) -> BridgeConfigDocument;
        fn processing_core_command(self: Pin<&mut BackendBridge>, cache_root: &str, request: &str) -> String;
        fn profile_command(self: Pin<&mut BackendBridge>, base: &str, request: &str) -> String;
        /// #398 M2c (ABI 32): re-apply the startup configuration (the last
        /// applied local profile or central method). JSON {ok, restored, kind,
        /// error?, notice?, roi_pending?}; `profile_base` only serves a legacy
        /// profile selection when no startup pointer exists.
        fn restore_startup_configuration(self: Pin<&mut BackendBridge>, profile_base: &str) -> String;
        fn fetch_config_document(self: Pin<&mut BackendBridge>, path: &str) -> BridgeCheckedConfigDocument;
        fn apply_config_document(self: Pin<&mut BackendBridge>, path: &str, baseline: &str, patch: &str) -> BridgeConfigTransactionResult;


        /// Merge-apply a processing configuration document (schema v8, BE-3):
        /// only keys present in the JSON change; malformed values fail the
        /// whole command without touching state.
        fn apply_processing_config_json(self: Pin<&mut BackendBridge>, json: &str)
            -> BridgeCommandResult;

        /// Set the realtime processing ROI (schema v8, BE-3). w==0 || h==0
        /// clears the ROI.
        fn set_processing_roi(
            self: Pin<&mut BackendBridge>,
            x: i32,
            y: i32,
            w: i32,
            h: i32,
        ) -> BridgeCommandResult;

        /// Pull the current background image (Mono8, binary — schema v8).
        /// `valid` is false when no background is set.
        fn fetch_background_image(self: Pin<&mut BackendBridge>) -> BridgeFrame;

        /// Set the background image from raw Mono8 bytes (len == w*h).
        fn set_background_image(
            self: Pin<&mut BackendBridge>,
            width: u64,
            height: u64,
            data: &[u8],
        ) -> BridgeCommandResult;

        /// Clear the background image.
        fn clear_background_image(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;

        /// Pull the processing-core identity/pin status (schema v8, BE-3).
        fn fetch_processing_core_status(self: Pin<&mut BackendBridge>)
            -> BridgeProcessingCoreStatus;

        /// Start a device-discovery job (schema v14, issue #419). Never
        /// blocks on hardware; poll `fetch_device_discovery`.
        fn start_device_discovery(
            self: Pin<&mut BackendBridge>,
            request: &BridgeDiscoveryRequest,
        ) -> BridgeDiscoveryStart;

        /// Convenience: camera + framegrabber job with default bounds (the
        /// pre-v14 `fetch_camera_discovery` scope, asynchronously).
        fn start_camera_discovery(self: Pin<&mut BackendBridge>) -> BridgeDiscoveryStart;

        /// Request cancellation of a running job; false for unknown/ended
        /// jobs. Never disconnects an established device.
        fn cancel_device_discovery(self: Pin<&mut BackendBridge>, job_id: u64) -> bool;

        /// Value snapshot of a job; never waits for a worker.
        fn fetch_device_discovery(self: Pin<&mut BackendBridge>, job_id: u64)
            -> BridgeDiscoverySnapshot;

        /// Install the shell's HTTPS POST for the central profile registry
        /// (schema v25, #398). Call before `initialize`; returns false (and
        /// installs nothing) afterwards. Without a transport the registry
        /// stays inert even when configured.
        fn set_registry_transport(
            self: Pin<&mut BackendBridge>,
            transport: fn(request: &BridgeHttpRequest) -> BridgeHttpResponse,
        ) -> bool;

        /// True once the in-flight registry request `cancel_handle` should be
        /// abandoned (registry cancel or backend shutdown). Unknown or
        /// finished handles report true.
        fn registry_request_cancelled(cancel_handle: u64) -> bool;

        /// Central profile registry commands (schema v25, #398): each enqueues
        /// a worker job and returns its ID (0 = refused: not initialized,
        /// registry not configured, or invalid argument). Never blocks on
        /// the network.
        fn registry_sign_in(self: Pin<&mut BackendBridge>, email: &str, password: &str) -> u64;
        fn registry_sign_out(self: Pin<&mut BackendBridge>) -> u64;
        fn registry_refresh(self: Pin<&mut BackendBridge>) -> u64;
        fn registry_download(self: Pin<&mut BackendBridge>, revision_id: &str) -> u64;
        /// Drop queued registry jobs and abort the running one.
        fn registry_cancel_all(self: Pin<&mut BackendBridge>) -> bool;
        /// Write a cached revision's files read-only for Apply (#398 M2b).
        fn registry_materialize(self: Pin<&mut BackendBridge>, revision_id: &str) -> u64;
        /// "Mark validated" (#398 M2b): `evidence_file` is a test-run HDF5
        /// recorded with `revision_id` applied on this instrument under the
        /// current method context; checked before the job is queued.
        fn registry_record_validation(
            self: Pin<&mut BackendBridge>,
            revision_id: &str,
            evidence_file: &str,
            passed: bool,
        ) -> BridgeRegistryValidationRequest;
        /// #398 M3b authoring: drafts stay local until submitted; review and
        /// publication need the matching project role (server-enforced) and
        /// a reason. `state` = `registry_central_states`.
        fn registry_new_draft_from_revision(
            self: Pin<&mut BackendBridge>,
            revision_id: &str,
            use_current_config: bool,
        ) -> BridgeRegistryCommand;
        fn registry_new_method_draft(
            self: Pin<&mut BackendBridge>,
            project_id: &str,
            name: &str,
            release_notes: &str,
        ) -> BridgeRegistryCommand;
        fn registry_set_draft_notes(self: Pin<&mut BackendBridge>, draft_id: &str, notes: &str)
            -> BridgeRegistryCommand;
        fn registry_draft_from_head(self: Pin<&mut BackendBridge>, draft_id: &str, keep_draft_config: bool)
            -> BridgeRegistryCommand;
        fn registry_submit_draft(self: Pin<&mut BackendBridge>, draft_id: &str, as_branch: bool)
            -> BridgeRegistryCommand;
        fn registry_delete_draft(self: Pin<&mut BackendBridge>, draft_id: &str) -> BridgeRegistryCommand;
        fn registry_transition(self: Pin<&mut BackendBridge>, revision_id: &str, state: u32, reason: &str)
            -> BridgeRegistryCommand;
        fn registry_fetch_history(self: Pin<&mut BackendBridge>, revision_id: &str) -> BridgeRegistryCommand;
        /// #398 M2c Apply: preview, then apply a materialized published or
        /// superseded revision's config.json exactly (refused while a run is
        /// in flight). Local and synchronous.
        fn registry_plan_apply(self: Pin<&mut BackendBridge>, revision_id: &str) -> BridgeMethodApplyPlan;
        fn registry_apply_method(self: Pin<&mut BackendBridge>, revision_id: &str) -> BridgeMethodApplyResult;
        /// Value snapshot of the registry worker; never waits on a request.
        fn fetch_registry_snapshot(self: Pin<&mut BackendBridge>) -> BridgeRegistrySnapshot;
        fn fetch_registry_job(self: Pin<&mut BackendBridge>, job_id: u64) -> BridgeRegistryJob;

        /// Pull the authoritative selected-device snapshot (schema v7, BE-2).
        fn fetch_camera_selection(self: Pin<&mut BackendBridge>) -> BridgeCameraSelection;

        /// Select a hardware (EGrabber) camera by interface/device index
        /// (schema v7, BE-2). Invalid indices fail with a structured error.
        fn select_hardware_camera(
            self: Pin<&mut BackendBridge>,
            interface_index: i32,
            device_index: i32,
            label: &str,
        ) -> BridgeCommandResult;

        /// Select a MindVision camera by enumeration index; optionally apply a
        /// JSON config file (schema v7, BE-2).
        fn select_mindvision_camera(
            self: Pin<&mut BackendBridge>,
            camera_index: i32,
            label: &str,
            config_path: &str,
        ) -> BridgeCommandResult;

        /// Apply a JS camera script to the selected hardware camera (stops
        /// capture first; capture stays stopped — schema v7, BE-2).
        fn apply_camera_script(self: Pin<&mut BackendBridge>, script_path: &str)
            -> BridgeCommandResult;

        /// Issue a GenICam DeviceReset to the selected hardware camera.
        fn soft_trigger_camera(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;

        /// Camera & Alignment (ABI 20): show the whole sensor (`overview`) or the saved
        /// experiment window; a running capture restarts in the new mode. Rejected during an
        /// experiment or recording, and for cameras without an overview.
        fn set_camera_overview(self: Pin<&mut BackendBridge>, overview: bool) -> BridgeCommandResult;
        /// Save the experiment window (ROI 1, sensor coordinates) placed on the overview.
        fn save_camera_roi(self: Pin<&mut BackendBridge>, x: i32, y: i32, width: i32, height: i32)
            -> BridgeCommandResult;
        /// Mode, sensor size, saved window, window steps and the last camera read-back (JSON).
        fn fetch_camera_geometry(self: Pin<&mut BackendBridge>) -> String;
        /// Where the science runs (ABI 21): `{"science": "host"|"pl", "host_processing": bool,
        /// "aravis": bool}`. On the PL the host pipeline's commands are refused.
        fn fetch_platform_info(self: Pin<&mut BackendBridge>) -> String;
        /// Reconciled run accounting as JSON (ABI 31, #549): `source` is "review" (the file loaded
        /// for review) or "last_run" (the run that finished last in this session).
        fn fetch_run_accounting(self: Pin<&mut BackendBridge>, source: &str) -> String;
        /// PZ7035 camera mode (ABI 27, #501 P1): "align" | "run" with the Run window offset.
        fn set_instrument_mode(self: Pin<&mut BackendBridge>, mode: &str, x: i32, y: i32) -> BridgeCommandResult;
        /// The client's wall clock (ms since the epoch): stamps saved files on a board without an RTC (G14).
        fn sync_wall_clock(self: Pin<&mut BackendBridge>, unix_ms: i64) -> BridgeCommandResult;
        /// #649 v1 (ABI 34): the every-frame ring's status as JSON (also the `ring` block of the instrument status).
        fn fetch_ring_status(self: Pin<&mut BackendBridge>) -> String;
        /// One buffered frame as a 'MIBR' packet; empty on failure (`ring_frame_error` says why).
        fn fetch_ring_frame(self: Pin<&mut BackendBridge>, seq: u64) -> Vec<u8>;
        /// Why the last `fetch_ring_frame` failed: `<outcome>: <why>` (out_of_range, overwritten, malformed, unavailable).
        fn ring_frame_error(self: Pin<&mut BackendBridge>) -> String;
        /// Stop in Run: hold the ring for playback (controller only).
        fn ring_freeze(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;
        /// Resume Run: re-arm, a new ring (controller only).
        fn ring_resume(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;
        /// #667 S1 (ABI 35): the SATA SSD record store, read only. `fetch_ssd_status` is JSON (state, reason, log room, the open run and its
        /// counters, what the last mount-time recovery closed; also a short `ssd` block of the instrument status), `fetch_ssd_runs` the run table.
        fn fetch_ssd_status(self: Pin<&mut BackendBridge>) -> String;
        fn fetch_ssd_runs(self: Pin<&mut BackendBridge>) -> String;
        /// #667 (ABI 36): the lease of one SSD run download, taken by the HTTP route (not a WebSocket command). JSON `{ok, lease, run_id, records, bytes,
        /// max_seconds, argv[]}` or `{ok: false, reason}`; refused while anything records. `ssd_export_end` releases it after the reader has exited (idempotent).
        fn ssd_export_begin(self: Pin<&mut BackendBridge>, run: u32, from: u64, count: u64) -> String;
        fn ssd_export_end(self: Pin<&mut BackendBridge>, lease: u64, bytes_sent: u64, outcome: &str);
        /// Service / Commissioning mode latch; raw LED values are refused outside it.
        fn set_service_mode(self: Pin<&mut BackendBridge>, on: bool) -> BridgeCommandResult;
        /// Raw LED delay/width in µs (Service mode, per-mode limits).
        fn set_instrument_led(self: Pin<&mut BackendBridge>, delay_us: f64, width_us: f64) -> BridgeCommandResult;
        /// Run mode: one PL cell capture as an MIBC packet; empty when unavailable.
        fn fetch_run_preview(self: Pin<&mut BackendBridge>) -> Vec<u8>;
        /// PZ7035 identity and health for preflight (#501): `{"available": bool, "error"?,
        /// "core": {...}, "led": {...}, "link": {...}, "latency": {...}}`. Read-only.
        fn fetch_instrument_status(self: Pin<&mut BackendBridge>) -> String;
        fn reset_hardware_camera(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;

        /// Enable/disable monitoring accumulation (schema v6, BE-5). Disabled
        /// monitoring skips the per-frame image clones entirely.
        fn monitoring_set_active(self: Pin<&mut BackendBridge>, active: bool)
            -> BridgeCommandResult;

        /// Atomically clear the monitoring buffers and appended totals.
        fn monitoring_clear(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;

        /// Pull a bounded monitoring snapshot: at most `max_rows` most-recent
        /// metric rows (metrics only — never image payloads).
        fn fetch_monitoring_snapshot(self: Pin<&mut BackendBridge>, max_rows: u64)
            -> BridgeMonitoringSnapshot;

        /// Set the sorter trigger pulse duration in microseconds.
        fn trigger_set_pulse_duration(self: Pin<&mut BackendBridge>, pulse_us: i32)
            -> BridgeCommandResult;

        /// Fire one manual sorter pulse (fails without an attached camera).
        fn trigger_manual_pulse(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;

        /// Start/stop the periodic trigger test generator.
        fn trigger_periodic_start(self: Pin<&mut BackendBridge>, interval_ms: i32)
            -> BridgeCommandResult;
        fn trigger_periodic_stop(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;

        /// Pull the sorter trigger status snapshot.
        fn fetch_trigger_status(self: Pin<&mut BackendBridge>) -> BridgeTriggerStatus;

        /// Drain and return all events queued since the last poll. The queue is
        /// bounded drop-oldest (`MIB_BRIDGE_MAX_QUEUE`, default 4096); when
        /// events were dropped, the batch starts with a `QueueOverflow` marker.
        fn poll_events(self: Pin<&mut BackendBridge>) -> Vec<BridgeEvent>;

        /// Total events dropped by the bounded queue since construction.
        fn queue_overflow_total(&self) -> u64;

        /// Pull the latest frame's metadata + pixel bytes (one copy).
        fn close_review(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;
        fn fetch_preview_buffer(self: Pin<&mut BackendBridge>) -> String;
        fn save_preview_buffer(self: Pin<&mut BackendBridge>, request: &str) -> String;
        fn fetch_latest_frame(self: Pin<&mut BackendBridge>) -> BridgeFrame;

        /// Pull a specific frame by absolute index (metadata + one byte copy).
        fn fetch_frame_by_index(self: Pin<&mut BackendBridge>, frame_index: u64)
            -> BridgeFrame;

        /// Pull the current realtime processing stats (fps + pixel→micron).
        fn fetch_processing_stats(self: Pin<&mut BackendBridge>) -> BridgeProcessingStats;
    }
}

// The bridge object may be MOVED between threads (e.g. held in a Tauri
// `State`/`Mutex`, or an async task), but it is NOT `Sync`: commands funnel
// through the single owner and are not safe to call concurrently. This matches
// the threading contract in ADR 0003 — events are the only thing that fan out,
// and they do so through the shim's own mutex-guarded queue, not through shared
// access to `BackendBridge`. Marking it `Send` (but never `Sync`) is therefore
// sound and is what lets a `Mutex<UniquePtr<BackendBridge>>` be `Send + Sync`.
#[cfg(not(feature = "review-only"))]
unsafe impl Send for ffi::BackendBridge {}

// Compile-time guard for the Tauri consumption pattern: a
// `Mutex<UniquePtr<BackendBridge>>` (what a Tauri `State` holds) must be
// `Send + Sync`. This holds iff `BackendBridge: Send` (above) — and breaks
// loudly if someone ever adds a `Sync` requirement the type can't meet.
#[cfg(not(feature = "review-only"))]
const _: fn() = || {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<std::sync::Mutex<cxx::UniquePtr<ffi::BackendBridge>>>();
};

#[cfg(not(feature = "review-only"))]
fn bytes_to_vec(bytes: &[u8]) -> Vec<u8> {
    bytes.to_vec()
}
