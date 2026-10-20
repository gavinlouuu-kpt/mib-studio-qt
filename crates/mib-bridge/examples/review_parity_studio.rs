//! MIB Studio half of the review parity harness (#512 S6, plan
//! docs/exec-plans/active/2026-10-18-one-review-implementation.md; driven by
//! `tools/review_parity/run.sh`).
//!
//! Opens `<source.h5>` through the backend bridge (`BackendBridge`, the
//! facade MIB Studio's Review tab and YOFO Studio's remote client call) with
//! the requests the tab builds, and writes to `<out>/studio/`:
//!   summary.json     what was loaded, the live factor / ring-ratio range as
//!                    opened and as compared, the export status snapshots,
//!                    the overlay renders
//!   charts_app.json  `fetch_review_charts_json` as opened: MIB Studio's live
//!                    factor and range (decisions D3 / D4)
//!   charts.json      `fetch_review_charts_json` with the live factor and
//!                    range set to the file's recorded ones (as the Qt dump
//!                    sets its live config), so the compared outputs share
//!                    the other halves' inputs
//!   metrics.csv      `review_export_json` metrics_csv (experiment files)
//!   all/             `review_export_json` all, with the backend-rendered
//!                    chart TIFFs the facade writes (decision D5)
//!   overlays/        `render_review_overlay` PNGs: valid / invalid frames
//!                    0, middle, last × modes 0..4 × ROI off / on (decision D2)
//!
//! The file's recorded factor and range are read through the review bridge
//! (ReviewSession, the reader the Qt tab and YOFO Review use for TD-17); a file
//! without them keeps the live values (D3 / D4: the live value is the fallback).
//!
//! ```text
//! cargo run --release --example review_parity_studio -- <source.h5> <out>
//! ```

#[cfg(feature = "review-only")]
fn main() {
    eprintln!("review_parity_studio drives the backend bridge; build it without --features review-only");
    std::process::exit(2);
}

#[cfg(not(feature = "review-only"))]
fn main() {
    studio::main();
}

#[cfg(not(feature = "review-only"))]
mod studio {
    use std::path::{Path, PathBuf};
    use std::time::{Duration, Instant};

    use mib_bridge::{ffi, review_ffi};
    use serde_json::{json, Value};

    /// Frames rendered per class: first, middle, last.
    fn sample_indices(count: u64) -> Vec<u64> {
        let mut v: Vec<u64> = if count == 0 { Vec::new() } else { vec![0, count / 2, count - 1] };
        v.dedup();
        v
    }

    /// The overlay file name both halves use (`yofo/overlays/<name>.ppm|pgm`).
    pub fn overlay_name(valid: bool, index: u64, mode: u32, roi: bool) -> String {
        format!("{}_{index:06}_m{mode}_roi{}", if valid { "valid" } else { "invalid" }, u8::from(roi))
    }

    /// The file's recorded factor and ring-ratio range, as ReviewSession reads
    /// them; `None` where the file records none.
    struct Recorded {
        factor: Option<f64>,
        ring: Option<(f64, f64)>,
    }

    fn read_recorded(source: &str) -> Result<Recorded, String> {
        let mut review = review_ffi::new_review_bridge();
        review.pin_mut().initialize("");
        let opened = review.pin_mut().review_open(source);
        if !opened.ok {
            return Err(format!("review bridge open failed: {}", opened.message));
        }
        let info = review.pin_mut().fetch_review_info();
        review.pin_mut().review_close();
        review.pin_mut().shutdown();
        Ok(Recorded {
            factor: info.pixel_to_micron_from_file.then_some(info.pixel_to_micron),
            ring: info.has_recorded_config.then_some((info.ring_ratio_min, info.ring_ratio_max)),
        })
    }

    struct Live {
        factor: f64,
        ring_min: f64,
        ring_max: f64,
    }

    fn live(bridge: &mut cxx::UniquePtr<ffi::BackendBridge>) -> Result<Live, String> {
        let doc = bridge.pin_mut().fetch_processing_config_json();
        if !doc.valid {
            return Err("fetch_processing_config_json: not valid".into());
        }
        let v: Value = serde_json::from_str(&doc.json).map_err(|e| format!("processing config JSON: {e}"))?;
        let number = |v: &Value, what: &str| v.as_f64().ok_or_else(|| format!("processing config has no {what}"));
        Ok(Live {
            factor: number(&v["pixel_to_micron"], "pixel_to_micron")?,
            ring_min: number(&v["image_processing"]["ring_ratio_min"], "image_processing.ring_ratio_min")?,
            ring_max: number(&v["image_processing"]["ring_ratio_max"], "image_processing.ring_ratio_max")?,
        })
    }

    fn charts(bridge: &mut cxx::UniquePtr<ffi::BackendBridge>, path: &Path) -> Value {
        let text = bridge.pin_mut().fetch_review_charts_json();
        std::fs::write(path, &text).expect("write charts JSON");
        serde_json::from_str(&text).unwrap_or_else(|e| json!({"valid": false, "error": format!("unparsable: {e}")}))
    }

    /// `review_export_json`, then the status snapshot polled (as
    /// exportControls.tsx does) until this operation's terminal state.
    fn export(bridge: &mut cxx::UniquePtr<ffi::BackendBridge>, request: &Value, what: &str) -> Result<Value, String> {
        let started = bridge.pin_mut().review_export_json(&request.to_string());
        if !started.ok {
            return Err(format!("{what} refused: {}", started.message));
        }
        let id = started.operation_id.to_string();
        let deadline = Instant::now() + Duration::from_secs(1800);
        while Instant::now() < deadline {
            let _ = bridge.pin_mut().poll_events();
            let status: Value = serde_json::from_str(&bridge.pin_mut().review_export_status_json())
                .map_err(|e| format!("{what} status JSON: {e}"))?;
            if status["operation_id"] == id.as_str() {
                match status["state"].as_str() {
                    Some("completed") => return Ok(status),
                    Some("cancelled") | Some("failed") => {
                        return Err(format!("{what} ended {}: {}", status["state"], status["error"]))
                    }
                    _ => {}
                }
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        Err(format!("{what} timed out"))
    }

    pub fn main() {
        let args: Vec<String> = std::env::args().skip(1).collect();
        if args.len() != 2 {
            eprintln!("usage: review_parity_studio <source.h5> <out>");
            std::process::exit(2);
        }
        let source = &args[0];
        let out: PathBuf = Path::new(&args[1]).join("studio");
        let _ = std::fs::remove_dir_all(&out);
        std::fs::create_dir_all(&out).expect("create output dir");
        // Headless and offline, as the Qt dump runs its AppBackend.
        for (key, value) in [
            ("MIB_CAMERA_MODE", "mock"),
            ("MIB_DISABLED_SERVICES", "auto_update,autofocus,trigger,syringe_pump"),
            ("MIB_STUDIO_PROCESSING_CORE_BASE_URL", "http://invalid-registry.example"),
            ("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/mib-lut-manifest.json"),
        ] {
            if std::env::var_os(key).is_none() {
                std::env::set_var(key, value);
            }
        }

        let recorded = match read_recorded(source) {
            Ok(r) => r,
            Err(e) => {
                eprintln!("{e}");
                std::process::exit(1);
            }
        };

        let data_dir = std::env::temp_dir().join(format!("mib_review_parity_studio_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&data_dir);
        let mut bridge = ffi::new_backend_bridge();
        if !bridge.pin_mut().initialize(&data_dir.to_string_lossy()) {
            eprintln!("backend bridge initialize failed");
            std::process::exit(1);
        }
        let mut failures: Vec<String> = Vec::new();
        let finish = |bridge: &mut cxx::UniquePtr<ffi::BackendBridge>, summary: &Value, failures: &[String]| {
            std::fs::write(out.join("summary.json"), serde_json::to_string_pretty(summary).unwrap())
                .expect("write summary.json");
            let _ = bridge.pin_mut().close_review();
            bridge.pin_mut().shutdown();
            let _ = std::fs::remove_dir_all(&data_dir);
            println!("studio dump: {}", out.display());
            if !failures.is_empty() {
                for f in failures {
                    eprintln!("{f}");
                }
                std::process::exit(1);
            }
        };

        let load = bridge.pin_mut().load_recording(source);
        if !load.ok {
            failures.push(format!("load_recording failed: {}", load.message));
            finish(&mut bridge, &json!({"source": source, "failures": failures}), &failures);
            return;
        }
        let meta = bridge.pin_mut().fetch_review_metadata();
        let recording = meta.recording_file;
        let as_opened = match live(&mut bridge) {
            Ok(l) => l,
            Err(e) => {
                failures.push(e);
                finish(&mut bridge, &json!({"source": source, "failures": failures}), &failures);
                return;
            }
        };
        let mut summary = json!({
            "source": source,
            "file_path": meta.file_path,
            "recording_file": recording,
            "total_valid": meta.total_valid,
            "total_invalid": meta.total_invalid,
            "roi": [meta.roi_x, meta.roi_y, meta.roi_w, meta.roi_h],
            "app": {
                "pixel_to_micron": as_opened.factor,
                "ring_ratio_min": as_opened.ring_min,
                "ring_ratio_max": as_opened.ring_max,
            },
        });

        // As opened: MIB Studio's live factor and range (D3 / D4).
        let charts_app = charts(&mut bridge, &out.join("charts_app.json"));
        if !recording && charts_app["valid"] != true {
            failures.push(format!("charts as opened: {}", charts_app["error"]));
        }

        // Compared outputs: the live factor and range set to the recorded ones.
        let factor = recorded.factor.unwrap_or(as_opened.factor);
        let (ring_min, ring_max) = recorded.ring.unwrap_or((as_opened.ring_min, as_opened.ring_max));
        let applied = bridge.pin_mut().apply_processing_config_json(
            &json!({
                "pixel_to_micron": factor,
                "image_processing": {"ring_ratio_min": ring_min, "ring_ratio_max": ring_max},
            })
            .to_string(),
        );
        if !applied.ok {
            failures.push(format!("apply_processing_config_json refused: {}", applied.message));
        }
        let compared = live(&mut bridge).unwrap_or(Live { factor: f64::NAN, ring_min: f64::NAN, ring_max: f64::NAN });
        summary["pixel_to_micron"] = json!(compared.factor);
        summary["compared"] = json!({
            "pixel_to_micron": compared.factor,
            "pixel_to_micron_from_file": recorded.factor.is_some(),
            "ring_ratio_min": compared.ring_min,
            "ring_ratio_max": compared.ring_max,
            "ring_range_from_file": recorded.ring.is_some(),
        });
        let compared_charts = charts(&mut bridge, &out.join("charts.json"));
        if !recording && compared_charts["valid"] != true {
            failures.push(format!("charts: {}", compared_charts["error"]));
        }

        // Exports with the requests exportControls.tsx builds (default frame
        // class, series and overlay options; App.tsx passes the live factor).
        let base = |format: &str, root: &Path| {
            json!({
                "output_root": root.to_string_lossy(),
                "format": format,
                "frames": "both",
                "series": {"enabled": true, "start": 0},
                "isoelastic_overlays": true,
                "conversion_factor": compared.factor,
                "keep_partial_on_failure": true,
            })
        };
        if !recording {
            let csv = out.join("metrics.csv");
            let mut request = base("metrics_csv", &out);
            request["explicit_destination"] = json!(csv.to_string_lossy());
            match export(&mut bridge, &request, "export metrics") {
                Ok(status) => summary["metrics_export"] = status,
                Err(e) => failures.push(e),
            }
        }
        let all = out.join("all");
        std::fs::create_dir_all(&all).expect("create all/");
        match export(&mut bridge, &base("all", &all), "export all") {
            Ok(status) => summary["all_export"] = status,
            Err(e) => failures.push(e),
        }

        // Saved-image overlays as SavedReviewImage.tsx requests them.
        if !recording {
            let dir = out.join("overlays");
            std::fs::create_dir_all(&dir).expect("create overlays/");
            let mut rendered = Vec::new();
            for (valid, images, masks) in [
                (true, &meta.valid_images, &meta.valid_masks),
                (false, &meta.invalid_images, &meta.invalid_masks),
            ] {
                if !images.present {
                    continue;
                }
                let modes: Vec<u32> = if masks.present { (0..=4).collect() } else { vec![0] };
                for index in sample_indices(images.count) {
                    for &mode in &modes {
                        for roi in [false, true] {
                            let name = overlay_name(valid, index, mode, roi);
                            let request = json!({"source_path": meta.file_path, "valid": valid, "index": index, "mode": mode, "roi": roi});
                            let png = bridge.pin_mut().render_review_overlay(&request.to_string());
                            if png.is_empty() {
                                failures.push(format!("render_review_overlay {name}: empty"));
                                continue;
                            }
                            std::fs::write(dir.join(format!("{name}.png")), &png).expect("write overlay");
                            rendered.push(json!(name));
                        }
                    }
                }
            }
            summary["overlays"] = json!(rendered);
        }

        summary["failures"] = json!(failures);
        finish(&mut bridge, &summary, &failures);
    }
}
