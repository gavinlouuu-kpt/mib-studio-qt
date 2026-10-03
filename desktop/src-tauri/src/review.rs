//! Review commands over the review bridge (ADR 0008, plan
//! 2026-10-01-standalone-review-app). The one review surface for both
//! products: MIB Studio's Review tab and the YOFO Review window call these
//! and nothing else for review. Images travel as binary frame packets
//! (`frame_packet`), everything else as lossless JSON.

use mib_bridge::review_ffi;
use serde::Serialize;
use tauri::ipc::Response;
use tauri::State;

use std::sync::Mutex;

use crate::frame_packet;
use crate::wire::serialize_u64;
use crate::AppState;

/// Flattened review command result (`command` = contract command_types
/// Review = 9).
#[derive(Serialize, Clone)]
pub struct ReviewCmdResult {
    transport_version: u32,
    ok: bool,
    command: u32,
    message: String,
    #[serde(serialize_with = "serialize_u64")]
    operation_id: u64,
}

impl From<review_ffi::ReviewResult> for ReviewCmdResult {
    fn from(r: review_ffi::ReviewResult) -> Self {
        ReviewCmdResult {
            transport_version: frame_packet::JSON_TRANSPORT_VERSION,
            ok: r.ok,
            command: 9,
            message: r.message,
            operation_id: r.operation_id,
        }
    }
}

#[derive(Serialize, Clone, Default)]
pub struct ReviewDatasetInfo {
    present: bool,
    #[serde(serialize_with = "serialize_u64")]
    count: u64,
    height: i32,
    width: i32,
    channels: i32,
}

fn dataset_info(d: review_ffi::ReviewDatasetInfo) -> ReviewDatasetInfo {
    ReviewDatasetInfo { present: d.present, count: d.count, height: d.height, width: d.width, channels: d.channels }
}

#[derive(Serialize, Clone, Default)]
pub struct ReviewInfo {
    transport_version: u32,
    valid: bool,
    file_open: bool,
    file_path: String,
    recording_file: bool,
    #[serde(serialize_with = "serialize_u64")]
    start_time_ns: u64,
    #[serde(serialize_with = "serialize_u64")]
    end_time_ns: u64,
    #[serde(serialize_with = "serialize_u64")]
    total_valid: u64,
    #[serde(serialize_with = "serialize_u64")]
    total_invalid: u64,
    #[serde(serialize_with = "serialize_u64")]
    filtered_frames: u64,
    roi_x: i32,
    roi_y: i32,
    roi_w: i32,
    roi_h: i32,
    has_background: bool,
    has_core_identity: bool,
    core_version: String,
    core_source: String,
    core_release_tag: String,
    valid_images: ReviewDatasetInfo,
    invalid_images: ReviewDatasetInfo,
    valid_masks: ReviewDatasetInfo,
    invalid_masks: ReviewDatasetInfo,
    recorded_images: ReviewDatasetInfo,
    has_series: bool,
    #[serde(serialize_with = "serialize_u64")]
    series_count: u64,
    multi_image_enabled: bool,
    #[serde(serialize_with = "serialize_u64")]
    multi_image_count: u64,
    has_accounting: bool,
    accounting_completion: u32,
    accounting_reconciled: bool,
    #[serde(serialize_with = "serialize_u64")]
    accounting_empty: u64,
    #[serde(serialize_with = "serialize_u64")]
    accounting_rejected: u64,
    #[serde(serialize_with = "serialize_u64")]
    accounting_processing_failed: u64,
    #[serde(serialize_with = "serialize_u64")]
    accounting_store_loss: u64,
    #[serde(serialize_with = "serialize_u64")]
    accounting_persisted: u64,
    #[serde(serialize_with = "serialize_u64")]
    accounting_admitted: u64,
    #[serde(serialize_with = "serialize_u64")]
    accounting_persistence_failed: u64,
    accounting_summary: String,
    pixel_to_micron: f64,
    pixel_to_micron_from_file: bool,
    kde_analysis_json: String,
    kde_live_json: String,
    has_recorded_config: bool,
    ring_ratio_min: f64,
    ring_ratio_max: f64,
}

#[derive(Serialize, Clone, Default)]
pub struct ReviewRow {
    #[serde(serialize_with = "serialize_u64")]
    frame_index: u64,
    #[serde(serialize_with = "serialize_u64")]
    timestamp_ns: u64,
    valid: bool,
    target_group: bool,
    touches_border: bool,
    has_single_inner_contour: bool,
    in_range: bool,
    in_channel: bool,
    inner_contour_count: i32,
    object_id: i32,
    object_count: i32,
    track_id: i32,
    #[serde(serialize_with = "serialize_u64")]
    track_first_frame: u64,
    #[serde(serialize_with = "serialize_u64")]
    track_last_frame: u64,
    track_observation_count: i32,
    bbox_x: f64,
    bbox_y: f64,
    bbox_width: f64,
    bbox_height: f64,
    centroid_x: f64,
    centroid_y: f64,
    area: f64,
    area_um2: f64,
    deformability: f64,
    area_ratio: f64,
    ring_ratio: f64,
    laplacian_variance: Option<f64>,
    youngs_modulus: f64,
    brightness_q1: f64,
    brightness_q2: f64,
    brightness_q3: f64,
    brightness_q4: f64,
}

#[derive(Serialize, Clone, Default)]
pub struct ReviewRows {
    transport_version: u32,
    valid: bool,
    #[serde(serialize_with = "serialize_u64")]
    total: u64,
    #[serde(serialize_with = "serialize_u64")]
    offset: u64,
    rows: Vec<ReviewRow>,
}

#[derive(Serialize, Clone, Default)]
pub struct ReviewScatter {
    transport_version: u32,
    valid: bool,
    pixel_to_micron: f64,
    /// Decimal strings (exact u64), parallel arrays.
    frame_index: Vec<String>,
    valid_position: Vec<String>,
    area_um2: Vec<f64>,
    deformability: Vec<f64>,
    target_group: Vec<u8>,
    ring_ratio: Vec<f64>,
}

#[derive(Serialize, Clone, Default)]
pub struct ReviewEvent {
    #[serde(serialize_with = "serialize_u64")]
    operation_id: u64,
    kind: u32,
    state: u32,
    #[serde(serialize_with = "serialize_u64")]
    progress: u64,
    #[serde(serialize_with = "serialize_u64")]
    total: u64,
    message: String,
}

#[derive(Serialize, Clone, Default)]
pub struct ReviewEvents {
    transport_version: u32,
    events: Vec<ReviewEvent>,
}

fn parse_u64(value: &str) -> Result<u64, String> {
    let n = value.parse::<u64>().map_err(|_| "INVALID_U64".to_string())?;
    if n.to_string() != value {
        return Err("INVALID_U64".into());
    }
    Ok(n)
}

fn finite(n: f64) -> Option<f64> {
    if n.is_finite() { Some(n) } else { None }
}

/// YOFO Review: the review bridge's ABI (shared document with the backend
/// bridge). The MIB Studio build keeps its `abi_version` command.
#[cfg(feature = "review-only")]
#[tauri::command]
pub fn abi_version() -> u32 {
    review_ffi::review_bridge_abi_version()
}

#[cfg(feature = "review-only")]
#[tauri::command]
pub fn is_initialized(state: State<AppState>) -> Result<bool, String> {
    let guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.is_initialized())
}

#[cfg(feature = "review-only")]
#[tauri::command]
pub fn init(app: tauri::AppHandle, state: State<AppState>, data_dir: String) -> Result<bool, String> {
    let dir = crate::resolve_data_dir(&app, data_dir)?;
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().initialize(&dir))
}

/// A file the OS asked us to open after launch began: macOS delivers Finder
/// opens (double-click, "Open With", drag onto the Dock icon) as
/// `RunEvent::Opened`, possibly before the webview listens. `run()` stores it
/// here and emits [`OPEN_FILE_EVENT`]; whoever takes it first opens it, so a
/// file never opens twice.
static PENDING_OPEN: Mutex<Option<String>> = Mutex::new(None);

/// Event the window listens for (payload: the path) — a file to open now.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
pub const OPEN_FILE_EVENT: &str = "review-open-file";

#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
pub fn set_pending_open(path: String) {
    *PENDING_OPEN.lock().unwrap_or_else(|e| e.into_inner()) = Some(path);
}

fn take_pending_open() -> Option<String> {
    PENDING_OPEN.lock().unwrap_or_else(|e| e.into_inner()).take()
}

/// The HDF5 file to open at startup: a pending OS open request (macOS Finder),
/// else the first `.h5`/`.hdf5` argument (`yofo-review run.h5`, the Windows /
/// Linux file association), else "".
#[tauri::command]
pub fn review_launch_path() -> String {
    take_pending_open().unwrap_or_else(|| launch_path_from(std::env::args().skip(1)))
}

/// The pending OS open request only ("" when none) — what the window takes
/// when [`OPEN_FILE_EVENT`] arrives while it is running.
#[tauri::command]
pub fn review_take_open_request() -> String {
    take_pending_open().unwrap_or_default()
}

pub fn launch_path_from(args: impl Iterator<Item = String>) -> String {
    for a in args {
        let lower = a.to_ascii_lowercase();
        if (lower.ends_with(".h5") || lower.ends_with(".hdf5")) && std::path::Path::new(&a).is_file() {
            return a;
        }
    }
    String::new()
}

#[tauri::command]
pub fn review_abi_version() -> u32 {
    review_ffi::review_bridge_abi_version()
}

#[tauri::command]
pub fn review_open(state: State<AppState>, path: String) -> Result<ReviewCmdResult, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_open(&path).into())
}

#[tauri::command]
pub fn review_close(state: State<AppState>) -> Result<ReviewCmdResult, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_close().into())
}

/// Fallback pixel-to-micron for files without a recorded factor (TD-17).
#[tauri::command]
pub fn review_set_pixel_to_micron(state: State<AppState>, factor: f64) -> Result<(), String> {
    if !factor.is_finite() || factor <= 0.0 {
        return Err("INVALID_FACTOR".into());
    }
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    guard.pin_mut().set_fallback_pixel_to_micron(factor);
    Ok(())
}

#[tauri::command]
pub fn fetch_review_info(state: State<AppState>) -> Result<ReviewInfo, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    let m = guard.pin_mut().fetch_review_info();
    Ok(ReviewInfo {
        transport_version: frame_packet::JSON_TRANSPORT_VERSION,
        valid: m.valid,
        file_open: m.file_open,
        file_path: m.file_path,
        recording_file: m.recording_file,
        start_time_ns: m.start_time_ns,
        end_time_ns: m.end_time_ns,
        total_valid: m.total_valid,
        total_invalid: m.total_invalid,
        filtered_frames: m.filtered_frames,
        roi_x: m.roi_x,
        roi_y: m.roi_y,
        roi_w: m.roi_w,
        roi_h: m.roi_h,
        has_background: m.has_background,
        has_core_identity: m.has_core_identity,
        core_version: m.core_version,
        core_source: m.core_source,
        core_release_tag: m.core_release_tag,
        valid_images: dataset_info(m.valid_images),
        invalid_images: dataset_info(m.invalid_images),
        valid_masks: dataset_info(m.valid_masks),
        invalid_masks: dataset_info(m.invalid_masks),
        recorded_images: dataset_info(m.recorded_images),
        has_series: m.has_series,
        series_count: m.series_count,
        multi_image_enabled: m.multi_image_enabled,
        multi_image_count: m.multi_image_count,
        has_accounting: m.has_accounting,
        accounting_completion: m.accounting_completion,
        accounting_reconciled: m.accounting_reconciled,
        accounting_empty: m.accounting_empty,
        accounting_rejected: m.accounting_rejected,
        accounting_processing_failed: m.accounting_processing_failed,
        accounting_store_loss: m.accounting_store_loss,
        accounting_persisted: m.accounting_persisted,
        accounting_admitted: m.accounting_admitted,
        accounting_persistence_failed: m.accounting_persistence_failed,
        accounting_summary: m.accounting_summary,
        pixel_to_micron: m.pixel_to_micron,
        pixel_to_micron_from_file: m.pixel_to_micron_from_file,
        kde_analysis_json: m.kde_analysis_json,
        kde_live_json: m.kde_live_json,
        has_recorded_config: m.has_recorded_config,
        ring_ratio_min: m.ring_ratio_min,
        ring_ratio_max: m.ring_ratio_max,
    })
}

#[tauri::command]
pub fn fetch_review_rows(
    state: State<AppState>,
    valid: bool,
    offset: String,
    count: u32,
) -> Result<ReviewRows, String> {
    let offset = parse_u64(&offset)?;
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    let p = guard.pin_mut().fetch_review_rows(valid, offset, u64::from(count));
    Ok(ReviewRows {
        transport_version: frame_packet::JSON_TRANSPORT_VERSION,
        valid: p.valid,
        total: p.total,
        offset: p.offset,
        rows: p
            .rows
            .into_iter()
            .map(|r| ReviewRow {
                frame_index: r.frame_index,
                timestamp_ns: r.timestamp_ns,
                valid: r.valid,
                target_group: r.target_group,
                touches_border: r.touches_border,
                has_single_inner_contour: r.has_single_inner_contour,
                in_range: r.in_range,
                in_channel: r.in_channel,
                inner_contour_count: r.inner_contour_count,
                object_id: r.object_id,
                object_count: r.object_count,
                track_id: r.track_id,
                track_first_frame: r.track_first_frame,
                track_last_frame: r.track_last_frame,
                track_observation_count: r.track_observation_count,
                bbox_x: r.bbox_x,
                bbox_y: r.bbox_y,
                bbox_width: r.bbox_width,
                bbox_height: r.bbox_height,
                centroid_x: r.centroid_x,
                centroid_y: r.centroid_y,
                area: r.area,
                area_um2: r.area_um2,
                deformability: r.deformability,
                area_ratio: r.area_ratio,
                ring_ratio: r.ring_ratio,
                laplacian_variance: finite(r.laplacian_variance),
                youngs_modulus: r.youngs_modulus,
                brightness_q1: r.brightness_q1,
                brightness_q2: r.brightness_q2,
                brightness_q3: r.brightness_q3,
                brightness_q4: r.brightness_q4,
            })
            .collect(),
    })
}

/// One review image (pull kind 3): `dataset` is a contract
/// `review_image_datasets` value, `overlay` an `overlay_modes` value. The
/// packet is Mono8 or RGB8 (contract `review_pixel_formats`).
#[tauri::command]
pub fn fetch_review_frame(
    state: State<AppState>,
    dataset: u32,
    index: String,
    overlay: u32,
    roi_overlay: bool,
) -> Result<Response, String> {
    let index = parse_u64(&index)?;
    let frame = {
        let mut guard = state.review.lock().map_err(|e| e.to_string())?;
        guard.pin_mut().fetch_review_frame(dataset, index, overlay, roi_overlay)
    };
    frame_packet::encode(frame, 3).map(Response::new)
}

#[tauri::command]
pub fn fetch_review_series_count(state: State<AppState>, index: String) -> Result<String, String> {
    let index = parse_u64(&index)?;
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().fetch_review_series_count(index).to_string())
}

/// Series image `k` of valid frame `index` (pull kind 6).
#[tauri::command]
pub fn fetch_review_series_packet(
    state: State<AppState>,
    index: String,
    k: String,
    overlay: u32,
    roi_overlay: bool,
) -> Result<Response, String> {
    let index = parse_u64(&index)?;
    let k = parse_u64(&k)?;
    let frame = {
        let mut guard = state.review.lock().map_err(|e| e.to_string())?;
        guard.pin_mut().fetch_review_series_frame(index, k, overlay, roi_overlay)
    };
    frame_packet::encode(frame, 6).map(Response::new)
}

/// A thumbnail page (pull kind 5): one frame of width `size` and height
/// `size × count`; `frame_index` carries the page offset.
#[tauri::command]
pub fn fetch_review_thumbnails_packet(
    state: State<AppState>,
    valid: bool,
    offset: String,
    count: u32,
    size: u32,
    overlay: u32,
    roi_overlay: bool,
) -> Result<Response, String> {
    let offset = parse_u64(&offset)?;
    let frame = {
        let mut guard = state.review.lock().map_err(|e| e.to_string())?;
        guard
            .pin_mut()
            .fetch_review_thumbnails(valid, offset, u64::from(count), size, overlay, roi_overlay)
    };
    frame_packet::encode(frame, 5).map(Response::new)
}

#[tauri::command]
pub fn fetch_review_scatter(state: State<AppState>) -> Result<ReviewScatter, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    let s = guard.pin_mut().fetch_review_scatter();
    Ok(ReviewScatter {
        transport_version: frame_packet::JSON_TRANSPORT_VERSION,
        valid: s.valid,
        pixel_to_micron: s.pixel_to_micron,
        frame_index: s.frame_index.iter().map(|v| v.to_string()).collect(),
        valid_position: s.valid_position.iter().map(|v| v.to_string()).collect(),
        area_um2: s.area_um2,
        deformability: s.deformability,
        target_group: s.target_group,
        ring_ratio: s.ring_ratio,
    })
}

#[tauri::command]
pub fn review_save_core_record(
    state: State<AppState>,
    json: String,
    overwrite: bool,
) -> Result<ReviewCmdResult, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_save_core_record(&json, overwrite).into())
}

#[derive(Serialize, Clone, Default)]
pub struct ReviewDensity {
    transport_version: u32,
    valid: bool,
    ready: bool,
    levels: Vec<u8>,
    level_count: u32,
    bandwidth_factor: f64,
    core_fraction: f64,
    computed_record_json: String,
}

/// Chart snapshots the shell rendered (PNG bytes), staged one per raw IPC
/// call (`review_stage_chart`, no JSON number arrays) and consumed by the
/// next Export All / Export Charts. Same name replaces.
static STAGED_CHARTS: Mutex<Vec<(String, Vec<u8>)>> = Mutex::new(Vec::new());
/// Upper bound per staged snapshot (a 1200 × 1200 PNG is well under 8 MB).
const MAX_CHART_BYTES: usize = 32 * 1024 * 1024;
const MAX_STAGED_CHARTS: usize = 8;

/// The name rule the backend applies (`ReviewJobs::validChartName`): a bare
/// file name ending .tif / .tiff / .png.
pub fn valid_chart_name(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    !name.is_empty()
        && name.len() <= 128
        && !name.starts_with('.')
        && !name.contains(['/', '\\', ':'])
        && [".tiff", ".tif", ".png"].iter().any(|ext| lower.len() > ext.len() && lower.ends_with(ext))
}

fn take_staged_charts() -> Vec<review_ffi::ReviewChartSnapshot> {
    let mut staged = STAGED_CHARTS.lock().unwrap_or_else(|e| e.into_inner());
    std::mem::take(&mut *staged)
        .into_iter()
        .map(|(name, encoded)| review_ffi::ReviewChartSnapshot { name, encoded })
        .collect()
}

/// Stage one chart snapshot: raw body = PNG bytes, header `x-chart-name`.
/// Returns how many are staged.
#[tauri::command]
pub fn review_stage_chart(request: tauri::ipc::Request) -> Result<u32, String> {
    let name = request
        .headers()
        .get("x-chart-name")
        .and_then(|v| v.to_str().ok())
        .ok_or("missing x-chart-name header")?
        .to_string();
    if !valid_chart_name(&name) {
        return Err(format!("invalid chart name: {name}"));
    }
    let tauri::ipc::InvokeBody::Raw(bytes) = request.body() else {
        return Err("chart body must be raw bytes".into());
    };
    if bytes.is_empty() || bytes.len() > MAX_CHART_BYTES {
        return Err(format!("chart {name}: {} bytes is out of range", bytes.len()));
    }
    let mut staged = STAGED_CHARTS.lock().map_err(|e| e.to_string())?;
    staged.retain(|(n, _)| *n != name);
    if staged.len() >= MAX_STAGED_CHARTS {
        return Err("too many staged charts".into());
    }
    staged.push((name, bytes.clone()));
    Ok(staged.len() as u32)
}

#[tauri::command]
pub fn review_clear_charts() {
    STAGED_CHARTS.lock().unwrap_or_else(|e| e.into_inner()).clear();
}

/// File names in `dir` (empty when it is not a readable directory): the
/// shell picks default export names with the backend's `_2`, `_3` rule
/// (`HdfExportService::nextAvailableName`, mirrored in exports/naming.ts).
#[tauri::command]
pub fn review_list_dir(dir: String) -> Vec<String> {
    std::fs::read_dir(&dir)
        .map(|it| it.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect())
        .unwrap_or_default()
}

fn series_range(export_series: bool, start: &str, end: &str) -> Result<(bool, u64, u64), String> {
    let s = parse_u64(start)?;
    let e = if end.is_empty() { u64::MAX } else { parse_u64(end)? };
    Ok((export_series, s, e))
}

#[tauri::command]
pub fn review_export_metrics(state: State<AppState>, output_path: String) -> Result<ReviewCmdResult, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_export_metrics(&output_path).into())
}

#[tauri::command]
pub fn review_export_all(
    state: State<AppState>,
    output_root: String,
    export_series: bool,
    series_start: String,
    series_end: String,
) -> Result<ReviewCmdResult, String> {
    let (es, s, e) = series_range(export_series, &series_start, &series_end)?;
    let snaps = take_staged_charts();
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_export_all(&output_root, es, s, e, snaps).into())
}

/// Export Charts: the staged snapshots into `output_dir` (all or nothing).
#[tauri::command]
pub fn review_export_charts(state: State<AppState>, output_dir: String) -> Result<ReviewCmdResult, String> {
    let snaps = take_staged_charts();
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_export_charts(&output_dir, snaps).into())
}

#[tauri::command]
pub fn review_batch_export(
    state: State<AppState>,
    sources: Vec<String>,
    output_root: String,
    metrics_only: bool,
    export_series: bool,
    series_start: String,
    series_end: String,
) -> Result<ReviewCmdResult, String> {
    let (es, s, e) = series_range(export_series, &series_start, &series_end)?;
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_batch_export(sources, &output_root, metrics_only, es, s, e).into())
}

#[tauri::command]
pub fn review_regenerate_masks(
    state: State<AppState>,
    source: u32,
    source_path: String,
    start_index: String,
    count: String,
    output_path: String,
    use_recorded_config: bool,
    synthesize_background: bool,
) -> Result<ReviewCmdResult, String> {
    let start = parse_u64(&start_index)?;
    let n = parse_u64(&count)?;
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard
        .pin_mut()
        .review_regenerate_masks(source, &source_path, start, n, &output_path, use_recorded_config, synthesize_background)
        .into())
}

#[tauri::command]
pub fn review_compute_core(state: State<AppState>, core_fraction: f64) -> Result<ReviewCmdResult, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().review_compute_core(core_fraction).into())
}

#[tauri::command]
pub fn fetch_review_computed_core_json(state: State<AppState>) -> Result<String, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().fetch_review_computed_core_json())
}

#[tauri::command]
pub fn review_request_density(
    state: State<AppState>,
    bandwidth_factor: f64,
    core_fraction: f64,
    levels: u32,
    want_core_record: bool,
) -> Result<ReviewCmdResult, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard
        .pin_mut()
        .review_request_density(bandwidth_factor, core_fraction, levels, want_core_record)
        .into())
}

#[tauri::command]
pub fn fetch_review_density(state: State<AppState>) -> Result<ReviewDensity, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    let d = guard.pin_mut().fetch_review_density();
    Ok(ReviewDensity {
        transport_version: frame_packet::JSON_TRANSPORT_VERSION,
        valid: d.valid,
        ready: d.ready,
        levels: d.levels,
        level_count: d.level_count,
        bandwidth_factor: d.bandwidth_factor,
        core_fraction: d.core_fraction,
        computed_record_json: d.computed_record_json,
    })
}

#[tauri::command]
pub fn review_jobs_busy(state: State<AppState>) -> Result<bool, String> {
    let guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.review_jobs_busy())
}

#[tauri::command]
pub fn poll_review_events(state: State<AppState>) -> Result<ReviewEvents, String> {
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    let events = guard.pin_mut().poll_review_events();
    Ok(ReviewEvents {
        transport_version: frame_packet::JSON_TRANSPORT_VERSION,
        events: events
            .into_iter()
            .map(|e| ReviewEvent {
                operation_id: e.operation_id,
                kind: e.kind,
                state: e.state,
                progress: e.progress,
                total: e.total,
                message: e.message,
            })
            .collect(),
    })
}

#[tauri::command]
pub fn cancel_review_operation(state: State<AppState>, operation_id: String) -> Result<ReviewCmdResult, String> {
    let id = parse_u64(&operation_id)?;
    let mut guard = state.review.lock().map_err(|e| e.to_string())?;
    Ok(guard.pin_mut().cancel_review_operation(id).into())
}

#[cfg(test)]
mod tests {
    //! Headless: the desktop crate links the review bridge and reads the
    //! C++-written fixture through it in both feature configurations.
    use mib_bridge::review_ffi;
    use serial_test::serial;

    #[test]
    #[serial]
    fn pending_open_request_is_taken_once_and_wins_over_argv() {
        assert_eq!(super::review_take_open_request(), "");
        super::set_pending_open("/data/finder.h5".into());
        assert_eq!(super::review_launch_path(), "/data/finder.h5");
        assert_eq!(super::review_take_open_request(), "", "taken by the launch read");
        super::set_pending_open("/data/second.h5".into());
        assert_eq!(super::review_take_open_request(), "/data/second.h5");
        assert_eq!(super::review_take_open_request(), "");
    }

    #[test]
    fn chart_names_follow_the_backend_rule() {
        for ok in ["scatter_plot.tiff", "ring_width_histogram.tiff", "a.TIF", "chart.png"] {
            assert!(super::valid_chart_name(ok), "{ok}");
        }
        for bad in ["", ".hidden.tiff", "../x.tiff", "a/b.tiff", "a\\b.tiff", "c:x.tiff", "x.csv", ".tiff"] {
            assert!(!super::valid_chart_name(bad), "{bad}");
        }
    }

    #[test]
    fn launch_path_picks_the_first_existing_hdf5_argument() {
        let path = std::env::temp_dir().join(format!("mib_launch_{}.H5", std::process::id()));
        std::fs::write(&path, b"x").unwrap();
        let p = path.to_string_lossy().into_owned();
        let args = vec!["--flag".to_string(), "missing.h5".to_string(), p.clone(), "other.h5".to_string()];
        assert_eq!(super::launch_path_from(args.into_iter()), p);
        assert_eq!(super::launch_path_from(vec!["notes.txt".to_string()].into_iter()), "");
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    #[serial]
    fn review_bridge_round_trip() {
        let path = std::env::temp_dir().join(format!("mib_desktop_review_{}.h5", std::process::id()));
        let _ = std::fs::remove_file(&path);
        assert!(review_ffi::review_fixture_write_experiment(&path.to_string_lossy()));
        let mut bridge = review_ffi::new_review_bridge();
        assert!(bridge.pin_mut().initialize(""));
        assert!(bridge.pin_mut().review_open(&path.to_string_lossy()).ok);
        let info = bridge.pin_mut().fetch_review_info();
        assert!(info.file_open && info.total_valid == 10);
        let frame = bridge.pin_mut().fetch_review_frame(0, 0, 1, true);
        assert!(frame.valid);
        let packet = crate::frame_packet::encode(frame, 3).unwrap();
        assert_eq!(u64::from_le_bytes(packet[48..56].try_into().unwrap()), crate::frame_packet::PIXEL_FORMAT_RGB8);
        assert!(bridge.pin_mut().review_close().ok);
        let _ = std::fs::remove_file(&path);
    }
}
