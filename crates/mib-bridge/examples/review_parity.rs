//! YOFO Review half of the parity sign-off (plan 2026-10-01-standalone-review-app,
//! PR 8; driven by `tools/review_parity/run.sh`).
//!
//! Opens `<source.h5>` through the review bridge exactly as YOFO Review does
//! and writes to `<out>/yofo/`:
//!   summary.json  ReviewInfo facts (accounting summary, px→µm, ring-ratio range)
//!   scatter.json  the scatter arrays the Charts view plots (TS extents / bins
//!                 are computed from it by `desktop/scripts/review-parity-charts.test.ts`)
//!   metrics.csv   Export Metrics (experiment files)
//!   all/          Export All (all series images) without chart snapshots
//!   core.json     the full-run core record ComputeCore produces (fraction 0.9)
//!
//! ```text
//! cargo run --features review-only --example review_parity -- <source.h5> <out> [--fallback 0.4886]
//! ```

use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use mib_bridge::review_ffi as ffi;
use serde_json::json;

/// YOFO Review's default fallback px→µm (`DEFAULT_PX_TO_UM`, ReviewApp.tsx).
const DEFAULT_FALLBACK: f64 = 0.4886;

fn wait(bridge: &mut cxx::UniquePtr<ffi::ReviewBridge>, started: &ffi::ReviewResult, what: &str) -> Result<String, String> {
    if !started.ok {
        return Err(format!("{what} refused: {}", started.message));
    }
    let deadline = Instant::now() + Duration::from_secs(1800);
    while Instant::now() < deadline {
        for e in bridge.pin_mut().poll_review_events() {
            if e.operation_id != started.operation_id {
                continue;
            }
            match e.state {
                2 => return Ok(e.message),
                s if s >= 3 => return Err(format!("{what} ended in state {s}: {}", e.message)),
                _ => {}
            }
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    Err(format!("{what} timed out"))
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 2 {
        eprintln!("usage: review_parity <source.h5> <out> [--fallback <px→µm>]");
        std::process::exit(2);
    }
    let source = &args[0];
    let out: PathBuf = Path::new(&args[1]).join("yofo");
    let fallback = args
        .iter()
        .position(|a| a == "--fallback")
        .and_then(|i| args.get(i + 1))
        .and_then(|v| v.parse::<f64>().ok())
        .unwrap_or(DEFAULT_FALLBACK);
    let _ = std::fs::remove_dir_all(&out);
    std::fs::create_dir_all(&out).expect("create output dir");

    let mut bridge = ffi::new_review_bridge();
    bridge.pin_mut().initialize("");
    bridge.pin_mut().set_fallback_pixel_to_micron(fallback);
    let opened = bridge.pin_mut().review_open(source);
    if !opened.ok {
        eprintln!("open failed: {}", opened.message);
        std::process::exit(1);
    }
    let info = bridge.pin_mut().fetch_review_info();
    let recording = info.recording_file;
    let mut failures: Vec<String> = Vec::new();

    let mut summary = json!({
        "source": source,
        "recording_file": recording,
        "total_valid": info.total_valid,
        "total_invalid": info.total_invalid,
        "accounting_summary": info.accounting_summary,
        "pixel_to_micron": info.pixel_to_micron,
        "pixel_to_micron_from_file": info.pixel_to_micron_from_file,
        "has_recorded_config": info.has_recorded_config,
        "ring_ratio_min": info.ring_ratio_min,
        "ring_ratio_max": info.ring_ratio_max,
    });

    if !recording {
        let sc = bridge.pin_mut().fetch_review_scatter();
        let scatter = json!({
            "valid": sc.valid,
            "pixel_to_micron": sc.pixel_to_micron,
            "area_um2": sc.area_um2,
            "deformability": sc.deformability,
            "ring_ratio": sc.ring_ratio,
        });
        std::fs::write(out.join("scatter.json"), scatter.to_string()).expect("write scatter.json");
        summary["scatter_count"] = json!(sc.area_um2.len());

        let csv = out.join("metrics.csv");
        let r = bridge.pin_mut().review_export_metrics(&csv.to_string_lossy());
        if let Err(e) = wait(&mut bridge, &r, "export metrics") {
            failures.push(e);
        }

        let r = bridge.pin_mut().review_compute_core(0.9);
        match wait(&mut bridge, &r, "compute core") {
            Ok(msg) => {
                summary["core_message"] = json!(msg);
                let core = bridge.pin_mut().fetch_review_computed_core_json();
                std::fs::write(out.join("core.json"), core).expect("write core.json");
            }
            Err(e) => failures.push(e),
        }
    }

    let all = out.join("all");
    std::fs::create_dir_all(&all).expect("create all/");
    // All series images: the Qt prompt's default (0..count-1); review.rs maps an
    // empty end to u64::MAX.
    let r = bridge.pin_mut().review_export_all(&all.to_string_lossy(), true, 0, u64::MAX, Vec::new());
    if let Err(e) = wait(&mut bridge, &r, "export all") {
        failures.push(e);
    }

    summary["failures"] = json!(failures);
    std::fs::write(out.join("summary.json"), serde_json::to_string_pretty(&summary).unwrap()).expect("write summary.json");
    bridge.pin_mut().review_close();
    println!("yofo dump: {}", out.display());
    if !failures.is_empty() {
        for f in &failures {
            eprintln!("{f}");
        }
        std::process::exit(1);
    }
}
