//! Name-based dispatch of the shared commands for non-Tauri transports (YOFO Studio S5).
//!
//! Arguments arrive as the JSON object the webview passes to Tauri's `invoke`: camelCase keys
//! for snake_case parameters, so a client can switch transport without changing a call site.
//! GENERATED from the command signatures when the layer was extracted; keep it in step with
//! `lib.rs` (the `every_command_is_dispatchable` test lists every name).

use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::{AppState, AutofocusConfig, DiscoveryRequest};

/// A command's result: JSON for documents, raw bytes for frame packets (96-byte MIBF header).
#[derive(Debug, Clone, PartialEq)]
pub enum Reply {
    Json(Value),
    Binary(Vec<u8>),
}

/// Host services the desktop app gets from Tauri and a server from its configuration.
pub trait Host: Send + Sync {
    fn data_dir(&self) -> Result<String, String>;
    fn resource_dir(&self) -> Result<String, String>;
    fn processing_core_cache_dir(&self) -> Result<String, String>;
}

fn args<T: DeserializeOwned>(value: Value) -> Result<T, String> {
    serde_json::from_value(if value.is_null() { Value::Object(Default::default()) } else { value })
        .map_err(|e| format!("INVALID_ARGUMENTS: {e}"))
}

fn json<T: Serialize>(value: T) -> Result<Reply, String> {
    serde_json::to_value(value).map(Reply::Json).map_err(|e| e.to_string())
}

/// Every command name `dispatch` accepts, `init` and `processing_core_command` included.
pub const COMMANDS: &[&str] = &[
    "abi_version",
    "is_initialized",
    "configure_mock",
    "start_capture",
    "stop_capture",
    "seek_latest",
    "poll_events",
    "poll_events_exact",
    "start_recording",
    "stop_recording",
    "close_review",
    "load_recording",
    "seek_index",
    "fetch_frame",
    "fetch_frame_by_index",
    "frame_bytes",
    "fetch_frame_packet",
    "fetch_indexed_frame_packet",
    "apply_processing",
    "fetch_experiment_readiness",
    "experiment_start",
    "fetch_capture_lifecycle",
    "experiment_acknowledge_fault",
    "experiment_stop",
    "experiment_cancel",
    "fetch_experiment_status",
    "set_processed_preview_enabled",
    "fetch_processed_preview",
    "background_calibration_command",
    "background_calibration_status",
    "startup_discovery_set_preference",
    "startup_discovery_run",
    "startup_discovery_status",
    "pulse_generator_command",
    "pulse_generator_status",
    "autofocus_connect_endpoint",
    "autofocus_connect",
    "autofocus_disconnect",
    "autofocus_set_enabled",
    "autofocus_jog",
    "autofocus_set_config",
    "fetch_autofocus_status",
    "fetch_autofocus_config",
    "pump_connect_endpoint",
    "pump_connect_model",
    "pump_connect",
    "pump_disconnect",
    "pump_set_flow_rate",
    "pump_set_direction",
    "pump_start",
    "pump_stop",
    "pump_purge",
    "pump_stop_purge",
    "pump_set_syringe_volume",
    "pump_poll_status",
    "fetch_pump_status",
    "pump_scan_addresses",
    "fetch_review_metadata",
    "fetch_review_metrics_page",
    "fetch_review_image",
    "review_image_bytes",
    "fetch_review_frame_packet",
    "render_review_overlay",
    "fetch_review_reanalysis_preview",
    "fetch_monitoring_chart_reference",
    "fetch_review_charts_json",
    "review_reanalysis_json",
    "review_reanalysis_status_json",
    "review_export_json",
    "review_export_status_json",
    "review_export_csv",
    "fetch_processing_config_json",
    "apply_processing_config_json",
    "set_processing_roi",
    "fetch_background",
    "background_bytes",
    "fetch_background_packet",
    "set_background_from_current_frame",
    "clear_background_image",
    "fetch_processing_core_status",
    "start_device_discovery",
    "start_camera_discovery",
    "fetch_device_discovery",
    "cancel_device_discovery",
    "fetch_camera_selection",
    "select_hardware_camera",
    "select_mindvision_camera",
    "apply_camera_script",
    "soft_trigger_camera",
    "reset_hardware_camera",
    "monitoring_set_active",
    "monitoring_clear",
    "fetch_monitoring_snapshot",
    "trigger_set_pulse_duration",
    "trigger_manual_pulse",
    "trigger_periodic_start",
    "trigger_periodic_stop",
    "fetch_trigger_status",
    "cancel_operation",
    "queue_overflow_total",
    "fetch_processing_stats",
    "fetch_config_document",
    "apply_config_document",
    "profile_command",
    "profile_fetch_url",
    "fetch_preview_buffer",
    "save_preview_buffer",
    "camera_document",
    "init",
    "processing_core_command",
    "set_camera_overview",
    "save_camera_roi",
    "fetch_camera_geometry",
    "fetch_platform_info",
    "set_instrument_mode",
    "set_service_mode",
    "set_instrument_led",
    "fetch_run_preview",
    "fetch_instrument_status",
];

/// Run one command. Unknown names fail with `UNKNOWN_COMMAND`; blocking commands (profile and
/// core downloads, preview saves) block the caller, so call this off any event loop.
pub fn dispatch(state: &AppState, host: &dyn Host, name: &str, value: Value) -> Result<Reply, String> {
    match name {
        "init" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "dataDir", default)] data_dir: String,
            }
            let a: A = args(value)?;
            let dir = if a.data_dir.trim().is_empty() { host.data_dir()? } else { a.data_dir };
            crate::init(state, &dir, &host.resource_dir()?).and_then(json)
        }
        "processing_core_command" => {
            #[derive(Deserialize)]
            struct A {
                request: String,
            }
            let a: A = args(value)?;
            crate::config_document::processing_core_command(state, &host.processing_core_cache_dir()?, a.request).and_then(json)
        }
        "abi_version" => json(crate::abi_version()),
        "is_initialized" => crate::is_initialized(state).and_then(json),
        "configure_mock" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "frameDir")] frame_dir: String,
                #[serde(rename = "frameIntervalMs")] frame_interval_ms: i32,
                #[serde(rename = "loopFiles")] loop_files: bool,
            }
            let a: A = args(value)?;
            crate::configure_mock(state, a.frame_dir, a.frame_interval_ms, a.loop_files).and_then(json)
        }
        "start_capture" => crate::start_capture(state).and_then(json),
        "stop_capture" => crate::stop_capture(state).and_then(json),
        "seek_latest" => crate::seek_latest(state).and_then(json),
        "poll_events" => crate::poll_events().and_then(json),
        "poll_events_exact" => crate::poll_events_exact(state).and_then(json),
        "start_recording" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "filePath")] file_path: String,
            }
            let a: A = args(value)?;
            crate::start_recording(state, a.file_path).and_then(json)
        }
        "stop_recording" => crate::stop_recording(state).and_then(json),
        "close_review" => crate::close_review(state).and_then(json),
        "load_recording" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "filePath")] file_path: String,
            }
            let a: A = args(value)?;
            crate::load_recording(state, a.file_path).and_then(json)
        }
        "seek_index" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "frameIndex")] frame_index: String,
            }
            let a: A = args(value)?;
            crate::seek_index(state, a.frame_index).and_then(json)
        }
        "fetch_frame" => crate::fetch_frame().map(Reply::Binary),
        "fetch_frame_by_index" => crate::fetch_frame_by_index().map(Reply::Binary),
        "frame_bytes" => crate::frame_bytes().map(Reply::Binary),
        "fetch_frame_packet" => crate::fetch_frame_packet(state).map(Reply::Binary),
        "fetch_indexed_frame_packet" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "frameIndex")] frame_index: String,
            }
            let a: A = args(value)?;
            crate::fetch_indexed_frame_packet(state, a.frame_index).map(Reply::Binary)
        }
        "apply_processing" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "realtimeEnabled")] realtime_enabled: bool,
                #[serde(rename = "pixelToMicron")] pixel_to_micron: f64,
            }
            let a: A = args(value)?;
            crate::apply_processing(state, a.realtime_enabled, a.pixel_to_micron).and_then(json)
        }
        "fetch_experiment_readiness" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "outputPath")] output_path: String,
            }
            let a: A = args(value)?;
            crate::fetch_experiment_readiness(state, a.output_path).and_then(json)
        }
        "experiment_start" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "outputPath")] output_path: String,
            }
            let a: A = args(value)?;
            crate::experiment_start(state, a.output_path).and_then(json)
        }
        "fetch_capture_lifecycle" => crate::fetch_capture_lifecycle(state).and_then(json),
        "experiment_acknowledge_fault" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "expectedRun")] expected_run: String,
                #[serde(rename = "faultRevision")] fault_revision: String,
                #[serde(rename = "code")] code: String,
                #[serde(rename = "message")] message: String,
                #[serde(rename = "confirmed")] confirmed: bool,
            }
            let a: A = args(value)?;
            crate::experiment_acknowledge_fault(state, a.expected_run, a.fault_revision, a.code, a.message, a.confirmed).and_then(json)
        }
        "experiment_stop" => crate::experiment_stop(state).and_then(json),
        "experiment_cancel" => crate::experiment_cancel(state).and_then(json),
        "fetch_experiment_status" => crate::fetch_experiment_status(state).and_then(json),
        "set_processed_preview_enabled" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "enabled")] enabled: bool,
            }
            let a: A = args(value)?;
            crate::set_processed_preview_enabled(state, a.enabled).and_then(json)
        }
        "fetch_processed_preview" => crate::fetch_processed_preview(state).map(Reply::Binary),
        "background_calibration_command" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "json")] json: String,
            }
            let a: A = args(value)?;
            crate::background_calibration_command(state, a.json).and_then(json)
        }
        "background_calibration_status" => crate::background_calibration_status(state).and_then(json),
        "startup_discovery_set_preference" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "json")] json: String,
            }
            let a: A = args(value)?;
            crate::startup_discovery_set_preference(state, a.json).and_then(json)
        }
        "startup_discovery_run" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "action")] action: String,
            }
            let a: A = args(value)?;
            crate::startup_discovery_run(state, a.action).and_then(json)
        }
        "startup_discovery_status" => crate::startup_discovery_status(state).and_then(json),
        "pulse_generator_command" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "json")] json: String,
            }
            let a: A = args(value)?;
            crate::pulse_generator_command(state, a.json).and_then(json)
        }
        "pulse_generator_status" => crate::pulse_generator_status(state).and_then(json),
        "autofocus_connect_endpoint" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "backend")] backend: String,
                #[serde(rename = "endpoint")] endpoint: String,
                #[serde(rename = "comPort")] com_port: i32,
                #[serde(rename = "baudRate")] baud_rate: i32,
                #[serde(rename = "deviceAddress")] device_address: i32,
            }
            let a: A = args(value)?;
            crate::autofocus_connect_endpoint(state, a.backend, a.endpoint, a.com_port, a.baud_rate, a.device_address).and_then(json)
        }
        "autofocus_connect" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "comPort")] com_port: i32,
                #[serde(rename = "baudRate")] baud_rate: i32,
                #[serde(rename = "deviceAddress")] device_address: i32,
            }
            let a: A = args(value)?;
            crate::autofocus_connect(state, a.com_port, a.baud_rate, a.device_address).and_then(json)
        }
        "autofocus_disconnect" => crate::autofocus_disconnect(state).and_then(json),
        "autofocus_set_enabled" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "enabled")] enabled: bool,
            }
            let a: A = args(value)?;
            crate::autofocus_set_enabled(state, a.enabled).and_then(json)
        }
        "autofocus_jog" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "up")] up: bool,
            }
            let a: A = args(value)?;
            crate::autofocus_jog(state, a.up).and_then(json)
        }
        "autofocus_set_config" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "config")] config: AutofocusConfig,
            }
            let a: A = args(value)?;
            crate::autofocus_set_config(state, a.config).and_then(json)
        }
        "fetch_autofocus_status" => crate::fetch_autofocus_status(state).and_then(json),
        "fetch_autofocus_config" => crate::fetch_autofocus_config(state).and_then(json),
        "pump_connect_endpoint" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
                #[serde(rename = "portName")] port_name: String,
                #[serde(rename = "baudRate")] baud_rate: i32,
                #[serde(rename = "modbusAddress")] modbus_address: i32,
            }
            let a: A = args(value)?;
            crate::pump_connect_endpoint(state, a.pump, a.port_name, a.baud_rate, a.modbus_address).and_then(json)
        }
        "pump_connect_model" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
                #[serde(rename = "model")] model: u32,
                #[serde(rename = "portName")] port_name: String,
                #[serde(rename = "baudRate")] baud_rate: i32,
                #[serde(rename = "modbusAddress")] modbus_address: i32,
                #[serde(rename = "microlitersPerRev")] microliters_per_rev: f64,
            }
            let a: A = args(value)?;
            crate::pump_connect_model(state, a.pump, a.model, a.port_name, a.baud_rate, a.modbus_address, a.microliters_per_rev).and_then(json)
        }
        "pump_connect" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
                #[serde(rename = "comPort")] com_port: i32,
                #[serde(rename = "baudRate")] baud_rate: i32,
                #[serde(rename = "modbusAddress")] modbus_address: i32,
            }
            let a: A = args(value)?;
            crate::pump_connect(state, a.pump, a.com_port, a.baud_rate, a.modbus_address).and_then(json)
        }
        "pump_disconnect" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
            }
            let a: A = args(value)?;
            crate::pump_disconnect(state, a.pump).and_then(json)
        }
        "pump_set_flow_rate" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
                #[serde(rename = "rate")] rate: f64,
                #[serde(rename = "unit")] unit: i32,
            }
            let a: A = args(value)?;
            crate::pump_set_flow_rate(state, a.pump, a.rate, a.unit).and_then(json)
        }
        "pump_set_direction" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
                #[serde(rename = "direction")] direction: u32,
            }
            let a: A = args(value)?;
            crate::pump_set_direction(state, a.pump, a.direction).and_then(json)
        }
        "pump_start" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
            }
            let a: A = args(value)?;
            crate::pump_start(state, a.pump).and_then(json)
        }
        "pump_stop" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
            }
            let a: A = args(value)?;
            crate::pump_stop(state, a.pump).and_then(json)
        }
        "pump_purge" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
                #[serde(rename = "direction")] direction: u32,
            }
            let a: A = args(value)?;
            crate::pump_purge(state, a.pump, a.direction).and_then(json)
        }
        "pump_stop_purge" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
            }
            let a: A = args(value)?;
            crate::pump_stop_purge(state, a.pump).and_then(json)
        }
        "pump_set_syringe_volume" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
                #[serde(rename = "volume")] volume: i32,
                #[serde(rename = "unit")] unit: i32,
            }
            let a: A = args(value)?;
            crate::pump_set_syringe_volume(state, a.pump, a.volume, a.unit).and_then(json)
        }
        "pump_poll_status" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
            }
            let a: A = args(value)?;
            crate::pump_poll_status(state, a.pump).and_then(json)
        }
        "fetch_pump_status" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pump")] pump: u32,
            }
            let a: A = args(value)?;
            crate::fetch_pump_status(state, a.pump).and_then(json)
        }
        "pump_scan_addresses" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "comPort")] com_port: i32,
                #[serde(rename = "baudRate")] baud_rate: i32,
                #[serde(rename = "startAddress")] start_address: i32,
                #[serde(rename = "endAddress")] end_address: i32,
                #[serde(rename = "timeoutMs")] timeout_ms: i32,
            }
            let a: A = args(value)?;
            crate::pump_scan_addresses(state, a.com_port, a.baud_rate, a.start_address, a.end_address, a.timeout_ms).and_then(json)
        }
        "fetch_review_metadata" => crate::fetch_review_metadata(state).and_then(json),
        "fetch_review_metrics_page" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "valid")] valid: bool,
                #[serde(rename = "offset")] offset: u64,
                #[serde(rename = "count")] count: u64,
            }
            let a: A = args(value)?;
            crate::fetch_review_metrics_page(state, a.valid, a.offset, a.count).and_then(json)
        }
        "fetch_review_image" => crate::fetch_review_image().map(Reply::Binary),
        "review_image_bytes" => crate::review_image_bytes().map(Reply::Binary),
        "fetch_review_frame_packet" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "dataset")] dataset: u32,
                #[serde(rename = "index")] index: String,
            }
            let a: A = args(value)?;
            crate::fetch_review_frame_packet(state, a.dataset, a.index).map(Reply::Binary)
        }
        "render_review_overlay" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "json")] json: String,
            }
            let a: A = args(value)?;
            crate::render_review_overlay(state, a.json).map(Reply::Binary)
        }
        "fetch_review_reanalysis_preview" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "json")] json: String,
            }
            let a: A = args(value)?;
            crate::fetch_review_reanalysis_preview(state, a.json).map(Reply::Binary)
        }
        "fetch_monitoring_chart_reference" => crate::fetch_monitoring_chart_reference(state).and_then(json),
        "fetch_review_charts_json" => crate::fetch_review_charts_json(state).and_then(json),
        "review_reanalysis_json" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "json")] json: String,
            }
            let a: A = args(value)?;
            crate::review_reanalysis_json(state, a.json).and_then(json)
        }
        "review_reanalysis_status_json" => crate::review_reanalysis_status_json(state).and_then(json),
        "review_export_json" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "json")] json: String,
            }
            let a: A = args(value)?;
            crate::review_export_json(state, a.json).and_then(json)
        }
        "review_export_status_json" => crate::review_export_status_json(state).and_then(json),
        "review_export_csv" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "outputPath")] output_path: String,
            }
            let a: A = args(value)?;
            crate::review_export_csv(state, a.output_path).and_then(json)
        }
        "fetch_processing_config_json" => crate::fetch_processing_config_json(state).and_then(json),
        "apply_processing_config_json" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "json")] json: String,
            }
            let a: A = args(value)?;
            crate::apply_processing_config_json(state, a.json).and_then(json)
        }
        "set_processing_roi" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "x")] x: i32,
                #[serde(rename = "y")] y: i32,
                #[serde(rename = "w")] w: i32,
                #[serde(rename = "h")] h: i32,
            }
            let a: A = args(value)?;
            crate::set_processing_roi(state, a.x, a.y, a.w, a.h).and_then(json)
        }
        "fetch_background" => crate::fetch_background().map(Reply::Binary),
        "background_bytes" => crate::background_bytes().map(Reply::Binary),
        "fetch_background_packet" => crate::fetch_background_packet(state).map(Reply::Binary),
        "set_background_from_current_frame" => crate::set_background_from_current_frame(state).and_then(json),
        "clear_background_image" => crate::clear_background_image(state).and_then(json),
        "fetch_processing_core_status" => crate::fetch_processing_core_status(state).and_then(json),
        "start_device_discovery" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "request")] request: DiscoveryRequest,
            }
            let a: A = args(value)?;
            crate::start_device_discovery(state, a.request).and_then(json)
        }
        "start_camera_discovery" => crate::start_camera_discovery(state).and_then(json),
        "fetch_device_discovery" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "jobId")] job_id: String,
            }
            let a: A = args(value)?;
            crate::fetch_device_discovery(state, a.job_id).and_then(json)
        }
        "cancel_device_discovery" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "jobId")] job_id: String,
            }
            let a: A = args(value)?;
            crate::cancel_device_discovery(state, a.job_id).and_then(json)
        }
        "fetch_camera_selection" => crate::fetch_camera_selection(state).and_then(json),
        "select_hardware_camera" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "interfaceIndex")] interface_index: i32,
                #[serde(rename = "deviceIndex")] device_index: i32,
                #[serde(rename = "label")] label: String,
            }
            let a: A = args(value)?;
            crate::select_hardware_camera(state, a.interface_index, a.device_index, a.label).and_then(json)
        }
        "select_mindvision_camera" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "cameraIndex")] camera_index: i32,
                #[serde(rename = "label")] label: String,
                #[serde(rename = "configPath")] config_path: String,
            }
            let a: A = args(value)?;
            crate::select_mindvision_camera(state, a.camera_index, a.label, a.config_path).and_then(json)
        }
        "apply_camera_script" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "scriptPath")] script_path: String,
            }
            let a: A = args(value)?;
            crate::apply_camera_script(state, a.script_path).and_then(json)
        }
        "soft_trigger_camera" => crate::soft_trigger_camera(state).and_then(json),
        "reset_hardware_camera" => crate::reset_hardware_camera(state).and_then(json),
        "monitoring_set_active" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "active")] active: bool,
            }
            let a: A = args(value)?;
            crate::monitoring_set_active(state, a.active).and_then(json)
        }
        "monitoring_clear" => crate::monitoring_clear(state).and_then(json),
        "fetch_monitoring_snapshot" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "maxRows")] max_rows: u64,
            }
            let a: A = args(value)?;
            crate::fetch_monitoring_snapshot(state, a.max_rows).and_then(json)
        }
        "trigger_set_pulse_duration" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "pulseUs")] pulse_us: i32,
            }
            let a: A = args(value)?;
            crate::trigger_set_pulse_duration(state, a.pulse_us).and_then(json)
        }
        "trigger_manual_pulse" => crate::trigger_manual_pulse(state).and_then(json),
        "trigger_periodic_start" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "intervalMs")] interval_ms: i32,
            }
            let a: A = args(value)?;
            crate::trigger_periodic_start(state, a.interval_ms).and_then(json)
        }
        "trigger_periodic_stop" => crate::trigger_periodic_stop(state).and_then(json),
        "fetch_trigger_status" => crate::fetch_trigger_status(state).and_then(json),
        "cancel_operation" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "operationId")] operation_id: String,
            }
            let a: A = args(value)?;
            crate::cancel_operation(state, a.operation_id).and_then(json)
        }
        "queue_overflow_total" => crate::queue_overflow_total(state).and_then(json),
        "fetch_processing_stats" => crate::fetch_processing_stats(state).and_then(json),
        "fetch_config_document" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "path")] path: String,
            }
            let a: A = args(value)?;
            crate::config_document::fetch_config_document(state, a.path).and_then(json)
        }
        "apply_config_document" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "path")] path: String,
                #[serde(rename = "baseline")] baseline: String,
                #[serde(rename = "patch")] patch: String,
            }
            let a: A = args(value)?;
            crate::config_document::apply_config_document(state, a.path, a.baseline, a.patch).and_then(json)
        }
        "profile_command" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "base")] base: String,
                #[serde(rename = "request")] request: String,
            }
            let a: A = args(value)?;
            crate::config_document::profile_command(state, a.base, a.request).and_then(json)
        }
        "profile_fetch_url" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "url")] url: String,
            }
            let a: A = args(value)?;
            crate::config_document::profile_fetch_url(a.url).and_then(json)
        }
        "fetch_preview_buffer" => crate::preview_buffer::fetch_preview_buffer(state).and_then(json),
        "save_preview_buffer" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "request")] request: String,
            }
            let a: A = args(value)?;
            crate::preview_buffer::save_preview_buffer(state, a.request).and_then(json)
        }
        "camera_document" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "action")] action: String,
                #[serde(rename = "path")] path: String,
                #[serde(rename = "kind")] kind: String,
                #[serde(rename = "baseline")] baseline: String,
                #[serde(rename = "text")] text: String,
            }
            let a: A = args(value)?;
            crate::camera_document::camera_document(a.action, a.path, a.kind, a.baseline, a.text).and_then(json)
        }
        "set_instrument_mode" => {
            #[derive(Deserialize)]
            struct A {
                mode: String,
                #[serde(default)]
                x: i32,
                #[serde(default)]
                y: i32,
            }
            let a: A = args(value)?;
            crate::set_instrument_mode(state, &a.mode, a.x, a.y).and_then(json)
        }
        "set_service_mode" => {
            #[derive(Deserialize)]
            struct A {
                on: bool,
            }
            let a: A = args(value)?;
            crate::set_service_mode(state, a.on).and_then(json)
        }
        "set_instrument_led" => {
            #[derive(Deserialize)]
            #[serde(rename_all = "camelCase")]
            struct A {
                delay_us: f64,
                width_us: f64,
            }
            let a: A = args(value)?;
            crate::set_instrument_led(state, a.delay_us, a.width_us).and_then(json)
        }
        "fetch_run_preview" => crate::fetch_run_preview(state).map(Reply::Binary),
        "set_camera_overview" => {
            #[derive(Deserialize)]
            struct A {
                overview: bool,
            }
            let a: A = args(value)?;
            crate::set_camera_overview(state, a.overview).and_then(json)
        }
        "save_camera_roi" => {
            #[derive(Deserialize)]
            struct A {
                x: i32,
                y: i32,
                w: i32,
                h: i32,
            }
            let a: A = args(value)?;
            crate::save_camera_roi(state, a.x, a.y, a.w, a.h).and_then(json)
        }
        "fetch_camera_geometry" => crate::fetch_camera_geometry(state).and_then(json),
        "fetch_platform_info" => crate::fetch_platform_info(state).and_then(json),
        "fetch_instrument_status" => crate::fetch_instrument_status(state).and_then(json),
        _ => Err(format!("UNKNOWN_COMMAND: {name}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use serial_test::serial;

    struct TestHost(String);
    impl Host for TestHost {
        fn data_dir(&self) -> Result<String, String> { Ok(self.0.clone()) }
        fn resource_dir(&self) -> Result<String, String> { Ok(String::new()) }
        fn processing_core_cache_dir(&self) -> Result<String, String> { Ok(self.0.clone()) }
    }

    /// Every command the Tauri shell registers is reachable by name, except the desktop-only
    /// ones (app paths, preferences, updater, installers, the shell log) and the central
    /// profile registry (`registry::`, #398): it needs the shell's HTTPS transport and its
    /// sign-in carries a password, which must not cross the WebSocket.
    #[test]
    fn every_command_is_dispatchable() {
        let tauri = include_str!("../../../desktop/src-tauri/src/lib.rs");
        let start = tauri.find("generate_handler![").expect("handler list");
        let end = start + tauri[start..].find("])").expect("handler list end");
        let desktop_only = ["app_paths", "get_preferences", "set_preferences", "shell_log", "inspect_app_update",
            "check_tauri_app_update", "verify_tauri_app_installer", "launch_tauri_app_installer",
            "clear_tauri_installer_cache"];
        for path in tauri[start + 18..end].split(',').map(str::trim) {
            let name = path.rsplit("::").next().unwrap().trim();
            if name.is_empty() || desktop_only.contains(&name) || path.starts_with("registry::") {
                continue;
            }
            assert!(COMMANDS.contains(&name), "{name} is a Tauri command but not dispatchable");
        }
    }

    #[test]
    #[serial]
    fn mock_capture_through_dispatch() {
        let data = std::env::temp_dir().join(format!("mib_dispatch_{}", std::process::id()));
        let state = AppState::new();
        let host = TestHost(data.to_string_lossy().into_owned());
        assert_eq!(dispatch(&state, &host, "init", json!({ "dataDir": "" })).unwrap(), Reply::Json(json!(true)));
        let frames = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../data/mock_frames");
        let r = dispatch(&state, &host, "configure_mock",
            json!({ "frameDir": frames.to_string_lossy(), "frameIntervalMs": 5, "loopFiles": true })).unwrap();
        let Reply::Json(r) = r else { panic!("configure_mock returns JSON") };
        assert_eq!(r["ok"], json!(true), "{r}");
        assert!(matches!(dispatch(&state, &host, "start_capture", Value::Null), Ok(Reply::Json(_))));
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        let packet = loop {
            match dispatch(&state, &host, "fetch_frame_packet", Value::Null) {
                // A header-only packet means no frame has been captured yet.
                Ok(Reply::Binary(bytes)) if bytes.len() > crate::frame_packet::HEADER_BYTES => break bytes,
                _ if std::time::Instant::now() < deadline => std::thread::sleep(std::time::Duration::from_millis(20)),
                other => panic!("no frame packet: {other:?}"),
            }
        };
        assert_eq!(&packet[..4], b"MIBF");
        let Reply::Json(events) = dispatch(&state, &host, "poll_events_exact", Value::Null).unwrap() else {
            panic!("events are JSON")
        };
        assert_eq!(events["transport_version"], json!(1));
        assert!(dispatch(&state, &host, "stop_capture", Value::Null).is_ok());
        assert!(dispatch(&state, &host, "no_such_command", Value::Null).unwrap_err().starts_with("UNKNOWN_COMMAND"));
        assert!(dispatch(&state, &host, "seek_index", json!({ "frameIndex": 3 }))
            .unwrap_err().starts_with("INVALID_ARGUMENTS"), "u64 indices travel as strings");
        state.bridge.lock().unwrap().pin_mut().shutdown();
        let _ = std::fs::remove_dir_all(&data);
    }
}
