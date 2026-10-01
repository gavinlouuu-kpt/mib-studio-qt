//! Headless contract test for the review bridge (ADR 0008, plan
//! 2026-10-01-standalone-review-app). Runs in both feature configurations:
//! with `review-only` this is the only bridge in the crate and the binary
//! links no AppBackend.
//!
//! Drives a ReviewBridge over the C++-written experiment fixture: open →
//! info (TD-17 factor, accounting, KDE record) → full-column rows → frames
//! with backend-composed overlays (RGB8) → series → thumbnail strip →
//! scatter → core-record save/overwrite refusal → close; plus the fault
//! paths (missing file, bad dataset/overlay, out of range).

use mib_bridge::review_ffi as ffi;
use serial_test::serial;

fn fixture_path(tag: &str) -> std::path::PathBuf {
    std::env::temp_dir().join(format!("mib_review_bridge_{tag}_{}.h5", std::process::id()))
}

const RGB8: u64 = 0x0218_0014;

#[test]
#[serial]
fn review_bridge_reads_the_fixture_file() {
    let path = fixture_path("exp");
    let _ = std::fs::remove_file(&path);
    assert!(ffi::review_fixture_write_experiment(&path.to_string_lossy()), "fixture write failed");

    let contract: serde_json::Value =
        serde_json::from_str(include_str!("../contract/bridge-contract.json")).unwrap();
    assert_eq!(contract["abi_version"].as_u64().unwrap() as u32, ffi::review_bridge_abi_version());
    assert_eq!(contract["review_pixel_formats"]["Rgb8"].as_u64().unwrap(), RGB8);
    assert_eq!(contract["overlay_modes"]["FilteredMask"].as_u64().unwrap(), 4);

    let mut bridge = ffi::new_review_bridge();
    assert!(bridge.pin_mut().initialize(""));
    assert!(bridge.is_initialized());

    // Nothing open: info says closed, reads fail safely.
    let info = bridge.pin_mut().fetch_review_info();
    assert!(info.valid && !info.file_open);
    assert!(!bridge.pin_mut().fetch_review_rows(true, 0, 5).valid);
    assert!(!bridge.pin_mut().fetch_review_frame(0, 0, 0, false).valid);
    assert!(!bridge.pin_mut().fetch_review_scatter().valid);
    let missing = bridge.pin_mut().review_open(&fixture_path("missing").to_string_lossy());
    assert!(!missing.ok && !missing.message.is_empty());

    // Open + info.
    bridge.pin_mut().set_fallback_pixel_to_micron(0.9);
    let opened = bridge.pin_mut().review_open(&path.to_string_lossy());
    assert!(opened.ok, "{}", opened.message);
    let info = bridge.pin_mut().fetch_review_info();
    assert!(info.valid && info.file_open && !info.recording_file);
    assert_eq!((info.total_valid, info.total_invalid), (10, 4));
    assert_eq!((info.roi_x, info.roi_y, info.roi_w, info.roi_h), (4, 2, 20, 16));
    assert!(info.valid_images.present && info.valid_images.count == 10 && info.valid_images.width == 32);
    assert!(info.has_series && info.series_count == 3);
    assert!(info.has_accounting && info.accounting_rejected == 4 && info.accounting_completion == 0);
    assert!(info.accounting_summary.contains("run complete"));
    assert!(info.pixel_to_micron_from_file && (info.pixel_to_micron - 0.25).abs() < 1e-12, "TD-17");
    assert!(!info.kde_analysis_json.is_empty() && info.kde_live_json.is_empty());
    assert!(info.file_path.ends_with(".h5"));

    // Rows: full columns, bounded, stable totals.
    let rows = bridge.pin_mut().fetch_review_rows(true, 2, 3);
    assert!(rows.valid && rows.total == 10 && rows.offset == 2 && rows.rows.len() == 3);
    let r = &rows.rows[0];
    assert_eq!((r.frame_index, r.object_id, r.track_id), (4, 4, 2));
    assert!((r.area_um2 - r.area * 0.25 * 0.25).abs() < 1e-9);
    assert!(r.has_single_inner_contour && r.brightness_q4 == 4.5);
    assert!(!rows.rows[1].valid, "frame 3 fails validation");
    let beyond = bridge.pin_mut().fetch_review_rows(true, 50, 5);
    assert!(beyond.valid && beyond.rows.is_empty() && beyond.total == 10);
    let invalid = bridge.pin_mut().fetch_review_rows(false, 0, 50);
    assert!(invalid.valid && invalid.total == 4 && invalid.rows.len() == 4);

    // Frames: Mono8 raw, RGB8 overlays that differ, ROI red, masks raw.
    let plain = bridge.pin_mut().fetch_review_frame(0, 1, 0, false);
    assert!(plain.valid && plain.pixel_format == 0 && plain.width == 32 && plain.height == 24);
    assert_eq!(plain.stride_bytes, 32);
    assert_eq!(plain.data.len(), 32 * 24);
    let contour = bridge.pin_mut().fetch_review_frame(0, 1, 1, false);
    let all_mask = bridge.pin_mut().fetch_review_frame(0, 1, 3, false);
    assert!(contour.valid && contour.pixel_format == RGB8 && contour.stride_bytes == 96);
    assert_eq!(contour.data.len(), 96 * 24);
    assert!(all_mask.valid && all_mask.data != contour.data);
    let roi = bridge.pin_mut().fetch_review_frame(0, 1, 0, true);
    assert!(roi.valid && roi.pixel_format == RGB8);
    assert!(roi.data.chunks(3).any(|p| p == [255, 0, 0]), "ROI rectangle is red");
    let mask = bridge.pin_mut().fetch_review_frame(3, 1, 1, true);
    assert!(mask.valid && mask.pixel_format == 0, "masks ignore overlay/roi");
    assert!(!bridge.pin_mut().fetch_review_frame(0, 10, 0, false).valid, "index beyond");
    assert!(!bridge.pin_mut().fetch_review_frame(2, 0, 0, false).valid, "recorded on experiment");
    assert!(!bridge.pin_mut().fetch_review_frame(99, 0, 0, false).valid, "unknown dataset");
    assert!(!bridge.pin_mut().fetch_review_frame(0, 0, 99, false).valid, "unknown overlay");

    // Series.
    assert_eq!(bridge.pin_mut().fetch_review_series_count(0), 3);
    assert_eq!(bridge.pin_mut().fetch_review_series_count(10), 0);
    let s2 = bridge.pin_mut().fetch_review_series_frame(0, 2, 0, false);
    assert!(s2.valid && s2.pixel_format == 0 && s2.width == 32);
    assert!(!bridge.pin_mut().fetch_review_series_frame(0, 3, 0, false).valid);

    // Thumbnails: one packed frame, size x (size*count), bounded page.
    let strip = bridge.pin_mut().fetch_review_thumbnails(true, 8, 5, 16, 0, false);
    assert!(strip.valid && strip.width == 16 && strip.height == 32 && strip.pixel_format == 0);
    assert_eq!(strip.frame_index, 8);
    assert_eq!(strip.data.len(), 16 * 32);
    let colour = bridge.pin_mut().fetch_review_thumbnails(true, 0, 3, 16, 1, true);
    assert!(colour.valid && colour.pixel_format == RGB8 && colour.height == 48 && colour.stride_bytes == 48);
    assert!(!bridge.pin_mut().fetch_review_thumbnails(true, 0, 3, 0, 0, false).valid, "size 0");
    assert!(!bridge.pin_mut().fetch_review_thumbnails(true, 0, 0, 16, 0, false).valid, "count 0");
    assert!(!bridge.pin_mut().fetch_review_thumbnails(true, 0, 65, 128, 0, false).valid, "taller than max_dimension");

    // Scatter: isValid rows only, µm² with the recorded factor.
    let sc = bridge.pin_mut().fetch_review_scatter();
    assert!(sc.valid && sc.frame_index.len() == 8);
    assert_eq!((sc.valid_position[3], sc.frame_index[3]), (4, 8));
    assert!((sc.area_um2[0] - 100.0 * 0.25 * 0.25).abs() < 1e-9);
    assert_eq!((sc.target_group[0], sc.target_group[1]), (1, 0));
    assert!((sc.pixel_to_micron - 0.25).abs() < 1e-12);

    // Core record: refuse, then overwrite; reads keep working.
    let refused = bridge.pin_mut().review_save_core_record("{\"x\":1}", false);
    assert!(!refused.ok);
    let saved = bridge.pin_mut().review_save_core_record("{\"schema\":1,\"cell_count\":99}", true);
    assert!(saved.ok, "{}", saved.message);
    assert!(bridge.pin_mut().fetch_review_info().kde_analysis_json.contains("99"));
    assert!(bridge.pin_mut().fetch_review_frame(0, 1, 0, false).valid);

    // Jobs (PR 1b): tracked operations through poll_review_events.
    assert!(bridge.pin_mut().poll_review_events().is_empty());
    assert!(!bridge.pin_mut().cancel_review_operation(12345).ok);
    let wait_terminal = |bridge: &mut cxx::UniquePtr<ffi::ReviewBridge>, id: u64| -> ffi::ReviewEvent {
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(60);
        while std::time::Instant::now() < deadline {
            for e in bridge.pin_mut().poll_review_events() {
                if e.operation_id == id && e.state >= 2 {
                    return e;
                }
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        panic!("operation {id} did not finish");
    };
    // Export metrics: Completed, CSV written with the Qt-parity header.
    let csv = fixture_path("metrics").with_extension("csv");
    let started = bridge.pin_mut().review_export_metrics(&csv.to_string_lossy());
    assert!(started.ok && started.operation_id != 0, "{}", started.message);
    let done = wait_terminal(&mut bridge, started.operation_id);
    assert_eq!((done.kind, done.state), (0, 2), "export: {}", done.message);
    let text = std::fs::read_to_string(&csv).unwrap();
    assert!(text.starts_with("Frame Type"), "csv header");
    assert!(!bridge.review_jobs_busy());
    let _ = std::fs::remove_file(&csv);
    // Density: one level per scatter point; the fixture has a stored record,
    // so nothing is computed.
    let started = bridge.pin_mut().review_request_density(1.0, 0.9, 8, true);
    assert!(started.ok, "{}", started.message);
    let done = wait_terminal(&mut bridge, started.operation_id);
    assert_eq!((done.kind, done.state), (5, 2), "density: {}", done.message);
    let density = bridge.pin_mut().fetch_review_density();
    assert!(density.valid && density.ready && density.level_count == 8);
    assert_eq!(density.levels.len(), 8);
    assert!(density.levels.iter().all(|l| *l < 8));
    assert!(density.computed_record_json.is_empty());
    // Compute core: record JSON retrievable, saveable through the session.
    let started = bridge.pin_mut().review_compute_core(0.9);
    assert!(started.ok, "{}", started.message);
    let done = wait_terminal(&mut bridge, started.operation_id);
    assert_eq!((done.kind, done.state), (4, 2), "core: {}", done.message);
    let json = bridge.pin_mut().fetch_review_computed_core_json();
    assert!(json.contains("\"full-run\""), "{json}");
    assert!(bridge.pin_mut().review_save_core_record(&json, true).ok);
    // Refusals: unknown regenerate source, empty batch.
    assert!(!bridge.pin_mut().review_regenerate_masks(99, "", 0, 0, "x.h5", true, false).ok);
    assert!(!bridge.pin_mut().review_batch_export(Vec::new(), "/tmp", true, true, 0, u64::MAX).ok);

    assert!(bridge.pin_mut().review_close().ok);
    assert!(!bridge.pin_mut().fetch_review_info().file_open);
    bridge.pin_mut().shutdown();
    assert!(!bridge.is_initialized());
    let _ = std::fs::remove_file(&path);
}
