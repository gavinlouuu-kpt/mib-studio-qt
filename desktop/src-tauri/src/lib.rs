//! MIB Studio desktop shell (React + Tauri v2) — Phase 3 of epic #246.
//!
//! Wraps the Phase 2 Rust ↔ C++ bridge (`mib-bridge`, ADR 0003) as Tauri
//! commands so a React webview can drive the Qt-free C++ backend. The first
//! vertical slice is the **mock camera end to end**: configure a mock camera,
//! start capture, pull frames (raw Mono8 bytes — no per-frame base64), and
//! drain status/frame events.

use std::sync::Mutex;

use mib_bridge::ffi::{self, BridgeEventKind};
use serde::Serialize;
use tauri::ipc::Response;
use tauri::{Manager, State};

mod event_transport;
mod frame_packet;
mod platform;
pub mod updater;

struct AppState {
    bridge: Mutex<cxx::UniquePtr<ffi::BackendBridge>>,

}

/// Flattened command result handed to JS.
#[derive(Serialize, Clone)]
struct CmdResult {
    transport_version: u32,
    ok: bool,
    command: u32,
    message: String,
    /// Non-zero when the command started/targeted a tracked operation
    /// (schema v4) — correlates with OperationStatus events.
    #[serde(serialize_with = "event_transport::serialize_u64")]
    operation_id: u64,
}

impl From<ffi::BridgeCommandResult> for CmdResult {
    fn from(r: ffi::BridgeCommandResult) -> Self {
        CmdResult {
            transport_version: frame_packet::JSON_TRANSPORT_VERSION,
            ok: r.ok,
            command: r.command,
            message: r.message,
            operation_id: r.operation_id,
        }
    }
}

/// Realtime processing stats snapshot for the webview.
#[derive(Serialize, Clone, Default)]
struct ProcessingStats {
    transport_version: u32,
    valid: bool,
    algo_fps1s: Option<f64>,
    valid_fps1s: Option<f64>,
    invalid_fps1s: Option<f64>,
    pixel_to_micron: Option<f64>,
}

fn kind_name(k: BridgeEventKind) -> &'static str {
    match k {
        BridgeEventKind::FrameReady => "FrameReady",
        BridgeEventKind::CameraStatus => "CameraStatus",
        BridgeEventKind::RecordingStatus => "RecordingStatus",
        BridgeEventKind::ProcessingResult => "ProcessingResult",
        BridgeEventKind::PlaybackPosition => "PlaybackPosition",
        BridgeEventKind::BackendError => "BackendError",
        BridgeEventKind::OperationStatus => "OperationStatus",
        BridgeEventKind::QueueOverflow => "QueueOverflow",
        BridgeEventKind::ExperimentStatus => "ExperimentStatus",
        // Fail-safe for additive kinds this build does not know (ADR 0004):
        // consumers must ignore "Unknown" rather than crash.
        _ => "Unknown",
    }
}

#[tauri::command]
fn abi_version() -> u32 {
    ffi::bridge_abi_version()
}

#[tauri::command]
fn is_initialized(state: State<AppState>) -> Result<bool, String> {
    let guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.is_initialized())
}

#[tauri::command]
fn init(
    app: tauri::AppHandle,
    state: State<AppState>,
    data_dir: String,
) -> Result<bool, String> {
    // An empty data_dir means "use the platform app-data dir" — AppBackend
    // rejects an empty path, so resolve it here (found by the Xvfb E2E run:
    // the UI passed "" and init always failed).
    let dir = if data_dir.trim().is_empty() {
        app.path()
            .app_data_dir()
            .map_err(|e| format!("resolve app data dir: {e}"))?
            .to_string_lossy()
            .into_owned()
    } else {
        data_dir
    };
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().initialize(&dir))
}

#[tauri::command]
fn configure_mock(
    state: State<AppState>,
    frame_dir: String,
    frame_interval_ms: i32,
    loop_files: bool,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard
        .pin_mut()
        .configure_mock_camera(&frame_dir, frame_interval_ms, loop_files)
        .into())
}

#[tauri::command]
fn start_capture(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().start_capture().into())
}

#[tauri::command]
fn stop_capture(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().stop_capture().into())
}

#[tauri::command]
fn seek_latest(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().playback_seek_latest().into())
}

#[tauri::command]
fn poll_events() -> Result<(), String> { Err("EVENT_PROTOCOL_UPGRADE_REQUIRED".into()) }

#[tauri::command]
fn poll_events_exact(state: State<AppState>) -> Result<event_transport::EventEnvelope, String> {
    let events = {
        let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
        guard.pin_mut().poll_events()
    };
    Ok(event_transport::encode(events))
}

#[tauri::command]
fn start_recording(state: State<AppState>, file_path: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().start_frame_recording(&file_path).into())
}

#[tauri::command]
fn stop_recording(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().stop_frame_recording().into())
}

#[tauri::command]
fn load_recording(state: State<AppState>, file_path: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().load_recording(&file_path).into())
}

#[tauri::command]
fn seek_index(state: State<AppState>, frame_index: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().playback_seek_index(parse_frame_index(&frame_index)?).into())
}

// Legacy split-cache commands fail explicitly: an old webview must reload,
// never receive a substitute frame from another request.
#[tauri::command]
fn fetch_frame() -> Result<Response, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
#[tauri::command]
fn fetch_frame_by_index() -> Result<Response, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
#[tauri::command]
fn frame_bytes() -> Result<Response, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }

fn parse_frame_index(value: &str) -> Result<u64, String> {
    let n = value.parse::<u64>().map_err(|_| "INVALID_U64".to_string())?;
    if n.to_string() != value { return Err("INVALID_U64".into()); }
    Ok(n)
}

#[tauri::command]
fn fetch_frame_packet(state: State<AppState>) -> Result<Response, String> {
    let frame = {
        let mut bridge = state.bridge.lock().map_err(|e| e.to_string())?;
        bridge.pin_mut().fetch_latest_frame()
    };
    frame_packet::encode(frame, 1).map(Response::new)
}

#[tauri::command]
fn fetch_indexed_frame_packet(state: State<AppState>, frame_index: String) -> Result<Response, String> {
    let index = parse_frame_index(&frame_index)?;
    let frame = {
        let mut bridge = state.bridge.lock().map_err(|e| e.to_string())?;
        bridge.pin_mut().fetch_frame_by_index(index)
    };
    frame_packet::encode(frame, 2).map(Response::new)
}

#[tauri::command]
fn apply_processing(
    state: State<AppState>,
    realtime_enabled: bool,
    pixel_to_micron: f64,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard
        .pin_mut()
        .apply_processing(realtime_enabled, pixel_to_micron)
        .into())
}

/// Experiment lifecycle snapshot for the webview (schema v5, BE-4).
#[derive(Serialize, Clone, Default)]
struct ExperimentStatus {
    transport_version: u32,
    valid: bool,
    state: u32,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    start_time_ns: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    end_time_ns: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    valid_buffered: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    invalid_buffered: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    valid_saved: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    invalid_saved: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    dropped_valid: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    dropped_invalid: u64,
    flushing: bool,
    cancelled: bool,
    output_path: String,
    message: String,
    // ABI 13: the shared coordinator's full status.
    #[serde(serialize_with = "event_transport::serialize_u64")]
    start_generation: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    readiness_generation: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    capture_generation: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    persistence_admitted: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    persistence_committed: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    persistence_failed: u64,
    terminal: bool,
    finalization_ok: bool,
    completion: u32,
    completion_reason: String,
    fault_code: String,
    fault_message: String,
}

/// One readiness gate for the webview (ABI 13).
#[derive(Serialize, Clone, Default)]
struct ReadinessGate {
    id: String,
    status: u32,
    reason: String,
    remediation: String,
}

/// Experiment readiness evaluation for the webview (ABI 13).
#[derive(Serialize, Clone, Default)]
struct ExperimentReadiness {
    transport_version: u32,
    valid: bool,
    ready: bool,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    generation: u64,
    gates: Vec<ReadinessGate>,
}

/// Evaluate experiment readiness for a destination (ABI 13).
#[tauri::command]
fn fetch_experiment_readiness(state: State<AppState>, output_path: String) -> Result<ExperimentReadiness, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let r = guard.pin_mut().fetch_experiment_readiness(&output_path);
    Ok(ExperimentReadiness {
        transport_version: frame_packet::JSON_TRANSPORT_VERSION,
        valid: r.valid,
        ready: r.ready,
        generation: r.generation,
        gates: r
            .gates
            .into_iter()
            .map(|g| ReadinessGate { id: g.id, status: g.status, reason: g.reason, remediation: g.remediation })
            .collect(),
    })
}

/// Start an experiment (backend-owned lifecycle; schema v5).
#[tauri::command]
fn experiment_start(state: State<AppState>, output_path: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().experiment_start(&output_path).into())
}

/// Request an asynchronous experiment stop (final flush + metadata + close).
#[tauri::command]
fn experiment_stop(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().experiment_stop().into())
}

/// Like stop, but the terminal status is marked cancelled.
#[tauri::command]
fn experiment_cancel(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().experiment_cancel().into())
}

/// Pull the current experiment lifecycle snapshot.
#[tauri::command]
fn fetch_experiment_status(state: State<AppState>) -> Result<ExperimentStatus, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let s = guard.pin_mut().fetch_experiment_status();
    Ok(ExperimentStatus {
        transport_version: frame_packet::JSON_TRANSPORT_VERSION,
        valid: s.valid,
        state: s.state,
        start_time_ns: s.start_time_ns,
        end_time_ns: s.end_time_ns,
        valid_buffered: s.valid_buffered,
        invalid_buffered: s.invalid_buffered,
        valid_saved: s.valid_saved,
        invalid_saved: s.invalid_saved,
        dropped_valid: s.dropped_valid,
        dropped_invalid: s.dropped_invalid,
        flushing: s.flushing,
        cancelled: s.cancelled,
        output_path: s.output_path,
        message: s.message,
        start_generation: s.start_generation,
        readiness_generation: s.readiness_generation,
        capture_generation: s.capture_generation,
        persistence_admitted: s.persistence_admitted,
        persistence_committed: s.persistence_committed,
        persistence_failed: s.persistence_failed,
        terminal: s.terminal,
        finalization_ok: s.finalization_ok,
        completion: s.completion,
        completion_reason: s.completion_reason,
        fault_code: s.fault_code,
        fault_message: s.fault_message,
    })
}

/// Autofocus/nanopositioner status for the webview (schema v11, BE-8).
#[derive(Serialize, Clone, Default)]
struct AutofocusStatus {
    valid: bool,
    connected: bool,
    enabled: bool,
    current_voltage: f64,
    com_port: i32,
    average_ring_ratio: f64,
    median_ring_ratio: f64,
    last_ring_ratio_update_us: u64,
    ring_ratio_age_us: u64,
}

/// Autofocus configuration for the webview (schema v11, BE-8).
#[derive(Serialize, serde::Deserialize, Clone, Default)]
struct AutofocusConfig {
    valid: bool,
    focus_setpoint: f64,
    focus_range: f64,
    voltage_step: f64,
    fine_voltage_step: f64,
    max_voltage: f64,
    min_voltage: f64,
    initial_voltage: f64,
    manual_voltage_step: f64,
    ring_ratio_stale_ms: i32,
    require_new_sample_per_step: bool,
    min_samples_per_step: i32,
    safe_shutdown_voltage: f64,
    focus_direction: bool,
}

#[tauri::command]
fn autofocus_connect(
    state: State<AppState>,
    com_port: i32,
    baud_rate: i32,
    device_address: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().autofocus_connect(com_port, baud_rate, device_address).into())
}

#[tauri::command]
fn autofocus_disconnect(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().autofocus_disconnect().into())
}

#[tauri::command]
fn autofocus_set_enabled(state: State<AppState>, enabled: bool) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().autofocus_set_enabled(enabled).into())
}

/// Manual voltage jog: `up == true` increases, else decreases.
#[tauri::command]
fn autofocus_jog(state: State<AppState>, up: bool) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().autofocus_jog(up).into())
}

#[tauri::command]
fn autofocus_set_config(state: State<AppState>, config: AutofocusConfig) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let c = ffi::BridgeAutofocusConfig {
        valid: true,
        focus_setpoint: config.focus_setpoint,
        focus_range: config.focus_range,
        voltage_step: config.voltage_step,
        fine_voltage_step: config.fine_voltage_step,
        max_voltage: config.max_voltage,
        min_voltage: config.min_voltage,
        initial_voltage: config.initial_voltage,
        manual_voltage_step: config.manual_voltage_step,
        ring_ratio_stale_ms: config.ring_ratio_stale_ms,
        require_new_sample_per_step: config.require_new_sample_per_step,
        min_samples_per_step: config.min_samples_per_step,
        safe_shutdown_voltage: config.safe_shutdown_voltage,
        focus_direction: config.focus_direction,
    };
    Ok(guard.pin_mut().autofocus_set_config(c).into())
}

#[tauri::command]
fn fetch_autofocus_status(state: State<AppState>) -> Result<AutofocusStatus, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let s = guard.pin_mut().fetch_autofocus_status();
    Ok(AutofocusStatus {
        valid: s.valid,
        connected: s.connected,
        enabled: s.enabled,
        current_voltage: s.current_voltage,
        com_port: s.com_port,
        average_ring_ratio: s.average_ring_ratio,
        median_ring_ratio: s.median_ring_ratio,
        last_ring_ratio_update_us: s.last_ring_ratio_update_us,
        ring_ratio_age_us: s.ring_ratio_age_us,
    })
}

#[tauri::command]
fn fetch_autofocus_config(state: State<AppState>) -> Result<AutofocusConfig, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let c = guard.pin_mut().fetch_autofocus_config();
    Ok(AutofocusConfig {
        valid: c.valid,
        focus_setpoint: c.focus_setpoint,
        focus_range: c.focus_range,
        voltage_step: c.voltage_step,
        fine_voltage_step: c.fine_voltage_step,
        max_voltage: c.max_voltage,
        min_voltage: c.min_voltage,
        initial_voltage: c.initial_voltage,
        manual_voltage_step: c.manual_voltage_step,
        ring_ratio_stale_ms: c.ring_ratio_stale_ms,
        require_new_sample_per_step: c.require_new_sample_per_step,
        min_samples_per_step: c.min_samples_per_step,
        safe_shutdown_voltage: c.safe_shutdown_voltage,
        focus_direction: c.focus_direction,
    })
}

/// Authoritative per-pump snapshot for the webview (schema v10, BE-7).
#[derive(Serialize, Clone, Default)]
struct PumpStatus {
    valid: bool,
    connected: bool,
    run_status: u32,
    current_flow_rate: f64,
    accumulated_volume: f64,
    min_flow_rate: f64,
    max_flow_rate: f64,
    stalled: bool,
    com_port: i32,
    baud_rate: i32,
    modbus_address: i32,
    configured_flow_rate: f64,
    flow_rate_unit: i32,
    direction: u32,
}

#[tauri::command]
fn pump_connect(
    state: State<AppState>,
    pump: u32,
    com_port: i32,
    baud_rate: i32,
    modbus_address: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_connect(pump, com_port, baud_rate, modbus_address).into())
}

#[tauri::command]
fn pump_disconnect(state: State<AppState>, pump: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_disconnect(pump).into())
}

#[tauri::command]
fn pump_set_flow_rate(
    state: State<AppState>,
    pump: u32,
    rate: f64,
    unit: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_set_flow_rate(pump, rate, unit).into())
}

#[tauri::command]
fn pump_set_direction(state: State<AppState>, pump: u32, direction: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_set_direction(pump, direction).into())
}

#[tauri::command]
fn pump_start(state: State<AppState>, pump: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_start(pump).into())
}

#[tauri::command]
fn pump_stop(state: State<AppState>, pump: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_stop(pump).into())
}

#[tauri::command]
fn pump_purge(state: State<AppState>, pump: u32, direction: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_purge(pump, direction).into())
}

#[tauri::command]
fn pump_stop_purge(state: State<AppState>, pump: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_stop_purge(pump).into())
}

#[tauri::command]
fn pump_set_syringe_volume(
    state: State<AppState>,
    pump: u32,
    volume: i32,
    unit: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_set_syringe_volume(pump, volume, unit).into())
}

#[tauri::command]
fn pump_poll_status(state: State<AppState>, pump: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_poll_status(pump).into())
}

#[tauri::command]
fn fetch_pump_status(state: State<AppState>, pump: u32) -> Result<PumpStatus, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let s = guard.pin_mut().fetch_pump_status(pump);
    Ok(PumpStatus {
        valid: s.valid,
        connected: s.connected,
        run_status: s.run_status,
        current_flow_rate: s.current_flow_rate,
        accumulated_volume: s.accumulated_volume,
        min_flow_rate: s.min_flow_rate,
        max_flow_rate: s.max_flow_rate,
        stalled: s.stalled,
        com_port: s.com_port,
        baud_rate: s.baud_rate,
        modbus_address: s.modbus_address,
        configured_flow_rate: s.configured_flow_rate,
        flow_rate_unit: s.flow_rate_unit,
        direction: s.direction,
    })
}

#[tauri::command]
fn pump_scan_addresses(
    state: State<AppState>,
    com_port: i32,
    baud_rate: i32,
    start_address: i32,
    end_address: i32,
    timeout_ms: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard
        .pin_mut()
        .pump_scan_addresses(com_port, baud_rate, start_address, end_address, timeout_ms)
        .into())
}

/// Per-dataset capabilities of the loaded review file (schema v9, BE-6).
#[derive(Serialize, Clone, Default)]
struct ReviewDatasetInfo {
    present: bool,
    count: u64,
    height: i32,
    width: i32,
    channels: i32,
}

/// Review metadata of the loaded HDF5 file (schema v9, BE-6).
#[derive(Serialize, Clone, Default)]
struct ReviewMetadata {
    valid: bool,
    file_open: bool,
    recording_file: bool,
    start_time_ns: u64,
    end_time_ns: u64,
    total_valid: u64,
    total_invalid: u64,
    roi_x: i32,
    roi_y: i32,
    roi_w: i32,
    roi_h: i32,
    has_background: bool,
    has_core_identity: bool,
    core_version: String,
    core_source: String,
    core_release_tag: String,
    valid_images: ReviewDatasetInfo,
    invalid_images: ReviewDatasetInfo,
    valid_masks: ReviewDatasetInfo,
    invalid_masks: ReviewDatasetInfo,
    recorded_images: ReviewDatasetInfo,
    file_path: String,
}

/// One page of review metrics (schema v9, BE-6).
#[derive(Serialize, Clone, Default)]
struct ReviewMetricsPage {
    valid: bool,
    total: u64,
    offset: u64,
    rows: Vec<MonitoringRow>,
}

fn dataset_info(d: ffi::BridgeReviewDatasetInfo) -> ReviewDatasetInfo {
    ReviewDatasetInfo {
        present: d.present,
        count: d.count,
        height: d.height,
        width: d.width,
        channels: d.channels,
    }
}

/// Pull the review metadata of the loaded HDF5 file.
#[tauri::command]
fn fetch_review_metadata(state: State<AppState>) -> Result<ReviewMetadata, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let m = guard.pin_mut().fetch_review_metadata();
    Ok(ReviewMetadata {
        valid: m.valid,
        file_open: m.file_open,
        recording_file: m.recording_file,
        start_time_ns: m.start_time_ns,
        end_time_ns: m.end_time_ns,
        total_valid: m.total_valid,
        total_invalid: m.total_invalid,
        roi_x: m.roi_x,
        roi_y: m.roi_y,
        roi_w: m.roi_w,
        roi_h: m.roi_h,
        has_background: m.has_background,
        has_core_identity: m.has_core_identity,
        core_version: m.core_version,
        core_source: m.core_source,
        core_release_tag: m.core_release_tag,
        valid_images: dataset_info(m.valid_images),
        invalid_images: dataset_info(m.invalid_images),
        valid_masks: dataset_info(m.valid_masks),
        invalid_masks: dataset_info(m.invalid_masks),
        recorded_images: dataset_info(m.recorded_images),
        file_path: m.file_path,
    })
}

/// Pull one bounded page of review metrics.
#[tauri::command]
fn fetch_review_metrics_page(
    state: State<AppState>,
    valid: bool,
    offset: u64,
    count: u64,
) -> Result<ReviewMetricsPage, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let p = guard.pin_mut().fetch_review_metrics_page(valid, offset, count);
    Ok(ReviewMetricsPage {
        valid: p.valid,
        total: p.total,
        offset: p.offset,
        rows: p
            .rows
            .into_iter()
            .map(|r| MonitoringRow {
                frame_index: r.frame_index,
                timestamp_ns: r.timestamp_ns,
                valid: r.valid,
                target_group: r.target_group,
                object_id: r.object_id,
                object_count: r.object_count,
                track_id: r.track_id,
                centroid_x: r.centroid_x,
                centroid_y: r.centroid_y,
                area: r.area,
                deformability: r.deformability,
                area_ratio: r.area_ratio,
                ring_ratio: r.ring_ratio,
                youngs_modulus: r.youngs_modulus,
            })
            .collect(),
    })
}

#[tauri::command]
fn fetch_review_image() -> Result<Response, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
#[tauri::command]
fn review_image_bytes() -> Result<Response, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
#[tauri::command]
fn fetch_review_frame_packet(state: State<AppState>, dataset: u32, index: String) -> Result<Response, String> {
    let index = parse_frame_index(&index)?;
    let frame = {
        let mut bridge = state.bridge.lock().map_err(|e| e.to_string())?;
        bridge.pin_mut().fetch_review_image(dataset, index)
    };
    frame_packet::encode(frame, 3).map(Response::new)
}

/// Start a cancellable metrics CSV export job for the loaded file.
#[tauri::command]
fn review_export_csv(state: State<AppState>, output_path: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_export_csv(&output_path).into())
}

/// Full processing configuration document (schema v8, BE-3).
#[derive(Serialize, Clone, Default)]
struct ConfigDocument {
    valid: bool,
    json: String,
}

/// Processing-core identity/pin status (schema v8, BE-3).
#[derive(Serialize, Clone, Default)]
struct ProcessingCoreStatus {
    valid: bool,
    active_version: String,
    contract_version: u32,
    engine_abi_version: u32,
    source: String,
    release_tag: String,
    build_id: String,
    artifact_sha256: String,
    required_version: String,
    pin_satisfied: bool,
}

/// Pull the full processing configuration document (lossless JSON).
#[tauri::command]
fn fetch_processing_config_json(state: State<AppState>) -> Result<ConfigDocument, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let d = guard.pin_mut().fetch_processing_config_json();
    Ok(ConfigDocument { valid: d.valid, json: d.json })
}

/// Merge-apply a processing configuration document.
#[tauri::command]
fn apply_processing_config_json(state: State<AppState>, json: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().apply_processing_config_json(&json).into())
}

/// Set (or clear, with w/h == 0) the realtime processing ROI.
#[tauri::command]
fn set_processing_roi(
    state: State<AppState>,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().set_processing_roi(x, y, w, h).into())
}

#[tauri::command]
fn fetch_background() -> Result<Response, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
#[tauri::command]
fn background_bytes() -> Result<Response, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
#[tauri::command]
fn fetch_background_packet(state: State<AppState>) -> Result<Response, String> {
    let frame = {
        let mut bridge = state.bridge.lock().map_err(|e| e.to_string())?;
        bridge.pin_mut().fetch_background_image()
    };
    frame_packet::encode(frame, 4).map(Response::new)
}

/// Set the processing background from the latest live frame — the operator's
/// "Set Background" action. Pixels stay on the Rust side (no webview copy).
#[tauri::command]
fn set_background_from_current_frame(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let frame = guard.pin_mut().fetch_latest_frame();
    if !frame.valid {
        return Ok(CmdResult {
            transport_version: frame_packet::JSON_TRANSPORT_VERSION,
            ok: false,
            command: 2, // ProcessingSettings
            message: "No live frame available to capture as background".into(),
            operation_id: 0,
        });
    }
    // The bridge background path expects tightly-packed Mono8 (len == w*h).
    let width = frame.width as usize;
    let height = frame.height as usize;
    let stride = if frame.stride_bytes > 0 { frame.stride_bytes as usize } else { width };
    let mut packed = Vec::with_capacity(width * height);
    for row in 0..height {
        let start = row * stride;
        packed.extend_from_slice(&frame.data[start..start + width]);
    }
    Ok(guard
        .pin_mut()
        .set_background_image(frame.width, frame.height, &packed)
        .into())
}

/// Clear the processing background image.
#[tauri::command]
fn clear_background_image(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().clear_background_image().into())
}

/// Pull the processing-core identity/pin status.
#[tauri::command]
fn fetch_processing_core_status(state: State<AppState>) -> Result<ProcessingCoreStatus, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let s = guard.pin_mut().fetch_processing_core_status();
    Ok(ProcessingCoreStatus {
        valid: s.valid,
        active_version: s.active_version,
        contract_version: s.contract_version,
        engine_abi_version: s.engine_abi_version,
        source: s.source,
        release_tag: s.release_tag,
        build_id: s.build_id,
        artifact_sha256: s.artifact_sha256,
        required_version: s.required_version,
        pin_satisfied: s.pin_satisfied,
    })
}

/// One discovered camera for the webview (schema v7, BE-2).
#[derive(Serialize, Clone, Default)]
struct DiscoveredCamera {
    camera_type: u32,
    camera_index: i32,
    interface_index: i32,
    device_index: i32,
    interface_id: String,
    device_id: String,
    model_name: String,
    firmware_version: String,
    label: String,
}

/// One discovered framegrabber stream for the webview (schema v7, BE-2).
#[derive(Serialize, Clone, Default)]
struct DiscoveredFramegrabber {
    interface_index: i32,
    device_index: i32,
    stream_index: i32,
    interface_id: String,
    device_id: String,
    stream_id: String,
    model_name: String,
    label: String,
}

/// Camera discovery result for the webview (schema v7, BE-2).
#[derive(Serialize, Clone, Default)]
struct CameraDiscovery {
    valid: bool,
    cameras: Vec<DiscoveredCamera>,
    framegrabbers: Vec<DiscoveredFramegrabber>,
}

/// Authoritative selected-device snapshot for the webview (schema v7, BE-2).
#[derive(Serialize, Clone, Default)]
struct CameraSelection {
    valid: bool,
    mode: u32,
    interface_index: i32,
    device_index: i32,
    label: String,
    mindvision_index: i32,
    mindvision_config_path: String,
    camera_script_path: String,
    mock_frame_dir: String,
    mock_interval_ms: i32,
    mock_loop: bool,
    configured: bool,
    running: bool,
}

/// Enumerate cameras/framegrabbers (EGrabber + MindVision + the mock entry).
#[tauri::command]
fn fetch_camera_discovery(state: State<AppState>) -> Result<CameraDiscovery, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let d = guard.pin_mut().fetch_camera_discovery();
    Ok(CameraDiscovery {
        valid: d.valid,
        cameras: d
            .cameras
            .into_iter()
            .map(|c| DiscoveredCamera {
                camera_type: c.camera_type,
                camera_index: c.camera_index,
                interface_index: c.interface_index,
                device_index: c.device_index,
                interface_id: c.interface_id,
                device_id: c.device_id,
                model_name: c.model_name,
                firmware_version: c.firmware_version,
                label: c.label,
            })
            .collect(),
        framegrabbers: d
            .framegrabbers
            .into_iter()
            .map(|g| DiscoveredFramegrabber {
                interface_index: g.interface_index,
                device_index: g.device_index,
                stream_index: g.stream_index,
                interface_id: g.interface_id,
                device_id: g.device_id,
                stream_id: g.stream_id,
                model_name: g.model_name,
                label: g.label,
            })
            .collect(),
    })
}

/// Pull the authoritative selected-device snapshot.
#[tauri::command]
fn fetch_camera_selection(state: State<AppState>) -> Result<CameraSelection, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let s = guard.pin_mut().fetch_camera_selection();
    Ok(CameraSelection {
        valid: s.valid,
        mode: s.mode,
        interface_index: s.interface_index,
        device_index: s.device_index,
        label: s.label,
        mindvision_index: s.mindvision_index,
        mindvision_config_path: s.mindvision_config_path,
        camera_script_path: s.camera_script_path,
        mock_frame_dir: s.mock_frame_dir,
        mock_interval_ms: s.mock_interval_ms,
        mock_loop: s.mock_loop,
        configured: s.configured,
        running: s.running,
    })
}

/// Select a hardware (EGrabber) camera.
#[tauri::command]
fn select_hardware_camera(
    state: State<AppState>,
    interface_index: i32,
    device_index: i32,
    label: String,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard
        .pin_mut()
        .select_hardware_camera(interface_index, device_index, &label)
        .into())
}

/// Select a MindVision camera (optionally applying a JSON config).
#[tauri::command]
fn select_mindvision_camera(
    state: State<AppState>,
    camera_index: i32,
    label: String,
    config_path: String,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard
        .pin_mut()
        .select_mindvision_camera(camera_index, &label, &config_path)
        .into())
}

/// Apply a JS camera script to the selected hardware camera.
#[tauri::command]
fn apply_camera_script(state: State<AppState>, script_path: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().apply_camera_script(&script_path).into())
}

/// Issue a GenICam DeviceReset to the selected hardware camera.
#[tauri::command]
fn reset_hardware_camera(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().reset_hardware_camera().into())
}

/// One monitoring metric row for the webview (schema v6, BE-5).
#[derive(Serialize, Clone, Default)]
struct MonitoringRow {
    frame_index: u64,
    timestamp_ns: u64,
    valid: bool,
    target_group: bool,
    object_id: i32,
    object_count: i32,
    track_id: i32,
    centroid_x: f64,
    centroid_y: f64,
    area: f64,
    deformability: f64,
    area_ratio: f64,
    ring_ratio: f64,
    youngs_modulus: f64,
}

/// Bounded monitoring snapshot for the webview (schema v6, BE-5).
#[derive(Serialize, Clone, Default)]
struct MonitoringSnapshot {
    valid: bool,
    monitoring_active: bool,
    valid_held: u64,
    invalid_held: u64,
    valid_appended: u64,
    invalid_appended: u64,
    capacity: u64,
    latest_timestamp_ns: u64,
    rows: Vec<MonitoringRow>,
}

/// Sorter trigger status for the webview (schema v6, BE-5).
#[derive(Serialize, Clone, Default)]
struct TriggerStatus {
    valid: bool,
    camera_attached: bool,
    pulse_duration_us: i32,
    trigger_count: u64,
    last_onset_us: f64,
    last_object_id: i32,
    last_track_id: i32,
    periodic_active: bool,
    periodic_interval_ms: i32,
}

/// Enable/disable monitoring accumulation (visibility-gated by the UI).
#[tauri::command]
fn monitoring_set_active(state: State<AppState>, active: bool) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().monitoring_set_active(active).into())
}

/// Atomically clear the monitoring buffers.
#[tauri::command]
fn monitoring_clear(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().monitoring_clear().into())
}

/// Pull a bounded monitoring snapshot (metrics only — never image payloads).
#[tauri::command]
fn fetch_monitoring_snapshot(
    state: State<AppState>,
    max_rows: u64,
) -> Result<MonitoringSnapshot, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let s = guard.pin_mut().fetch_monitoring_snapshot(max_rows);
    Ok(MonitoringSnapshot {
        valid: s.valid,
        monitoring_active: s.monitoring_active,
        valid_held: s.valid_held,
        invalid_held: s.invalid_held,
        valid_appended: s.valid_appended,
        invalid_appended: s.invalid_appended,
        capacity: s.capacity,
        latest_timestamp_ns: s.latest_timestamp_ns,
        rows: s
            .rows
            .into_iter()
            .map(|r| MonitoringRow {
                frame_index: r.frame_index,
                timestamp_ns: r.timestamp_ns,
                valid: r.valid,
                target_group: r.target_group,
                object_id: r.object_id,
                object_count: r.object_count,
                track_id: r.track_id,
                centroid_x: r.centroid_x,
                centroid_y: r.centroid_y,
                area: r.area,
                deformability: r.deformability,
                area_ratio: r.area_ratio,
                ring_ratio: r.ring_ratio,
                youngs_modulus: r.youngs_modulus,
            })
            .collect(),
    })
}

/// Set the sorter trigger pulse duration (µs).
#[tauri::command]
fn trigger_set_pulse_duration(state: State<AppState>, pulse_us: i32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().trigger_set_pulse_duration(pulse_us).into())
}

/// Fire one manual sorter pulse.
#[tauri::command]
fn trigger_manual_pulse(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().trigger_manual_pulse().into())
}

/// Start the periodic trigger test generator.
#[tauri::command]
fn trigger_periodic_start(state: State<AppState>, interval_ms: i32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().trigger_periodic_start(interval_ms).into())
}

/// Stop the periodic trigger test generator.
#[tauri::command]
fn trigger_periodic_stop(state: State<AppState>) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().trigger_periodic_stop().into())
}

/// Pull the sorter trigger status snapshot.
#[tauri::command]
fn fetch_trigger_status(state: State<AppState>) -> Result<TriggerStatus, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let s = guard.pin_mut().fetch_trigger_status();
    Ok(TriggerStatus {
        valid: s.valid,
        camera_attached: s.camera_attached,
        pulse_duration_us: s.pulse_duration_us,
        trigger_count: s.trigger_count,
        last_onset_us: s.last_onset_us,
        last_object_id: s.last_object_id,
        last_track_id: s.last_track_id,
        periodic_active: s.periodic_active,
        periodic_interval_ms: s.periodic_interval_ms,
    })
}

/// Request cancellation of a tracked operation (schema v4). Fails safely for
/// unknown/finished IDs.
#[tauri::command]
fn cancel_operation(state: State<AppState>, operation_id: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().cancel_operation(parse_frame_index(&operation_id)?).into())
}

/// Total events dropped by the bounded bridge queue (schema v4 observability).
#[tauri::command]
fn queue_overflow_total(state: State<AppState>) -> Result<String, String> {
    let guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.queue_overflow_total().to_string())
}

#[tauri::command]
fn fetch_processing_stats(state: State<AppState>) -> Result<ProcessingStats, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let s = guard.pin_mut().fetch_processing_stats();
    Ok(ProcessingStats {
        transport_version: frame_packet::JSON_TRANSPORT_VERSION,
        valid: s.valid,
        algo_fps1s: if s.valid && s.algo_fps1s.is_finite() { Some(s.algo_fps1s) } else { None },
        valid_fps1s: if s.valid && s.valid_fps1s.is_finite() { Some(s.valid_fps1s) } else { None },
        invalid_fps1s: if s.valid && s.invalid_fps1s.is_finite() { Some(s.invalid_fps1s) } else { None },
        pixel_to_micron: if s.valid && s.pixel_to_micron.is_finite() { Some(s.pixel_to_micron) } else { None },
    })
}

#[cfg(test)]
mod tests {
    use super::kind_name;
    use mib_bridge::ffi::{self, BridgeEventKind};
    use serial_test::serial;
    use std::time::{Duration, Instant};

    #[test]
    fn event_kind_names_are_stable() {
        assert_eq!(kind_name(BridgeEventKind::FrameReady), "FrameReady");
        assert_eq!(kind_name(BridgeEventKind::CameraStatus), "CameraStatus");
        assert_eq!(kind_name(BridgeEventKind::BackendError), "BackendError");
        assert_eq!(kind_name(BridgeEventKind::OperationStatus), "OperationStatus");
        assert_eq!(kind_name(BridgeEventKind::QueueOverflow), "QueueOverflow");
        // Additive kinds from a newer bridge fail safely as "Unknown" (ADR
        // 0004) — consumers ignore them instead of crashing.
        assert_eq!(kind_name(BridgeEventKind { repr: 9999 }), "Unknown");
    }

    // Headless proof that the desktop crate links the bridge and the mock-camera
    // vertical slice works end to end (no Tauri runtime, no display).
    #[test]
    #[serial]
    fn mock_camera_slice_round_trip() {
        let sample = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../data/mock_frames/frame_00000.tiff");
        assert!(sample.exists(), "sample frame missing: {}", sample.display());
        let dir = std::env::temp_dir().join(format!("mib_desktop_slice_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        for i in 0..4 {
            std::fs::copy(&sample, dir.join(format!("frame_{i:04}.tiff"))).unwrap();
        }
        let data = std::env::temp_dir().join(format!("mib_desktop_data_{}", std::process::id()));

        let mut bridge = ffi::new_backend_bridge();
        assert!(bridge.pin_mut().initialize(&data.to_string_lossy()));
        assert!(bridge
            .pin_mut()
            .configure_mock_camera(&dir.to_string_lossy(), 5, true)
            .ok);
        assert!(bridge.pin_mut().start_capture().ok);

        let mut got = false;
        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline {
            let f = bridge.pin_mut().fetch_latest_frame();
            if f.valid {
                assert_eq!(f.width, 512);
                assert_eq!(f.height, 96);
                assert!(!f.data.is_empty());
                got = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(got, "no live frame within 5s");

        assert!(bridge.pin_mut().stop_capture().ok);
        bridge.pin_mut().shutdown();
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(&data);
    }

    // Headless proof of the recording + review slice: record a clip, load it
    // back, and pull a frame by index.
    #[test]
    #[serial]
    fn record_and_review_round_trip() {
        let sample = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../data/mock_frames/frame_00000.tiff");
        let dir = std::env::temp_dir().join(format!("mib_desktop_rev_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        for i in 0..4 {
            std::fs::copy(&sample, dir.join(format!("frame_{i:04}.tiff"))).unwrap();
        }
        let data = std::env::temp_dir().join(format!("mib_desktop_rev_data_{}", std::process::id()));
        let rec = std::env::temp_dir().join(format!("mib_desktop_rev_{}.h5", std::process::id()));

        let mut bridge = ffi::new_backend_bridge();
        assert!(bridge.pin_mut().initialize(&data.to_string_lossy()));
        assert!(bridge
            .pin_mut()
            .configure_mock_camera(&dir.to_string_lossy(), 5, true)
            .ok);
        assert!(bridge.pin_mut().start_capture().ok);

        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline && !bridge.pin_mut().fetch_latest_frame().valid {
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(bridge.pin_mut().start_frame_recording(&rec.to_string_lossy()).ok);
        std::thread::sleep(Duration::from_millis(200));
        assert!(bridge.pin_mut().stop_frame_recording().ok);
        assert!(bridge.pin_mut().stop_capture().ok);

        assert!(bridge.pin_mut().load_recording(&rec.to_string_lossy()).ok);
        assert!(bridge.pin_mut().playback_seek_index(0).ok);
        let frame = bridge.pin_mut().fetch_frame_by_index(0);
        assert!(frame.valid && frame.width == 512 && frame.height == 96);

        bridge.pin_mut().shutdown();
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(&data);
        let _ = std::fs::remove_file(&rec);
    }

    // Headless proof of the processing slice: apply settings, then pull stats.
    #[test]
    #[serial]
    fn processing_settings_round_trip() {
        let sample = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../data/mock_frames/frame_00000.tiff");
        let dir = std::env::temp_dir().join(format!("mib_desktop_proc_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        for i in 0..4 {
            std::fs::copy(&sample, dir.join(format!("frame_{i:04}.tiff"))).unwrap();
        }
        let data = std::env::temp_dir().join(format!("mib_desktop_proc_data_{}", std::process::id()));

        let mut bridge = ffi::new_backend_bridge();
        assert!(bridge.pin_mut().initialize(&data.to_string_lossy()));
        assert!(bridge
            .pin_mut()
            .configure_mock_camera(&dir.to_string_lossy(), 5, true)
            .ok);
        assert!(bridge.pin_mut().apply_processing(true, 3.0).ok);
        assert!(bridge.pin_mut().start_capture().ok);
        std::thread::sleep(Duration::from_millis(150));

        let stats = bridge.pin_mut().fetch_processing_stats();
        assert!(stats.valid);
        assert!((stats.pixel_to_micron - 3.0).abs() < 1e-9);

        assert!(bridge.pin_mut().stop_capture().ok);
        bridge.pin_mut().shutdown();
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(&data);
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .manage(AppState {
            bridge: Mutex::new(ffi::new_backend_bridge()),
        })
        .invoke_handler(tauri::generate_handler![
            abi_version,
            is_initialized,
            init,
            configure_mock,
            start_capture,
            stop_capture,
            seek_latest,
            poll_events,
            poll_events_exact,
            fetch_frame_packet,
            fetch_indexed_frame_packet,
            fetch_review_frame_packet,
            fetch_background_packet,
            fetch_frame,
            frame_bytes,
            start_recording,
            stop_recording,
            load_recording,
            seek_index,
            fetch_frame_by_index,
            apply_processing,
            fetch_processing_stats,
            cancel_operation,
            queue_overflow_total,
            platform::app_paths,
            platform::get_preferences,
            platform::set_preferences,
            platform::shell_log,
            experiment_start,
            experiment_stop,
            experiment_cancel,
            fetch_experiment_status,
            fetch_experiment_readiness,
            autofocus_connect,
            autofocus_disconnect,
            autofocus_set_enabled,
            autofocus_jog,
            autofocus_set_config,
            fetch_autofocus_status,
            fetch_autofocus_config,
            pump_connect,
            pump_disconnect,
            pump_set_flow_rate,
            pump_set_direction,
            pump_start,
            pump_stop,
            pump_purge,
            pump_stop_purge,
            pump_set_syringe_volume,
            pump_poll_status,
            fetch_pump_status,
            pump_scan_addresses,
            fetch_review_metadata,
            fetch_review_metrics_page,
            fetch_review_image,
            review_image_bytes,
            review_export_csv,
            fetch_processing_config_json,
            apply_processing_config_json,
            set_processing_roi,
            fetch_background,
            background_bytes,
            set_background_from_current_frame,
            clear_background_image,
            fetch_processing_core_status,
            fetch_camera_discovery,
            fetch_camera_selection,
            select_hardware_camera,
            select_mindvision_camera,
            apply_camera_script,
            reset_hardware_camera,
            monitoring_set_active,
            monitoring_clear,
            fetch_monitoring_snapshot,
            trigger_set_pulse_duration,
            trigger_manual_pulse,
            trigger_periodic_start,
            trigger_periodic_stop,
            fetch_trigger_status,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
