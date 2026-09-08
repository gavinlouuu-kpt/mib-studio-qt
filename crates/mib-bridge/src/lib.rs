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
        /// u3 persistenceCommitted (legacy "validSaved"), u4 0 (legacy
        /// "invalidSaved", split no longer tracked), u5 startWallClockNs;
        /// f0/experiment_end_time_ns endWallClockNs,
        /// f1/experiment_dropped_valid persistence pending,
        /// f2/experiment_dropped_invalid persistenceFailed; b0 flushing,
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
    /// initialized. Legacy fields: `valid_saved` = persistence committed,
    /// `invalid_saved` = 0, `dropped_valid` = persistence pending,
    /// `dropped_invalid` = persistence failed.
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
    }

    /// One discovered camera (schema v7, BE-2). `camera_type` is a contract
    /// `camera_types` value (0 EGrabber, 1 MindVision, 2 Mock — the mock
    /// source is a synthetic always-present entry).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeDiscoveredCamera {
        pub camera_type: u32,
        pub camera_index: i32,
        pub interface_index: i32,
        pub device_index: i32,
        pub interface_id: String,
        pub device_id: String,
        pub model_name: String,
        pub firmware_version: String,
        pub label: String,
    }

    /// One discovered framegrabber stream (schema v7, BE-2).
    #[derive(Debug, Clone, Default)]
    pub struct BridgeDiscoveredFramegrabber {
        pub interface_index: i32,
        pub device_index: i32,
        pub stream_index: i32,
        pub interface_id: String,
        pub device_id: String,
        pub stream_id: String,
        pub model_name: String,
        pub label: String,
    }

    /// Camera discovery result (schema v7, BE-2). Hardware lists are empty on
    /// platforms without the EGrabber/MindVision SDKs.
    #[derive(Debug, Clone, Default)]
    pub struct BridgeCameraDiscovery {
        pub valid: bool,
        pub cameras: Vec<BridgeDiscoveredCamera>,
        pub framegrabbers: Vec<BridgeDiscoveredFramegrabber>,
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
        pub configured_flow_rate: f64,
        pub flow_rate_unit: i32,
        pub direction: u32,
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
        pub valid: bool,
        pub frame_index: u64,
        pub timestamp_ns: u64,
        pub width: u64,
        pub height: u64,
        pub pixel_format: u64,
        pub stride_bytes: u64,
        pub data: Vec<u8>,
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
        fn bridge_abi_version() -> u32;

        fn initialize(self: Pin<&mut BackendBridge>, data_dir: &str) -> bool;
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

        /// Like `experiment_stop`, but the terminal status is marked cancelled.
        /// The HDF5 file is still finalized so it remains readable.
        fn experiment_cancel(self: Pin<&mut BackendBridge>) -> BridgeCommandResult;

        /// Pull the current experiment lifecycle snapshot (schema v5).
        fn fetch_experiment_status(self: Pin<&mut BackendBridge>) -> BridgeExperimentStatus;

        /// Autofocus / nanopositioner commands (schema v11, BE-8). On
        /// platforms without the Coremor SDK, connect fails with a structured
        /// message and every other command stays safe.
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
        fn pump_connect(
            self: Pin<&mut BackendBridge>,
            pump: u32,
            com_port: i32,
            baud_rate: i32,
            modbus_address: i32,
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
        fn review_export_csv(self: Pin<&mut BackendBridge>, output_path: &str)
            -> BridgeCommandResult;

        /// Pull the full processing configuration document (schema v8, BE-3).
        fn fetch_processing_config_json(self: Pin<&mut BackendBridge>) -> BridgeConfigDocument;

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

        /// Enumerate cameras/framegrabbers (schema v7, BE-2): EGrabber +
        /// MindVision hardware (empty without the SDKs) plus the synthetic
        /// mock entry. Discovery is a pull and touches no selection state.
        fn fetch_camera_discovery(self: Pin<&mut BackendBridge>) -> BridgeCameraDiscovery;

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
unsafe impl Send for ffi::BackendBridge {}

// Compile-time guard for the Tauri consumption pattern: a
// `Mutex<UniquePtr<BackendBridge>>` (what a Tauri `State` holds) must be
// `Send + Sync`. This holds iff `BackendBridge: Send` (above) — and breaks
// loudly if someone ever adds a `Sync` requirement the type can't meet.
const _: fn() = || {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<std::sync::Mutex<cxx::UniquePtr<ffi::BackendBridge>>>();
};
