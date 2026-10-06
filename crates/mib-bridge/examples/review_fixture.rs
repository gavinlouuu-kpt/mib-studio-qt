//! Write a review fixture HDF5 through the review bridge (ADR 0014).
//!
//! Dev / screenshot / manual-test helper, no AppBackend needed:
//!
//! ```text
//! # small synthetic experiment file (10 valid, 4 invalid, series, ROI, accounting, KDE record)
//! cargo run --example review_fixture -- out.h5
//! # Charts population: N valid cells in two seeded clusters, no stored core record
//! cargo run --example review_fixture -- out.h5 --population 3000
//! # real cells: run the bundled kernel over a folder of frames (regenerate-masks job)
//! cargo run --example review_fixture -- out.h5 --from-folder build/vendor/assets/<frames dir>
//! ```

use std::time::{Duration, Instant};

use mib_bridge::review_ffi as ffi;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(out) = args.first() else {
        eprintln!("usage: review_fixture <out.h5> [--population <cells> | --from-folder <dir>]");
        std::process::exit(2);
    };
    let folder = args.iter().position(|a| a == "--from-folder").and_then(|i| args.get(i + 1));
    let population = args.iter().position(|a| a == "--population").and_then(|i| args.get(i + 1));

    if let Some(n) = population {
        let cells: u32 = n.parse().unwrap_or_else(|_| {
            eprintln!("--population takes a cell count");
            std::process::exit(2);
        });
        if !ffi::review_fixture_write_population(out, cells, 20261001) {
            eprintln!("population fixture write failed: {out}");
            std::process::exit(1);
        }
        println!("wrote population fixture {out} ({cells} valid cells)");
        return;
    }

    let Some(folder) = folder else {
        if !ffi::review_fixture_write_experiment(out) {
            eprintln!("fixture write failed: {out}");
            std::process::exit(1);
        }
        println!("wrote synthetic fixture {out}");
        return;
    };

    let mut bridge = ffi::new_review_bridge();
    bridge.pin_mut().initialize("");
    // Folder source (4), default processing config, no recorded config.
    let started = bridge.pin_mut().review_regenerate_masks(4, folder, 0, 0, out, false, true);
    if !started.ok {
        eprintln!("regenerate refused: {}", started.message);
        std::process::exit(1);
    }
    let deadline = Instant::now() + Duration::from_secs(600);
    while Instant::now() < deadline {
        for e in bridge.pin_mut().poll_review_events() {
            if e.operation_id != started.operation_id {
                continue;
            }
            match e.state {
                1 => eprint!("\r{} / {} {}", e.progress, e.total, e.message),
                2 => {
                    println!("\nwrote {}", e.message);
                    return;
                }
                s if s >= 3 => {
                    eprintln!("\njob ended in state {s}: {}", e.message);
                    std::process::exit(1);
                }
                _ => {}
            }
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    eprintln!("timed out");
    std::process::exit(1);
}
