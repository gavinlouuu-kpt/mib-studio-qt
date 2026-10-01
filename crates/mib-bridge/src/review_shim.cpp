// C++ side of the review bridge (ADR 0008). See review_shim.h.
#include "mib-bridge/src/review_shim.h"

// cxx-generated definitions of the shared structs.
#include "mib-bridge/src/review_bridge.rs.h"

#include "backend/processing/KdeCoreRecord.h"
#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"
#include "backend/processing/ProcessingService.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace mib_review_bridge {

namespace {

namespace br = backend::review;

// Contract pins (bridge-contract.json): review_image_datasets, overlay_modes.
static_assert(static_cast<std::uint32_t>(br::ReviewDataset::ValidImage) == 0);
static_assert(static_cast<std::uint32_t>(br::ReviewDataset::InvalidImage) == 1);
static_assert(static_cast<std::uint32_t>(br::ReviewDataset::RecordedImage) == 2);
static_assert(static_cast<std::uint32_t>(br::ReviewDataset::ValidMask) == 3);
static_assert(static_cast<std::uint32_t>(br::ReviewDataset::InvalidMask) == 4);
static_assert(static_cast<std::uint32_t>(br::OverlayMode::None) == 0);
static_assert(static_cast<std::uint32_t>(br::OverlayMode::AllContour) == 1);
static_assert(static_cast<std::uint32_t>(br::OverlayMode::OuterInnerColorCoded) == 2);
static_assert(static_cast<std::uint32_t>(br::OverlayMode::AllMask) == 3);
static_assert(static_cast<std::uint32_t>(br::OverlayMode::FilteredMask) == 4);
static_assert(static_cast<std::uint32_t>(br::ReviewJobKind::ExportMetrics) == 0);
static_assert(static_cast<std::uint32_t>(br::ReviewJobKind::Density) == 5);
static_assert(static_cast<std::uint32_t>(br::ReviewJobState::Started) == 0);
static_assert(static_cast<std::uint32_t>(br::ReviewJobState::TimedOut) == 5);
static_assert(static_cast<std::uint32_t>(br::RegenerateSource::CurrentValid) == 0);
static_assert(static_cast<std::uint32_t>(br::RegenerateSource::Folder) == 4);
static_assert(static_cast<std::uint32_t>(backend::recording::RunCompletionState::Complete) == 0);
static_assert(static_cast<std::uint32_t>(backend::recording::RunCompletionState::Unknown) == 4);

// Contract review_pixel_formats.
constexpr std::uint64_t kPixelFormatMono8 = 0;
constexpr std::uint64_t kPixelFormatRgb8 = 0x02180014; // GenICam PFNC RGB8

constexpr std::uint32_t kMaxDataset = 4;
constexpr std::uint32_t kMaxOverlay = 4;

std::string toStd(rust::Str s) { return std::string(s.data(), s.size()); }

ReviewResult ok(const std::string& message, std::uint64_t operationId = 0) {
    ReviewResult r{};
    r.ok = true;
    r.message = rust::String(message);
    r.operation_id = operationId;
    return r;
}

ReviewResult fail(const std::string& message) {
    ReviewResult r{};
    r.ok = false;
    r.message = rust::String(message);
    return r;
}

ReviewFrame toFrame(const br::ReviewImage& image) {
    ReviewFrame out{};
    if (image.data.empty() || image.width == 0 || image.height == 0) return out;
    out.valid = true;
    out.frame_index = image.frameIndex;
    out.width = image.width;
    out.height = image.height;
    out.pixel_format = image.channels == 3 ? kPixelFormatRgb8 : kPixelFormatMono8;
    out.stride_bytes = image.width * image.channels;
    out.data.reserve(image.data.size());
    for (std::uint8_t b : image.data) out.data.push_back(b);
    return out;
}

ReviewDatasetInfo toInfo(const br::DatasetInfo& in) {
    ReviewDatasetInfo out{};
    out.present = in.present;
    out.count = in.count;
    out.height = in.height;
    out.width = in.width;
    out.channels = in.channels;
    return out;
}

} // namespace

constexpr std::size_t kEventQueueCapacity = 4096;

struct ReviewBridge::Impl {
    br::ReviewSession session;
    // The bundled kernel for regenerate masks (ProcessingService owns no
    // threads until start(), which review never calls).
    backend::services::ProcessingService processing;
    bool initialized{false};
    std::mutex eventMutex;
    std::deque<ReviewEvent> events;
    std::unique_ptr<br::ReviewJobs> jobs;

    Impl() {
        jobs = std::make_unique<br::ReviewJobs>(session, &processing, [this](const br::ReviewJobEvent& e) {
            // Runs on the job thread: convert + enqueue, bounded drop-oldest.
            ReviewEvent out{};
            out.operation_id = e.operationId;
            out.kind = static_cast<std::uint32_t>(e.kind);
            out.state = static_cast<std::uint32_t>(e.state);
            out.progress = e.progress;
            out.total = e.total;
            out.message = rust::String(e.message);
            std::scoped_lock lock(eventMutex);
            events.push_back(std::move(out));
            while (events.size() > kEventQueueCapacity) events.pop_front();
        });
    }

    ReviewResult started(std::uint64_t id, const std::string& error, const char* what) {
        if (id == 0) return fail(error.empty() ? std::string(what) + " refused" : error);
        return ok(std::string(what) + " started", id);
    }
};

ReviewBridge::ReviewBridge() : impl_(std::make_unique<Impl>()) {}

ReviewBridge::~ReviewBridge() {
    try {
        impl_->jobs->shutdown();
        impl_->session.close();
    } catch (...) {
    }
}

bool ReviewBridge::initialize(rust::Str data_dir) {
    (void)data_dir; // file logging arrives with the jobs (PR 1b)
    impl_->initialized = true;
    return true;
}

void ReviewBridge::shutdown() {
    try {
        impl_->jobs->shutdown();
        impl_->session.close();
    } catch (...) {
    }
    impl_->initialized = false;
}

bool ReviewBridge::is_initialized() const { return impl_->initialized; }

ReviewResult ReviewBridge::review_open(rust::Str path) {
    try {
        std::string error;
        if (!impl_->session.open(toStd(path), &error)) return fail(error);
        return ok("Review file opened");
    } catch (const std::exception& e) {
        return fail(std::string("review_open: ") + e.what());
    } catch (...) {
        return fail("review_open: unknown error");
    }
}

ReviewResult ReviewBridge::review_close() {
    try {
        impl_->session.close();
        return ok("Review file closed");
    } catch (...) {
        return fail("review_close: unknown error");
    }
}

void ReviewBridge::set_fallback_pixel_to_micron(double factor) {
    impl_->session.setFallbackPixelToMicron(factor);
}

ReviewInfo ReviewBridge::fetch_review_info() {
    ReviewInfo out{};
    try {
        const br::ReviewMetadata m = impl_->session.metadata();
        out.valid = true;
        out.file_open = m.fileOpen;
        if (!m.fileOpen) return out;
        out.file_path = rust::String(m.filePath);
        out.recording_file = m.recordingFile;
        out.start_time_ns = m.startTimeNs;
        out.end_time_ns = m.endTimeNs;
        out.total_valid = m.totalValid;
        out.total_invalid = m.totalInvalid;
        out.filtered_frames = m.filteredFrames;
        out.roi_x = m.roi.x;
        out.roi_y = m.roi.y;
        out.roi_w = m.roi.w;
        out.roi_h = m.roi.h;
        out.has_background = m.hasBackground;
        out.has_core_identity = m.hasCoreIdentity;
        out.core_version = rust::String(m.coreVersion);
        out.core_source = rust::String(m.coreSource);
        out.core_release_tag = rust::String(m.coreReleaseTag);
        out.valid_images = toInfo(m.validImages);
        out.invalid_images = toInfo(m.invalidImages);
        out.valid_masks = toInfo(m.validMasks);
        out.invalid_masks = toInfo(m.invalidMasks);
        out.recorded_images = toInfo(m.recordedImages);
        out.has_series = m.hasSeries;
        out.series_count = m.seriesCount;
        out.multi_image_enabled = m.multiImageEnabled;
        out.multi_image_count = m.multiImageCount;
        out.has_accounting = m.hasAccounting;
        if (m.hasAccounting) {
            const auto& a = m.accounting;
            out.accounting_completion = static_cast<std::uint32_t>(a.completion);
            out.accounting_reconciled = a.reconciled;
            out.accounting_empty = a.empty;
            out.accounting_rejected = a.scientificallyRejected;
            out.accounting_processing_failed = a.processingFailed;
            out.accounting_store_loss = a.storeOverwritten + a.storeNotCommitted + a.storeMalformed;
            out.accounting_persisted = a.persistenceCommitted;
            out.accounting_admitted = a.persistenceAdmitted;
            out.accounting_persistence_failed = a.persistenceFailed;
        } else {
            out.accounting_completion =
                static_cast<std::uint32_t>(backend::recording::RunCompletionState::Unknown);
        }
        out.accounting_summary = rust::String(impl_->session.accountingSummary());
        out.pixel_to_micron = m.pixelToMicron;
        out.pixel_to_micron_from_file = m.pixelToMicronFromFile;
        out.kde_analysis_json = rust::String(m.kdeAnalysisJson);
        out.kde_live_json = rust::String(m.kdeLiveJson);
    } catch (...) {
        out.valid = false;
    }
    return out;
}

ReviewRows ReviewBridge::fetch_review_rows(bool valid, std::uint64_t offset, std::uint64_t count) {
    ReviewRows out{};
    try {
        std::vector<br::MetricRow> rows;
        std::uint64_t total = 0;
        if (!impl_->session.metricsPage(valid, offset, std::min<std::uint64_t>(count, 10000), rows, total)) {
            return out;
        }
        out.valid = true;
        out.total = total;
        out.offset = offset;
        out.rows.reserve(rows.size());
        for (const auto& r : rows) {
            ReviewRow row{};
            row.frame_index = r.frameIndex;
            row.timestamp_ns = r.timestampNs;
            row.valid = r.valid;
            row.target_group = r.targetGroup;
            row.touches_border = r.touchesBorder;
            row.has_single_inner_contour = r.hasSingleInnerContour;
            row.in_range = r.inRange;
            row.in_channel = r.inChannel;
            row.inner_contour_count = r.innerContourCount;
            row.object_id = r.objectId;
            row.object_count = r.objectCount;
            row.track_id = r.trackId;
            row.track_first_frame = r.trackFirstFrame;
            row.track_last_frame = r.trackLastFrame;
            row.track_observation_count = r.trackObservationCount;
            row.bbox_x = r.bboxX;
            row.bbox_y = r.bboxY;
            row.bbox_width = r.bboxWidth;
            row.bbox_height = r.bboxHeight;
            row.centroid_x = r.centroidX;
            row.centroid_y = r.centroidY;
            row.area = r.area;
            row.area_um2 = r.areaUm2;
            row.deformability = r.deformability;
            row.area_ratio = r.areaRatio;
            row.ring_ratio = r.ringRatio;
            row.laplacian_variance = r.laplacianVariance;
            row.youngs_modulus = r.youngsModulus;
            row.brightness_q1 = r.brightnessQ1;
            row.brightness_q2 = r.brightnessQ2;
            row.brightness_q3 = r.brightnessQ3;
            row.brightness_q4 = r.brightnessQ4;
            out.rows.push_back(std::move(row));
        }
    } catch (...) {
        out.valid = false;
        out.rows.clear();
    }
    return out;
}

ReviewFrame ReviewBridge::fetch_review_frame(std::uint32_t dataset, std::uint64_t index,
                                             std::uint32_t overlay, bool roi_overlay) {
    if (dataset > kMaxDataset || overlay > kMaxOverlay) return ReviewFrame{};
    try {
        br::ReviewImage image;
        if (!impl_->session.fetchImage(static_cast<br::ReviewDataset>(dataset), index,
                                       static_cast<br::OverlayMode>(overlay), roi_overlay, image)) {
            return ReviewFrame{};
        }
        return toFrame(image);
    } catch (...) {
        return ReviewFrame{};
    }
}

std::uint64_t ReviewBridge::fetch_review_series_count(std::uint64_t index) {
    try {
        std::uint64_t count = 0;
        if (!impl_->session.seriesInfo(index, count)) return 0;
        return count;
    } catch (...) {
        return 0;
    }
}

ReviewFrame ReviewBridge::fetch_review_series_frame(std::uint64_t index, std::uint64_t k,
                                                    std::uint32_t overlay, bool roi_overlay) {
    if (overlay > kMaxOverlay) return ReviewFrame{};
    try {
        br::ReviewImage image;
        if (!impl_->session.fetchSeriesImage(index, k, static_cast<br::OverlayMode>(overlay), roi_overlay,
                                             image)) {
            return ReviewFrame{};
        }
        return toFrame(image);
    } catch (...) {
        return ReviewFrame{};
    }
}

ReviewFrame ReviewBridge::fetch_review_thumbnails(bool valid, std::uint64_t offset, std::uint64_t count,
                                                  std::uint32_t size, std::uint32_t overlay,
                                                  bool roi_overlay) {
    // One strip is one frame packet: size × (size·count) must stay within the
    // contract's max_dimension (8192).
    constexpr std::uint64_t kMaxDimension = 8192;
    if (overlay > kMaxOverlay || size == 0 || size > 512 || count == 0 ||
        static_cast<std::uint64_t>(size) * count > kMaxDimension) {
        return ReviewFrame{};
    }
    try {
        br::ThumbnailStrip strip;
        if (!impl_->session.thumbnails(valid, offset, count, size, static_cast<br::OverlayMode>(overlay),
                                       roi_overlay, strip) ||
            !strip.valid || strip.count == 0) {
            return ReviewFrame{};
        }
        ReviewFrame out{};
        out.valid = true;
        out.frame_index = strip.offset;
        out.width = strip.size;
        out.height = static_cast<std::uint64_t>(strip.size) * strip.count;
        out.pixel_format = strip.channels == 3 ? kPixelFormatRgb8 : kPixelFormatMono8;
        out.stride_bytes = static_cast<std::uint64_t>(strip.size) * strip.channels;
        out.data.reserve(strip.data.size());
        for (std::uint8_t b : strip.data) out.data.push_back(b);
        return out;
    } catch (...) {
        return ReviewFrame{};
    }
}

ReviewScatter ReviewBridge::fetch_review_scatter() {
    ReviewScatter out{};
    try {
        if (!impl_->session.isOpen()) return out;
        const br::ScatterData s = impl_->session.scatter();
        out.valid = true;
        out.pixel_to_micron = s.pixelToMicron;
        for (auto v : s.frameIndex) out.frame_index.push_back(v);
        for (auto v : s.validPosition) out.valid_position.push_back(v);
        for (auto v : s.areaUm2) out.area_um2.push_back(v);
        for (auto v : s.deformability) out.deformability.push_back(v);
        for (auto v : s.targetGroup) out.target_group.push_back(v);
    } catch (...) {
        out = ReviewScatter{};
    }
    return out;
}

ReviewResult ReviewBridge::review_save_core_record(rust::Str json, bool overwrite) {
    try {
        std::string error;
        if (!impl_->session.saveCoreRecordJson(toStd(json), overwrite, &error)) return fail(error);
        return ok("Core record saved");
    } catch (const std::exception& e) {
        return fail(std::string("review_save_core_record: ") + e.what());
    } catch (...) {
        return fail("review_save_core_record: unknown error");
    }
}

rust::Vec<ReviewEvent> ReviewBridge::poll_review_events() {
    rust::Vec<ReviewEvent> out;
    std::scoped_lock lock(impl_->eventMutex);
    while (!impl_->events.empty()) {
        out.push_back(std::move(impl_->events.front()));
        impl_->events.pop_front();
    }
    return out;
}

ReviewResult ReviewBridge::cancel_review_operation(std::uint64_t operation_id) {
    if (impl_->jobs->cancel(operation_id)) return ok("Cancellation requested", operation_id);
    return fail("No such review operation");
}

ReviewResult ReviewBridge::review_export_metrics(rust::Str output_path) {
    try {
        std::string error;
        return impl_->started(impl_->jobs->startExportMetrics(toStd(output_path), &error), error, "Metrics export");
    } catch (const std::exception& e) {
        return fail(std::string("review_export_metrics: ") + e.what());
    }
}

ReviewResult ReviewBridge::review_export_all(rust::Str output_root, bool export_series, std::uint64_t series_start,
                                             std::uint64_t series_end, rust::Vec<ReviewChartSnapshot> charts) {
    try {
        br::ExportAllRequest req;
        req.outputRoot = toStd(output_root);
        req.series.exportSeries = export_series;
        req.series.startInclusive = series_start;
        req.series.endInclusive = series_end;
        for (const auto& c : charts) {
            br::ChartSnapshot snap;
            snap.name = std::string(c.name);
            snap.encoded.assign(c.encoded.begin(), c.encoded.end());
            req.charts.push_back(std::move(snap));
        }
        std::string error;
        return impl_->started(impl_->jobs->startExportAll(req, &error), error, "Export All");
    } catch (const std::exception& e) {
        return fail(std::string("review_export_all: ") + e.what());
    }
}

ReviewResult ReviewBridge::review_batch_export(rust::Vec<rust::String> sources, rust::Str output_root,
                                               bool metrics_only, bool export_series, std::uint64_t series_start,
                                               std::uint64_t series_end) {
    try {
        br::BatchExportRequest req;
        for (const auto& s : sources) req.sources.push_back(std::string(s));
        req.outputRoot = toStd(output_root);
        req.metricsOnly = metrics_only;
        req.series.exportSeries = export_series;
        req.series.startInclusive = series_start;
        req.series.endInclusive = series_end;
        std::string error;
        return impl_->started(impl_->jobs->startBatchExport(req, &error), error, "Batch export");
    } catch (const std::exception& e) {
        return fail(std::string("review_batch_export: ") + e.what());
    }
}

ReviewResult ReviewBridge::review_regenerate_masks(std::uint32_t source, rust::Str source_path,
                                                   std::uint64_t start_index, std::uint64_t count,
                                                   rust::Str output_path, bool use_recorded_config,
                                                   bool synthesize_background) {
    if (source > static_cast<std::uint32_t>(br::RegenerateSource::Folder)) return fail("Unknown regenerate source");
    try {
        br::RegenerateMasksRequest req;
        req.source = static_cast<br::RegenerateSource>(source);
        req.sourcePath = toStd(source_path);
        req.startIndex = start_index;
        req.count = count;
        req.outputPath = toStd(output_path);
        req.useRecordedConfig = use_recorded_config;
        req.synthesizeBackground = synthesize_background;
        std::string error;
        return impl_->started(impl_->jobs->startRegenerateMasks(req, &error), error, "Regenerate masks");
    } catch (const std::exception& e) {
        return fail(std::string("review_regenerate_masks: ") + e.what());
    }
}

ReviewResult ReviewBridge::review_compute_core(double core_fraction) {
    try {
        std::string error;
        return impl_->started(impl_->jobs->startComputeCore(core_fraction, &error), error, "Core contour");
    } catch (const std::exception& e) {
        return fail(std::string("review_compute_core: ") + e.what());
    }
}

rust::String ReviewBridge::fetch_review_computed_core_json() {
    return rust::String(impl_->jobs->computedCoreJson());
}

ReviewResult ReviewBridge::review_request_density(double bandwidth_factor, double core_fraction,
                                                  std::uint32_t levels, bool want_core_record) {
    try {
        br::DensityRequest req;
        req.bandwidthFactor = bandwidth_factor;
        req.coreFraction = core_fraction;
        req.levels = static_cast<int>(std::min<std::uint32_t>(levels, 64));
        req.wantCoreRecord = want_core_record;
        std::string error;
        return impl_->started(impl_->jobs->startDensity(req, &error), error, "Density estimate");
    } catch (const std::exception& e) {
        return fail(std::string("review_request_density: ") + e.what());
    }
}

ReviewDensity ReviewBridge::fetch_review_density() {
    ReviewDensity out{};
    try {
        const br::DensityResult d = impl_->jobs->density();
        out.valid = true;
        out.ready = d.ready;
        out.level_count = static_cast<std::uint32_t>(d.levelCount);
        out.bandwidth_factor = d.bandwidthFactor;
        out.core_fraction = d.coreFraction;
        out.computed_record_json = rust::String(d.computedRecordJson);
        out.levels.reserve(d.levels.size());
        for (auto l : d.levels) out.levels.push_back(l);
    } catch (...) {
        out = ReviewDensity{};
    }
    return out;
}

bool ReviewBridge::review_jobs_busy() const { return impl_->jobs->busy(); }

std::unique_ptr<ReviewBridge> new_review_bridge() { return std::make_unique<ReviewBridge>(); }

// Shared with bridge_abi_version() (one contract document). v15 adds the
// review bridge: ReviewSession-backed info/rows/frames/series/thumbnails/
// scatter/core-record calls, overlay_modes, review_pixel_formats and the
// review event queue.
std::uint32_t review_bridge_abi_version() { return 15; }

bool review_fixture_write_experiment(rust::Str path) {
    try {
        using backend::services::Hdf5Service;
        using backend::services::ProcessedFrame;
        constexpr int kH = 24, kW = 32;
        auto pattern = [&](std::uint64_t index, int offset) {
            cv::Mat m(kH, kW, CV_8UC1);
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x)
                    m.at<std::uint8_t>(y, x) =
                        static_cast<std::uint8_t>((x * 3 + y * 5 + static_cast<int>(index) * 7 + offset) % 200);
            return m;
        };
        cv::Mat ring = cv::Mat::zeros(kH, kW, CV_8UC1);
        cv::circle(ring, cv::Point(kW / 2, kH / 2), 9, cv::Scalar(255), -1);
        cv::circle(ring, cv::Point(kW / 2, kH / 2), 4, cv::Scalar(0), -1);
        auto make = [&](std::uint64_t idx, bool valid, bool passes, bool series) {
            ProcessedFrame f;
            f.index = idx;
            f.timestampNs = (idx + 1) * 1000ULL;
            f.originalImage = pattern(idx, valid ? 0 : 50);
            f.processedImage = ring.clone();
            f.validation.isValid = passes;
            f.validation.isTargetGroup = valid && (idx % 4 == 0);
            f.validation.hasSingleInnerContour = true;
            f.validation.objectId = static_cast<int>(idx);
            f.validation.objectCount = 1;
            f.validation.trackId = static_cast<int>(idx / 2);
            f.validation.area = 100.0 + static_cast<double>(idx);
            f.validation.deformability = 0.1 + 0.01 * static_cast<double>(idx);
            f.validation.ringRatio = 0.5;
            f.validation.brightness.q1 = 1.5;
            f.validation.brightness.q4 = 4.5;
            if (series)
                for (int s = 0; s < 3; ++s) f.seriesImages.push_back(pattern(idx, 100 + s * 17));
            return f;
        };
        std::vector<ProcessedFrame> valid, invalid;
        for (int i = 0; i < 10; ++i) valid.push_back(make(static_cast<std::uint64_t>(i * 2), true, i != 3 && i != 7, true));
        for (int i = 0; i < 4; ++i) invalid.push_back(make(static_cast<std::uint64_t>(i * 2 + 1), false, false, false));
        Hdf5Service hdf5;
        if (!hdf5.openFile(toStd(path))) return false;
        if (!hdf5.initializeDatasets()) return false;
        if (!hdf5.appendFrames(valid, invalid)) return false;
        backend::services::ProcessingConfig cfg;
        backend::services::ProcessingService::Roi roi{4, 2, 20, 16};
        if (!hdf5.writeExperimentInfo(1000, 5000, valid.size(), invalid.size(), cfg, roi)) return false;
        if (!hdf5.writeRunSnapshotJson("{\"schema\":1,\"pixel_to_micron\":0.25,\"profile_id\":\"fixture\"}", "{}")) return false;
        backend::recording::RecordingAccountingSnapshot a;
        a.admitted = 14;
        a.processed = 10;
        a.scientificallyRejected = 4;
        a.persistenceAdmitted = 14;
        a.persistenceCommitted = 14;
        a.completion = backend::recording::RunCompletionState::Complete;
        a.reconciled = true;
        if (!hdf5.writeRunAccounting(a)) return false;
        backend::monitoring::KdeCoreRecord r;
        r.provisional = false;
        r.source = "full-run";
        r.cellCount = 8;
        r.pixelToMicron = 0.25;
        r.contours.push_back({{1, 0.1}, {2, 0.2}, {1, 0.3}});
        if (!hdf5.writeKdeAnalysisJson(backend::monitoring::toJson(r))) return false;
        hdf5.closeFile();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace mib_review_bridge
