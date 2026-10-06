//! Headless contract test for the Rust <-> C++ bridge (epic #246, ADR 0003).
//!
//! Drives a real lifecycle against the linked Qt-free backend:
//!   init -> configure mock camera -> start -> observe CameraStatus + FrameReady
//!   events -> fetch_latest_frame -> stop -> shutdown.
//! Asserts the event contract and frame metadata. Runs with no Qt, no webkit,
//! no display — this is the Phase 2 gate and the boundary regression guard.

// The backend bridge is not compiled under `review-only` (ADR 0014); the
// review bridge has its own test (tests/review_bridge.rs).
#![cfg(not(feature = "review-only"))]

use std::path::PathBuf;
use std::time::{Duration, Instant};

use mib_bridge::ffi::{self, BridgeEventKind};
use serial_test::serial;

/// Copy the committed sample frame into a fresh temp dir under OUT-of-tree
/// scratch, enough times for the mock camera (looping) to produce a stream.
fn make_frame_dir() -> PathBuf {
    // A real 512x96 microscopy frame (the .png in the same dir is a 1x1
    // placeholder unsuitable for the pipeline).
    let sample = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../data/mock_frames/frame_00000.tiff");
    assert!(sample.exists(), "sample frame missing: {}", sample.display());

    let dir = std::env::temp_dir().join(format!("mib_bridge_contract_{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    for i in 0..4 {
        let dst = dir.join(format!("frame_{i:04}.tiff"));
        std::fs::copy(&sample, &dst).unwrap();
    }
    dir
}

fn drain_until<F: Fn(&ffi::BridgeEvent) -> bool>(
    bridge: &mut cxx::UniquePtr<ffi::BackendBridge>,
    pred: F,
    timeout: Duration,
) -> Option<ffi::BridgeEvent> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        for ev in bridge.pin_mut().poll_events() {
            if pred(&ev) {
                return Some(ev);
            }
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    None
}

#[test]
fn abi_version_is_stable() {
    // The command/event schema is versioned (ADR 0003). v2 added the review
    // commands; v3 the processing commands; v4 operation state, the bounded
    // event queue, and the extended error sources (ADR 0004); v5 the
    // experiment lifecycle (BE-4); v6 monitoring snapshots + trigger (BE-5);
    // v7 camera discovery/selection (BE-2); v8 config round-trip, ROI/
    // background, and core status (BE-3); v9 paged HDF5 review + export jobs
    // (BE-6); v10 syringe-pump commands/status (BE-7); v11 autofocus/
    // nanopositioner control (BE-8); v12 exact u64 companions; v13 the
    // shared-backend experiment lifecycle (#372): command actions, start/
    // stop outcomes, run completion states, readiness gate statuses, typed
    // ExperimentStatus companions and fetch_experiment_readiness.
    // v14 the asynchronous device-discovery jobs (#419, ADR 0005).
    // v20 Camera & Alignment: set_camera_overview, save_camera_roi,
    // fetch_camera_geometry (YOFO Studio; MindVision and Aravis cameras).
    // v21 fetch_platform_info: science on the PL (YOFO Studio ADR 0008).
    // v22 pump_connect_model: dLSP syringe or Tushui peristaltic per slot.
    // v23 develop (19) and the instrument line (20-22) as one contract; no
    // new commands (ADR 0011 single renumber, so no release build carries an
    // interim number).
    // v24 #501 instrument UI P0: fetch_platform_info capabilities and
    // fetch_instrument_status (PZ7035 PL core, LED, link, latency).
    // v25 the central profile registry (#398): registry_* commands, snapshot
    // and contract groups, and the shell-injected HTTPS transport. Built as a
    // provisional 15 and renumbered once; 24 = #501 P0; 15 and 19-24 are
    // never reused.
    // v26 = ZC300 stage bridge (#464, ADR 0013): stage_* commands,
    // fetch_stage_status, operation kind StageMove, discovery
    // kind MotionStage, stage_move_states. It landed before #501 P1, so it
    // took 26 under the landing-order rule; 27 reserved for #501 P1.
    // v27 #501 P1: set_instrument_mode, set_service_mode, set_instrument_led,
    // fetch_run_preview (PZ7035 Align/Run camera modes).
    // v28 central method authoring (#398 M3b): registry_new_draft_from_revision,
    // registry_new_method_draft, registry_set_draft_notes,
    // registry_draft_from_head, registry_submit_draft, registry_delete_draft,
    // registry_transition, registry_fetch_history -> BridgeRegistryCommand,
    // and the snapshot's drafts, methods, history and submit_conflict.
    // v29 central-method Apply in the React shell (#398 M2c):
    // registry_plan_apply and registry_apply_method over the backend
    // config.json applier.
    // v30 = ZC300 stage without homing (#464, ADR 0013 Amendment 1): stage_home and
    // operation kind StageReference removed; stage_set_zero(mid_travel) added;
    // fetch_stage_status: referenced -> zero_set, + mid_travel_declared and
    // session_only_zero, soft_min_um/soft_max_um -> envelope_min_um/envelope_max_um.
    assert_eq!(ffi::bridge_abi_version(), 30);
}

// ABI 27 (#501 P1): off the PZ7035 the camera-mode commands are refused cleanly, the raw LED
// is refused, and there is no run preview.
#[test]
#[serial]
fn instrument_mode_commands_off_the_instrument() {
    let data = std::env::temp_dir().join(format!("mib_bridge_modes_{}", std::process::id()));
    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data.to_string_lossy()));
    let refused = bridge.pin_mut().set_instrument_mode("run", 152, 200);
    assert!(!refused.ok && refused.message.contains("no PZ7035 control"), "{}", refused.message);
    let bad = bridge.pin_mut().set_instrument_mode("sideways", 0, 0);
    assert!(!bad.ok && bad.message.contains("align or run"), "{}", bad.message);
    assert!(bridge.pin_mut().set_service_mode(true).ok);
    assert!(!bridge.pin_mut().set_instrument_led(7.0, 60.0).ok);
    assert!(bridge.pin_mut().set_service_mode(false).ok);
    assert!(bridge.pin_mut().fetch_run_preview().is_empty());
    let info: serde_json::Value = serde_json::from_str(&bridge.pin_mut().fetch_platform_info()).unwrap();
    assert_eq!(info["capabilities"]["run_mode"], serde_json::json!(false), "{info}");
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data);
}

// ABI 20: a camera without a full-sensor overview (the mock) reports it and
// refuses the overview and a window save cleanly; leaving overview is a no-op.
#[test]
#[serial]
fn camera_alignment_commands_without_overview_camera() {
    let dir = make_frame_dir();
    let data = std::env::temp_dir().join(format!("mib_bridge_align_{}", std::process::id()));
    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data.to_string_lossy()));
    assert!(bridge.pin_mut().configure_mock_camera(&dir.to_string_lossy(), 5, true).ok);
    let geometry: serde_json::Value = serde_json::from_str(&bridge.pin_mut().fetch_camera_geometry()).unwrap();
    assert_eq!(geometry["supported"], serde_json::json!(false), "{geometry}");
    let refused = bridge.pin_mut().set_camera_overview(true);
    assert!(!refused.ok);
    assert!(refused.message.contains("no full-sensor overview"), "{}", refused.message);
    assert!(bridge.pin_mut().set_camera_overview(false).ok, "leaving overview is always possible");
    assert!(!bridge.pin_mut().save_camera_roi(0, 0, 64, 64).ok);
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::remove_dir_all(&data);
}

// #501: the desktop build reports MIB surfaces and no PZ7035; instrument
// status is unavailable off the PZ7035 and never fails the call.
#[test]
#[serial]
fn platform_capabilities_and_instrument_status_on_the_desktop() {
    let data = std::env::temp_dir().join(format!("mib_bridge_platform_{}", std::process::id()));
    let mut bridge = ffi::new_backend_bridge();
    let early: serde_json::Value = serde_json::from_str(&bridge.pin_mut().fetch_instrument_status()).unwrap();
    assert_eq!(early["available"], serde_json::json!(false), "{early}");
    assert!(bridge.pin_mut().initialize(&data.to_string_lossy()));
    let info: serde_json::Value = serde_json::from_str(&bridge.pin_mut().fetch_platform_info()).unwrap();
    let caps = &info["capabilities"];
    assert_eq!(caps["instrument"], serde_json::json!("desktop"), "{info}");
    for host_only in ["autofocus", "trigger", "host_background", "frame_buffer", "reanalysis", "core_updates", "egrabber_script"] {
        assert_eq!(caps[host_only], serde_json::json!(true), "{host_only}: {info}");
    }
    for pz_only in ["pl_identity", "led_strobe", "align_mode", "run_mode"] {
        assert_eq!(caps[pz_only], serde_json::json!(false), "{pz_only}: {info}");
    }
    assert!(caps["pump"].is_null(), "{info}");
    let status: serde_json::Value = serde_json::from_str(&bridge.pin_mut().fetch_instrument_status()).unwrap();
    assert_eq!(status["available"], serde_json::json!(false), "{status}");
    assert!(status["error"].as_str().is_some_and(|e| !e.is_empty()), "{status}");
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data);
}

// BE-8: the autofocus command surface fails safely without hardware, the
// config round-trips without QSettings types, and the status exposes the
// focus-metric freshness explicitly. (The Coremor transport is a platform
// stub on Linux; the real sweep is the Windows half of #278.)
#[test]
#[serial]
fn autofocus_commands_and_config_roundtrip() {
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_af_data_{}", std::process::id()));
    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));

    // Status is valid and disconnected; freshness is explicit (0 = never).
    let status = bridge.pin_mut().fetch_autofocus_status();
    assert!(status.valid && !status.connected && !status.enabled);
    assert_eq!(status.last_ring_ratio_update_us, 0);
    assert_eq!(status.ring_ratio_age_us, 0);

    // New typed endpoint validation never probes a device on malformed input.
    assert!(!bridge.pin_mut().autofocus_connect_endpoint("auto", "x", -1, 115200, 1).ok);
    assert!(!bridge.pin_mut().autofocus_connect_endpoint("oeabt", "", -1, 115200, 1).ok);
    assert!(!bridge.pin_mut().autofocus_connect_endpoint("unknown", "x", -1, 115200, 1).ok);
    let pulse: serde_json::Value = serde_json::from_str(&bridge.pin_mut().pulse_generator_status()).unwrap();
    assert_eq!(pulse["valid"], true);
    assert_eq!(pulse["connected"], false);
    for request in [r#"{"action":"connect","port":"","address":1}"#, r#"{"action":"frequency","channel":0,"value":0}"#, r#"{"action":"duty","channel":5,"value":50}"#] {
        assert!(!bridge.pin_mut().pulse_generator_command(request).ok);
    }
    // Structured parameter errors and safe failure without hardware.
    assert!(!bridge.pin_mut().autofocus_connect(-1, 115200, 1).ok);
    assert!(!bridge.pin_mut().autofocus_connect(3, 115200, 999).ok);
    // A port number no host can have: on a Windows bench a low number such as
    // COM3 may exist (e.g. the Intel AMT serial-over-LAN port) and the Coremor
    // DLL will open it and tolerate an implausible first read, so "fails
    // without hardware" must not depend on which ports the host happens to have.
    let connect = bridge.pin_mut().autofocus_connect(250, 115200, 1);
    assert!(!connect.ok, "connect must fail without the Coremor SDK/hardware");

    // Control on a disconnected controller fails cleanly; disable is safe.
    assert!(!bridge.pin_mut().autofocus_set_enabled(true).ok);
    assert!(!bridge.pin_mut().autofocus_jog(true).ok);
    assert!(!bridge.pin_mut().autofocus_jog(false).ok);
    assert!(bridge.pin_mut().autofocus_set_enabled(false).ok);
    assert!(bridge.pin_mut().autofocus_disconnect().ok, "disconnect is idempotent");

    // Config round-trips through plain values (no QSettings anywhere).
    let mut config = bridge.pin_mut().fetch_autofocus_config();
    assert!(config.valid);
    config.focus_setpoint = 22.5;
    config.voltage_step = 2.0;
    config.ring_ratio_stale_ms = 900;
    config.require_new_sample_per_step = false;
    config.focus_direction = false;
    assert!(bridge.pin_mut().autofocus_set_config(config.clone()).ok);
    let round = bridge.pin_mut().fetch_autofocus_config();
    assert!((round.focus_setpoint - 22.5).abs() < 1e-9);
    assert!((round.voltage_step - 2.0).abs() < 1e-9);
    assert_eq!(round.ring_ratio_stale_ms, 900);
    assert!(!round.require_new_sample_per_step);
    assert!(!round.focus_direction);

    // Invalid voltage range is a structured error that leaves config intact.
    let mut bad = round.clone();
    bad.min_voltage = 50.0;
    bad.max_voltage = 10.0;
    assert!(!bridge.pin_mut().autofocus_set_config(bad).ok);
    assert!((bridge.pin_mut().fetch_autofocus_config().focus_setpoint - 22.5).abs() < 1e-9);

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data_dir);
}

// BE-7: the pump command surface fails safely without hardware — structured
// errors for invalid ids/ports/params, valid-but-disconnected snapshots, and
// the address scan completing as a tracked operation. (The full fake-Modbus
// end-to-end for both pump identities is backend.pump_bridge_facade in CTest.)
#[test]
#[serial]
fn pump_commands_fail_safely_without_hardware() {
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_pump_data_{}", std::process::id()));
    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));

    // Snapshots: valid for both identities, invalid pump id fails.
    let sample = bridge.pin_mut().fetch_pump_status(0);
    assert!(sample.valid && !sample.connected);
    assert!(bridge.pin_mut().fetch_pump_status(1).valid);
    assert!(!bridge.pin_mut().fetch_pump_status(9).valid);

    // Structured parameter errors.
    assert!(!bridge.pin_mut().pump_connect(9, 3, 115200, 1).ok);
    assert!(!bridge.pin_mut().pump_connect(0, -1, 115200, 1).ok);
    assert!(!bridge.pin_mut().pump_connect(0, 3, 115200, 300).ok);
    assert!(!bridge.pin_mut().pump_set_flow_rate(0, -5.0, 100).ok);
    assert!(!bridge.pin_mut().pump_set_syringe_volume(0, 0, 1).ok);
    assert_eq!(sample.model, 0, "slots default to the dLSP syringe model");
    assert!(!bridge.pin_mut().pump_connect_model(0, 2, "/dev/ttyPS1", 115200, 3, 25.0).ok);
    assert!(!bridge.pin_mut().pump_connect_model(0, 1, "/dev/ttyPS1", 115200, 3, 0.0).ok);
    assert!(!bridge.pin_mut().pump_connect_model(0, 1, "/dev/ttyPS1", 115200, 300, 25.0).ok);

    // No hardware on this platform: a real connect fails without hanging, and
    // control commands on a disconnected pump fail cleanly.
    assert!(!bridge.pin_mut().pump_connect(0, 200, 115200, 1).ok);
    assert!(!bridge.pin_mut().pump_start(0).ok);
    assert!(!bridge.pin_mut().pump_stop(0).ok);
    assert!(!bridge.pin_mut().pump_purge(0, 0).ok);

    // The address scan runs as a tracked operation and completes (empty
    // result without hardware) rather than blocking the bridge.
    let _ = bridge.pin_mut().poll_events();
    let scan = bridge.pin_mut().pump_scan_addresses(200, 115200, 1, 2, 20);
    assert!(scan.ok && scan.operation_id != 0, "scan failed to start: {}", scan.message);
    let deadline = Instant::now() + Duration::from_secs(10);
    let mut completed = false;
    while Instant::now() < deadline && !completed {
        for e in bridge.pin_mut().poll_events() {
            if e.kind == BridgeEventKind::OperationStatus && e.u0 == scan.operation_id && e.u2 == 2
            {
                assert!(e.text.is_empty(), "unexpected scan result: {}", e.text);
                completed = true;
            }
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    assert!(completed, "pump scan did not complete");

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data_dir);
}

// Z stage (#464, ADR 0013 Amendment 1): with no controller the commands fail
// cleanly and never hang; Stop is always accepted; the snapshot reports a stage
// that is neither connected, zeroed nor limit-verified.
#[test]
#[serial]
fn stage_commands_fail_safely_without_hardware() {
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_stage_data_{}", std::process::id()));
    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));

    let status = bridge.pin_mut().fetch_stage_status();
    assert!(status.valid && !status.connected && !status.zero_set && !status.limits_verified && !status.busy);
    assert!(!status.mid_travel_declared && status.envelope_min_um == 0.0 && status.envelope_max_um == 0.0);
    assert!(!status.session_only_zero, "power-cycle detection is on by default");

    assert!(bridge.pin_mut().stage_stop().ok, "Stop is always accepted");
    assert!(!bridge.pin_mut().stage_move_to(0.0).ok);
    assert!(!bridge.pin_mut().stage_move_by(10.0).ok);
    for mid_travel in [false, true] {
        let zero = bridge.pin_mut().stage_set_zero(mid_travel);
        assert!(!zero.ok && zero.operation_id == 0, "no stage to zero");
    }
    assert!(!bridge.pin_mut().stage_apply_profile().ok);
    assert!(!bridge.pin_mut().stage_connect("", "", 0).ok, "no endpoint configured");
    assert!(!bridge.pin_mut().stage_connect("ttyMIB-NO-SUCH-PORT", "", 300).ok, "address out of range");
    let started = Instant::now();
    assert!(!bridge.pin_mut().stage_connect("ttyMIB-NO-SUCH-PORT", "", 1).ok);
    assert!(started.elapsed() < Duration::from_secs(10), "a missing port fails fast");
    assert!(bridge.pin_mut().stage_disconnect().ok);
    assert!(!data_dir.join("stage_limits_verified.json").exists(), "the bridge never writes the limits record");

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data_dir);
}

// BE-6: review metadata, paged metrics, bounded image pulls, and the
// cancellable CSV export job — over a recording produced in-test, so
// Qt-written fixtures and bridge-written files share one code path.
#[test]
#[serial]
fn review_metadata_pages_images_and_export_job() {
    let frame_dir = make_frame_dir();
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_rev6_data_{}", std::process::id()));
    let rec_path = std::env::temp_dir().join(format!("mib_bridge_rev6_{}.h5", std::process::id()));
    let csv_path = std::env::temp_dir().join(format!("mib_bridge_rev6_{}.csv", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));

    // Without a loaded file: metadata reports closed, export fails safely.
    let meta = bridge.pin_mut().fetch_review_metadata();
    assert!(meta.valid && !meta.file_open);
    assert!(!bridge.pin_mut().review_export_csv(&csv_path.to_string_lossy()).ok);

    // Produce a short recording.
    assert!(bridge
        .pin_mut()
        .configure_mock_camera(&frame_dir.to_string_lossy(), 5, true)
        .ok);
    assert!(bridge.pin_mut().start_capture().ok);
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline && !bridge.pin_mut().fetch_latest_frame().valid {
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(bridge.pin_mut().start_frame_recording(&rec_path.to_string_lossy()).ok);
    std::thread::sleep(Duration::from_millis(200));
    assert!(bridge.pin_mut().stop_frame_recording().ok);
    assert!(bridge.pin_mut().stop_capture().ok);
    assert!(bridge.pin_mut().load_recording(&rec_path.to_string_lossy()).ok);

    // Metadata: recording mode, counts, dataset capabilities.
    let meta = bridge.pin_mut().fetch_review_metadata();
    assert!(meta.valid && meta.file_open && meta.recording_file);
    assert!(meta.total_valid > 0, "no recorded frames reported");
    assert!(meta.recorded_images.present);
    assert_eq!(meta.recorded_images.width, 512);
    assert_eq!(meta.recorded_images.height, 96);
    assert!(meta.file_path.ends_with(".h5"));

    // Paged metrics: bounded pages, stable totals, out-of-range is empty.
    let page = bridge.pin_mut().fetch_review_metrics_page(true, 0, 5);
    assert!(page.valid);
    assert!(page.total == meta.total_valid, "page total mismatch");
    assert!(page.rows.len() <= 5);
    let beyond = bridge.pin_mut().fetch_review_metrics_page(true, page.total + 10, 5);
    assert!(beyond.valid && beyond.rows.is_empty());

    // Bounded image pull from the recorded dataset; invalid dataset ids and
    // out-of-range indices fail safely.
    let image = bridge.pin_mut().fetch_review_image(2, 0);
    assert!(image.valid && image.width == 512 && image.height == 96);
    assert!(!image.data.is_empty());
    assert!(!bridge.pin_mut().fetch_review_image(99, 0).valid);
    assert!(!bridge.pin_mut().fetch_review_image(2, 1_000_000).valid);

    // Export job: tracked operation with progress + Completed; CSV exists
    // with the Qt-parity header; the source file remains readable.
    let _ = bridge.pin_mut().poll_events();
    let export = bridge.pin_mut().review_export_csv(&csv_path.to_string_lossy());
    assert!(export.ok, "export failed to start: {}", export.message);
    assert!(export.operation_id != 0);

    let deadline = Instant::now() + Duration::from_secs(10);
    let mut completed = false;
    while Instant::now() < deadline && !completed {
        for e in bridge.pin_mut().poll_events() {
            if e.kind == BridgeEventKind::OperationStatus && e.u0 == export.operation_id {
                assert_ne!(e.u2, 3, "export failed: {}", e.text);
                if e.u2 == 2 {
                    completed = true;
                }
            }
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    assert!(completed, "export did not complete");
    let retained: serde_json::Value = serde_json::from_str(&bridge.pin_mut().review_export_status_json()).unwrap();
    assert_eq!(retained["state"], "completed");
    assert_eq!(retained["operation_id"], export.operation_id.to_string());
    assert_eq!(retained["final_path"], csv_path.to_string_lossy().as_ref());
    let csv = std::fs::read_to_string(&csv_path).unwrap();
    assert!(csv.starts_with("Frame Type,Index,Timestamp,Object Id"));
    assert!(csv.lines().count() as u64 >= meta.total_valid, "missing CSV rows");

    // Failure path cleans partial outputs: unwritable directory fails the
    // job and leaves no file behind.
    let bad_path = csv_path.join("mib_export.csv"); // existing file cannot be a parent
    let bad = bridge.pin_mut().review_export_csv(&bad_path.to_string_lossy());
    assert!(bad.ok, "job starts, then fails asynchronously");
    let deadline = Instant::now() + Duration::from_secs(10);
    let mut failed = false;
    while Instant::now() < deadline && !failed {
        for e in bridge.pin_mut().poll_events() {
            if e.kind == BridgeEventKind::OperationStatus
                && e.u0 == bad.operation_id
                && e.u2 == 3
            {
                failed = true;
            }
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    assert!(failed, "bad-path export did not report Failed");
    assert!(!bad_path.exists());
    // Source recording is intact after the failed job.
    assert!(bridge.pin_mut().load_recording(&rec_path.to_string_lossy()).ok);

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&frame_dir);
    let _ = std::fs::remove_dir_all(&data_dir);
    let _ = std::fs::remove_file(&rec_path);
    let _ = std::fs::remove_file(&csv_path);
}

// BE-3: lossless processing-config round-trip, ROI/background binary
// transfer, external-change observability, and core identity/pin status.
#[test]
#[serial]
fn processing_config_roundtrip_and_core_status() {
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_cfg_data_{}", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));

    // Full config document round-trips without loss.
    let doc = bridge.pin_mut().fetch_processing_config_json();
    assert!(doc.valid, "config document unavailable");
    let parsed: serde_json::Value = serde_json::from_str(&doc.json).unwrap();
    assert!(parsed["image_processing"]["filters"].is_object());
    let version_before = parsed["config_version"].as_u64().unwrap();

    // Merge-apply changes only the present keys; everything else survives.
    let baseline_blur = parsed["image_processing"]["gaussian_blur_size"].as_i64().unwrap();
    let apply = bridge.pin_mut().apply_processing_config_json(
        r#"{
            "image_processing": {"area_threshold_min": 42,
                                 "target_group": {"enabled": true, "area_min": 50}},
            "realtime_processing": {"mode": "async_batch", "batch_size": 8},
            "flush_interval": 512,
            "pixel_to_micron": 4.5
        }"#,
    );
    assert!(apply.ok, "apply failed: {}", apply.message);

    let doc2 = bridge.pin_mut().fetch_processing_config_json();
    let parsed2: serde_json::Value = serde_json::from_str(&doc2.json).unwrap();
    assert_eq!(parsed2["image_processing"]["area_threshold_min"], 42);
    assert_eq!(parsed2["image_processing"]["target_group"]["enabled"], true);
    assert_eq!(parsed2["image_processing"]["target_group"]["area_min"], 50);
    assert_eq!(
        parsed2["image_processing"]["gaussian_blur_size"].as_i64().unwrap(),
        baseline_blur,
        "absent key must keep its value (no silent default substitution)"
    );
    assert_eq!(parsed2["realtime_processing"]["mode"], "async_batch");
    assert_eq!(parsed2["realtime_processing"]["batch_size"], 8);
    assert_eq!(parsed2["flush_interval"], 512);
    assert!((parsed2["pixel_to_micron"].as_f64().unwrap() - 4.5).abs() < 1e-9);
    // External-change observability: the monotonic version advanced.
    assert!(parsed2["config_version"].as_u64().unwrap() > version_before);

    // Malformed documents fail without touching state.
    assert!(!bridge.pin_mut().apply_processing_config_json("{not json").ok);
    let bad_type = bridge
        .pin_mut()
        .apply_processing_config_json(r#"{"image_processing": {"gaussian_blur_size": "big"}}"#);
    assert!(!bad_type.ok, "type mismatch must fail");
    let parsed3: serde_json::Value =
        serde_json::from_str(&bridge.pin_mut().fetch_processing_config_json().json).unwrap();
    assert_eq!(
        parsed3["image_processing"]["gaussian_blur_size"].as_i64().unwrap(),
        baseline_blur
    );

    // ROI set/get round-trip.
    assert!(bridge.pin_mut().set_processing_roi(4, 8, 100, 50).ok);
    let parsed4: serde_json::Value =
        serde_json::from_str(&bridge.pin_mut().fetch_processing_config_json().json).unwrap();
    assert_eq!(parsed4["roi"]["x"], 4);
    assert_eq!(parsed4["roi"]["w"], 100);

    // Background image: binary set/get/clear.
    assert!(!bridge.pin_mut().fetch_background_image().valid, "no background expected yet");
    let bg: Vec<u8> = (0..(64 * 32)).map(|i| (i % 251) as u8).collect();
    assert!(!bridge.pin_mut().set_background_image(64, 32, &bg[..10]).ok, "bad length must fail");
    assert!(bridge.pin_mut().set_background_image(64, 32, &bg).ok);
    let fetched = bridge.pin_mut().fetch_background_image();
    assert!(fetched.valid && fetched.width == 64 && fetched.height == 32);
    assert_eq!(fetched.data.as_slice(), bg.as_slice(), "background bytes must round-trip");
    let parsed5: serde_json::Value =
        serde_json::from_str(&bridge.pin_mut().fetch_processing_config_json().json).unwrap();
    assert_eq!(parsed5["background_set"], true);
    assert!(bridge.pin_mut().clear_background_image().ok);
    assert!(!bridge.pin_mut().fetch_background_image().valid);

    // Core identity/pin status is observable and consistent.
    let core = bridge.pin_mut().fetch_processing_core_status();
    assert!(core.valid);
    assert!(!core.active_version.is_empty());
    assert!(core.contract_version >= 1);
    // No administrator pin in the test environment -> satisfied.
    assert!(core.pin_satisfied);

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data_dir);
}

// BE-2: camera discovery/selection contract — mock enumeration/selection is
// headless-testable, invalid indices/paths return structured errors, and the
// selected-device snapshot is authoritative and survives capture start/stop.
#[test]
#[serial]
fn camera_discovery_and_selection_contract() {
    let frame_dir = make_frame_dir();
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_cam_data_{}", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));

    // Discovery is a job (ABI 14): start, poll to a terminal state. A camera
    // job always carries the synthetic mock entry (camera_type 2, synthetic);
    // hardware candidates are empty without the SDKs on this platform.
    let start = bridge.pin_mut().start_camera_discovery();
    assert!(start.accepted && start.job_id != 0, "camera discovery job refused: {}", start.reason);
    let deadline = Instant::now() + Duration::from_secs(30);
    let snapshot = loop {
        let s = bridge.pin_mut().fetch_device_discovery(start.job_id);
        assert!(s.valid, "discovery snapshot invalid for job {}", start.job_id);
        // 2 Completed, 3 Cancelled, 4 Failed (contract discovery_job_states).
        if s.state >= 2 {
            break s;
        }
        assert!(Instant::now() < deadline, "camera discovery job did not finish");
        std::thread::sleep(Duration::from_millis(10));
    };
    // On platforms with camera SDKs the job Completes (2); without them every
    // provider reports MissingSdk and the job is Failed (4). Both are terminal
    // and the facade still appends the synthetic mock entry.
    assert!(
        snapshot.state == 2 || snapshot.state == 4,
        "camera discovery job should reach a terminal state (Completed=2 or Failed=4), got {}",
        snapshot.state,
    );
    assert!(
        snapshot.candidates.iter().any(|c| c.camera_type == 2 && c.synthetic && c.kind == 0),
        "mock camera entry missing from discovery"
    );
    assert!(!bridge.pin_mut().cancel_device_discovery(start.job_id), "cancel of an ended job is refused");
    assert!(!bridge.pin_mut().fetch_device_discovery(987_654_321).valid, "unknown job is invalid");
    // A refused request reports a contract discovery_error_kinds value.
    let empty = ffi::BridgeDiscoveryRequest::default();
    let refused = bridge.pin_mut().start_device_discovery(&empty);
    assert!(!refused.accepted && refused.rejection == 1, "empty request is InvalidRequest (1)");

    // The boot selection is authoritative: on platforms without the camera
    // SDKs the backend falls back to a mock factory at initialize, so the
    // mode is either None (hardware default) or Mock (fallback) — never a
    // hardware/MindVision claim without the SDK.
    let selection = bridge.pin_mut().fetch_camera_selection();
    assert!(selection.valid);
    assert!(
        selection.mode == 0 || selection.mode == 1,
        "unexpected boot selection mode {}",
        selection.mode
    );

    // Structured errors for invalid selections / paths.
    assert!(!bridge.pin_mut().select_hardware_camera(-1, 0, "bad").ok);
    assert!(!bridge.pin_mut().select_mindvision_camera(-1, "bad", "").ok);
    let script = bridge.pin_mut().apply_camera_script("/nonexistent/script.js");
    assert!(!script.ok && script.message.contains("No hardware camera selected"));
    assert!(!bridge.pin_mut().reset_hardware_camera().ok);

    // Configure the mock source; the snapshot becomes authoritative.
    assert!(bridge
        .pin_mut()
        .configure_mock_camera(&frame_dir.to_string_lossy(), 7, true)
        .ok);
    let selection = bridge.pin_mut().fetch_camera_selection();
    assert_eq!(selection.mode, 1, "expected Mock selection mode");
    assert!(selection.configured);
    assert_eq!(selection.mock_interval_ms, 7);
    assert!(selection.mock_loop);
    assert!(selection.mock_frame_dir.ends_with(
        frame_dir.file_name().unwrap().to_str().unwrap()
    ));
    assert!(!selection.running);

    // Selection survives capture start/stop and reports running state.
    assert!(bridge.pin_mut().start_capture().ok);
    let running = bridge.pin_mut().fetch_camera_selection();
    assert!(running.running && running.mode == 1 && running.configured);
    assert!(bridge.pin_mut().stop_capture().ok);
    let stopped = bridge.pin_mut().fetch_camera_selection();
    assert!(!stopped.running && stopped.mode == 1 && stopped.configured);

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&frame_dir);
    let _ = std::fs::remove_dir_all(&data_dir);
}

// BE-5: bounded monitoring snapshot semantics and the sorter trigger contract
// (manual, periodic, stop, and failure behavior with the mock camera's
// trigger-output emulation).
#[test]
#[serial]
fn monitoring_and_trigger_contract() {
    let frame_dir = make_frame_dir();
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_mon_data_{}", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
    assert!(bridge
        .pin_mut()
        .configure_mock_camera(&frame_dir.to_string_lossy(), 5, true)
        .ok);

    // Failure behavior: no camera attached yet -> manual pulse and periodic
    // start fail with a structured message.
    let res = bridge.pin_mut().trigger_manual_pulse();
    assert!(!res.ok && res.message.contains("No camera"), "{}", res.message);
    assert!(!bridge.pin_mut().trigger_periodic_start(10).ok);
    // Invalid parameters are structured errors.
    assert!(!bridge.pin_mut().trigger_set_pulse_duration(0).ok);
    assert!(!bridge.pin_mut().trigger_periodic_start(0).ok);

    let reference: serde_json::Value = serde_json::from_str(&bridge.pin_mut().fetch_monitoring_chart_reference()).unwrap();
    assert!(!reference["curves"].as_array().unwrap().is_empty());
    assert!(reference["curve_source"].as_str().unwrap().contains("30 um"));
    // Monitoring enable/disable/clear round-trip.
    assert!(bridge.pin_mut().monitoring_set_active(true).ok);
    let snap = bridge.pin_mut().fetch_monitoring_snapshot(50);
    assert!(snap.valid && snap.monitoring_active);
    assert_eq!(snap.capacity, 1000);
    assert!(snap.rows.len() <= 50, "snapshot not bounded: {}", snap.rows.len());
    assert!(bridge.pin_mut().monitoring_clear().ok);
    let cleared = bridge.pin_mut().fetch_monitoring_snapshot(50);
    assert_eq!(cleared.valid_appended, 0);
    assert_eq!(cleared.invalid_appended, 0);
    assert!(bridge.pin_mut().monitoring_set_active(false).ok);
    assert!(!bridge.pin_mut().fetch_monitoring_snapshot(50).monitoring_active);

    // Attach the camera by starting capture; the trigger service receives it
    // through the camera-ready callback.
    assert!(bridge.pin_mut().start_capture().ok);
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline && !bridge.pin_mut().fetch_trigger_status().camera_attached {
        std::thread::sleep(Duration::from_millis(10));
    }
    let status = bridge.pin_mut().fetch_trigger_status();
    assert!(status.valid && status.camera_attached, "trigger camera never attached");

    // Pulse duration round-trips.
    assert!(bridge.pin_mut().trigger_set_pulse_duration(5).ok);
    assert_eq!(bridge.pin_mut().fetch_trigger_status().pulse_duration_us, 5);

    // Manual pulse increments the trigger count.
    let before = bridge.pin_mut().fetch_trigger_status().trigger_count;
    assert!(bridge.pin_mut().trigger_manual_pulse().ok);
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline
        && bridge.pin_mut().fetch_trigger_status().trigger_count <= before
    {
        std::thread::sleep(Duration::from_millis(5));
    }
    assert!(
        bridge.pin_mut().fetch_trigger_status().trigger_count > before,
        "manual pulse did not fire"
    );

    // Periodic test: counts grow while running, stop halts the generator.
    let before = bridge.pin_mut().fetch_trigger_status().trigger_count;
    assert!(bridge.pin_mut().trigger_periodic_start(5).ok);
    assert!(bridge.pin_mut().fetch_trigger_status().periodic_active);
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline
        && bridge.pin_mut().fetch_trigger_status().trigger_count < before + 3
    {
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(
        bridge.pin_mut().fetch_trigger_status().trigger_count >= before + 3,
        "periodic test did not generate pulses"
    );
    assert!(bridge.pin_mut().trigger_periodic_stop().ok);
    assert!(!bridge.pin_mut().fetch_trigger_status().periodic_active);

    assert!(bridge.pin_mut().stop_capture().ok);
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&frame_dir);
    let _ = std::fs::remove_dir_all(&data_dir);
}

// The Rust enum values must match contract/bridge-contract.json — the single
// source of truth also pinned by shim.cpp static_asserts (C++) and the
// generated desktop/src/bridgeContract.ts (TypeScript). Drift fails here.
#[test]
fn rust_enums_match_contract_json() {
    let contract: serde_json::Value =
        serde_json::from_str(include_str!("../contract/bridge-contract.json")).unwrap();

    assert_eq!(
        contract["abi_version"].as_u64().unwrap() as u32,
        ffi::bridge_abi_version(),
        "abi_version drifted between JSON contract and bridge"
    );

    let kinds = contract["event_kinds"].as_object().unwrap();
    let expected: &[(&str, BridgeEventKind)] = &[
        ("FrameReady", BridgeEventKind::FrameReady),
        ("CameraStatus", BridgeEventKind::CameraStatus),
        ("RecordingStatus", BridgeEventKind::RecordingStatus),
        ("ProcessingResult", BridgeEventKind::ProcessingResult),
        ("PlaybackPosition", BridgeEventKind::PlaybackPosition),
        ("BackendError", BridgeEventKind::BackendError),
        ("OperationStatus", BridgeEventKind::OperationStatus),
        ("QueueOverflow", BridgeEventKind::QueueOverflow),
        ("ExperimentStatus", BridgeEventKind::ExperimentStatus),
    ];
    assert_eq!(kinds.len(), expected.len(), "event kind count drifted");
    for (name, kind) in expected {
        assert_eq!(
            kinds[*name].as_u64().unwrap() as u32,
            kind.repr,
            "event kind {name} drifted from the JSON contract"
        );
    }

    // ABI 13 experiment groups: pinned in C++ by static_asserts in shim.cpp
    // (the shared coordinator's enums); pinned here for the Rust/TS side.
    let groups: &[(&str, &[(&str, u64)])] = &[
        ("experiment_command_actions", &[("EvaluateReadiness", 0), ("Start", 1), ("Stop", 2), ("Status", 3)]),
        ("experiment_start_outcomes", &[("Started", 0), ("NotReady", 1), ("StaleReadiness", 2), ("AlreadyActive", 3),
                                        ("StorageFailed", 4), ("ProvenanceFailed", 5), ("Busy", 6)]),
        ("experiment_stop_outcomes", &[("Accepted", 0), ("NotActive", 1), ("Busy", 2)]),
        ("run_completion_states", &[("Complete", 0), ("IntentionallyPartial", 1), ("IncompleteLoss", 2), ("Failed", 3), ("Unknown", 4)]),
        ("readiness_gate_statuses", &[("Pass", 0), ("Warn", 1), ("Fail", 2), ("Unavailable", 3), ("NotRequired", 4)]),
        // ABI 14 discovery groups (#419): pinned in C++ by static_asserts in shim.cpp.
        ("discovery_device_kinds", &[("Camera", 0), ("Framegrabber", 1), ("Nanopositioner", 2), ("PulseGenerator", 3),
                                     ("MotionStage", 4)]),
        // Z stage (#464): pinned in C++ by static_asserts in shim.cpp.
        ("stage_move_states", &[("Idle", 0), ("Moving", 1), ("Homing", 2), ("Faulted", 3)]),
        ("discovery_job_states", &[("Queued", 0), ("Running", 1), ("Completed", 2), ("Cancelled", 3), ("Failed", 4)]),
        ("discovery_identity_strengths", &[("None", 0), ("SessionLocal", 1), ("Persistent", 2)]),
        ("discovery_identification_statuses", &[("Identified", 0), ("Unidentified", 1), ("Ambiguous", 2), ("Unsupported", 3)]),
        ("discovery_error_kinds", &[("None", 0), ("InvalidRequest", 1), ("Busy", 2), ("OpenFailed", 3), ("PermissionDenied", 4),
                                    ("Timeout", 5), ("MalformedResponse", 6), ("Unsupported", 7), ("MissingSdk", 8),
                                    ("ProviderException", 9), ("Cancelled", 10), ("Overflow", 11), ("ShuttingDown", 12),
                                    ("TooManyJobs", 13)]),
        // ABI 25 registry groups (#398): pinned in C++ by static_asserts in shim.cpp.
        ("registry_session_states", &[("SignedOut", 0), ("SignedIn", 1), ("CachedOffline", 2)]),
        ("registry_connectivity", &[("Unknown", 0), ("Online", 1), ("Offline", 2), ("AuthenticationRequired", 3),
                                    ("PermissionDenied", 4), ("Failed", 5)]),
        (
            "registry_job_kinds",
            &[
                ("SignIn", 0),
                ("SignOut", 1),
                ("Refresh", 2),
                ("Download", 3),
                ("Materialize", 4),
                ("RecordValidation", 5),
                ("SaveDraft", 6),
                ("DeleteDraft", 7),
                ("SubmitDraft", 8),
                ("Transition", 9),
                ("FetchHistory", 10),
            ],
        ),
        ("registry_job_states", &[("Queued", 0), ("Running", 1), ("Succeeded", 2), ("Partial", 3), ("Failed", 4),
                                  ("Cancelled", 5)]),
        ("registry_central_states", &[("Submitted", 0), ("Approved", 1), ("Rejected", 2), ("Published", 3),
                                      ("Superseded", 4), ("Archived", 5), ("Revoked", 6)]),
        ("registry_local_validation", &[("None", 0), ("Passed", 1), ("Failed", 2)]),
    ];
    for (group, values) in groups {
        let obj = contract[*group].as_object().unwrap_or_else(|| panic!("missing contract group {group}"));
        assert_eq!(obj.len(), values.len(), "{group} count drifted");
        for (name, value) in *values {
            assert_eq!(obj[*name].as_u64().unwrap(), *value, "{group}.{name} drifted");
        }
    }
    let payload = contract["event_payloads"]["ExperimentStatus"].as_object().unwrap();
    for field in ["startGeneration", "persistenceAdmitted", "persistenceCommitted", "persistenceFailed",
                  "completion", "terminal", "finalizationOk"] {
        assert!(payload.contains_key(field), "ExperimentStatus payload lacks {field}");
    }
}

// Duplicate/late commands must fail safely without desynchronizing state
// (BE-1): double start is idempotent, double stop is idempotent, and the
// pipeline still works afterwards.
#[test]
#[serial]
fn duplicate_start_stop_cannot_desynchronize() {
    let frame_dir = make_frame_dir();
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_dup_data_{}", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
    assert!(bridge
        .pin_mut()
        .configure_mock_camera(&frame_dir.to_string_lossy(), 5, true)
        .ok);

    assert!(bridge.pin_mut().start_capture().ok);
    // Duplicate start while running: must not tear down or double-spawn.
    assert!(bridge.pin_mut().start_capture().ok);

    let deadline = Instant::now() + Duration::from_secs(5);
    let mut got = false;
    while Instant::now() < deadline {
        if bridge.pin_mut().fetch_latest_frame().valid {
            got = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(got, "no frame after duplicate start");

    assert!(bridge.pin_mut().stop_capture().ok);
    // Duplicate stop: idempotent.
    assert!(bridge.pin_mut().stop_capture().ok);

    // The pipeline still works after the duplicate commands.
    assert!(bridge.pin_mut().start_capture().ok);
    assert!(bridge.pin_mut().stop_capture().ok);

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&frame_dir);
    let _ = std::fs::remove_dir_all(&data_dir);
}

// Cancelling an unknown or finished operation fails safely (BE-1).
#[test]
#[serial]
fn cancel_unknown_operation_fails_safely() {
    let data_dir =
        std::env::temp_dir().join(format!("mib_bridge_cancel_data_{}", std::process::id()));
    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));

    let res = bridge.pin_mut().cancel_operation(999_999);
    assert!(!res.ok, "cancelling an unknown operation must not report ok");
    assert!(
        res.message.to_lowercase().contains("unknown"),
        "unexpected message: {}",
        res.message
    );

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data_dir);
}

// A recording load is a tracked operation: the command result carries a
// non-zero operation_id and Started/Completed OperationStatus events with the
// same id bracket it (BE-1).
#[test]
#[serial]
fn recording_load_emits_operation_lifecycle() {
    let frame_dir = make_frame_dir();
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_op_data_{}", std::process::id()));
    let rec_path = std::env::temp_dir().join(format!("mib_bridge_op_{}.h5", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
    assert!(bridge
        .pin_mut()
        .configure_mock_camera(&frame_dir.to_string_lossy(), 5, true)
        .ok);
    assert!(bridge.pin_mut().start_capture().ok);
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline && !bridge.pin_mut().fetch_latest_frame().valid {
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(bridge.pin_mut().start_frame_recording(&rec_path.to_string_lossy()).ok);
    std::thread::sleep(Duration::from_millis(200));
    assert!(bridge.pin_mut().stop_frame_recording().ok);
    assert!(bridge.pin_mut().stop_capture().ok);
    // Discard events so far; focus on the load.
    let _ = bridge.pin_mut().poll_events();

    let load = bridge.pin_mut().load_recording(&rec_path.to_string_lossy());
    assert!(load.ok, "load_recording failed: {}", load.message);
    assert!(load.operation_id != 0, "load did not report an operation id");

    let events = bridge.pin_mut().poll_events();
    let op_events: Vec<_> = events
        .iter()
        .filter(|e| e.kind == BridgeEventKind::OperationStatus && e.u0 == load.operation_id)
        .collect();
    // u2 = state: 0 Started .. 2 Completed (contract values).
    assert!(
        op_events.iter().any(|e| e.u2 == 0),
        "no Started operation event for the load"
    );
    assert!(
        op_events.iter().any(|e| e.u2 == 2),
        "no Completed operation event for the load"
    );
    // A finished operation can no longer be cancelled.
    assert!(!bridge.pin_mut().cancel_operation(load.operation_id).ok);

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&frame_dir);
    let _ = std::fs::remove_dir_all(&data_dir);
    let _ = std::fs::remove_file(&rec_path);
}

// Mock E2E for the experiment lifecycle (BE-4): preconditions, start →
// accumulate → stop → the finalized HDF5 reopens, and the experiment's
// operation lifecycle completes.
#[test]
#[serial]
fn experiment_lifecycle_end_to_end() {
    let frame_dir = make_frame_dir();
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_exp_data_{}", std::process::id()));
    let out_path = std::env::temp_dir().join(format!("mib_bridge_exp_{}.h5", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
    assert!(bridge
        .pin_mut()
        .configure_mock_camera(&frame_dir.to_string_lossy(), 5, true)
        .ok);

    // Precondition: the shared coordinator's readiness gate `camera.session`
    // blocks Start without a running camera (issue #369/#372); the refusal
    // names the blocking gate ids, never a free-text reason.
    let readiness = bridge.pin_mut().fetch_experiment_readiness(&out_path.to_string_lossy());
    assert!(readiness.valid && !readiness.ready, "readiness must block without a running camera");
    let camera_gate = readiness
        .gates
        .iter()
        .find(|g| g.id == "camera.session")
        .expect("camera.session gate present");
    assert!(camera_gate.status == 2 || camera_gate.status == 3, "camera.session should Fail/Unavailable");
    let early = bridge.pin_mut().experiment_start(&out_path.to_string_lossy());
    assert!(!early.ok, "experiment started without a running camera");
    assert!(
        early.message.contains("camera.session"),
        "unexpected precondition message: {}",
        early.message
    );

    assert!(bridge.pin_mut().apply_processing(true, 1.0).ok);
    assert!(bridge.pin_mut().start_capture().ok);
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline && !bridge.pin_mut().fetch_latest_frame().valid {
        std::thread::sleep(Duration::from_millis(10));
    }

    let start = bridge.pin_mut().experiment_start(&out_path.to_string_lossy());
    assert!(start.ok, "experiment_start failed: {}", start.message);
    assert!(start.operation_id != 0, "experiment has no operation id");

    // Duplicate start must fail without desynchronizing state.
    assert!(!bridge.pin_mut().experiment_start(&out_path.to_string_lossy()).ok);

    // The camera cannot stop underneath an active experiment (Qt parity).
    assert!(!bridge.pin_mut().stop_capture().ok);

    let status = bridge.pin_mut().fetch_experiment_status();
    assert!(status.valid);
    assert_eq!(status.state, 2, "experiment should be Active");
    assert!(status.output_path.ends_with(".h5"));

    std::thread::sleep(Duration::from_millis(400)); // let frames accumulate

    let stop = bridge.pin_mut().experiment_stop();
    assert!(stop.ok, "experiment_stop failed: {}", stop.message);

    // The stop finalizes asynchronously; wait for the terminal Idle status.
    let deadline = Instant::now() + Duration::from_secs(10);
    let mut finalized = false;
    while Instant::now() < deadline {
        let s = bridge.pin_mut().fetch_experiment_status();
        if s.valid && s.state == 0 {
            finalized = true;
            break;
        }
        assert_ne!(s.state, 4, "experiment failed: {}", s.message);
        std::thread::sleep(Duration::from_millis(20));
    }
    assert!(finalized, "experiment did not finalize within 10s");
    {
        // ABI 13: the terminal snapshot carries the reconciled outcome.
        let s = bridge.pin_mut().fetch_experiment_status();
        assert!(s.terminal, "terminal flag");
        assert!(s.finalization_ok, "finalization ok: {}", s.completion_reason);
        assert_eq!(s.completion, 0, "clean mock run should be Complete: {}", s.completion_reason);
        assert_eq!(s.start_generation, 1);
        assert!(s.readiness_generation > 0 && s.capture_generation > 0);
        assert_eq!(s.persistence_committed, s.persistence_admitted, "remainder committed");
        assert!(s.fault_code.is_empty(), "no fault: {}", s.fault_message);
    }
    {
        let events = bridge.pin_mut().poll_events();
        let terminal = events
            .iter()
            .find(|e| e.kind == BridgeEventKind::ExperimentStatus && e.experiment_terminal)
            .expect("terminal ExperimentStatus event with typed companions");
        assert_eq!(terminal.experiment_completion, 0);
        assert!(terminal.experiment_finalization_ok);
        assert_eq!(terminal.experiment_start_generation, 1);
        // A refused duplicate Start (AlreadyActive) gets its own Failed
        // operation and must not disturb the running experiment's id.
        assert!(
            events.iter().any(|e| e.kind == BridgeEventKind::OperationStatus
                && e.u0 != start.operation_id && e.u2 == 3),
            "refused duplicate Start should close its operation as Failed"
        );
        // Terminal events: an ExperimentStatus(Idle) and the operation Completed.
        assert!(
            events
                .iter()
                .any(|e| e.kind == BridgeEventKind::ExperimentStatus && e.u0 == 0),
            "no terminal ExperimentStatus event"
        );
        assert!(
            events
                .iter()
                .any(|e| e.kind == BridgeEventKind::OperationStatus
                    && e.u0 == start.operation_id
                    && e.u2 == 2),
            "no Completed operation event for the experiment"
        );
    }


    // Double stop fails safely.
    assert!(!bridge.pin_mut().experiment_stop().ok);

    // The finalized file reopens through the review path.
    assert!(bridge.pin_mut().stop_capture().ok);
    let status = bridge.pin_mut().fetch_experiment_status();
    let load = bridge.pin_mut().load_recording(&status.output_path);
    assert!(load.ok, "finalized experiment file failed to load: {}", load.message);

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&frame_dir);
    let _ = std::fs::remove_dir_all(&data_dir);
    let _ = std::fs::remove_file(&status.output_path);
}

// The shim event queue is bounded drop-oldest and overflow is observable
// (BE-1): with a tiny capacity, flooding events yields a bounded poll batch
// prefixed by a QueueOverflow marker.
#[test]
#[serial]
fn event_queue_overflow_is_bounded_and_observable() {
    std::env::set_var("MIB_BRIDGE_MAX_QUEUE", "8");
    let data_dir =
        std::env::temp_dir().join(format!("mib_bridge_ovf_data_{}", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
    let _ = bridge.pin_mut().poll_events();

    // Every seek emits a PlaybackPosition event even with no frames loaded.
    for _ in 0..40 {
        let _ = bridge.pin_mut().playback_seek_latest();
    }

    let events = bridge.pin_mut().poll_events();
    assert!(
        events.len() <= 8 + 1,
        "poll batch exceeded the bounded capacity: {}",
        events.len()
    );
    let overflow = &events[0];
    assert_eq!(
        overflow.kind,
        BridgeEventKind::QueueOverflow,
        "first event of an overflowed batch must be the QueueOverflow marker"
    );
    assert!(overflow.u0 > 0, "overflow marker reports zero dropped events");
    assert_eq!(overflow.u1, bridge.queue_overflow_total());
    assert!(bridge.queue_overflow_total() > 0);

    bridge.pin_mut().shutdown();
    std::env::remove_var("MIB_BRIDGE_MAX_QUEUE");
    let _ = std::fs::remove_dir_all(&data_dir);
}

#[test]
#[serial]
fn processing_settings_and_stats() {
    let frame_dir = make_frame_dir();
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_proc_data_{}", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
    assert!(bridge
        .pin_mut()
        .configure_mock_camera(&frame_dir.to_string_lossy(), 5, true)
        .ok);

    // Apply processing settings (enable realtime + a pixel→micron scale).
    let res = bridge.pin_mut().apply_processing(true, 2.5);
    assert!(res.ok, "apply_processing failed: {}", res.message);

    assert!(bridge.pin_mut().start_capture().ok);
    std::thread::sleep(Duration::from_millis(200));

    let stats = bridge.pin_mut().fetch_processing_stats();
    assert!(stats.valid, "fetch_processing_stats returned invalid");
    // The scale we set should round-trip.
    assert!((stats.pixel_to_micron - 2.5).abs() < 1e-9, "pixel_to_micron not applied");

    assert!(bridge.pin_mut().stop_capture().ok);
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&frame_dir);
    let _ = std::fs::remove_dir_all(&data_dir);
}

#[test]
#[serial]
fn lifecycle_produces_status_and_frame_events() {
    let frame_dir = make_frame_dir();
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_data_{}", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(!bridge.is_initialized());

    assert!(
        bridge.pin_mut().initialize(&data_dir.to_string_lossy()),
        "backend facade initialize failed"
    );
    assert!(bridge.is_initialized());

    // Configure the mock camera through the command channel.
    let res = bridge.pin_mut().configure_mock_camera(
        &frame_dir.to_string_lossy(),
        5, // ms between frames
        true,
    );
    assert!(res.ok, "configure_mock_camera failed: {}", res.message);

    // A CameraStatus event reporting configured should have been emitted.
    let configured = drain_until(
        &mut bridge,
        |e| e.kind == BridgeEventKind::CameraStatus && e.b0, // b0 = configured
        Duration::from_secs(2),
    );
    assert!(configured.is_some(), "no CameraStatus(configured) event");

    // Start capture; live frames flow into the playback store and are pulled on
    // demand (never pushed through the event channel, never base64 — ADR 0003).
    let res = bridge.pin_mut().start_capture();
    assert!(res.ok, "start_capture failed: {}", res.message);

    // Poll the pull path until a live frame is available.
    let mut frame = ffi::BridgeFrame::default();
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        let f = bridge.pin_mut().fetch_latest_frame();
        if f.valid {
            frame = f;
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(frame.valid, "no live frame available within 5s");
    // The sample frame is 512x96.
    assert_eq!(frame.width, 512, "unexpected frame width");
    assert_eq!(frame.height, 96, "unexpected frame height");
    assert!(!frame.data.is_empty(), "frame has no pixel bytes");
    if frame.stride_bytes > 0 {
        assert_eq!(
            frame.data.len() as u64,
            frame.stride_bytes * frame.height,
            "pixel byte count should equal stride * height"
        );
    }

    // Seek to latest — this is the push path: it emits FrameReady +
    // PlaybackPosition events the webview subscribes to.
    let res = bridge.pin_mut().playback_seek_latest();
    assert!(res.ok, "playback_seek_latest failed: {}", res.message);

    let frame_ready = drain_until(
        &mut bridge,
        |e| e.kind == BridgeEventKind::FrameReady,
        Duration::from_secs(2),
    );
    let fr = frame_ready.expect("no FrameReady event after seek");
    // u2 = width, u3 = height from the flattened FrameReady mapping.
    assert!(fr.u2 > 0 && fr.u3 > 0, "FrameReady reported zero dimensions");

    // Stop and shut down cleanly.
    let res = bridge.pin_mut().stop_capture();
    assert!(res.ok, "stop_capture failed: {}", res.message);
    bridge.pin_mut().shutdown();
    assert!(!bridge.is_initialized());

    let _ = std::fs::remove_dir_all(&frame_dir);
    let _ = std::fs::remove_dir_all(&data_dir);
}

#[test]
#[serial]
fn record_then_load_and_review() {
    let frame_dir = make_frame_dir();
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_rec_data_{}", std::process::id()));
    let rec_path = std::env::temp_dir().join(format!("mib_bridge_rec_{}.h5", std::process::id()));

    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
    assert!(bridge
        .pin_mut()
        .configure_mock_camera(&frame_dir.to_string_lossy(), 5, true)
        .ok);
    assert!(bridge.pin_mut().start_capture().ok);

    // Let some frames flow, then record a short clip to HDF5.
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline && !bridge.pin_mut().fetch_latest_frame().valid {
        std::thread::sleep(Duration::from_millis(10));
    }
    let rec = bridge.pin_mut().start_frame_recording(&rec_path.to_string_lossy());
    assert!(rec.ok, "start_frame_recording failed: {}", rec.message);
    std::thread::sleep(Duration::from_millis(200));
    assert!(bridge.pin_mut().stop_frame_recording().ok);
    assert!(bridge.pin_mut().stop_capture().ok);

    // Load the recording back and review it by absolute index.
    let load = bridge.pin_mut().load_recording(&rec_path.to_string_lossy());
    assert!(load.ok, "load_recording failed: {}", load.message);

    let seek = bridge.pin_mut().playback_seek_index(0);
    assert!(seek.ok, "playback_seek_index failed: {}", seek.message);

    let frame = bridge.pin_mut().fetch_frame_by_index(0);
    assert!(frame.valid, "fetch_frame_by_index(0) returned invalid");
    assert!(frame.width > 0 && frame.height > 0 && !frame.data.is_empty());

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&frame_dir);
    let _ = std::fs::remove_dir_all(&data_dir);
    let _ = std::fs::remove_file(&rec_path);
}

// ---- Central profile registry (schema v25, #398) ----
// The shell-injected transport is a plain `fn` pointer, so the test
// transports report through statics.
static REGISTRY_CALLS: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
static REGISTRY_HANDLE_LIVE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static REGISTRY_SAW_APIKEY: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static REGISTRY_HUNG: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

/// Answers every request like Supabase Auth rejecting the credentials.
fn registry_transport_reject(request: &ffi::BridgeHttpRequest) -> ffi::BridgeHttpResponse {
    use std::sync::atomic::Ordering;
    REGISTRY_CALLS.fetch_add(1, Ordering::SeqCst);
    REGISTRY_HANDLE_LIVE.store(!ffi::registry_request_cancelled(request.cancel_handle), Ordering::SeqCst);
    REGISTRY_SAW_APIKEY.store(
        request.headers.iter().any(|h| h.name == "apikey" && h.value == "sb_publishable_test")
            && request.url.starts_with("https://registry.example/auth/v1/token")
            && request.timeout_ms > 0
            && request.max_response_bytes > 0,
        Ordering::SeqCst,
    );
    ffi::BridgeHttpResponse { status: 400, body: br#"{"error_code":"invalid_credentials"}"#.to_vec() }
}

/// Blocks until the backend cancels the request, then reports a transport failure.
fn registry_transport_hang(request: &ffi::BridgeHttpRequest) -> ffi::BridgeHttpResponse {
    use std::sync::atomic::Ordering;
    REGISTRY_HUNG.fetch_add(1, Ordering::SeqCst);
    while !ffi::registry_request_cancelled(request.cancel_handle) {
        std::thread::sleep(Duration::from_millis(5));
    }
    ffi::BridgeHttpResponse { status: 0, body: Vec::new() }
}

fn wait_registry_job(bridge: &mut cxx::UniquePtr<ffi::BackendBridge>, job_id: u64) -> ffi::BridgeRegistryJob {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let job = bridge.pin_mut().fetch_registry_job(job_id);
        assert_eq!(job.job_id, job_id, "registry job {job_id} unknown");
        // 2 Succeeded, 3 Partial, 4 Failed, 5 Cancelled (registry_job_states).
        if job.state >= 2 {
            return job;
        }
        assert!(Instant::now() < deadline, "registry job {job_id} did not finish");
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
#[serial]
fn registry_unconfigured_is_inert() {
    std::env::remove_var("MIB_PROFILE_REGISTRY_URL");
    std::env::remove_var("MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY");
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_registry_off_{}", std::process::id()));
    let mut bridge = ffi::new_backend_bridge();
    let before = bridge.pin_mut().fetch_registry_snapshot();
    assert!(!before.valid, "no registry snapshot before initialize");
    assert_eq!(bridge.pin_mut().registry_refresh(), 0, "uninitialized bridge refuses");
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
    let s = bridge.pin_mut().fetch_registry_snapshot();
    assert!(s.valid && !s.configured, "no registry env: valid snapshot, not configured");
    assert_eq!(bridge.pin_mut().registry_sign_in("a@b", "pw"), 0, "inert registry refuses sign-in");
    assert_eq!(bridge.pin_mut().registry_refresh(), 0);
    assert!(
        !bridge.pin_mut().set_registry_transport(registry_transport_reject),
        "the transport is fixed once the backend is initialized"
    );
    assert!(ffi::registry_request_cancelled(424_242), "unknown cancel handle reads as cancelled");
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(&data_dir);
}

#[test]
#[serial]
fn checked_config_document_roundtrip_and_conflict() {
    let data_dir = std::env::temp_dir().join(format!("mib_checked_config_{}", std::process::id()));
    std::fs::create_dir_all(&data_dir).unwrap();
    let path = data_dir.join("config.json");
    std::fs::write(&path, r#"{"custom":{"keep":17},"image_processing":{"area_threshold_min":1}}"#).unwrap();
    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
    let doc = bridge.pin_mut().fetch_config_document(&path.to_string_lossy());
    assert!(doc.ok, "{}", doc.error);
    assert_eq!(doc.revision.len(), 64);
    let patch = r#"{"image_processing":{"area_threshold_min":2}}"#;
    let result = bridge.pin_mut().apply_config_document(&doc.path, &doc.revision, patch);
    assert!(result.saved && result.applied && result.verified, "{}", result.error);
    assert!(!result.conflict);
    assert_ne!(result.revision, doc.revision);
    let stale = bridge.pin_mut().apply_config_document(&doc.path, &doc.revision, patch);
    assert!(stale.conflict && !stale.saved && !stale.applied);
    let after = bridge.pin_mut().fetch_config_document(&doc.path);
    let parsed: serde_json::Value = serde_json::from_str(&after.document_json).unwrap();
    assert_eq!(parsed["custom"]["keep"], 17);
    assert_eq!(parsed["image_processing"]["area_threshold_min"], 2);
    let missing = bridge.pin_mut().fetch_config_document(&data_dir.join("missing.json").to_string_lossy());
    assert!(!missing.ok && !missing.error.is_empty());
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(data_dir);
}

#[test]
#[serial]
fn registry_commands_through_shell_transport() {
    use std::sync::atomic::Ordering;
    std::env::set_var("MIB_PROFILE_REGISTRY_URL", "https://registry.example");
    std::env::set_var("MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY", "sb_publishable_test");
    let data_dir = std::env::temp_dir().join(format!("mib_bridge_registry_{}", std::process::id()));

    // A rejected sign-in travels through the shell transport and comes back
    // as contract values: job kind 0 SignIn, state 4 Failed; connectivity 3
    // AuthenticationRequired; session 0 SignedOut.
    {
        let mut bridge = ffi::new_backend_bridge();
        assert!(bridge.pin_mut().set_registry_transport(registry_transport_reject));
        assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
        let s = bridge.pin_mut().fetch_registry_snapshot();
        assert!(s.valid && s.configured && s.origin == "https://registry.example");
        let job_id = bridge.pin_mut().registry_sign_in("bob@lab", "wrong");
        assert_ne!(job_id, 0, "sign-in queued");
        let job = wait_registry_job(&mut bridge, job_id);
        assert_eq!((job.kind, job.state), (0, 4), "rejected sign-in: {}", job.message);
        let s = bridge.pin_mut().fetch_registry_snapshot();
        assert_eq!(s.session, 0);
        assert_eq!(s.connectivity, 3);
        assert!(!s.health_message.contains("wrong"), "password never echoed");
        assert!(REGISTRY_CALLS.load(Ordering::SeqCst) >= 1, "shell transport was used");
        assert!(REGISTRY_HANDLE_LIVE.load(Ordering::SeqCst), "cancel handle live during the call");
        assert!(REGISTRY_SAW_APIKEY.load(Ordering::SeqCst), "request shape (url, apikey, bounds)");
        assert_eq!(s.last_job.job_id, job_id);

        // #398 M2b surface: instrument identity, materialize (kind 4) and
        // "Mark validated" refusals cross the bridge as values.
        assert_eq!(s.instrument_id.len(), 36, "instrument UUID mirrored");
        assert_eq!(bridge.pin_mut().registry_materialize("../x"), 0, "unsafe revision ID refused");
        let materialize = bridge.pin_mut().registry_materialize("r1");
        assert_ne!(materialize, 0, "materialize queued");
        let job = wait_registry_job(&mut bridge, materialize);
        assert_eq!((job.kind, job.state), (4, 4), "no cache open: materialize fails: {}", job.message);
        let refused = bridge.pin_mut().registry_record_validation("r1", "/nonexistent/run.h5", true);
        assert_eq!(refused.job_id, 0, "validation without a cached revision is refused");
        assert!(!refused.error.is_empty(), "refusal carries a reason");

        // #398 M3b authoring surface: values and refusals cross the bridge.
        let s = bridge.pin_mut().fetch_registry_snapshot();
        assert!(s.drafts.is_empty() && s.methods.is_empty() && !s.submit_conflict.present);
        let no_config = bridge.pin_mut().registry_new_draft_from_revision("r1", true);
        assert_eq!(no_config.job_id, 0, "no applied config.json: refused");
        assert!(no_config.error.contains("config.json"), "reason: {}", no_config.error);
        let blank = bridge.pin_mut().registry_transition("r1", 1, "   ");
        assert_eq!(blank.job_id, 0, "blank reason refused");
        assert!(!blank.error.is_empty());
        assert_eq!(bridge.pin_mut().registry_transition("r1", 99, "x").job_id, 0, "unknown state refused");
        let missing = bridge.pin_mut().registry_set_draft_notes("nope", "notes");
        assert_eq!(missing.job_id, 0);
        assert!(missing.error.contains("not found"));
        let copy = bridge.pin_mut().registry_new_draft_from_revision("r1", false);
        assert_ne!(copy.job_id, 0, "copy draft queued");
        let job = wait_registry_job(&mut bridge, copy.job_id);
        assert_eq!((job.kind, job.state), (6, 4), "SaveDraft fails without a cache: {}", job.message);
        let submit = bridge.pin_mut().registry_submit_draft("d1", false);
        let job = wait_registry_job(&mut bridge, submit.job_id);
        assert_eq!((job.kind, job.state), (8, 4), "SubmitDraft needs a session");

        // #398 M2c Apply: refusals cross the bridge as values.
        let plan = bridge.pin_mut().registry_plan_apply("r1");
        assert!(!plan.ok && plan.error.contains("cache"), "uncached revision: {}", plan.error);
        let applied = bridge.pin_mut().registry_apply_method("r1");
        assert!(!applied.ok && !applied.error.is_empty() && applied.applied.is_empty());
        bridge.pin_mut().shutdown();
    }

    // A hung transport is aborted by registry_cancel_all (state 5 Cancelled)
    // and by backend shutdown, through the polled cancel handle.
    {
        let mut bridge = ffi::new_backend_bridge();
        assert!(bridge.pin_mut().set_registry_transport(registry_transport_hang));
        assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
        let hung_before = REGISTRY_HUNG.load(Ordering::SeqCst);
        let job_id = bridge.pin_mut().registry_sign_in("bob@lab", "pw");
        let deadline = Instant::now() + Duration::from_secs(10);
        while REGISTRY_HUNG.load(Ordering::SeqCst) == hung_before {
            assert!(Instant::now() < deadline, "transport never called");
            std::thread::sleep(Duration::from_millis(2));
        }
        assert!(bridge.pin_mut().fetch_registry_snapshot().busy, "snapshot answers while hung");
        assert!(bridge.pin_mut().registry_cancel_all());
        let job = wait_registry_job(&mut bridge, job_id);
        assert_eq!(job.state, 5, "cancel aborts the hung request");
        let s = bridge.pin_mut().fetch_registry_snapshot();
        assert_ne!(s.connectivity, 2, "an aborted request is not an outage");

        let hung_before = REGISTRY_HUNG.load(Ordering::SeqCst);
        bridge.pin_mut().registry_sign_in("bob@lab", "pw");
        while REGISTRY_HUNG.load(Ordering::SeqCst) == hung_before {
            std::thread::sleep(Duration::from_millis(2));
        }
        let started = Instant::now();
        bridge.pin_mut().shutdown();
        assert!(started.elapsed() < Duration::from_secs(8), "shutdown aborts the hung request");
        assert_eq!(bridge.pin_mut().registry_refresh(), 0, "shut-down bridge refuses");
    }

    std::env::remove_var("MIB_PROFILE_REGISTRY_URL");
    std::env::remove_var("MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY");
    let _ = std::fs::remove_dir_all(&data_dir);
}

#[test]
#[serial]
fn local_profiles_roundtrip_and_conflict() {
    let data_dir = std::env::temp_dir().join(format!("mib_profile_contract_{}",std::process::id()));
    let _ = std::fs::remove_dir_all(&data_dir);
    std::fs::create_dir_all(&data_dir).unwrap();
    let mut bridge = ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.as_path().to_string_lossy()));
    let base = data_dir.as_path().join("profiles").to_string_lossy().to_string();
    let create = r#"{"operation":"create","name":"test","document_json":"{\"pixel_to_micron_factor\":0.5}"}"#;
    let saved: serde_json::Value = serde_json::from_str(&bridge.pin_mut().profile_command(&base, create)).unwrap();
    assert_eq!(saved["ok"], true);
    let read: serde_json::Value = serde_json::from_str(&bridge.pin_mut().profile_command(&base,r#"{"operation":"read","name":"test"}"#)).unwrap();
    assert_eq!(read["profile"]["document_json"], "{\"pixel_to_micron_factor\":0.5}");
    let stale: serde_json::Value = serde_json::from_str(&bridge.pin_mut().profile_command(&base,r#"{"operation":"archive","name":"test","baseline":"stale"}"#)).unwrap();
    assert_eq!(stale["ok"],false);
    assert!(data_dir.as_path().join("profiles/test/config.json").exists());
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(data_dir);
}

#[test]
#[serial]
fn processing_core_management_bundled_roundtrip() {
    let data_dir=std::env::temp_dir().join(format!("mib_core_management_{}",std::process::id()));
    std::fs::create_dir_all(&data_dir).unwrap();
    let mut bridge=ffi::new_backend_bridge();
    assert!(bridge.pin_mut().initialize(&data_dir.to_string_lossy()));
    let cache=data_dir.join("cores").to_string_lossy().to_string();
    let info:serde_json::Value=serde_json::from_str(&bridge.pin_mut().processing_core_command(&cache,r#"{"operation":"info"}"#)).unwrap();
    assert_eq!(info["ok"],true);
    let activate:serde_json::Value=serde_json::from_str(&bridge.pin_mut().processing_core_command(&cache,r#"{"operation":"bundled"}"#)).unwrap();
    assert_eq!(activate["ok"],true);
    let restored:serde_json::Value=serde_json::from_str(&bridge.pin_mut().processing_core_command(&cache,r#"{"operation":"restore"}"#)).unwrap();
    assert_eq!(restored["ok"],true);
    assert_eq!(activate["active_version"],restored["active_version"]);
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(data_dir);
}

#[test]
#[serial]
fn recovery_refuses_unconfirmed_or_absent_fault_and_reports_capture_lifecycle() {
    let mut bridge = ffi::new_backend_bridge();
    let dir = std::env::temp_dir().join(format!("mib_recovery_contract_{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    assert!(bridge.pin_mut().initialize(dir.to_str().unwrap()));
    assert!(!bridge.pin_mut().experiment_acknowledge_fault(0, 0, "test", "fault", false).ok);
    assert!(!bridge.pin_mut().experiment_acknowledge_fault(0, 0, "test", "fault", true).ok);
    let status: serde_json::Value = serde_json::from_str(&bridge.pin_mut().fetch_capture_lifecycle()).unwrap();
    assert_eq!(status["valid"], true);
    assert_eq!(status["generation"], "0");
    assert_eq!(status["state"], "idle");
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_dir_all(dir);
}
