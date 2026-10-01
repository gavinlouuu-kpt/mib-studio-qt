//! Review bridge: `cxx` FFI over `backend::review::ReviewSession` (plan
//! 2026-10-01-standalone-review-app, ADR 0008).
//!
//! This is the one review surface every React shell uses — MIB Studio's
//! Review tab and the whole YOFO Review window. It is a separate bridge
//! module from [`crate::ffi`] on purpose: the C++ behind it
//! (`review_shim.cpp`) links `mib_review_core` + `mib_processing` only, so
//! the `review-only` cargo feature builds a binary with no `AppBackend`,
//! cameras, serial, SQLite or Sentry. Without the feature both bridges are
//! compiled and a MIB Studio shell holds one of each.
//!
//! Values are contract-pinned (`crates/mib-bridge/contract/bridge-contract.json`,
//! groups `review_image_datasets`, `overlay_modes`, `review_pixel_formats`,
//! `review_operation_kinds`, `operation_states`); additive only (ADR 0004).

#[cxx::bridge(namespace = "mib_review_bridge")]
pub mod review_ffi {
    /// Flattened result of a review command. `operation_id` is non-zero
    /// when the command started a tracked job (progress and the terminal
    /// state arrive through `poll_review_events`).
    #[derive(Debug, Clone, Default)]
    pub struct ReviewResult {
        pub ok: bool,
        pub message: String,
        pub operation_id: u64,
    }

    /// A packed 8-bit image: `pixel_format` is a contract
    /// `review_pixel_formats` value (Mono8 = 1 channel, RGB8 = 3), row
    /// stride = width × channels. `valid` false = nothing available.
    #[derive(Debug, Clone, Default)]
    pub struct ReviewFrame {
        pub valid: bool,
        pub frame_index: u64,
        pub width: u64,
        pub height: u64,
        pub pixel_format: u64,
        pub stride_bytes: u64,
        pub data: Vec<u8>,
    }

    #[derive(Debug, Clone, Default)]
    pub struct ReviewDatasetInfo {
        pub present: bool,
        pub count: u64,
        pub height: i32,
        pub width: i32,
        pub channels: i32,
    }

    /// Everything a shell shows about the open file without reading pixels.
    #[derive(Debug, Clone, Default)]
    pub struct ReviewInfo {
        pub valid: bool,
        pub file_open: bool,
        pub file_path: String,
        pub recording_file: bool,
        pub start_time_ns: u64,
        pub end_time_ns: u64,
        pub total_valid: u64,
        pub total_invalid: u64,
        pub filtered_frames: u64,
        pub roi_x: i32,
        pub roi_y: i32,
        pub roi_w: i32,
        pub roi_h: i32,
        pub has_background: bool,
        pub has_core_identity: bool,
        pub core_version: String,
        pub core_source: String,
        pub core_release_tag: String,
        pub valid_images: ReviewDatasetInfo,
        pub invalid_images: ReviewDatasetInfo,
        pub valid_masks: ReviewDatasetInfo,
        pub invalid_masks: ReviewDatasetInfo,
        pub recorded_images: ReviewDatasetInfo,
        pub has_series: bool,
        pub series_count: u64,
        pub multi_image_enabled: bool,
        pub multi_image_count: u64,
        /// Run accounting (issue #367); `has_accounting` false for legacy
        /// files. `accounting_completion` is a contract
        /// `run_completion_states` value.
        pub has_accounting: bool,
        pub accounting_completion: u32,
        pub accounting_reconciled: bool,
        pub accounting_empty: u64,
        pub accounting_rejected: u64,
        pub accounting_processing_failed: u64,
        pub accounting_store_loss: u64,
        pub accounting_persisted: u64,
        pub accounting_admitted: u64,
        pub accounting_persistence_failed: u64,
        /// The Qt status-line text (" · run complete — empty 0, …").
        pub accounting_summary: String,
        /// TD-17: the factor the file was recorded with when
        /// `pixel_to_micron_from_file`, else the host fallback.
        pub pixel_to_micron: f64,
        pub pixel_to_micron_from_file: bool,
        /// Stored KDE records as written (empty when absent).
        pub kde_analysis_json: String,
        pub kde_live_json: String,
    }

    /// One metrics row: the full FilterResult the Qt table shows.
    #[derive(Debug, Clone, Default)]
    pub struct ReviewRow {
        pub frame_index: u64,
        pub timestamp_ns: u64,
        pub valid: bool,
        pub target_group: bool,
        pub touches_border: bool,
        pub has_single_inner_contour: bool,
        pub in_range: bool,
        pub in_channel: bool,
        pub inner_contour_count: i32,
        pub object_id: i32,
        pub object_count: i32,
        pub track_id: i32,
        pub track_first_frame: u64,
        pub track_last_frame: u64,
        pub track_observation_count: i32,
        pub bbox_x: f64,
        pub bbox_y: f64,
        pub bbox_width: f64,
        pub bbox_height: f64,
        pub centroid_x: f64,
        pub centroid_y: f64,
        pub area: f64,
        pub area_um2: f64,
        pub deformability: f64,
        pub area_ratio: f64,
        pub ring_ratio: f64,
        pub laplacian_variance: f64,
        pub youngs_modulus: f64,
        pub brightness_q1: f64,
        pub brightness_q2: f64,
        pub brightness_q3: f64,
        pub brightness_q4: f64,
    }

    #[derive(Debug, Clone, Default)]
    pub struct ReviewRows {
        pub valid: bool,
        pub total: u64,
        pub offset: u64,
        pub rows: Vec<ReviewRow>,
    }

    /// Columnar valid-set scatter (rows with validation.isValid only, in
    /// valid-set order); `valid_position` is the 0-based index into the
    /// valid set (what the frame pulls take), `frame_index` the recorded
    /// ProcessedFrame::index (what the table and CSV show).
    #[derive(Debug, Clone, Default)]
    pub struct ReviewScatter {
        pub valid: bool,
        pub pixel_to_micron: f64,
        pub frame_index: Vec<u64>,
        pub valid_position: Vec<u64>,
        pub area_um2: Vec<f64>,
        pub deformability: Vec<f64>,
        pub target_group: Vec<u8>,
    }

    /// Lifecycle of a review job (export, batch, regenerate masks, core
    /// contour, density): `kind` is a contract `review_operation_kinds`
    /// value, `state` an `operation_states` value.
    #[derive(Debug, Clone, Default)]
    pub struct ReviewEvent {
        pub operation_id: u64,
        pub kind: u32,
        pub state: u32,
        pub progress: u64,
        pub total: u64,
        pub message: String,
    }

    unsafe extern "C++" {
        include!("mib-bridge/src/review_shim.h");

        /// Opaque owner of a `ReviewSession` (+ the processing service the
        /// jobs need). Dropping it closes the file and joins the jobs.
        type ReviewBridge;

        fn new_review_bridge() -> UniquePtr<ReviewBridge>;

        /// Same number as `bridge_abi_version()`; both bridges share the
        /// contract document.
        fn review_bridge_abi_version() -> u32;

        /// Test fixture (never a Tauri command): writes a small experiment
        /// file (10 valid frames, 4 invalid, 3-image series, ROI, run
        /// snapshot with pixel_to_micron 0.25, accounting, a stored KDE
        /// record) so bridge tests need no camera.
        fn review_fixture_write_experiment(path: &str) -> bool;

        /// Set up logging under `data_dir` (empty = no file log).
        fn initialize(self: Pin<&mut ReviewBridge>, data_dir: &str) -> bool;
        fn shutdown(self: Pin<&mut ReviewBridge>);
        fn is_initialized(&self) -> bool;

        /// Open a file read-only (replaces the current one).
        fn review_open(self: Pin<&mut ReviewBridge>, path: &str) -> ReviewResult;
        fn review_close(self: Pin<&mut ReviewBridge>) -> ReviewResult;
        /// Fallback factor for files without a recorded one (TD-17).
        fn set_fallback_pixel_to_micron(self: Pin<&mut ReviewBridge>, factor: f64);

        fn fetch_review_info(self: Pin<&mut ReviewBridge>) -> ReviewInfo;
        fn fetch_review_rows(
            self: Pin<&mut ReviewBridge>,
            valid: bool,
            offset: u64,
            count: u64,
        ) -> ReviewRows;
        /// One image with the overlay composed in the backend. `dataset` is a
        /// `review_image_datasets` value, `overlay` an `overlay_modes` value.
        fn fetch_review_frame(
            self: Pin<&mut ReviewBridge>,
            dataset: u32,
            index: u64,
            overlay: u32,
            roi_overlay: bool,
        ) -> ReviewFrame;
        /// Number of series images of valid frame `index` (0 = none).
        fn fetch_review_series_count(self: Pin<&mut ReviewBridge>, index: u64) -> u64;
        fn fetch_review_series_frame(
            self: Pin<&mut ReviewBridge>,
            index: u64,
            k: u64,
            overlay: u32,
            roi_overlay: bool,
        ) -> ReviewFrame;
        /// `count` thumbnails from `offset`, letterboxed into `size` px
        /// tiles, packed as one frame of width `size` and height
        /// `size × count` (tile k at rows k·size..(k+1)·size).
        fn fetch_review_thumbnails(
            self: Pin<&mut ReviewBridge>,
            valid: bool,
            offset: u64,
            count: u64,
            size: u32,
            overlay: u32,
            roi_overlay: bool,
        ) -> ReviewFrame;
        fn fetch_review_scatter(self: Pin<&mut ReviewBridge>) -> ReviewScatter;
        /// Write the full-run KDE core record; refuses an existing record
        /// unless `overwrite`.
        fn review_save_core_record(
            self: Pin<&mut ReviewBridge>,
            json: &str,
            overwrite: bool,
        ) -> ReviewResult;

        /// Drain job lifecycle events (bounded queue, drop-oldest).
        fn poll_review_events(self: Pin<&mut ReviewBridge>) -> Vec<ReviewEvent>;
        fn cancel_review_operation(self: Pin<&mut ReviewBridge>, operation_id: u64)
            -> ReviewResult;
    }
}

// Same threading contract as `ffi::BackendBridge` (ADR 0003): moved between
// threads behind a Mutex, never shared.
unsafe impl Send for review_ffi::ReviewBridge {}

const _: fn() = || {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<std::sync::Mutex<cxx::UniquePtr<review_ffi::ReviewBridge>>>();
};
