//! Transport-neutral command layer over the backend bridge (`mib-bridge`, ADR 0003).
//!
//! Extracted from the Tauri shell for YOFO Studio (impl spec S5): the desktop app exposes these
//! functions as Tauri commands, and `mib-bridge-server` exposes the same functions over a
//! WebSocket through [`dispatch::dispatch`]. One implementation, two transports.

use std::sync::Mutex;

use mib_bridge::ffi::{self, BridgeEventKind};
use serde::{Deserialize, Serialize};

pub mod camera_document;
pub mod config_document;
pub mod dispatch;
pub mod event_transport;
pub mod frame_packet;
pub mod preview_buffer;


/// The one backend bridge a process owns. Every transport (the Tauri webview, the YOFO Studio
/// WebSocket server) serialises its calls through this mutex.
pub struct AppState {
    pub bridge: Mutex<cxx::UniquePtr<ffi::BackendBridge>>,
}

impl AppState {
    pub fn new() -> Self {
        AppState { bridge: Mutex::new(ffi::new_backend_bridge()) }
    }
}

impl Default for AppState {
    fn default() -> Self {
        Self::new()
    }
}

/// Flattened command result handed to JS.
#[derive(Serialize, Clone)]
pub struct CmdResult {
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
pub struct ProcessingStats {
    transport_version: u32,
    valid: bool,
    algo_fps1s: Option<f64>,
    valid_fps1s: Option<f64>,
    invalid_fps1s: Option<f64>,
    pixel_to_micron: Option<f64>,
}

pub fn kind_name(k: BridgeEventKind) -> &'static str {
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

pub fn abi_version() -> u32 {
    ffi::bridge_abi_version()
}

pub fn is_initialized(state: &AppState) -> Result<bool, String> {
    let guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.is_initialized())
}

/// Initialise the backend. The host resolves an empty `data_dir` (AppBackend rejects an empty
/// path; found by the Xvfb E2E run: the UI passed "" and init always failed).
pub fn init(state: &AppState, data_dir: &str, resource_dir: &str) -> Result<bool, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().initialize_with_resources(data_dir, resource_dir))
}

pub fn configure_mock(
    state: &AppState,
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

pub fn start_capture(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().start_capture().into())
}

pub fn stop_capture(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().stop_capture().into())
}

pub fn seek_latest(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().playback_seek_latest().into())
}

pub fn poll_events() -> Result<(), String> { Err("EVENT_PROTOCOL_UPGRADE_REQUIRED".into()) }

pub fn poll_events_exact(state: &AppState) -> Result<event_transport::EventEnvelope, String> {
    let events = {
        let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
        guard.pin_mut().poll_events()
    };
    Ok(event_transport::encode(events))
}

pub fn start_recording(state: &AppState, file_path: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().start_frame_recording(&file_path).into())
}

pub fn stop_recording(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().stop_frame_recording().into())
}

pub fn close_review(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().close_review().into())
}

pub fn load_recording(state: &AppState, file_path: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().load_recording(&file_path).into())
}

pub fn seek_index(state: &AppState, frame_index: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().playback_seek_index(parse_frame_index(&frame_index)?).into())
}

// Legacy split-cache commands fail explicitly: an old webview must reload,
// never receive a substitute frame from another request.
pub fn fetch_frame() -> Result<Vec<u8>, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
pub fn fetch_frame_by_index() -> Result<Vec<u8>, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
pub fn frame_bytes() -> Result<Vec<u8>, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }

fn parse_frame_index(value: &str) -> Result<u64, String> {
    let n = value.parse::<u64>().map_err(|_| "INVALID_U64".to_string())?;
    if n.to_string() != value { return Err("INVALID_U64".into()); }
    Ok(n)
}

pub fn fetch_frame_packet(state: &AppState) -> Result<Vec<u8>, String> {
    let frame = {
        let mut bridge = state.bridge.lock().map_err(|e| e.to_string())?;
        bridge.pin_mut().fetch_latest_frame()
    };
    frame_packet::encode(frame, 1)
}

pub fn fetch_indexed_frame_packet(state: &AppState, frame_index: String) -> Result<Vec<u8>, String> {
    let index = parse_frame_index(&frame_index)?;
    let frame = {
        let mut bridge = state.bridge.lock().map_err(|e| e.to_string())?;
        bridge.pin_mut().fetch_frame_by_index(index)
    };
    frame_packet::encode(frame, 2)
}

pub fn apply_processing(
    state: &AppState,
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
pub struct ExperimentStatus {
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
    #[serde(serialize_with = "event_transport::serialize_u64")]
    fault_revision: u64,
    fault_code: String,
    fault_message: String,
}

/// One readiness gate for the webview (ABI 13).
#[derive(Serialize, Clone, Default)]
pub struct ReadinessGate {
    id: String,
    status: u32,
    reason: String,
    remediation: String,
}

/// Experiment readiness evaluation for the webview (ABI 13).
#[derive(Serialize, Clone, Default)]
pub struct ExperimentReadiness {
    transport_version: u32,
    valid: bool,
    ready: bool,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    generation: u64,
    gates: Vec<ReadinessGate>,
}

/// Evaluate experiment readiness for a destination (ABI 13).
pub fn fetch_experiment_readiness(state: &AppState, output_path: String) -> Result<ExperimentReadiness, String> {
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
pub fn experiment_start(state: &AppState, output_path: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().experiment_start(&output_path).into())
}

/// Authoritative capture state and retained failure details.
pub fn fetch_capture_lifecycle(state: &AppState) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().fetch_capture_lifecycle()).map_err(|e| e.to_string())
}

pub fn experiment_acknowledge_fault(state: &AppState, expected_run: String, fault_revision: String, code: String, message: String, confirmed: bool) -> Result<CmdResult, String> {
    let revision = fault_revision.parse::<u64>().map_err(|_| "Invalid fault revision".to_string())?;
    let generation = expected_run.parse::<u64>().map_err(|_| "Invalid run generation".to_string())?;
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().experiment_acknowledge_fault(generation, revision, &code, &message, confirmed).into())
}

/// Request asynchronous final flush, metadata persistence and close.
pub fn experiment_stop(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().experiment_stop().into())
}

/// Like stop, but the terminal status is marked cancelled.
pub fn experiment_cancel(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().experiment_cancel().into())
}

/// Pull the current experiment lifecycle snapshot.
pub fn fetch_experiment_status(state: &AppState) -> Result<ExperimentStatus, String> {
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
        fault_revision: s.fault_revision,
        fault_code: s.fault_code,
        fault_message: s.fault_message,
    })
}

/// Autofocus/nanopositioner status for the webview (schema v11, BE-8).
#[derive(Serialize, Clone, Default)]
pub struct AutofocusStatus {
    valid: bool,
    connected: bool,
    enabled: bool,
    current_voltage: f64,
    com_port: i32,
    backend_name: String,
    endpoint_id: String,
    average_ring_ratio: f64,
    median_ring_ratio: f64,
    last_ring_ratio_update_us: u64,
    ring_ratio_age_us: u64,
}

/// Autofocus configuration for the webview (schema v11, BE-8).
#[derive(Serialize, serde::Deserialize, Clone, Default)]
pub struct AutofocusConfig {
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

pub fn set_processed_preview_enabled(state: &AppState, enabled: bool) -> Result<(), String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    guard.pin_mut().set_processed_preview_enabled(enabled); Ok(())
}
pub fn fetch_processed_preview(state: &AppState) -> Result<Vec<u8>, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().fetch_processed_preview())
}

pub fn background_calibration_command(state: &AppState, json: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().background_calibration_command(&json).into())
}
pub fn background_calibration_status(state: &AppState) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().background_calibration_status()).map_err(|e| e.to_string())
}

pub fn startup_discovery_set_preference(state: &AppState, json: String) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().startup_discovery_set_preference(&json)).map_err(|e| e.to_string())
}

pub fn startup_discovery_run(state: &AppState, action: String) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().startup_discovery_run(&action)).map_err(|e| e.to_string())
}
pub fn startup_discovery_status(state: &AppState) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().startup_discovery_status()).map_err(|e| e.to_string())
}

pub fn pulse_generator_command(state: &AppState, json: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pulse_generator_command(&json).into())
}
pub fn pulse_generator_status(state: &AppState) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().pulse_generator_status()).map_err(|e| e.to_string())
}

pub fn autofocus_connect_endpoint(state: &AppState, backend: String, endpoint: String,
    com_port: i32, baud_rate: i32, device_address: i32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().autofocus_connect_endpoint(&backend, &endpoint, com_port, baud_rate, device_address).into())
}

pub fn autofocus_connect(
    state: &AppState,
    com_port: i32,
    baud_rate: i32,
    device_address: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().autofocus_connect(com_port, baud_rate, device_address).into())
}

pub fn autofocus_disconnect(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().autofocus_disconnect().into())
}

pub fn autofocus_set_enabled(state: &AppState, enabled: bool) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().autofocus_set_enabled(enabled).into())
}

/// Manual voltage jog: `up == true` increases, else decreases.
pub fn autofocus_jog(state: &AppState, up: bool) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().autofocus_jog(up).into())
}

pub fn autofocus_set_config(state: &AppState, config: AutofocusConfig) -> Result<CmdResult, String> {
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

pub fn fetch_autofocus_status(state: &AppState) -> Result<AutofocusStatus, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let s = guard.pin_mut().fetch_autofocus_status();
    Ok(AutofocusStatus {
        valid: s.valid,
        connected: s.connected,
        enabled: s.enabled,
        current_voltage: s.current_voltage,
        com_port: s.com_port,
        backend_name: s.backend_name,
        endpoint_id: s.endpoint_id,
        average_ring_ratio: s.average_ring_ratio,
        median_ring_ratio: s.median_ring_ratio,
        last_ring_ratio_update_us: s.last_ring_ratio_update_us,
        ring_ratio_age_us: s.ring_ratio_age_us,
    })
}

pub fn fetch_autofocus_config(state: &AppState) -> Result<AutofocusConfig, String> {
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
pub struct PumpStatus {
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
    port_name: String,
    configured_flow_rate: f64,
    flow_rate_unit: i32,
    direction: u32,
    model: u32,
    microliters_per_rev: f64,
    speed_rpm: f64,
}

pub fn pump_connect_endpoint(state: &AppState, pump: u32, port_name: String, baud_rate: i32, modbus_address: i32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_connect_endpoint(pump, &port_name, baud_rate, modbus_address).into())
}

#[allow(clippy::too_many_arguments)]
pub fn pump_connect_model(
    state: &AppState,
    pump: u32,
    model: u32,
    port_name: String,
    baud_rate: i32,
    modbus_address: i32,
    microliters_per_rev: f64,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard
        .pin_mut()
        .pump_connect_model(pump, model, &port_name, baud_rate, modbus_address, microliters_per_rev)
        .into())
}

pub fn pump_connect(
    state: &AppState,
    pump: u32,
    com_port: i32,
    baud_rate: i32,
    modbus_address: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_connect(pump, com_port, baud_rate, modbus_address).into())
}

pub fn pump_disconnect(state: &AppState, pump: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_disconnect(pump).into())
}

pub fn pump_set_flow_rate(
    state: &AppState,
    pump: u32,
    rate: f64,
    unit: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_set_flow_rate(pump, rate, unit).into())
}

pub fn pump_set_direction(state: &AppState, pump: u32, direction: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_set_direction(pump, direction).into())
}

pub fn pump_start(state: &AppState, pump: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_start(pump).into())
}

pub fn pump_stop(state: &AppState, pump: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_stop(pump).into())
}

pub fn pump_purge(state: &AppState, pump: u32, direction: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_purge(pump, direction).into())
}

pub fn pump_stop_purge(state: &AppState, pump: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_stop_purge(pump).into())
}

pub fn pump_set_syringe_volume(
    state: &AppState,
    pump: u32,
    volume: i32,
    unit: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_set_syringe_volume(pump, volume, unit).into())
}

pub fn pump_poll_status(state: &AppState, pump: u32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().pump_poll_status(pump).into())
}

pub fn fetch_pump_status(state: &AppState, pump: u32) -> Result<PumpStatus, String> {
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
        port_name: s.port_name,
        configured_flow_rate: s.configured_flow_rate,
        flow_rate_unit: s.flow_rate_unit,
        direction: s.direction,
        model: s.model,
        microliters_per_rev: s.microliters_per_rev,
        speed_rpm: s.speed_rpm,
    })
}

pub fn pump_scan_addresses(
    state: &AppState,
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
pub struct ReviewDatasetInfo {
    present: bool,
    count: u64,
    height: i32,
    width: i32,
    channels: i32,
}

/// Review metadata of the loaded HDF5 file (schema v9, BE-6).
#[derive(Serialize, Clone, Default)]
pub struct ReviewMetadata {
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
pub struct ReviewMetricsPage {
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
pub fn fetch_review_metadata(state: &AppState) -> Result<ReviewMetadata, String> {
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
pub fn fetch_review_metrics_page(
    state: &AppState,
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
                pixel_to_micron: r.pixel_to_micron,
            })
            .collect(),
    })
}

pub fn fetch_review_image() -> Result<Vec<u8>, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
pub fn review_image_bytes() -> Result<Vec<u8>, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
pub fn fetch_review_frame_packet(state: &AppState, dataset: u32, index: String) -> Result<Vec<u8>, String> {
    let index = parse_frame_index(&index)?;
    let frame = {
        let mut bridge = state.bridge.lock().map_err(|e| e.to_string())?;
        bridge.pin_mut().fetch_review_image(dataset, index)
    };
    frame_packet::encode(frame, 3)
}

pub fn render_review_overlay(state: &AppState, json: String) -> Result<Vec<u8>, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let bytes = guard.pin_mut().render_review_overlay(&json);
    if bytes.is_empty() { return Err("Saved image/mask unavailable for this source/index".into()); }
    Ok(bytes)
}

pub fn fetch_review_reanalysis_preview(state: &AppState, json: String) -> Result<Vec<u8>, String> {
    let frame = {
        let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
        guard.pin_mut().fetch_review_reanalysis_preview(&json)
    };
    frame_packet::encode(frame, 3)
}

pub fn fetch_monitoring_chart_reference(state: &AppState) -> Result<String, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().fetch_monitoring_chart_reference())
}
pub fn fetch_review_charts_json(state: &AppState) -> Result<String, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().fetch_review_charts_json())
}

pub fn review_reanalysis_json(state: &AppState, json: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_reanalysis_json(&json).into())
}
pub fn review_reanalysis_status_json(state: &AppState) -> Result<String, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_reanalysis_status_json())
}

pub fn review_export_json(state: &AppState, json: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_export_json(&json).into())
}

pub fn review_export_status_json(state: &AppState) -> Result<String, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_export_status_json())
}

/// Start a cancellable metrics CSV export job for the loaded file.
pub fn review_export_csv(state: &AppState, output_path: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_export_csv(&output_path).into())
}

/// Full processing configuration document (schema v8, BE-3).
#[derive(Serialize, Clone, Default)]
pub struct ConfigDocument {
    valid: bool,
    json: String,
}

/// Processing-core identity/pin status (schema v8, BE-3).
#[derive(Serialize, Clone, Default)]
pub struct ProcessingCoreStatus {
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
pub fn fetch_processing_config_json(state: &AppState) -> Result<ConfigDocument, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let d = guard.pin_mut().fetch_processing_config_json();
    Ok(ConfigDocument { valid: d.valid, json: d.json })
}

/// Merge-apply a processing configuration document.
pub fn apply_processing_config_json(state: &AppState, json: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().apply_processing_config_json(&json).into())
}

/// Set (or clear, with w/h == 0) the realtime processing ROI.
pub fn set_processing_roi(
    state: &AppState,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().set_processing_roi(x, y, w, h).into())
}

pub fn fetch_background() -> Result<Vec<u8>, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
pub fn background_bytes() -> Result<Vec<u8>, String> { Err("FRAME_PROTOCOL_UPGRADE_REQUIRED".into()) }
pub fn fetch_background_packet(state: &AppState) -> Result<Vec<u8>, String> {
    let frame = {
        let mut bridge = state.bridge.lock().map_err(|e| e.to_string())?;
        bridge.pin_mut().fetch_background_image()
    };
    frame_packet::encode(frame, 4)
}

/// Set the processing background from the latest live frame — the operator's
/// "Set Background" action. Pixels stay on the Rust side (no webview copy).
pub fn set_background_from_current_frame(state: &AppState) -> Result<CmdResult, String> {
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
pub fn clear_background_image(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().clear_background_image().into())
}

/// Pull the processing-core identity/pin status.
pub fn fetch_processing_core_status(state: &AppState) -> Result<ProcessingCoreStatus, String> {
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

/// Device-discovery request from the webview (schema v14, #419). Camelcase
/// keys match the TypeScript client; every field is optional there.
#[derive(Deserialize, Clone, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct DiscoveryRequest {
    kinds: Vec<u32>,
    providers: Vec<String>,
    has_serial_scope: bool,
    serial_port_name: String,
    baud_rate: i32,
    data_bits: i32,
    parity: String,
    stop_bits: i32,
    address_from: i32,
    address_to: i32,
    per_address_timeout_ms: i32,
    initial_delay_ms: i32,
    deadline_ms: i32,
    max_retries: i32,
    retry_delay_ms: i32,
    origin: String,
}

/// Outcome of starting a discovery job (schema v14).
#[derive(Serialize, Clone, Default)]
pub struct DiscoveryStart {
    accepted: bool,
    coalesced: bool,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    job_id: u64,
    rejection: u32,
    reason: String,
}

/// One discovered device for the webview (schema v14).
#[derive(Serialize, Clone, Default)]
pub struct DiscoveredDevice {
    kind: u32,
    provider_id: String,
    display_name: String,
    system_path: String,
    persistent_id: String,
    sdk_index: i32,
    interface_index: i32,
    device_index: i32,
    stream_index: i32,
    bus_address: i32,
    stable_identity: String,
    identity_strength: u32,
    identification: u32,
    claimed_by: Vec<String>,
    capabilities: Vec<String>,
    synthetic: bool,
    camera_type: i32,
    interface_id: String,
    device_id: String,
    stream_id: String,
    model_name: String,
    firmware_version: String,
    label: String,
}

#[derive(Serialize, Clone, Default)]
pub struct DiscoveryError {
    provider_id: String,
    kind: u32,
    message: String,
    endpoint: String,
}

/// Bounded discovery snapshot for the webview (schema v14).
#[derive(Serialize, Clone, Default)]
pub struct DiscoverySnapshot {
    valid: bool,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    job_id: u64,
    #[serde(serialize_with = "event_transport::serialize_u64")]
    generation: u64,
    state: u32,
    complete: bool,
    overflow: bool,
    attempt: i32,
    max_attempts: i32,
    kinds: Vec<u32>,
    candidates: Vec<DiscoveredDevice>,
    errors: Vec<DiscoveryError>,
    providers_run: Vec<String>,
    origin: String,
}

impl From<ffi::BridgeDiscoveryStart> for DiscoveryStart {
    fn from(s: ffi::BridgeDiscoveryStart) -> Self {
        DiscoveryStart {
            accepted: s.accepted,
            coalesced: s.coalesced,
            job_id: s.job_id,
            rejection: s.rejection,
            reason: s.reason,
        }
    }
}

impl From<ffi::BridgeDiscoverySnapshot> for DiscoverySnapshot {
    fn from(s: ffi::BridgeDiscoverySnapshot) -> Self {
        DiscoverySnapshot {
            valid: s.valid,
            job_id: s.job_id,
            generation: s.generation,
            state: s.state,
            complete: s.complete,
            overflow: s.overflow,
            attempt: s.attempt,
            max_attempts: s.max_attempts,
            kinds: s.kinds,
            candidates: s
                .candidates
                .into_iter()
                .map(|d| DiscoveredDevice {
                    kind: d.kind,
                    provider_id: d.provider_id,
                    display_name: d.display_name,
                    system_path: d.system_path,
                    persistent_id: d.persistent_id,
                    sdk_index: d.sdk_index,
                    interface_index: d.interface_index,
                    device_index: d.device_index,
                    stream_index: d.stream_index,
                    bus_address: d.bus_address,
                    stable_identity: d.stable_identity,
                    identity_strength: d.identity_strength,
                    identification: d.identification,
                    claimed_by: d.claimed_by,
                    capabilities: d.capabilities,
                    synthetic: d.synthetic,
                    camera_type: d.camera_type,
                    interface_id: d.interface_id,
                    device_id: d.device_id,
                    stream_id: d.stream_id,
                    model_name: d.model_name,
                    firmware_version: d.firmware_version,
                    label: d.label,
                })
                .collect(),
            errors: s
                .errors
                .into_iter()
                .map(|e| DiscoveryError {
                    provider_id: e.provider_id,
                    kind: e.kind,
                    message: e.message,
                    endpoint: e.endpoint,
                })
                .collect(),
            providers_run: s.providers_run,
            origin: s.origin,
        }
    }
}

/// Authoritative selected-device snapshot for the webview (schema v7, BE-2).
#[derive(Serialize, Clone, Default)]
pub struct CameraSelection {
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

/// Start a device-discovery job (schema v14, #419); never blocks on hardware.
pub fn start_device_discovery(state: &AppState, request: DiscoveryRequest) -> Result<DiscoveryStart, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let req = ffi::BridgeDiscoveryRequest {
        kinds: request.kinds,
        providers: request.providers,
        has_serial_scope: request.has_serial_scope,
        serial_port_name: request.serial_port_name,
        baud_rate: request.baud_rate,
        data_bits: request.data_bits,
        parity: request.parity.bytes().next().unwrap_or(b'N'),
        stop_bits: request.stop_bits,
        address_from: request.address_from,
        address_to: request.address_to,
        per_address_timeout_ms: request.per_address_timeout_ms,
        initial_delay_ms: request.initial_delay_ms,
        deadline_ms: request.deadline_ms,
        max_retries: request.max_retries,
        retry_delay_ms: request.retry_delay_ms,
        origin: if request.origin.is_empty() { "tauri".to_string() } else { request.origin },
    };
    Ok(guard.pin_mut().start_device_discovery(&req).into())
}

/// Start the camera + framegrabber discovery job (schema v14).
pub fn start_camera_discovery(state: &AppState) -> Result<DiscoveryStart, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().start_camera_discovery().into())
}

/// Poll a discovery job's bounded snapshot (schema v14).
pub fn fetch_device_discovery(state: &AppState, job_id: String) -> Result<DiscoverySnapshot, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().fetch_device_discovery(parse_frame_index(&job_id)?).into())
}

/// Cancel a running discovery job (schema v14); false for unknown/ended jobs.
pub fn cancel_device_discovery(state: &AppState, job_id: String) -> Result<bool, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().cancel_device_discovery(parse_frame_index(&job_id)?))
}

/// Pull the authoritative selected-device snapshot.
pub fn fetch_camera_selection(state: &AppState) -> Result<CameraSelection, String> {
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
pub fn select_hardware_camera(
    state: &AppState,
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
pub fn select_mindvision_camera(
    state: &AppState,
    camera_index: i32,
    label: String,
    config_path: String,
) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let selected = guard.pin_mut().fetch_camera_selection();
    let experiment = guard.pin_mut().fetch_experiment_status();
    if !selected.valid || !experiment.valid || selected.running || matches!(experiment.state, 1..=3) {
        return Err("Stop capture and finalize the experiment before changing camera settings".into());
    }

    Ok(guard
        .pin_mut()
        .select_mindvision_camera(camera_index, &label, &config_path)
        .into())
}

/// Apply a JS camera script to the selected hardware camera.
pub fn apply_camera_script(state: &AppState, script_path: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let selected = guard.pin_mut().fetch_camera_selection();
    let experiment = guard.pin_mut().fetch_experiment_status();
    if !selected.valid || !experiment.valid || selected.running || matches!(experiment.state, 1..=3) {
        return Err("Stop capture and finalize the experiment before changing camera settings".into());
    }

    Ok(guard.pin_mut().apply_camera_script(&script_path).into())
}

/// Explicit MindVision software exposure trigger; never an automatic startup action.
/// Camera & Alignment (ABI 20): full-sensor overview on/off; a running capture restarts.
pub fn set_camera_overview(state: &AppState, overview: bool) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().set_camera_overview(overview).into())
}

/// Save the experiment window (ROI 1, sensor coordinates) placed on the overview.
pub fn save_camera_roi(state: &AppState, x: i32, y: i32, w: i32, h: i32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().save_camera_roi(x, y, w, h).into())
}

/// Where the science runs and what this build has (ABI 21).
pub fn fetch_platform_info(state: &AppState) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().fetch_platform_info()).map_err(|e| e.to_string())
}

/// PZ7035 identity and health for preflight (#501); `available: false` elsewhere.
pub fn fetch_instrument_status(state: &AppState) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().fetch_instrument_status()).map_err(|e| e.to_string())
}

/// Mode, sensor size, saved window, window steps and the camera's last read-back.
pub fn fetch_camera_geometry(state: &AppState) -> Result<serde_json::Value, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    serde_json::from_str(&guard.pin_mut().fetch_camera_geometry()).map_err(|e| e.to_string())
}

pub fn soft_trigger_camera(state: &AppState) -> Result<CmdResult, String> {
    let mut guard=state.bridge.lock().map_err(|e|e.to_string())?;
    let selected=guard.pin_mut().fetch_camera_selection();
    let experiment=guard.pin_mut().fetch_experiment_status();
    if !selected.valid || !selected.configured || selected.mode != 3 || !selected.running || !experiment.valid || matches!(experiment.state,1..=3) {
        return Err("Software trigger requires a running MindVision camera and an idle experiment".into());
    }
    Ok(guard.pin_mut().soft_trigger_camera().into())
}

/// Issue a GenICam DeviceReset to the selected hardware camera.
pub fn reset_hardware_camera(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    let selected = guard.pin_mut().fetch_camera_selection();
    let experiment = guard.pin_mut().fetch_experiment_status();
    if !selected.valid || !experiment.valid || selected.running || matches!(experiment.state, 1..=3) {
        return Err("Stop capture and finalize the experiment before changing camera settings".into());
    }

    Ok(guard.pin_mut().reset_hardware_camera().into())
}

/// One monitoring metric row for the webview (schema v6, BE-5).
#[derive(Serialize, Clone, Default)]
pub struct MonitoringRow {
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
    pixel_to_micron: f64,
}

/// Bounded monitoring snapshot for the webview (schema v6, BE-5).
#[derive(Serialize, Clone, Default)]
pub struct MonitoringSnapshot {
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
pub struct TriggerStatus {
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
pub fn monitoring_set_active(state: &AppState, active: bool) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().monitoring_set_active(active).into())
}

/// Atomically clear the monitoring buffers.
pub fn monitoring_clear(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().monitoring_clear().into())
}

/// Pull a bounded monitoring snapshot (metrics only — never image payloads).
pub fn fetch_monitoring_snapshot(
    state: &AppState,
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
                pixel_to_micron: r.pixel_to_micron,
            })
            .collect(),
    })
}

/// Set the sorter trigger pulse duration (µs).
pub fn trigger_set_pulse_duration(state: &AppState, pulse_us: i32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().trigger_set_pulse_duration(pulse_us).into())
}

/// Fire one manual sorter pulse.
pub fn trigger_manual_pulse(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().trigger_manual_pulse().into())
}

/// Start the periodic trigger test generator.
pub fn trigger_periodic_start(state: &AppState, interval_ms: i32) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().trigger_periodic_start(interval_ms).into())
}

/// Stop the periodic trigger test generator.
pub fn trigger_periodic_stop(state: &AppState) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().trigger_periodic_stop().into())
}

/// Pull the sorter trigger status snapshot.
pub fn fetch_trigger_status(state: &AppState) -> Result<TriggerStatus, String> {
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
pub fn cancel_operation(state: &AppState, operation_id: String) -> Result<CmdResult, String> {
    let mut guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().cancel_operation(parse_frame_index(&operation_id)?).into())
}

/// Total events dropped by the bounded bridge queue (schema v4 observability).
pub fn queue_overflow_total(state: &AppState) -> Result<String, String> {
    let guard = state.bridge.lock().map_err(|e| e.to_string())?;
    Ok(guard.queue_overflow_total().to_string())
}

pub fn fetch_processing_stats(state: &AppState) -> Result<ProcessingStats, String> {
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
