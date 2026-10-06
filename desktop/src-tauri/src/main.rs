#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

// One crate, two apps. MIB Studio (default feature `studio`) is the library in
// lib.rs. YOFO Review (`--no-default-features --features review-only`,
// tauri.review.conf.json; ADR 0014) is built from the modules below and links
// only the review bridge.
#[cfg(all(feature = "studio", feature = "review-only"))]
compile_error!("build YOFO Review with --no-default-features --features review-only");

#[cfg(feature = "review-only")]
mod isoelastic;
#[cfg(feature = "review-only")]
mod platform;
#[cfg(feature = "review-only")]
mod review;
#[cfg(feature = "review-only")]
mod review_app;
#[cfg(feature = "review-only")]
mod review_packet;
#[cfg(feature = "review-only")]
mod review_update;
#[cfg(feature = "review-only")]
mod wire;

fn main() {
    #[cfg(feature = "review-only")]
    review_app::run();
    #[cfg(not(feature = "review-only"))]
    mib_studio_desktop_lib::run()
}
