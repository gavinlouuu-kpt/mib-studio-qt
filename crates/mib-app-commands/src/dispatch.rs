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
    "stage_connect",
    "stage_disconnect",
    "stage_move_to",
    "stage_move_by",
    "stage_set_zero",
    "stage_stop",
    "stage_apply_profile",
    "fetch_stage_status",
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
    "fetch_monitoring_cells",
    "monitoring_cell_pin",
    "monitoring_cell_unpin",
    "monitoring_cell_clear_pins",
    "fetch_monitoring_cell_packet",
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
    "restore_startup_configuration",
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
    "fetch_run_accounting",
    "set_instrument_mode",
    "sync_wall_clock",
    "fetch_ring_status",
    "fetch_ring_frame",
    "ring_freeze",
    "ring_resume",
    "fetch_run_fps",
    "apply_pz_camera_document",
    "check_pz_camera_document",
    "fetch_ssd_status",
    "fetch_ssd_runs",
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
                /// ABI 37: the client accepts that a stopped Run's buffered frames are discarded.
                #[serde(rename = "acknowledgeDiscardRing", default)] acknowledge_discard_ring: bool,
                /// ABI 39 (#712): the startRequest document as the client's JSON text, passed through
                /// unparsed (the backend alone decodes it and bounds those bytes). Absent or null: none.
                #[serde(rename = "startRequestJson", default)] start_request_json: Option<String>,
            }
            let a: A = args(value)?;
            crate::experiment_start(state, a.output_path, a.acknowledge_discard_ring, a.start_request_json).and_then(json)
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
        "experiment_stop" => {
            #[derive(Deserialize)]
            struct A {
                /// ABI 40 (#741): the start generation of the run to stop, a canonical decimal string;
                /// another run (or none) is refused with stop outcome RunMismatch. Absent or null: any run.
                #[serde(rename = "expectedStartGeneration", default)] expected_start_generation: Option<String>,
            }
            let a: A = args(value)?;
            crate::experiment_stop(state, a.expected_start_generation).and_then(json)
        }
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
        "stage_connect" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "portName", default)] port_name: String,
                #[serde(rename = "usbSerial", default)] usb_serial: String,
                #[serde(rename = "modbusAddress", default)] modbus_address: i32,
            }
            let a: A = args(value)?;
            crate::stage_connect(state, a.port_name, a.usb_serial, a.modbus_address).and_then(json)
        }
        "stage_disconnect" => crate::stage_disconnect(state).and_then(json),
        "stage_move_to" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "targetUm")] target_um: f64,
            }
            let a: A = args(value)?;
            crate::stage_move_to(state, a.target_um).and_then(json)
        }
        "stage_move_by" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "deltaUm")] delta_um: f64,
            }
            let a: A = args(value)?;
            crate::stage_move_by(state, a.delta_um).and_then(json)
        }
        "stage_set_zero" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "midTravel", default)] mid_travel: bool,
            }
            let a: A = args(value)?;
            crate::stage_set_zero(state, a.mid_travel).and_then(json)
        }
        "stage_stop" => crate::stage_stop(state).and_then(json),
        "stage_apply_profile" => crate::stage_apply_profile(state).and_then(json),
        "fetch_stage_status" => crate::fetch_stage_status(state).and_then(json),
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
        // ABI 41 (#716 A6a): live cell images. Identities are canonical decimal strings
        // (json_transport); a malformed one is an error reply and nothing reaches the backend.
        "fetch_monitoring_cells" => crate::fetch_monitoring_cells(state).and_then(json),
        "monitoring_cell_pin" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "expectedStartGeneration")] expected_start_generation: String,
                #[serde(rename = "captureEpoch")] capture_epoch: String,
                #[serde(rename = "frameIndex")] frame_index: String,
                #[serde(rename = "objectId")] object_id: i32,
            }
            let a: A = args(value)?;
            crate::monitoring_cell_pin(state, a.expected_start_generation, a.capture_epoch, a.frame_index, a.object_id)
                .and_then(json)
        }
        "monitoring_cell_unpin" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "startGeneration")] start_generation: String,
                #[serde(rename = "captureEpoch")] capture_epoch: String,
                #[serde(rename = "frameIndex")] frame_index: String,
                #[serde(rename = "objectId")] object_id: i32,
            }
            let a: A = args(value)?;
            crate::monitoring_cell_unpin(state, a.start_generation, a.capture_epoch, a.frame_index, a.object_id)
                .and_then(json)
        }
        "monitoring_cell_clear_pins" => crate::monitoring_cell_clear_pins(state).and_then(json),
        "fetch_monitoring_cell_packet" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "startGeneration")] start_generation: String,
                #[serde(rename = "captureEpoch")] capture_epoch: String,
                #[serde(rename = "frameIndex")] frame_index: String,
                #[serde(rename = "objectId")] object_id: i32,
                #[serde(rename = "layer")] layer: String,
            }
            let a: A = args(value)?;
            crate::fetch_monitoring_cell_packet(
                state,
                a.start_generation,
                a.capture_epoch,
                a.frame_index,
                a.object_id,
                a.layer,
            )
            .map(Reply::Binary)
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
        "restore_startup_configuration" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "profileBase")] profile_base: String,
            }
            let a: A = args(value)?;
            crate::config_document::restore_startup_configuration(state, a.profile_base).and_then(json)
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
            // The remote entry: the PZ7035 camera document is confined to <data>/camera/ (the server makes the command control-only).
            crate::camera_document::camera_document_confined(std::path::Path::new(&host.data_dir()?), a.action, a.path, a.kind, a.baseline, a.text).and_then(json)
        }
        "fetch_ring_status" => crate::fetch_ring_status(state).and_then(json),
        "fetch_ring_frame" => {
            #[derive(Deserialize)]
            struct A {
                seq: u64,
            }
            let a: A = args(value)?;
            crate::fetch_ring_frame(state, a.seq).map(Reply::Binary)
        }
        "ring_freeze" => crate::ring_freeze(state).and_then(json),
        "ring_resume" => crate::ring_resume(state).and_then(json),
        "fetch_run_fps" => crate::fetch_run_fps(state).and_then(json),
        "check_pz_camera_document" => {
            #[derive(Deserialize)]
            struct A {
                text: String,
            }
            let a: A = args(value)?;
            crate::check_pz_camera_document(state, &a.text).and_then(json)
        }
        "apply_pz_camera_document" => {
            #[derive(Deserialize)]
            struct A {
                path: String,
                /// ABI 38: the client accepts that a stopped Run's buffered frames are discarded by the re-entry.
                #[serde(rename = "acknowledgeDiscardRing", default)] acknowledge_discard_ring: bool,
            }
            let a: A = args(value)?;
            crate::apply_pz_camera_document(state, &a.path, a.acknowledge_discard_ring).and_then(json)
        }
        "fetch_ssd_status" => crate::fetch_ssd_status(state).and_then(json),
        "fetch_ssd_runs" => crate::fetch_ssd_runs(state).and_then(json),
        "sync_wall_clock" => {
            #[derive(Deserialize)]
            struct A {
                #[serde(rename = "unixMs")] unix_ms: i64,
            }
            let a: A = args(value)?;
            crate::sync_wall_clock(state, a.unix_ms).and_then(json)
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
        "fetch_run_accounting" => {
            #[derive(Deserialize)]
            struct A {
                source: String,
            }
            let a: A = args(value)?;
            crate::fetch_run_accounting(state, &a.source).and_then(json)
        }
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

    // The ring commands (ABI 34): `seq` is a JSON number (u64); a missing, negative or string value is INVALID_ARGUMENTS before the backend is asked, and off the
    // PZ7035 the status says there is no ring and the frame and freeze commands fail with their reason.
    #[test]
    #[serial]
    fn ring_commands_through_dispatch() {
        let data = std::env::temp_dir().join(format!("mib_dispatch_ring_{}", std::process::id()));
        let state = AppState::new();
        let host = TestHost(data.to_string_lossy().into_owned());
        assert_eq!(dispatch(&state, &host, "init", json!({ "dataDir": "" })).unwrap(), Reply::Json(json!(true)));
        for bad in [json!({}), json!({ "seq": -1 }), json!({ "seq": "3" }), json!({ "seq": 1.5 }), json!({ "seq": null })] {
            let err = dispatch(&state, &host, "fetch_ring_frame", bad.clone()).unwrap_err();
            assert!(err.starts_with("INVALID_ARGUMENTS"), "{bad}: {err}");
        }
        let err = dispatch(&state, &host, "fetch_ring_frame", json!({ "seq": 3 })).unwrap_err();
        assert!(err.starts_with("RING_FRAME_UNAVAILABLE"), "{err}");
        let Reply::Json(status) = dispatch(&state, &host, "fetch_ring_status", Value::Null).unwrap() else { panic!("JSON") };
        assert_eq!(status["available"], json!(false), "{status}");
        assert_eq!(status["run_frozen"], json!(false), "{status}");
        let Reply::Json(freeze) = dispatch(&state, &host, "ring_freeze", Value::Null).unwrap() else { panic!("JSON") };
        assert_eq!(freeze["ok"], json!(false), "{freeze}");
        let Reply::Json(resume) = dispatch(&state, &host, "ring_resume", Value::Null).unwrap() else { panic!("JSON") };
        assert_eq!(resume["ok"], json!(false), "{resume}");
        // The SSD record store (ABI 35): read only, open to every client, ABSENT without pzrec configured.
        let Reply::Json(ssd) = dispatch(&state, &host, "fetch_ssd_status", Value::Null).unwrap() else { panic!("JSON") };
        assert_eq!(ssd["state"], json!("ABSENT"), "{ssd}");
        let Reply::Json(runs) = dispatch(&state, &host, "fetch_ssd_runs", Value::Null).unwrap() else { panic!("JSON") };
        assert_eq!(runs["runs"], json!([]), "{runs}");
        state.bridge.lock().unwrap().pin_mut().shutdown();
        let _ = std::fs::remove_dir_all(&data);
    }

    // ABI 39 (#712): startRequestJson reaches the backend byte for byte on both host paths: the
    // remote server's dispatch and the Tauri command (which calls crate::experiment_start). A
    // duplicate key would be collapsed by a serde Value (Bob would start), and whitespace padding
    // would disappear on re-serialisation; the backend must see and refuse both.
    #[test]
    #[serial]
    fn start_request_json_reaches_the_backend_unparsed() {
        let data = std::env::temp_dir().join(format!("mib_dispatch_start_request_{}", std::process::id()));
        let state = AppState::new();
        let host = TestHost(data.to_string_lossy().into_owned());
        assert_eq!(dispatch(&state, &host, "init", json!({ "dataDir": "" })).unwrap(), Reply::Json(json!(true)));
        let out = data.join("start_request.h5").to_string_lossy().into_owned();
        let duplicate = r#"{"operator":{"name":"Alice","name":"Bob"}}"#;
        let padded = format!("{{{}\"sample_ref\":null}}", " ".repeat(16400));
        assert!(padded.len() > 16384, "raw request is over the bound");
        assert!(serde_json::from_str::<Value>(&padded).unwrap().to_string().len() < 64, "normalised it would pass");
        for (doc, why) in [(duplicate, "duplicate key"), (padded.as_str(), "at most 16384")] {
            let Reply::Json(remote) =
                dispatch(&state, &host, "experiment_start", json!({ "outputPath": out, "startRequestJson": doc })).unwrap()
            else {
                panic!("experiment_start returns JSON")
            };
            let message = remote["message"].as_str().unwrap_or_default().to_string();
            assert_eq!(remote["ok"], json!(false), "{remote}");
            assert!(message.contains("invalid start request") && message.contains(why), "dispatch: {message}");
            let tauri = crate::experiment_start(&state, out.clone(), false, Some(doc.to_string())).unwrap();
            assert!(!tauri.ok, "the Tauri path refuses too");
            assert!(tauri.message.contains("invalid start request") && tauri.message.contains(why), "tauri: {}", tauri.message);
        }
        // A valid document (whitespace and all) passes the parser; this host then refuses the start for
        // its own reason (no camera), never as an invalid request. Absent or null declares nothing.
        for args in [
            json!({ "outputPath": out, "startRequestJson": "  {\"sample_ref\" : null }\n" }),
            json!({ "outputPath": out, "startRequestJson": null }),
            json!({ "outputPath": out }),
        ] {
            let Reply::Json(r) = dispatch(&state, &host, "experiment_start", args.clone()).unwrap() else { panic!("JSON") };
            assert!(!r["message"].as_str().unwrap_or_default().contains("invalid start request"), "{args}: {r}");
        }
        state.bridge.lock().unwrap().pin_mut().shutdown();
        let _ = std::fs::remove_dir_all(&data);
    }

    // ABI 40 (#741): the typed outcomes reach the JSON reply on both host paths (dispatch for the
    // remote server, crate::experiment_* for Tauri), only on experiment replies; the stop's expected
    // start generation is a canonical decimal string, checked before anything is sent.
    #[test]
    #[serial]
    fn experiment_outcomes_and_expected_generation_through_dispatch() {
        let data = std::env::temp_dir().join(format!("mib_dispatch_outcomes_{}", std::process::id()));
        let state = AppState::new();
        let host = TestHost(data.to_string_lossy().into_owned());
        assert_eq!(dispatch(&state, &host, "init", json!({ "dataDir": "" })).unwrap(), Reply::Json(json!(true)));
        let out = data.join("outcomes.h5").to_string_lossy().into_owned();
        let reply = |name: &str, args: Value| match dispatch(&state, &host, name, args) {
            Ok(Reply::Json(r)) => r,
            other => panic!("{name}: {other:?}"),
        };

        // Start: NotReady (1) without a camera, InvalidRequest (7) for a refused request.
        let r = reply("experiment_start", json!({ "outputPath": out }));
        assert_eq!((r["ok"].clone(), r["start_outcome"].clone()), (json!(false), json!(1)), "{r}");
        assert!(r.get("stop_outcome").is_none(), "{r}");
        let r = reply("experiment_start", json!({ "outputPath": out, "startRequestJson": "{\"operator_name\":\"A\"}" }));
        assert_eq!(r["start_outcome"], json!(7), "{r}");
        let message = r["message"].as_str().unwrap();
        assert!(message.starts_with("invalid start request: ") && message.contains("operator_name"), "{r}");
        let tauri = crate::experiment_start(&state, out.clone(), false, Some("{\"sample_ref\":\"S\"}".into())).unwrap();
        assert_eq!(tauri.start_outcome, Some(7), "Tauri start reply: {}", tauri.message);

        // Stop: no generation is NotActive (1) as before; a generation while idle is RunMismatch (3).
        for args in [Value::Null, json!({}), json!({ "expectedStartGeneration": null })] {
            let r = reply("experiment_stop", args.clone());
            assert_eq!((r["ok"].clone(), r["stop_outcome"].clone(), r["message"].clone()), (json!(false), json!(1), json!("notActive")), "{args}: {r}");
            assert!(r.get("start_outcome").is_none(), "{r}");
        }
        for generation in ["0", "7", "18446744073709551615"] {
            let r = reply("experiment_stop", json!({ "expectedStartGeneration": generation }));
            assert_eq!((r["ok"].clone(), r["stop_outcome"].clone()), (json!(false), json!(3)), "{r}");
            assert_eq!(
                r["message"],
                json!(format!("stop refused: expected run {generation}, but no run is active; nothing was stopped")),
                "{r}"
            );
        }
        let tauri = crate::experiment_stop(&state, Some("7".into())).unwrap();
        assert_eq!((tauri.ok, tauri.stop_outcome, tauri.start_outcome), (false, Some(3), None), "{}", tauri.message);
        // A malformed generation is an error: nothing reaches the backend.
        for bad in ["", "+7", "07", " 7", "7 ", "-1", "7.0", "1e3", "0x7", "18446744073709551616"] {
            let error = dispatch(&state, &host, "experiment_stop", json!({ "expectedStartGeneration": bad })).unwrap_err();
            assert!(error.contains("Invalid expectedStartGeneration"), "{bad:?}: {error}");
            assert!(crate::experiment_stop(&state, Some(bad.to_string())).is_err(), "Tauri {bad:?}");
        }
        assert!(dispatch(&state, &host, "experiment_stop", json!({ "expectedStartGeneration": 7 }))
            .unwrap_err().starts_with("INVALID_ARGUMENTS"), "generations travel as strings");
        // Cancel carries the stop outcome too; other replies carry neither field.
        assert_eq!(reply("experiment_cancel", Value::Null)["stop_outcome"], json!(1));
        let other = reply("stop_capture", Value::Null);
        assert!(other.get("start_outcome").is_none() && other.get("stop_outcome").is_none(), "{other}");
        state.bridge.lock().unwrap().pin_mut().shutdown();
        let _ = std::fs::remove_dir_all(&data);
    }

    /// Blob frames as 8-bit BMPs (the mock camera decodes BMP as Mono8): a bright disk moving
    /// along the channel, detected as a valid cell under [`LENIENT`] by the real realtime path.
    pub(crate) fn blob_frames(tag: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("mib_dispatch_{tag}_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let (w, h) = (160u32, 96u32);
        for i in 0..8u32 {
            let (cx, cy, r) = (40 + 10 * i as i32, 48i32, 14i32);
            let offset: u32 = 14 + 40 + 1024;
            let mut b: Vec<u8> = Vec::new();
            b.extend_from_slice(b"BM");
            for v in [offset + w * h, 0, offset, 40] { b.extend_from_slice(&v.to_le_bytes()); }
            b.extend_from_slice(&(w as i32).to_le_bytes());
            b.extend_from_slice(&(h as i32).to_le_bytes());
            b.extend_from_slice(&1u16.to_le_bytes());
            b.extend_from_slice(&8u16.to_le_bytes());
            for v in [0u32, w * h, 2835, 2835, 256, 0] { b.extend_from_slice(&v.to_le_bytes()); }
            for g in 0..256u32 { b.extend_from_slice(&[g as u8, g as u8, g as u8, 0]); }
            for y in (0..h as i32).rev() {
                for x in 0..w as i32 {
                    b.push(if (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r { 220 } else { 0 });
                }
            }
            std::fs::write(dir.join(format!("frame_{i:04}.bmp")), b).unwrap();
        }
        dir
    }

    pub(crate) const LENIENT: &str = r#"{"image_processing": {"gaussian_blur_size": 3, "bg_subtract_threshold": 8,
        "morph_kernel_size": 3, "morph_iterations": 1, "empty_frame_pixel_threshold": 1, "auto_background_enabled": false,
        "filters": {"enable_border_check": false, "enable_area_range_check": false, "enable_deformability_range_check": false,
                    "enable_area_ratio_check": false, "enable_ring_ratio_check": false,
                    "enable_laplacian_variance_check": false, "require_single_inner_contour": false}}}"#;

    fn header_u32(p: &[u8], at: usize) -> u32 { u32::from_le_bytes(p[at..at + 4].try_into().unwrap()) }
    fn header_u64(p: &[u8], at: usize) -> u64 { u64::from_le_bytes(p[at..at + 8].try_into().unwrap()) }

    // ABI 41 (#716 A6a): the live cell commands through dispatch (the WebSocket's path) and the
    // Tauri functions: strict identities, the JSON listing, pins, and the pixels as frame packets
    // of pull kind 5 (image) and 6 (mask) — never in JSON.
    #[test]
    #[serial]
    fn monitoring_cells_through_dispatch() {
        let data = std::env::temp_dir().join(format!("mib_dispatch_cells_{}", std::process::id()));
        let frames = blob_frames("cells");
        let state = AppState::new();
        let host = TestHost(data.to_string_lossy().into_owned());
        assert_eq!(dispatch(&state, &host, "init", json!({ "dataDir": "" })).unwrap(), Reply::Json(json!(true)));
        let reply = |name: &str, args: Value| match dispatch(&state, &host, name, args) {
            Ok(Reply::Json(r)) => r,
            other => panic!("{name}: {other:?}"),
        };
        let packet = |args: Value| match dispatch(&state, &host, "fetch_monitoring_cell_packet", args) {
            Ok(Reply::Binary(p)) => p,
            other => panic!("packet: {other:?}"),
        };

        // Strict arguments: nothing reaches the backend on a malformed identity or layer.
        for bad in ["", "07", "+1", "1.0", "18446744073709551616"] {
            let e = dispatch(&state, &host, "monitoring_cell_pin",
                json!({ "expectedStartGeneration": bad, "captureEpoch": "1", "frameIndex": "1", "objectId": 0 })).unwrap_err();
            assert!(e.contains("Invalid expectedStartGeneration"), "{bad:?}: {e}");
            let e = dispatch(&state, &host, "monitoring_cell_unpin",
                json!({ "startGeneration": "0", "captureEpoch": "1", "frameIndex": bad, "objectId": 0 })).unwrap_err();
            assert!(e.contains("Invalid frameIndex"), "{bad:?}: {e}");
            let e = dispatch(&state, &host, "fetch_monitoring_cell_packet",
                json!({ "startGeneration": "0", "captureEpoch": bad, "frameIndex": "1", "objectId": 0, "layer": "image" }))
                .unwrap_err();
            assert!(e.contains("Invalid captureEpoch"), "{bad:?}: {e}");
        }
        assert!(dispatch(&state, &host, "monitoring_cell_pin",
            json!({ "expectedStartGeneration": 0, "captureEpoch": "1", "frameIndex": "1", "objectId": 0 }))
            .unwrap_err().starts_with("INVALID_ARGUMENTS"), "identities travel as strings");
        assert!(dispatch(&state, &host, "monitoring_cell_pin",
            json!({ "expectedStartGeneration": "0", "frameIndex": "1", "objectId": 0 }))
            .unwrap_err().starts_with("INVALID_ARGUMENTS"), "the capture epoch is required");
        let e = dispatch(&state, &host, "fetch_monitoring_cell_packet",
            json!({ "startGeneration": "0", "captureEpoch": "1", "frameIndex": "1", "objectId": 0, "layer": "rgb" })).unwrap_err();
        assert!(e.contains("Invalid layer"), "{e}");
        // Not held: an empty packet of the requested kind.
        for (layer, kind) in [("image", 5), ("mask", 6)] {
            let p = packet(json!({ "startGeneration": "0", "captureEpoch": "1", "frameIndex": "1", "objectId": 0, "layer": layer }));
            assert_eq!((p.len(), header_u32(&p, 8), header_u32(&p, 12)), (crate::frame_packet::HEADER_BYTES, 0, kind));
        }

        // Real cells: mock camera -> realtime path -> monitoring ring.
        assert_eq!(reply("apply_processing_config_json", json!({ "json": LENIENT }))["ok"], json!(true));
        assert_eq!(reply("apply_processing", json!({ "realtimeEnabled": true, "pixelToMicron": 0.5 }))["ok"], json!(true));
        let r = reply("configure_mock", json!({ "frameDir": frames.to_string_lossy(), "frameIntervalMs": 5, "loopFiles": true }));
        assert_eq!(r["ok"], json!(true), "{r}");
        assert_eq!(reply("monitoring_set_active", json!({ "active": true }))["ok"], json!(true));
        assert_eq!(reply("start_capture", Value::Null)["ok"], json!(true));
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
        let rows = loop {
            let snap = reply("fetch_monitoring_snapshot", json!({ "maxRows": 2000 }));
            let rows: Vec<Value> = snap["rows"].as_array().unwrap().iter().filter(|r| r["valid"] == json!(true)).cloned().collect();
            if rows.len() >= 3 { break rows; }
            assert!(std::time::Instant::now() < deadline, "no valid monitoring rows: {snap}");
            std::thread::sleep(std::time::Duration::from_millis(50));
        };
        assert!(rows.iter().all(|r| r["start_generation"] == json!("0")), "rows carry start_generation as a string");
        assert!(rows.iter().all(|r| r["capture_epoch"].as_str().is_some_and(|e| e != "0")),
                "rows carry capture_epoch as a string");

        let listing = reply("fetch_monitoring_cells", Value::Null);
        let cell = listing["strip"].as_array().unwrap().last().cloned().expect("a strip cell");
        let id = json!({ "startGeneration": cell["start_generation"], "captureEpoch": cell["capture_epoch"],
                         "frameIndex": cell["frame_index"], "objectId": cell["object_id"], "layer": "image" });
        let p = packet(id.clone());
        assert_eq!(&p[..4], b"MIBF");
        assert_eq!((header_u32(&p, 8), header_u32(&p, 12)), (1, 5), "a valid cell image packet");
        assert_eq!(header_u64(&p, 16).to_string(), cell["frame_index"].as_str().unwrap(), "the row's frame index");
        assert_eq!((header_u64(&p, 32), header_u64(&p, 40)),
                   (cell["image"]["width"].as_u64().unwrap(), cell["image"]["height"].as_u64().unwrap()));
        assert_eq!((header_u64(&p, 72), header_u64(&p, 88)), (0, 0), "no capture identities");
        assert_eq!(p.len() as u64, 96 + header_u64(&p, 64));
        let mut mask_id = id.clone();
        mask_id["layer"] = json!("mask");
        let m = packet(mask_id);
        assert_eq!((header_u32(&m, 12), header_u64(&m, 32)), (6, header_u64(&p, 32)), "the mask: kind 6, same crop");

        // Pins through dispatch and through the Tauri functions alike.
        let pin_args = json!({ "expectedStartGeneration": cell["start_generation"], "captureEpoch": cell["capture_epoch"],
                               "frameIndex": cell["frame_index"], "objectId": cell["object_id"] });
        let r = reply("monitoring_cell_pin", pin_args.clone());
        assert_eq!((r["ok"].clone(), r["outcome"].clone()), (json!(true), json!(0)), "{r}");
        assert_eq!(reply("monitoring_cell_pin", pin_args.clone())["outcome"], json!(2));
        let epoch = cell["capture_epoch"].as_str().unwrap().to_string();
        let tauri = crate::monitoring_cell_pin(&state, "3".into(), epoch.clone(), cell["frame_index"].as_str().unwrap().into(),
                                               cell["object_id"].as_i64().unwrap() as i32).unwrap();
        assert_eq!((tauri["ok"].clone(), tauri["outcome"].clone()), (json!(false), json!(3)), "{tauri}");
        let next_epoch = (epoch.parse::<u64>().unwrap() + 1).to_string();
        let other = crate::monitoring_cell_pin(&state, "0".into(), next_epoch, cell["frame_index"].as_str().unwrap().into(),
                                               cell["object_id"].as_i64().unwrap() as i32).unwrap();
        assert_eq!((other["ok"].clone(), other["outcome"].clone()), (json!(false), json!(6)), "CaptureMismatch: {other}");
        assert_eq!(reply("fetch_monitoring_cells", Value::Null)["pins"].as_array().unwrap().len(), 1);
        let u = reply("monitoring_cell_unpin", json!({ "startGeneration": cell["start_generation"],
                      "captureEpoch": cell["capture_epoch"], "frameIndex": cell["frame_index"], "objectId": cell["object_id"] }));
        assert_eq!((u["ok"].clone(), u["message"].clone()), (json!(true), json!("unpinned")), "{u}");
        let c = reply("monitoring_cell_clear_pins", Value::Null);
        assert_eq!((c["ok"].clone(), c["message"].clone()), (json!(true), json!("cleared 0 pin(s)")), "{c}");
        assert!(crate::monitoring_cell_clear_pins(&state).unwrap().ok);

        assert_eq!(reply("stop_capture", Value::Null)["ok"], json!(true));
        state.bridge.lock().unwrap().pin_mut().shutdown();
        let _ = std::fs::remove_dir_all(&frames);
        let _ = std::fs::remove_dir_all(&data);
    }

    #[test]
    fn canonical_u64_is_strict() {
        for (text, value) in [("0", Some(0)), ("7", Some(7)), ("18446744073709551615", Some(u64::MAX))] {
            assert_eq!(crate::parse_canonical_u64(text), value, "{text}");
        }
        for text in ["", "+7", "07", "00", " 7", "7 ", "-0", "7.0", "1e3", "0x7", "18446744073709551616", "７"] {
            assert_eq!(crate::parse_canonical_u64(text), None, "{text:?}");
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
