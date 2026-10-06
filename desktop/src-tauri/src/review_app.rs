//! YOFO Review (feature `review-only`, ADR 0014, plan
//! 2026-10-01-standalone-review-app): the standalone review app built from
//! this crate's binary with `tauri.review.conf.json`. It holds only the review
//! bridge — no camera, experiment or hardware state exists — and registers only
//! the review, platform, isoelastic and update commands. MIB Studio's shell
//! (`lib.rs`, default feature `studio`) is not compiled into this build.

use std::sync::Mutex;

use mib_bridge::review_ffi;
use tauri::Manager;

use crate::{isoelastic, platform, review, review_update};

pub struct AppState {
    pub review: Mutex<cxx::UniquePtr<review_ffi::ReviewBridge>>,
}

/// Resolve the data directory: an empty `data_dir` means "use the platform
/// app-data dir".
pub fn resolve_data_dir(app: &tauri::AppHandle, data_dir: String) -> Result<String, String> {
    if data_dir.trim().is_empty() {
        Ok(app
            .path()
            .app_data_dir()
            .map_err(|e| format!("resolve app data dir: {e}"))?
            .to_string_lossy()
            .into_owned())
    } else {
        Ok(data_dir)
    }
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .manage(AppState { review: Mutex::new(review_ffi::new_review_bridge()) })
        .invoke_handler(tauri::generate_handler![
            review::abi_version,
            review::is_initialized,
            review::init,
            platform::app_paths,
            platform::get_preferences,
            platform::set_preferences,
            platform::shell_log,
            review::review_abi_version,
            review::review_launch_path,
            review::review_take_open_request,
            review_update::review_check_update,
            review_update::review_install_update,
            isoelastic::fetch_isoelastic_curves,
            review::review_open,
            review::review_close,
            review::review_set_pixel_to_micron,
            review::fetch_review_info,
            review::fetch_review_rows,
            review::fetch_review_frame,
            review::fetch_review_series_count,
            review::fetch_review_series_packet,
            review::fetch_review_thumbnails_packet,
            review::fetch_review_scatter,
            review::review_save_core_record,
            review::review_export_metrics,
            review::review_export_all,
            review::review_export_charts,
            review::review_stage_chart,
            review::review_clear_charts,
            review::review_list_dir,
            review::review_batch_export,
            review::review_regenerate_masks,
            review::review_compute_core,
            review::fetch_review_computed_core_json,
            review::review_request_density,
            review::fetch_review_density,
            review::review_jobs_busy,
            review::poll_review_events,
            review::cancel_review_operation,
        ])
        .setup(|app| {
            // The updater exists only in builds that carry the minisign public
            // key (tauri.review.conf.json plugins.updater).
            if !review_update::configured_pubkey(app.config()).is_empty() {
                app.handle().plugin(tauri_plugin_updater::Builder::new().build())?;
            }
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("error while building tauri application")
        .run(|_app, _event| {
            // macOS Finder opens (the .h5 / .hdf5 association): queue the first
            // HDF5 file and tell the window. Windows and Linux pass the file as
            // an argument instead (review_launch_path).
            #[cfg(target_os = "macos")]
            if let tauri::RunEvent::Opened { urls } = &_event {
                use tauri::Emitter;
                let paths = urls
                    .iter()
                    .filter_map(|u| u.to_file_path().ok())
                    .map(|p| p.to_string_lossy().into_owned());
                let path = review::launch_path_from(paths);
                if !path.is_empty() {
                    review::set_pending_open(path.clone());
                    let _ = _app.emit(review::OPEN_FILE_EVENT, path);
                }
            }
        });
}
