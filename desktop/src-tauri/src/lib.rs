//! MIB Studio desktop shell (React + Tauri v2) — Phase 3 of epic #246.
//!
//! The commands live in `mib-app-commands` (transport-neutral, shared with the YOFO Studio
//! WebSocket server); this crate exposes them as typed Tauri commands and keeps what only a
//! desktop shell has: app paths, preferences, the updater and native installers.

use mib_app_commands as cmds;
use cmds::AppState;
use tauri::ipc::Response;
use tauri::{Manager, State};

mod platform;
mod registry;
mod registry_transport;
pub mod updater;
mod app_update;

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
    let resources = app.path().resource_dir().map_err(|e| e.to_string())?;
    cmds::init(&state, &dir, &resources.to_string_lossy())
}

#[tauri::command]
fn abi_version() -> u32 {
    cmds::abi_version()
}

#[tauri::command]
fn is_initialized(state: State<AppState>) -> Result<bool, String> {
    cmds::is_initialized(&state)
}

#[tauri::command]
fn configure_mock(state: State<AppState>, frame_dir: String, frame_interval_ms: i32, loop_files: bool) -> Result<cmds::CmdResult, String> {
    cmds::configure_mock(&state, frame_dir, frame_interval_ms, loop_files)
}

#[tauri::command]
fn start_capture(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::start_capture(&state)
}

#[tauri::command]
fn stop_capture(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::stop_capture(&state)
}

#[tauri::command]
fn seek_latest(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::seek_latest(&state)
}

#[tauri::command]
fn poll_events() -> Result<(), String> {
    cmds::poll_events()
}

#[tauri::command]
fn poll_events_exact(state: State<AppState>) -> Result<cmds::event_transport::EventEnvelope, String> {
    cmds::poll_events_exact(&state)
}

#[tauri::command]
fn start_recording(state: State<AppState>, file_path: String) -> Result<cmds::CmdResult, String> {
    cmds::start_recording(&state, file_path)
}

#[tauri::command]
fn stop_recording(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::stop_recording(&state)
}

#[tauri::command]
fn close_review(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::close_review(&state)
}

#[tauri::command]
fn load_recording(state: State<AppState>, file_path: String) -> Result<cmds::CmdResult, String> {
    cmds::load_recording(&state, file_path)
}

#[tauri::command]
fn seek_index(state: State<AppState>, frame_index: String) -> Result<cmds::CmdResult, String> {
    cmds::seek_index(&state, frame_index)
}

#[tauri::command]
fn fetch_frame() -> Result<Response, String> {
    cmds::fetch_frame().map(Response::new)
}

#[tauri::command]
fn fetch_frame_by_index() -> Result<Response, String> {
    cmds::fetch_frame_by_index().map(Response::new)
}

#[tauri::command]
fn frame_bytes() -> Result<Response, String> {
    cmds::frame_bytes().map(Response::new)
}

#[tauri::command]
fn fetch_frame_packet(state: State<AppState>) -> Result<Response, String> {
    cmds::fetch_frame_packet(&state).map(Response::new)
}

#[tauri::command]
fn fetch_indexed_frame_packet(state: State<AppState>, frame_index: String) -> Result<Response, String> {
    cmds::fetch_indexed_frame_packet(&state, frame_index).map(Response::new)
}

#[tauri::command]
fn apply_processing(state: State<AppState>, realtime_enabled: bool, pixel_to_micron: f64) -> Result<cmds::CmdResult, String> {
    cmds::apply_processing(&state, realtime_enabled, pixel_to_micron)
}

#[tauri::command]
fn fetch_experiment_readiness(state: State<AppState>, output_path: String) -> Result<cmds::ExperimentReadiness, String> {
    cmds::fetch_experiment_readiness(&state, output_path)
}

#[tauri::command]
fn experiment_start(state: State<AppState>, output_path: String) -> Result<cmds::CmdResult, String> {
    cmds::experiment_start(&state, output_path)
}

#[tauri::command]
fn fetch_capture_lifecycle(state: State<AppState>) -> Result<serde_json::Value, String> {
    cmds::fetch_capture_lifecycle(&state)
}

#[tauri::command]
fn experiment_acknowledge_fault(state: State<AppState>, expected_run: String, fault_revision: String, code: String, message: String, confirmed: bool) -> Result<cmds::CmdResult, String> {
    cmds::experiment_acknowledge_fault(&state, expected_run, fault_revision, code, message, confirmed)
}

#[tauri::command]
fn experiment_stop(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::experiment_stop(&state)
}

#[tauri::command]
fn experiment_cancel(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::experiment_cancel(&state)
}

#[tauri::command]
fn fetch_experiment_status(state: State<AppState>) -> Result<cmds::ExperimentStatus, String> {
    cmds::fetch_experiment_status(&state)
}

#[tauri::command]
fn set_processed_preview_enabled(state: State<AppState>, enabled: bool) -> Result<(), String> {
    cmds::set_processed_preview_enabled(&state, enabled)
}

#[tauri::command]
fn fetch_processed_preview(state: State<AppState>) -> Result<Response, String> {
    cmds::fetch_processed_preview(&state).map(Response::new)
}

#[tauri::command]
fn background_calibration_command(state: State<AppState>, json: String) -> Result<cmds::CmdResult, String> {
    cmds::background_calibration_command(&state, json)
}

#[tauri::command]
fn background_calibration_status(state: State<AppState>) -> Result<serde_json::Value, String> {
    cmds::background_calibration_status(&state)
}

#[tauri::command]
fn startup_discovery_set_preference(state: State<AppState>, json: String) -> Result<serde_json::Value, String> {
    cmds::startup_discovery_set_preference(&state, json)
}

#[tauri::command]
fn startup_discovery_run(state: State<AppState>, action: String) -> Result<serde_json::Value, String> {
    cmds::startup_discovery_run(&state, action)
}

#[tauri::command]
fn startup_discovery_status(state: State<AppState>) -> Result<serde_json::Value, String> {
    cmds::startup_discovery_status(&state)
}

#[tauri::command]
fn pulse_generator_command(state: State<AppState>, json: String) -> Result<cmds::CmdResult, String> {
    cmds::pulse_generator_command(&state, json)
}

#[tauri::command]
fn pulse_generator_status(state: State<AppState>) -> Result<serde_json::Value, String> {
    cmds::pulse_generator_status(&state)
}

#[tauri::command]
fn autofocus_connect_endpoint(state: State<AppState>, backend: String, endpoint: String, com_port: i32, baud_rate: i32, device_address: i32) -> Result<cmds::CmdResult, String> {
    cmds::autofocus_connect_endpoint(&state, backend, endpoint, com_port, baud_rate, device_address)
}

#[tauri::command]
fn autofocus_connect(state: State<AppState>, com_port: i32, baud_rate: i32, device_address: i32) -> Result<cmds::CmdResult, String> {
    cmds::autofocus_connect(&state, com_port, baud_rate, device_address)
}

#[tauri::command]
fn autofocus_disconnect(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::autofocus_disconnect(&state)
}

#[tauri::command]
fn autofocus_set_enabled(state: State<AppState>, enabled: bool) -> Result<cmds::CmdResult, String> {
    cmds::autofocus_set_enabled(&state, enabled)
}

#[tauri::command]
fn autofocus_jog(state: State<AppState>, up: bool) -> Result<cmds::CmdResult, String> {
    cmds::autofocus_jog(&state, up)
}

#[tauri::command]
fn autofocus_set_config(state: State<AppState>, config: cmds::AutofocusConfig) -> Result<cmds::CmdResult, String> {
    cmds::autofocus_set_config(&state, config)
}

#[tauri::command]
fn fetch_autofocus_status(state: State<AppState>) -> Result<cmds::AutofocusStatus, String> {
    cmds::fetch_autofocus_status(&state)
}

#[tauri::command]
fn fetch_autofocus_config(state: State<AppState>) -> Result<cmds::AutofocusConfig, String> {
    cmds::fetch_autofocus_config(&state)
}

#[tauri::command]
fn pump_connect_endpoint(state: State<AppState>, pump: u32, port_name: String, baud_rate: i32, modbus_address: i32) -> Result<cmds::CmdResult, String> {
    cmds::pump_connect_endpoint(&state, pump, port_name, baud_rate, modbus_address)
}

#[tauri::command]
#[allow(clippy::too_many_arguments)]
fn pump_connect_model(state: State<AppState>, pump: u32, model: u32, port_name: String, baud_rate: i32, modbus_address: i32, microliters_per_rev: f64) -> Result<cmds::CmdResult, String> {
    cmds::pump_connect_model(&state, pump, model, port_name, baud_rate, modbus_address, microliters_per_rev)
}

#[tauri::command]
fn pump_connect(state: State<AppState>, pump: u32, com_port: i32, baud_rate: i32, modbus_address: i32) -> Result<cmds::CmdResult, String> {
    cmds::pump_connect(&state, pump, com_port, baud_rate, modbus_address)
}

#[tauri::command]
fn pump_disconnect(state: State<AppState>, pump: u32) -> Result<cmds::CmdResult, String> {
    cmds::pump_disconnect(&state, pump)
}

#[tauri::command]
fn pump_set_flow_rate(state: State<AppState>, pump: u32, rate: f64, unit: i32) -> Result<cmds::CmdResult, String> {
    cmds::pump_set_flow_rate(&state, pump, rate, unit)
}

#[tauri::command]
fn pump_set_direction(state: State<AppState>, pump: u32, direction: u32) -> Result<cmds::CmdResult, String> {
    cmds::pump_set_direction(&state, pump, direction)
}

#[tauri::command]
fn pump_start(state: State<AppState>, pump: u32) -> Result<cmds::CmdResult, String> {
    cmds::pump_start(&state, pump)
}

#[tauri::command]
fn pump_stop(state: State<AppState>, pump: u32) -> Result<cmds::CmdResult, String> {
    cmds::pump_stop(&state, pump)
}

#[tauri::command]
fn pump_purge(state: State<AppState>, pump: u32, direction: u32) -> Result<cmds::CmdResult, String> {
    cmds::pump_purge(&state, pump, direction)
}

#[tauri::command]
fn pump_stop_purge(state: State<AppState>, pump: u32) -> Result<cmds::CmdResult, String> {
    cmds::pump_stop_purge(&state, pump)
}

#[tauri::command]
fn pump_set_syringe_volume(state: State<AppState>, pump: u32, volume: i32, unit: i32) -> Result<cmds::CmdResult, String> {
    cmds::pump_set_syringe_volume(&state, pump, volume, unit)
}

#[tauri::command]
fn pump_poll_status(state: State<AppState>, pump: u32) -> Result<cmds::CmdResult, String> {
    cmds::pump_poll_status(&state, pump)
}

#[tauri::command]
fn fetch_pump_status(state: State<AppState>, pump: u32) -> Result<cmds::PumpStatus, String> {
    cmds::fetch_pump_status(&state, pump)
}

// Z stage (#464, ADR 0013): safety is enforced in the backend (no motion
// before Home, soft limits, Stop always accepted, experiment lock).
#[tauri::command]
fn stage_connect(state: State<AppState>, port_name: String, usb_serial: String, modbus_address: i32) -> Result<cmds::CmdResult, String> {
    cmds::stage_connect(&state, port_name, usb_serial, modbus_address)
}

#[tauri::command]
fn stage_disconnect(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::stage_disconnect(&state)
}

#[tauri::command]
fn stage_move_to(state: State<AppState>, target_um: f64) -> Result<cmds::CmdResult, String> {
    cmds::stage_move_to(&state, target_um)
}

#[tauri::command]
fn stage_move_by(state: State<AppState>, delta_um: f64) -> Result<cmds::CmdResult, String> {
    cmds::stage_move_by(&state, delta_um)
}

#[tauri::command]
fn stage_home(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::stage_home(&state)
}

#[tauri::command]
fn stage_stop(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::stage_stop(&state)
}

#[tauri::command]
fn stage_apply_profile(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::stage_apply_profile(&state)
}

#[tauri::command]
fn fetch_stage_status(state: State<AppState>) -> Result<cmds::StageStatus, String> {
    cmds::fetch_stage_status(&state)
}

#[tauri::command]
fn pump_scan_addresses(state: State<AppState>, com_port: i32, baud_rate: i32, start_address: i32, end_address: i32, timeout_ms: i32) -> Result<cmds::CmdResult, String> {
    cmds::pump_scan_addresses(&state, com_port, baud_rate, start_address, end_address, timeout_ms)
}

#[tauri::command]
fn fetch_review_metadata(state: State<AppState>) -> Result<cmds::ReviewMetadata, String> {
    cmds::fetch_review_metadata(&state)
}

#[tauri::command]
fn fetch_review_metrics_page(state: State<AppState>, valid: bool, offset: u64, count: u64) -> Result<cmds::ReviewMetricsPage, String> {
    cmds::fetch_review_metrics_page(&state, valid, offset, count)
}

#[tauri::command]
fn fetch_review_image() -> Result<Response, String> {
    cmds::fetch_review_image().map(Response::new)
}

#[tauri::command]
fn review_image_bytes() -> Result<Response, String> {
    cmds::review_image_bytes().map(Response::new)
}

#[tauri::command]
fn fetch_review_frame_packet(state: State<AppState>, dataset: u32, index: String) -> Result<Response, String> {
    cmds::fetch_review_frame_packet(&state, dataset, index).map(Response::new)
}

#[tauri::command]
fn render_review_overlay(state: State<AppState>, json: String) -> Result<Response, String> {
    cmds::render_review_overlay(&state, json).map(Response::new)
}

#[tauri::command]
fn fetch_review_reanalysis_preview(state: State<AppState>, json: String) -> Result<Response, String> {
    cmds::fetch_review_reanalysis_preview(&state, json).map(Response::new)
}

#[tauri::command]
fn fetch_monitoring_chart_reference(state: State<AppState>) -> Result<String, String> {
    cmds::fetch_monitoring_chart_reference(&state)
}

#[tauri::command]
fn fetch_review_charts_json(state: State<AppState>) -> Result<String, String> {
    cmds::fetch_review_charts_json(&state)
}

#[tauri::command]
fn review_reanalysis_json(state: State<AppState>, json: String) -> Result<cmds::CmdResult, String> {
    cmds::review_reanalysis_json(&state, json)
}

#[tauri::command]
fn review_reanalysis_status_json(state: State<AppState>) -> Result<String, String> {
    cmds::review_reanalysis_status_json(&state)
}

#[tauri::command]
fn review_export_json(state: State<AppState>, json: String) -> Result<cmds::CmdResult, String> {
    cmds::review_export_json(&state, json)
}

#[tauri::command]
fn review_export_status_json(state: State<AppState>) -> Result<String, String> {
    cmds::review_export_status_json(&state)
}

#[tauri::command]
fn review_export_csv(state: State<AppState>, output_path: String) -> Result<cmds::CmdResult, String> {
    cmds::review_export_csv(&state, output_path)
}

#[tauri::command]
fn fetch_processing_config_json(state: State<AppState>) -> Result<cmds::ConfigDocument, String> {
    cmds::fetch_processing_config_json(&state)
}

#[tauri::command]
fn apply_processing_config_json(state: State<AppState>, json: String) -> Result<cmds::CmdResult, String> {
    cmds::apply_processing_config_json(&state, json)
}

#[tauri::command]
fn set_processing_roi(state: State<AppState>, x: i32, y: i32, w: i32, h: i32) -> Result<cmds::CmdResult, String> {
    cmds::set_processing_roi(&state, x, y, w, h)
}

#[tauri::command]
fn fetch_background() -> Result<Response, String> {
    cmds::fetch_background().map(Response::new)
}

#[tauri::command]
fn background_bytes() -> Result<Response, String> {
    cmds::background_bytes().map(Response::new)
}

#[tauri::command]
fn fetch_background_packet(state: State<AppState>) -> Result<Response, String> {
    cmds::fetch_background_packet(&state).map(Response::new)
}

#[tauri::command]
fn set_background_from_current_frame(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::set_background_from_current_frame(&state)
}

#[tauri::command]
fn clear_background_image(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::clear_background_image(&state)
}

#[tauri::command]
fn fetch_processing_core_status(state: State<AppState>) -> Result<cmds::ProcessingCoreStatus, String> {
    cmds::fetch_processing_core_status(&state)
}

#[tauri::command]
fn start_device_discovery(state: State<AppState>, request: cmds::DiscoveryRequest) -> Result<cmds::DiscoveryStart, String> {
    cmds::start_device_discovery(&state, request)
}

#[tauri::command]
fn start_camera_discovery(state: State<AppState>) -> Result<cmds::DiscoveryStart, String> {
    cmds::start_camera_discovery(&state)
}

#[tauri::command]
fn fetch_device_discovery(state: State<AppState>, job_id: String) -> Result<cmds::DiscoverySnapshot, String> {
    cmds::fetch_device_discovery(&state, job_id)
}

#[tauri::command]
fn cancel_device_discovery(state: State<AppState>, job_id: String) -> Result<bool, String> {
    cmds::cancel_device_discovery(&state, job_id)
}

#[tauri::command]
fn fetch_camera_selection(state: State<AppState>) -> Result<cmds::CameraSelection, String> {
    cmds::fetch_camera_selection(&state)
}

#[tauri::command]
fn select_hardware_camera(state: State<AppState>, interface_index: i32, device_index: i32, label: String) -> Result<cmds::CmdResult, String> {
    cmds::select_hardware_camera(&state, interface_index, device_index, label)
}

#[tauri::command]
fn select_mindvision_camera(state: State<AppState>, camera_index: i32, label: String, config_path: String) -> Result<cmds::CmdResult, String> {
    cmds::select_mindvision_camera(&state, camera_index, label, config_path)
}

#[tauri::command]
fn apply_camera_script(state: State<AppState>, script_path: String) -> Result<cmds::CmdResult, String> {
    cmds::apply_camera_script(&state, script_path)
}

#[tauri::command]
fn set_camera_overview(state: State<AppState>, overview: bool) -> Result<cmds::CmdResult, String> {
    cmds::set_camera_overview(&state, overview)
}

#[tauri::command]
fn save_camera_roi(state: State<AppState>, x: i32, y: i32, w: i32, h: i32) -> Result<cmds::CmdResult, String> {
    cmds::save_camera_roi(&state, x, y, w, h)
}

#[tauri::command]
fn fetch_platform_info(state: State<AppState>) -> Result<serde_json::Value, String> {
    cmds::fetch_platform_info(&state)
}

#[tauri::command]
fn fetch_instrument_status(state: State<AppState>) -> Result<serde_json::Value, String> {
    cmds::fetch_instrument_status(&state)
}

#[tauri::command]
fn fetch_camera_geometry(state: State<AppState>) -> Result<serde_json::Value, String> {
    cmds::fetch_camera_geometry(&state)
}

#[tauri::command]
fn soft_trigger_camera(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::soft_trigger_camera(&state)
}

#[tauri::command]
fn reset_hardware_camera(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::reset_hardware_camera(&state)
}

#[tauri::command]
fn monitoring_set_active(state: State<AppState>, active: bool) -> Result<cmds::CmdResult, String> {
    cmds::monitoring_set_active(&state, active)
}

#[tauri::command]
fn monitoring_clear(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::monitoring_clear(&state)
}

#[tauri::command]
fn fetch_monitoring_snapshot(state: State<AppState>, max_rows: u64) -> Result<cmds::MonitoringSnapshot, String> {
    cmds::fetch_monitoring_snapshot(&state, max_rows)
}

#[tauri::command]
fn trigger_set_pulse_duration(state: State<AppState>, pulse_us: i32) -> Result<cmds::CmdResult, String> {
    cmds::trigger_set_pulse_duration(&state, pulse_us)
}

#[tauri::command]
fn trigger_manual_pulse(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::trigger_manual_pulse(&state)
}

#[tauri::command]
fn trigger_periodic_start(state: State<AppState>, interval_ms: i32) -> Result<cmds::CmdResult, String> {
    cmds::trigger_periodic_start(&state, interval_ms)
}

#[tauri::command]
fn trigger_periodic_stop(state: State<AppState>) -> Result<cmds::CmdResult, String> {
    cmds::trigger_periodic_stop(&state)
}

#[tauri::command]
fn fetch_trigger_status(state: State<AppState>) -> Result<cmds::TriggerStatus, String> {
    cmds::fetch_trigger_status(&state)
}

#[tauri::command]
fn cancel_operation(state: State<AppState>, operation_id: String) -> Result<cmds::CmdResult, String> {
    cmds::cancel_operation(&state, operation_id)
}

#[tauri::command]
fn queue_overflow_total(state: State<AppState>) -> Result<String, String> {
    cmds::queue_overflow_total(&state)
}

#[tauri::command]
fn fetch_processing_stats(state: State<AppState>) -> Result<cmds::ProcessingStats, String> {
    cmds::fetch_processing_stats(&state)
}

#[tauri::command]
fn fetch_config_document(state: State<AppState>, path: String) -> Result<cmds::config_document::ConfigDocument, String> {
    cmds::config_document::fetch_config_document(&state, path)
}

#[tauri::command]
fn apply_config_document(state: State<AppState>, path: String, baseline: String, patch: String) -> Result<cmds::config_document::ConfigTransactionResult, String> {
    cmds::config_document::apply_config_document(&state, path, baseline, patch)
}

#[tauri::command]
fn profile_command(state: State<AppState>, base: String, request: String) -> Result<serde_json::Value, String> {
    cmds::config_document::profile_command(&state, base, request)
}

#[tauri::command]
async fn profile_fetch_url(url: String) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || cmds::config_document::profile_fetch_url(url))
        .await
        .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn processing_core_command(app: tauri::AppHandle, request: String) -> Result<serde_json::Value, String> {
    let cache = match std::env::var("MIB_STUDIO_PROCESSING_CORE_CACHE_DIR") {
        Ok(path) if !path.trim().is_empty() => std::path::PathBuf::from(path.trim()),
        _ => app.path().app_cache_dir().map_err(|e| e.to_string())?.join("processing-cores"),
    };
    tauri::async_runtime::spawn_blocking(move || {
        let state = app.state::<AppState>();
        cmds::config_document::processing_core_command(&state, &cache.to_string_lossy(), request)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn fetch_preview_buffer(state: State<AppState>) -> Result<serde_json::Value, String> {
    cmds::preview_buffer::fetch_preview_buffer(&state)
}

#[tauri::command]
async fn save_preview_buffer(app: tauri::AppHandle, request: String) -> Result<serde_json::Value, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let state = app.state::<AppState>();
        cmds::preview_buffer::save_preview_buffer(&state, request)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn camera_document(action: String, path: String, kind: String, baseline: String, text: String) -> Result<cmds::camera_document::CameraDocument, String> {
    cmds::camera_document::camera_document(action, path, kind, baseline, text)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .manage({
            // The registry HTTPS transport must be installed before the
            // backend initializes (it is read once in AppBackend::initialize).
            let state = AppState::new();
            if let Ok(mut bridge) = state.bridge.lock() {
                bridge.pin_mut().set_registry_transport(registry_transport::post);
            }
            state
        })
        .invoke_handler(tauri::generate_handler![
            registry::registry_sign_in,
            registry::registry_sign_out,
            registry::registry_refresh,
            registry::registry_download,
            registry::registry_cancel_all,
            registry::registry_materialize,
            registry::registry_record_validation,
            registry::fetch_registry_snapshot,
            registry::fetch_registry_job,
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
            close_review,
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
            fetch_preview_buffer,
            save_preview_buffer,


            updater::inspect_app_update,
            app_update::check_tauri_app_update,
            app_update::verify_tauri_app_installer,
            app_update::launch_tauri_app_installer,
            app_update::clear_tauri_installer_cache,
            processing_core_command,
            profile_fetch_url,
            profile_command,
            camera_document,
            fetch_config_document,
            apply_config_document,
            experiment_start,
            experiment_stop,
            experiment_acknowledge_fault,
            fetch_capture_lifecycle,
            experiment_cancel,
            fetch_experiment_status,
            fetch_experiment_readiness,
            set_processed_preview_enabled,
            fetch_processed_preview,
            background_calibration_command,
            background_calibration_status,
            startup_discovery_set_preference,
            startup_discovery_run,
            startup_discovery_status,
            pulse_generator_command,
            pulse_generator_status,
            autofocus_connect_endpoint,
            autofocus_connect,
            autofocus_disconnect,
            autofocus_set_enabled,
            autofocus_jog,
            autofocus_set_config,
            fetch_autofocus_status,
            fetch_autofocus_config,
            pump_connect_endpoint,
            pump_connect_model,
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
            stage_connect,
            stage_disconnect,
            stage_move_to,
            stage_move_by,
            stage_home,
            stage_stop,
            stage_apply_profile,
            fetch_stage_status,
            pump_scan_addresses,
            fetch_review_metadata,
            fetch_review_metrics_page,
            fetch_review_image,
            review_image_bytes,
            review_export_csv,
            review_export_json,
            render_review_overlay,
            fetch_review_charts_json,
            fetch_monitoring_chart_reference,
            fetch_review_reanalysis_preview,
            review_reanalysis_json,
            review_reanalysis_status_json,
            review_export_status_json,
            fetch_processing_config_json,
            apply_processing_config_json,
            set_processing_roi,
            fetch_background,
            background_bytes,
            set_background_from_current_frame,
            clear_background_image,
            fetch_processing_core_status,
            start_device_discovery,
            start_camera_discovery,
            fetch_device_discovery,
            cancel_device_discovery,
            fetch_camera_selection,
            select_hardware_camera,
            select_mindvision_camera,
            apply_camera_script,
            reset_hardware_camera,
            soft_trigger_camera,
            set_camera_overview,
            save_camera_roi,
            fetch_camera_geometry,
            fetch_platform_info,
            fetch_instrument_status,
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
