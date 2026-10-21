//! Headless contract test for the review bridge (ADR 0014, plan
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
        serde_json::from_str(include_str!("../contract/review-contract.json")).unwrap();
    assert_eq!(contract["review_abi_version"].as_u64().unwrap() as u32, ffi::review_bridge_abi_version());
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
    assert_eq!(sc.ring_ratio.len(), 8);
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
    // Export Charts: kind 6; a name with a path is refused up front; bytes
    // that do not decode fail the job (encoding is covered by review.jobs).
    let snap = |name: &str, bytes: Vec<u8>| ffi::ReviewChartSnapshot { name: name.into(), encoded: bytes };
    let dir = std::env::temp_dir();
    assert!(!bridge.pin_mut().review_export_charts(&dir.to_string_lossy(), vec![snap("../x.tiff", vec![1])]).ok);
    let started = bridge.pin_mut().review_export_charts(&dir.to_string_lossy(), vec![snap("mib_bridge_bad_chart.tiff", vec![1, 2, 3])]);
    assert!(started.ok, "{}", started.message);
    let done = wait_terminal(&mut bridge, started.operation_id);
    assert_eq!((done.kind, done.state), (6, 3), "charts: {}", done.message);
    assert!(!dir.join("mib_bridge_bad_chart.tiff").exists());
    // Regenerate masks (#512 S8, D6): never replaces a file. An existing
    // output is refused at start with the reason, untouched; a new one is
    // written (partial file, then a no-replace publish) and named in the
    // completion message.
    let existing = fixture_path("regen_existing");
    std::fs::write(&existing, b"keep me").unwrap();
    let refused = bridge.pin_mut().review_regenerate_masks(2, "", 0, 0, &existing.to_string_lossy(), true, false);
    assert!(!refused.ok && refused.operation_id == 0);
    assert_eq!(refused.message, "Output already exists; choose a new file");
    assert_eq!(std::fs::read(&existing).unwrap(), b"keep me");
    let fresh = fixture_path("regen_new");
    let _ = std::fs::remove_file(&fresh);
    let started = bridge.pin_mut().review_regenerate_masks(2, "", 0, 0, &fresh.to_string_lossy(), true, false);
    assert!(started.ok, "{}", started.message);
    let done = wait_terminal(&mut bridge, started.operation_id);
    assert_eq!((done.kind, done.state), (3, 2), "regenerate: {}", done.message);
    assert!(done.message.starts_with(&format!("{} (", fresh.to_string_lossy())) && done.message.contains(" images: "), "{}", done.message);
    assert!(fresh.exists());
    let dir = fresh.parent().unwrap();
    let stem = fresh.file_name().unwrap().to_string_lossy().into_owned();
    assert!(
        !std::fs::read_dir(dir).unwrap().filter_map(|e| e.ok()).any(|e| e.file_name().to_string_lossy().starts_with(&format!(".{stem}.partial-"))),
        "no partial file left"
    );
    let _ = std::fs::remove_file(&existing);
    let _ = std::fs::remove_file(&fresh);
    // Refusals: unknown regenerate source, empty batch.
    assert!(!bridge.pin_mut().review_regenerate_masks(99, "", 0, 0, "x.h5", true, false).ok);
    assert!(!bridge.pin_mut().review_batch_export(Vec::new(), "/tmp", true, true, 0, u64::MAX).ok);

    assert!(bridge.pin_mut().review_close().ok);
    assert!(!bridge.pin_mut().fetch_review_info().file_open);
    bridge.pin_mut().shutdown();
    assert!(!bridge.is_initialized());
    let _ = std::fs::remove_file(&path);
}

#[test]
#[serial]
fn population_fixture_feeds_the_charts_view() {
    let path = fixture_path("population");
    let _ = std::fs::remove_file(&path);
    assert!(!ffi::review_fixture_write_population(&path.to_string_lossy(), 0, 1), "zero cells refused");
    assert!(ffi::review_fixture_write_population(&path.to_string_lossy(), 600, 7), "fixture write failed");

    let mut bridge = ffi::new_review_bridge();
    assert!(bridge.pin_mut().initialize(""));
    assert!(bridge.pin_mut().review_open(&path.to_string_lossy()).ok);
    let info = bridge.pin_mut().fetch_review_info();
    assert_eq!((info.total_valid, info.total_invalid), (600, 60));
    assert!(info.has_recorded_config && info.ring_ratio_min < info.ring_ratio_max);
    assert!(info.kde_analysis_json.is_empty(), "no stored core record");

    // Scatter: every valid cell, ring ratios for the histogram.
    let sc = bridge.pin_mut().fetch_review_scatter();
    assert_eq!(sc.area_um2.len(), 600);
    assert_eq!(sc.ring_ratio.len(), 600);
    assert!(sc.ring_ratio.iter().all(|r| *r > 0.0));
    assert!(sc.area_um2.iter().all(|a| *a > 0.0) && sc.deformability.iter().all(|d| *d > 0.0));

    // Density: levels parallel to the scatter, and (no stored record) a
    // computed full-run record for the view to draw.
    assert!(bridge.pin_mut().review_request_density(1.0, 0.9, 8, true).ok);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(60);
    let density = loop {
        let d = bridge.pin_mut().fetch_review_density();
        if d.valid && d.ready {
            break d;
        }
        assert!(std::time::Instant::now() < deadline, "density did not finish");
        std::thread::sleep(std::time::Duration::from_millis(20));
    };
    assert_eq!(density.levels.len(), 600);
    assert!(density.levels.iter().any(|l| *l == 7), "densest level present");
    assert!(density.computed_record_json.contains("\"contours\""), "{}", density.computed_record_json);

    assert!(bridge.pin_mut().review_close().ok);
    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_file(&path);
}

// ---- Review ABI 2 (#717 B1a): run identity and the identity probe ----------------------------

const EXPERIMENT_ID: &str = "5b1d6f8e-2c4a-4e7b-9a3d-0f1e2d3c4b5a";

/// A v3 snapshot as RunProvenanceRecord writes it (the members the review reads).
fn v3_snapshot(experiment_id: Option<&str>, run_index: Option<u32>, sample_ref: Option<&str>) -> String {
    serde_json::json!({
        "schema_version": 3,
        "profile_id": "fixture",
        "pixel_to_micron": 0.25,
        "run_id": "3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f",
        "experiment_id": experiment_id,
        "run_index": run_index,
        "sample_ref": sample_ref,
        "operator": {"id": null, "name": "Gavin", "assurance": "selected"},
        "calibration_record_id": "cal-0042",
        "emodulus_lut": {
            "loaded": true, "lut_id": "lut-v2", "revision": "2", "source": "r2-public-catalog",
            "sha256": "41b9c0de", "catalog_sha256": "41b9c0de", "checksum_status": "verified",
        },
    })
    .to_string()
}

const ANNOTATION: &str = r#"{"schema_version":1,"sample":"Donor 4","condition":"CytoD 0.1","experimental_unit":null,"concentration":{"value":0.1,"unit":"uM"},"channel":null,"medium":"PBS","temperature_c":22.5,"notes":null,"free":{"plate":"B","batch":"2026-10-19"}}"#;

/// Every identity member absent: the flags say so and nothing stands in for a value.
fn assert_identity_absent(id: &ffi::ReviewRunIdentity, what: &str) {
    assert!(!id.has_snapshot && !id.snapshot_unreadable && id.snapshot_version == 0, "{what}: snapshot");
    assert!(!id.has_run_id && id.run_id.is_empty(), "{what}: run id");
    assert!(!id.has_experiment_id && id.experiment_id.is_empty(), "{what}: experiment id");
    assert!(!id.has_run_index && id.run_index == 0, "{what}: run index");
    assert!(!id.has_sample_ref && id.sample_ref.is_empty(), "{what}: sample ref");
    assert!(!id.has_calibration_record_id && !id.has_operator_id && !id.has_operator_name, "{what}: calibration / operator");
    assert!(!id.annotation.present && !id.annotation.unreadable && id.annotation.free.is_empty(), "{what}: annotation");
    assert!(!id.has_emodulus_lut && !id.emodulus_lut.loaded && id.emodulus_lut.lut_id.is_empty(), "{what}: E-LUT");
    assert!(!id.has_profile_id && id.profile_id.is_empty(), "{what}: profile");
}

fn assert_identity_declared(id: &ffi::ReviewRunIdentity, run_index: u32, sample_ref: &str) {
    assert!(id.has_snapshot && !id.snapshot_unreadable && id.snapshot_version == 3);
    assert!(id.has_run_id && id.run_id == "3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f");
    assert!(id.has_experiment_id && id.experiment_id == EXPERIMENT_ID);
    assert!(id.has_run_index && id.run_index == run_index);
    assert!(id.has_sample_ref && id.sample_ref == sample_ref);
    assert!(id.has_calibration_record_id && id.calibration_record_id == "cal-0042");
    assert!(!id.has_operator_id && id.has_operator_name && id.operator_name == "Gavin");
    // Review ABI 3 (#717 B3b): the E-LUT provenance and the profile.
    assert!(id.has_profile_id && id.profile_id == "fixture");
    let l = &id.emodulus_lut;
    assert!(id.has_emodulus_lut && l.loaded);
    assert!(l.has_lut_id && l.lut_id == "lut-v2" && l.has_revision && l.revision == "2");
    assert!(l.has_source && l.source == "r2-public-catalog" && l.has_checksum_status && l.checksum_status == "verified");
    assert!(l.has_sha256 && l.sha256 == "41b9c0de" && l.has_catalog_sha256 && l.catalog_sha256 == "41b9c0de");
    let a = &id.annotation;
    assert!(a.present && !a.unreadable && a.has_schema_version && a.schema_version == 1);
    assert!(a.has_sample && a.sample == "Donor 4" && a.has_condition && a.condition == "CytoD 0.1");
    assert!(!a.has_experimental_unit && a.experimental_unit.is_empty(), "null is absent");
    assert!(a.has_concentration && a.concentration_value == 0.1 && a.concentration_unit == "uM");
    assert!(!a.has_channel && a.has_medium && a.medium == "PBS");
    assert!(a.has_temperature_c && a.temperature_c == 22.5 && !a.has_notes);
    let free: Vec<(String, String)> = a.free.iter().map(|kv| (kv.key.clone(), kv.value.clone())).collect();
    assert_eq!(free, vec![("batch".into(), "2026-10-19".into()), ("plate".into(), "B".into())], "sorted by key");
}

#[test]
#[serial]
fn run_identity_in_info_and_the_probe() {
    let contract: serde_json::Value =
        serde_json::from_str(include_str!("../contract/review-contract.json")).unwrap();
    assert_eq!(ffi::review_bridge_abi_version(), 3, "review ABI 3 (#717 B3b; the identity since 2)");
    let max_paths = contract["review_probe"]["max_paths"].as_u64().unwrap() as usize;
    assert_eq!(max_paths, 256);

    let v3 = fixture_path("identity_v3");
    let run4 = fixture_path("identity_run4");
    let legacy = fixture_path("identity_legacy");
    let unreadable = fixture_path("identity_unreadable");
    let corrupt = fixture_path("identity_corrupt");
    let population = fixture_path("identity_population");
    for p in [&v3, &run4, &legacy, &unreadable, &corrupt] {
        let _ = std::fs::remove_file(p);
    }
    let s = |p: &std::path::PathBuf| p.to_string_lossy().into_owned();
    assert!(ffi::review_fixture_write_run(&s(&v3), &v3_snapshot(Some(EXPERIMENT_ID), Some(2), Some("S-03")), ANNOTATION));
    assert!(ffi::review_fixture_write_run(&s(&run4), &v3_snapshot(Some(EXPERIMENT_ID), Some(4), Some("S-05")), ANNOTATION));
    assert!(ffi::review_fixture_write_run(&s(&legacy), "", ""));
    assert!(ffi::review_fixture_write_run(&s(&unreadable), "not json {", "[1]"));
    std::fs::write(&corrupt, b"not an HDF5 file").unwrap();
    assert!(ffi::review_fixture_write_population(&s(&population), 50, 3));

    let mut bridge = ffi::new_review_bridge();
    assert!(bridge.pin_mut().initialize(""));

    // Nothing open: the identity is absent.
    let info = bridge.pin_mut().fetch_review_info();
    assert!(info.valid && !info.file_open);
    assert_identity_absent(&info.identity, "closed");

    // fetch_review_info: v3 as written; legacy absent; unreadable named.
    assert!(bridge.pin_mut().review_open(&s(&v3)).ok);
    let info = bridge.pin_mut().fetch_review_info();
    assert_identity_declared(&info.identity, 2, "S-03");
    assert!(info.pixel_to_micron_from_file, "the v3 snapshot still carries the TD-17 factor");
    assert!(bridge.pin_mut().review_open(&s(&legacy)).ok);
    let info = bridge.pin_mut().fetch_review_info();
    assert!(info.file_open && !info.has_accounting);
    assert_identity_absent(&info.identity, "legacy");
    assert!(bridge.pin_mut().review_open(&s(&unreadable)).ok, "an unreadable snapshot does not stop the open");
    let id = bridge.pin_mut().fetch_review_info().identity;
    assert!(!id.has_snapshot && id.snapshot_unreadable && !id.has_experiment_id && !id.has_run_index);
    assert!(!id.annotation.present && id.annotation.unreadable);

    // The probe, while v3 is open: per-file results in order, the open file untouched.
    assert!(bridge.pin_mut().review_open(&s(&v3)).ok);
    let frame_before = bridge.pin_mut().fetch_review_frame(0, 1, 1, true);
    let missing = fixture_path("identity_missing");
    let paths = vec![s(&v3), s(&run4), s(&legacy), s(&unreadable), s(&corrupt), s(&missing), s(&population), String::new()];
    let probe = bridge.pin_mut().review_probe_runs(paths.clone());
    assert!(probe.ok, "{}", probe.message);
    assert_eq!(probe.runs.iter().map(|r| r.path.clone()).collect::<Vec<_>>(), paths);
    let r = &probe.runs[0];
    assert!(r.ok && r.message.is_empty() && !r.recording_file);
    assert_identity_declared(&r.identity, 2, "S-03");
    assert!(r.has_start_time && r.start_time_ns == 1000 && r.has_end_time && r.end_time_ns == 5000);
    assert!(r.has_total_valid && r.total_valid == 10 && r.has_total_invalid && r.total_invalid == 4);
    assert!(r.has_accounting && r.accounting_completion == 0, "Complete");
    assert!(r.has_core_version && !r.core_version.is_empty());
    assert_identity_declared(&probe.runs[1].identity, 4, "S-05");
    let l = &probe.runs[2];
    assert!(l.ok && l.has_total_valid && !l.has_accounting && l.accounting_completion == 4, "legacy: no accounting");
    assert_identity_absent(&l.identity, "legacy probe");
    let u = &probe.runs[3];
    assert!(u.ok && u.identity.snapshot_unreadable && u.identity.annotation.unreadable);
    for (i, what) in [(4, "corrupt"), (5, "missing"), (7, "empty path")] {
        let e = &probe.runs[i];
        assert!(!e.ok && !e.message.is_empty(), "{what}: a per-file error");
        assert!(!e.has_start_time && !e.has_total_valid && !e.has_accounting && !e.has_core_version, "{what}: nothing else");
        assert_identity_absent(&e.identity, what);
    }
    // The population fixture writes a snapshot without schema_version: v1, no identity.
    let v1 = &probe.runs[6].identity;
    assert!(probe.runs[6].ok && v1.has_snapshot && v1.snapshot_version == 1 && !v1.has_experiment_id && !v1.has_run_index);
    assert!(!v1.has_emodulus_lut, "v1 records no E-LUT");
    let info = bridge.pin_mut().fetch_review_info();
    assert!(info.file_open && info.file_path == s(&v3), "the open file is left as it was");
    assert_identity_declared(&info.identity, 2, "S-03");
    let frame_after = bridge.pin_mut().fetch_review_frame(0, 1, 1, true);
    assert!(frame_after.valid && frame_after.data == frame_before.data, "the same pixels after the probe");

    // The cap: refused as a whole above it, served at it.
    let over = bridge.pin_mut().review_probe_runs(vec![s(&legacy); max_paths + 1]);
    assert!(!over.ok && over.runs.is_empty() && over.message.contains("257") && over.message.contains("256"), "{}", over.message);
    let at = bridge.pin_mut().review_probe_runs(vec![s(&legacy); max_paths]);
    assert!(at.ok && at.runs.len() == max_paths && at.runs.iter().all(|r| r.ok));
    let empty = bridge.pin_mut().review_probe_runs(Vec::new());
    assert!(empty.ok && empty.runs.is_empty());

    assert!(bridge.pin_mut().review_close().ok);
    bridge.pin_mut().shutdown();
    for p in [&v3, &run4, &legacy, &unreadable, &corrupt, &population] {
        let _ = std::fs::remove_file(p);
    }
}

// ---- Review ABI 3 (#717 B3b): track rows and reprocessing one record ------------------------

#[test]
#[serial]
fn inspect_track_rows_and_reprocess_one_record() {
    let contract: serde_json::Value =
        serde_json::from_str(include_str!("../contract/review-contract.json")).unwrap();
    let max_rows = contract["review_track_rows"]["max_rows"].as_u64().unwrap() as u32;
    assert_eq!(max_rows, 256);
    let recorded = contract["review_reprocess_record"]["settings"]["recorded"].as_u64().unwrap() as u32;
    let default = contract["review_reprocess_record"]["settings"]["default"].as_u64().unwrap() as u32;
    assert_eq!((recorded, default), (0, 1));
    assert_eq!(contract["review_reprocess_record"]["max_objects"].as_u64().unwrap(), 64);

    let path = fixture_path("inspect");
    let _ = std::fs::remove_file(&path);
    assert!(ffi::review_fixture_write_experiment(&path.to_string_lossy()), "fixture write failed");
    let mut bridge = ffi::new_review_bridge();
    assert!(bridge.pin_mut().initialize(""));

    // Nothing open: both refuse.
    let rows = bridge.pin_mut().fetch_review_track_rows(true, 2, 8);
    assert!(!rows.ok && !rows.message.is_empty() && rows.rows.is_empty());
    let r = bridge.pin_mut().review_reprocess_record(true, 0, recorded);
    assert!(!r.ok && !r.message.is_empty() && r.reprocessed.is_empty());

    // Nothing open: the object coordinate space is unknown (#756 review 2).
    let closed = bridge.pin_mut().fetch_review_info();
    assert_eq!((closed.object_coordinates.as_str(), closed.object_coordinates_source.as_str()), ("unknown", ""));
    let spaces = &contract["review_object_coordinates"];
    let values: Vec<&str> = spaces["values"].as_array().unwrap().iter().map(|v| v.as_str().unwrap()).collect();
    let sources: Vec<&str> = spaces["sources"].as_array().unwrap().iter().map(|v| v.as_str().unwrap()).collect();
    assert_eq!(values, ["roi_local", "image", "unknown"]);
    assert_eq!(sources, ["attribute", "snapshot", "mask_check", "roi_origin"]);
    assert_eq!(spaces["mask_check"]["max_records"].as_u64(), Some(64));
    assert_eq!(spaces["mask_check"]["min_decisive"].as_u64(), Some(8));
    assert_eq!(spaces["mask_check"]["min_agreement_percent"].as_u64(), Some(90));
    assert_eq!(spaces["mask_check"]["min_box_iou_percent"].as_u64(), Some(80));
    assert_eq!(spaces["mask_check"]["area_tolerance_percent"].as_u64(), Some(15));

    bridge.pin_mut().set_fallback_pixel_to_micron(0.9);
    assert!(bridge.pin_mut().review_open(&path.to_string_lossy()).ok);
    let before = std::fs::read(&path).unwrap();
    // The fixture records image coordinates (its ROI starts at (4, 2)).
    let info = bridge.pin_mut().fetch_review_info();
    assert_eq!(
        (info.object_coordinates.as_str(), info.object_coordinates_source.as_str(), info.object_origin_x, info.object_origin_y),
        ("image", "attribute", 0, 0)
    );

    // Track rows: the fixture tracks valid frame 2i as track i, invalid frame 2i+1 as track i.
    let rows = bridge.pin_mut().fetch_review_track_rows(true, 2, max_rows);
    assert!(rows.ok, "{}", rows.message);
    assert_eq!((rows.total, rows.truncated, rows.positions.clone()), (1, false, vec![2]));
    let page = bridge.pin_mut().fetch_review_rows(true, 2, 1);
    let (a, b) = (&rows.rows[0], &page.rows[0]);
    assert_eq!((a.frame_index, a.object_id, a.track_id), (b.frame_index, b.object_id, b.track_id));
    assert!(a.area == b.area && a.area_um2 == b.area_um2 && a.deformability == b.deformability, "the same row as a page");
    let invalid = bridge.pin_mut().fetch_review_track_rows(false, 1, max_rows);
    assert!(invalid.ok && invalid.positions == vec![1] && invalid.rows[0].frame_index == 3, "the invalid set by its own positions");
    let none = bridge.pin_mut().fetch_review_track_rows(true, 77, max_rows);
    assert!(none.ok && none.total == 0 && none.rows.is_empty() && !none.truncated);
    let untracked = bridge.pin_mut().fetch_review_track_rows(true, -1, max_rows);
    assert!(!untracked.ok && untracked.message.contains("not tracked"), "{}", untracked.message);
    let over = bridge.pin_mut().fetch_review_track_rows(true, 2, max_rows + 1);
    assert!(!over.ok && over.message.contains("257") && over.message.contains("256"), "{}", over.message);
    let zero = bridge.pin_mut().fetch_review_track_rows(true, 2, 0);
    assert!(zero.ok && zero.total == 1 && zero.rows.is_empty() && zero.truncated, "max 0 counts only");

    // Reprocess: the stored row is the record's own; the settings are named;
    // a match, when there is one, points into the reprocessed rows.
    for settings in [recorded, default] {
        let r = bridge.pin_mut().review_reprocess_record(true, 4, settings);
        assert!(r.ok, "{}", r.message);
        assert_eq!(r.settings, settings);
        assert_eq!(r.used_recorded_settings, settings == recorded, "the fixture records its config");
        assert!(r.message.starts_with("Reprocessed record 4 with ") && r.message.contains(&r.settings_label), "{}", r.message);
        assert!(!r.core_version.is_empty() && !r.core_source.is_empty(), "the core is named");
        let stored = &bridge.pin_mut().fetch_review_rows(true, 4, 1).rows[0];
        assert_eq!((r.stored.frame_index, r.stored.object_id, r.stored.area), (stored.frame_index, stored.object_id, stored.area));
        assert!(r.reprocessed.len() <= 64 && (!r.truncated || r.reprocessed.len() == 64));
        assert!(!r.has_match || (r.match_index as usize) < r.reprocessed.len());
        assert!(r.reprocessed.iter().all(|x| x.frame_index == stored.frame_index && x.track_id == -1), "the record's frame, untracked");
        assert_eq!(r.object_count_differs, r.reprocessed.len() as i64 != stored.object_count as i64);
        // The calibration it ran with (#756 review): the file's 0.25 µm/px
        // as recorded, the processing default otherwise; no E-LUT, no E.
        let expected = if settings == recorded { 0.25 } else { 0.4886 };
        assert!((r.pixel_to_micron - expected).abs() < 1e-12, "{} {}", r.pixel_to_micron, r.settings_label);
        assert!(!r.emodulus_recomputed && r.reprocessed.iter().all(|x| x.youngs_modulus.is_nan()));
    }
    let r = bridge.pin_mut().review_reprocess_record(true, 0, recorded);
    assert!(r.settings_label.starts_with("recorded settings") && r.settings_label.contains("px→µm 0.2500 recorded"), "{}", r.settings_label);
    assert_eq!(
        bridge.pin_mut().review_reprocess_record(true, 0, default).settings_label,
        "default settings (whole image, no background); px→µm 0.4886 default; E not recomputed (no E-LUT loaded)"
    );

    // Refusals: beyond the set, unknown settings.
    let beyond = bridge.pin_mut().review_reprocess_record(true, 10, recorded);
    assert!(!beyond.ok && beyond.message.contains("10 records"), "{}", beyond.message);
    let unknown = bridge.pin_mut().review_reprocess_record(true, 0, 2);
    assert!(!unknown.ok && unknown.message == "Unknown reprocess settings", "{}", unknown.message);

    // Never written: the file's bytes are what they were.
    assert!(bridge.pin_mut().review_close().ok);
    assert_eq!(std::fs::read(&path).unwrap(), before, "reprocessing writes nothing");

    // A legacy file (no attribute, no snapshot), its ROI at (4, 2), no boxes
    // for the mask check: unknown, and the match is refused (#756 review 2).
    let legacy = fixture_path("inspect_legacy");
    let _ = std::fs::remove_file(&legacy);
    assert!(ffi::review_fixture_write_run(&legacy.to_string_lossy(), "", ""));
    assert!(bridge.pin_mut().review_open(&legacy.to_string_lossy()).ok);
    let info = bridge.pin_mut().fetch_review_info();
    assert_eq!((info.object_coordinates.as_str(), info.object_coordinates_source.as_str()), ("unknown", ""));
    let refused = bridge.pin_mut().review_reprocess_record(true, 0, recorded);
    assert!(
        !refused.ok && refused.message.contains("cannot be matched") && refused.message.contains("starts at 4, 2"),
        "{}",
        refused.message
    );
    assert!(bridge.pin_mut().review_close().ok);
    let _ = std::fs::remove_file(&legacy);

    bridge.pin_mut().shutdown();
    let _ = std::fs::remove_file(&path);
}
