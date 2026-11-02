#include "backend/analysis/EvidenceGallery.h"
#include "backend/analysis/AnalysisHost.h"
#include "backend/database/SqliteService.h"
// C++ side of the review bridge (ADR 0014). See review_shim.h.
#include "mib-bridge/src/review_shim.h"

// cxx-generated definitions of the shared structs.
#include "mib-bridge/src/review_bridge.rs.h"

#include "backend/processing/KdeCoreRecord.h"
#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"
#include "backend/review/RunStatistics.h"
#include "backend/review/RunStatisticsFixture.h"
#include "backend/processing/ProcessingService.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <random>
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
// review-contract.json review_track_rows / review_reprocess_record (#717 B3b).
static_assert(br::kReviewTrackRowsMax == 256);
static_assert(br::kReviewReprocessMaxObjects == 64);
static_assert(static_cast<std::uint32_t>(br::ReprocessSettings::Recorded) == 0);
static_assert(static_cast<std::uint32_t>(br::ReprocessSettings::Default) == 1);
static_assert(static_cast<std::uint32_t>(br::ReviewJobKind::ExportMetrics) == 0);
static_assert(static_cast<std::uint32_t>(br::ReviewJobKind::Density) == 5);
static_assert(static_cast<std::uint32_t>(br::ReviewJobKind::ExportCharts) == 6);
static_assert(static_cast<std::uint32_t>(br::ReviewJobState::Started) == 0);
static_assert(static_cast<std::uint32_t>(br::ReviewJobState::TimedOut) == 5);
static_assert(static_cast<std::uint32_t>(br::RegenerateSource::CurrentValid) == 0);
static_assert(static_cast<std::uint32_t>(br::RegenerateSource::Folder) == 4);
static_assert(static_cast<std::uint32_t>(backend::recording::RunCompletionState::Complete) == 0);
static_assert(static_cast<std::uint32_t>(backend::recording::RunCompletionState::Unknown) == 4);
// review-contract.json review_probe.max_paths (review ABI 2, #717 B1a).
static_assert(br::kReviewProbeMaxPaths == 256);
// review-contract.json review_run_statistics (review ABI 6, #716 A7a).
static_assert(br::kRunQuickLookMaxPaths == 32);
static_assert(br::kRunQuickLookPathMaxBytes == 4096);
static_assert(static_cast<std::uint32_t>(br::RunStatisticsFixture::RawRecording) == 5);

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

// Optional text -> presence flag + value; an absent value stays empty and
// means nothing (the flag says so), never a stand-in.
void setText(const std::optional<std::string>& in, bool& has, rust::String& out) {
    has = in.has_value();
    out = has ? rust::String(*in) : rust::String();
}

ReviewRow toRow(const br::MetricRow& r) {
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
    return row;
}

ReviewAnnotation toAnnotation(const br::RunIdentity& id) {
    ReviewAnnotation out{};
    out.unreadable = id.annotationUnreadable;
    if (!id.annotation) return out;
    const auto& a = *id.annotation;
    out.present = true;
    out.has_schema_version = id.annotationSchemaVersion.has_value();
    out.schema_version = id.annotationSchemaVersion.value_or(0);
    setText(a.sample, out.has_sample, out.sample);
    setText(a.condition, out.has_condition, out.condition);
    setText(a.experimentalUnit, out.has_experimental_unit, out.experimental_unit);
    out.has_concentration = a.concentration.has_value();
    if (a.concentration) {
        out.concentration_value = a.concentration->value;
        out.concentration_unit = rust::String(a.concentration->unit);
    }
    setText(a.channel, out.has_channel, out.channel);
    setText(a.medium, out.has_medium, out.medium);
    out.has_temperature_c = a.temperatureC.has_value();
    out.temperature_c = a.temperatureC.value_or(0.0);
    setText(a.notes, out.has_notes, out.notes);
    for (const auto& [key, value] : a.free) {
        ReviewKeyValue kv{};
        kv.key = rust::String(key);
        kv.value = rust::String(value);
        out.free.push_back(std::move(kv));
    }
    return out;
}

ReviewRunIdentity toIdentity(const br::RunIdentity& id) {
    ReviewRunIdentity out{};
    out.has_snapshot = id.hasSnapshot;
    out.snapshot_unreadable = id.snapshotUnreadable;
    out.snapshot_version = id.hasSnapshot ? id.snapshotVersion : 0;
    setText(id.runId, out.has_run_id, out.run_id);
    const auto& d = id.declared;
    setText(d.experimentId, out.has_experiment_id, out.experiment_id);
    out.has_run_index = d.runIndex.has_value();
    out.run_index = d.runIndex.value_or(0);
    setText(d.sampleRef, out.has_sample_ref, out.sample_ref);
    setText(d.calibrationRecordId, out.has_calibration_record_id, out.calibration_record_id);
    if (d.selectedOperator) {
        setText(d.selectedOperator->id, out.has_operator_id, out.operator_id);
        setText(d.selectedOperator->name, out.has_operator_name, out.operator_name);
    }
    out.annotation = toAnnotation(id);
    out.has_emodulus_lut = id.eModulusLut.has_value();
    if (id.eModulusLut) {
        const auto& l = *id.eModulusLut;
        out.emodulus_lut.loaded = l.loaded;
        setText(l.lutId, out.emodulus_lut.has_lut_id, out.emodulus_lut.lut_id);
        setText(l.revision, out.emodulus_lut.has_revision, out.emodulus_lut.revision);
        setText(l.source, out.emodulus_lut.has_source, out.emodulus_lut.source);
        setText(l.sha256, out.emodulus_lut.has_sha256, out.emodulus_lut.sha256);
        setText(l.catalogSha256, out.emodulus_lut.has_catalog_sha256, out.emodulus_lut.catalog_sha256);
        setText(l.checksumStatus, out.emodulus_lut.has_checksum_status, out.emodulus_lut.checksum_status);
    }
    setText(id.eModulusLutProvenance, out.has_emodulus_lut_provenance, out.emodulus_lut_provenance);
    setText(id.profileId, out.has_profile_id, out.profile_id);
    return out;
}

ReviewRunProbe toProbe(const br::RunProbe& p) {
    ReviewRunProbe out{};
    out.path = rust::String(p.path);
    out.ok = p.ok;
    out.message = rust::String(p.message);
    out.recording_file = p.recordingFile;
    out.identity = toIdentity(p.identity);
    out.has_start_time = p.startTimeNs.has_value();
    out.start_time_ns = p.startTimeNs.value_or(0);
    out.has_end_time = p.endTimeNs.has_value();
    out.end_time_ns = p.endTimeNs.value_or(0);
    out.has_total_valid = p.totalValid.has_value();
    out.total_valid = p.totalValid.value_or(0);
    out.has_total_invalid = p.totalInvalid.has_value();
    out.total_invalid = p.totalInvalid.value_or(0);
    out.has_accounting = p.completion.has_value();
    out.accounting_completion = p.completion ? static_cast<std::uint32_t>(*p.completion)
                                             : static_cast<std::uint32_t>(backend::recording::RunCompletionState::Unknown);
    out.has_core_version = p.coreVersion.has_value();
    out.core_version = p.coreVersion ? rust::String(*p.coreVersion) : rust::String();
    return out;
}

} // namespace

constexpr std::size_t kEventQueueCapacity = 4096;

struct ReviewBridge::Impl {
    br::ReviewSession session;
    backend::services::SqliteService analysisDatabase;
    bool analysisDatabaseReady{false};
    std::string analysisDataDir;
    // The bundled kernel for regenerate masks (ProcessingService owns no
    // threads until start(), which review never calls).
    backend::services::ProcessingService processing;
    bool initialized{false};
    std::mutex eventMutex;
    std::deque<ReviewEvent> events;
    std::unique_ptr<br::ReviewJobs> jobs;

    Impl() {
        // Lane policy (#512 D7 as amended 2026-10-19): YOFO Review keeps one
        // job of any kind at a time, its own operation ids, and a job's lane
        // free before its terminal event is queued. It does not gain the
        // second lane (an export beside a regenerate / density / core job)
        // until #722: HDF5 is built without thread safety and Hdf5Service has
        // no process-wide lock. MIB Studio's facade runs both lanes at once
        // (ReviewJobsOptions::forFacade).
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
        }, br::ReviewJobsOptions::forReviewBridge());
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
    try {
        if (!toStd(data_dir).empty()) {
            const auto directory = std::filesystem::weakly_canonical(std::filesystem::absolute(toStd(data_dir)));
            if (impl_->analysisDatabaseReady && impl_->analysisDataDir != directory.string()) return false;
            if (!impl_->analysisDatabaseReady) {
                std::filesystem::create_directories(directory);
                impl_->analysisDatabaseReady = impl_->analysisDatabase.initialize((directory / "app.sqlite3").string());
                if (!impl_->analysisDatabaseReady) return false;
                impl_->analysisDataDir = directory.string();
            }
        }
    } catch (...) { return false; }
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
        out.object_coordinates = rust::String(br::toString(m.objectCoordinates.space));
        out.object_coordinates_source = rust::String(br::toString(m.objectCoordinates.source));
        out.object_origin_x = m.objectCoordinates.originX;
        out.object_origin_y = m.objectCoordinates.originY;
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
        out.has_recorded_config = m.hasRecordedConfig;
        out.ring_ratio_min = m.ringRatioMin;
        out.ring_ratio_max = m.ringRatioMax;
        out.identity = toIdentity(m.runIdentity);
    } catch (...) {
        out.valid = false;
    }
    return out;
}

ReviewProbeResult ReviewBridge::review_probe_runs(rust::Vec<rust::String> paths) {
    ReviewProbeResult out{};
    try {
        // Refuse an oversized list before copying it.
        if (paths.size() > br::kReviewProbeMaxPaths) {
            out.message = rust::String("Too many files to probe at once: " + std::to_string(paths.size()) +
                                       " (at most " + std::to_string(br::kReviewProbeMaxPaths) + ")");
            return out;
        }
        std::vector<std::string> list;
        list.reserve(paths.size());
        for (const auto& p : paths) list.emplace_back(std::string(p));
        std::vector<br::RunProbe> probes;
        std::string error;
        // Through the job lane: refused while a job runs, and holds the lane
        // so none starts; each file under the session's mutex (#722).
        if (!impl_->jobs->probeRuns(list, probes, &error)) {
            out.message = rust::String(error.empty() ? std::string("Probe refused") : error);
            return out;
        }
        out.ok = true;
        out.message = rust::String("Probed " + std::to_string(probes.size()) + " file(s)");
        out.runs.reserve(probes.size());
        for (const auto& p : probes) out.runs.push_back(toProbe(p));
    } catch (const std::exception& e) {
        out = ReviewProbeResult{};
        out.message = rust::String(std::string("review_probe_runs: ") + e.what());
    } catch (...) {
        out = ReviewProbeResult{};
        out.message = rust::String("review_probe_runs: unknown error");
    }
    return out;
}

rust::String ReviewBridge::review_analyze_population(rust::Str request_json) {
    return rust::String(impl_->jobs->analyzePopulation(toStd(request_json)));
}

rust::String ReviewBridge::review_run_statistics(rust::Str request_json) {
    try {
        br::RunQuickLookRequest request;
        std::string error;
        if (!br::parseRunQuickLookRequest(toStd(request_json), request, error))
            return rust::String(br::runQuickLookRefusedJson(error));
        // The same function as the backend bridge's run_quick_look; no cache and no instrument
        // here, so only a running review job defers a file (ReviewJobs::runStatistics, #722).
        const auto one = [this](const std::string& path) {
            br::RunStatisticsResult r;
            r.path = path;
            br::RunFileStamp stamp;
            if (!br::stampRunFile(path, stamp, r.message)) return r;
            r = impl_->jobs->runStatistics(path, "on_demand");
            r.path = path;
            r.stamp = stamp;
            return r;
        };
        std::vector<br::RunStatisticsResult> runs;
        runs.reserve(request.paths.size());
        for (const auto& path : request.paths) runs.push_back(one(path));
        std::optional<br::RunStatisticsResult> reference;
        if (request.reference) reference = one(request.reference->path);
        return rust::String(br::runQuickLookReplyJson(runs, request.reference, reference));
    } catch (const std::exception& e) {
        return rust::String(br::runQuickLookRefusedJson(std::string("review_run_statistics failed: ") + e.what()));
    } catch (...) {
        return rust::String(br::runQuickLookRefusedJson("review_run_statistics failed"));
    }
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
        for (const auto& r : rows) out.rows.push_back(toRow(r));
    } catch (...) {
        out.valid = false;
        out.rows.clear();
    }
    return out;
}

ReviewTrackRows ReviewBridge::fetch_review_track_rows(bool valid, std::int32_t track_id, std::uint32_t max) {
    ReviewTrackRows out{};
    try {
        br::TrackRows rows;
        std::string error;
        if (!impl_->session.trackRows(valid, track_id, max, rows, &error)) {
            out.message = rust::String(error);
            return out;
        }
        out.ok = true;
        out.total = rows.total;
        out.truncated = rows.truncated;
        for (const auto p : rows.positions) out.positions.push_back(p);
        for (const auto& r : rows.rows) out.rows.push_back(toRow(r));
    } catch (const std::exception& e) {
        out = ReviewTrackRows{};
        out.message = rust::String(std::string("fetch_review_track_rows: ") + e.what());
    } catch (...) {
        out = ReviewTrackRows{};
        out.message = rust::String("fetch_review_track_rows: unknown error");
    }
    return out;
}

ReviewReprocessResult ReviewBridge::review_reprocess_record(bool valid, std::uint64_t index, std::uint32_t settings) {
    ReviewReprocessResult out{};
    out.settings = settings;
    if (settings > static_cast<std::uint32_t>(br::ReprocessSettings::Default)) {
        out.message = rust::String("Unknown reprocess settings");
        return out;
    }
    try {
        br::ReprocessResult r;
        std::string error;
        if (!impl_->jobs->reprocessRecord(valid, index, static_cast<br::ReprocessSettings>(settings), r, &error)) {
            out.message = rust::String(error);
            return out;
        }
        out.ok = true;
        out.message = rust::String("Reprocessed record " + std::to_string(index) + " with " + r.settingsLabel);
        out.used_recorded_settings = r.usedRecordedSettings;
        out.settings_label = rust::String(r.settingsLabel);
        out.core_version = rust::String(r.coreVersion);
        out.core_source = rust::String(r.coreSource);
        out.pixel_to_micron = r.pixelToMicron;
        out.emodulus_recomputed = r.emodulusRecomputed;
        out.emodulus_lut = rust::String(r.emodulusLut);
        out.emodulus_lut_sha256 = rust::String(r.emodulusLutSha256);
        out.stored = toRow(r.stored);
        for (const auto& row : r.reprocessed) out.reprocessed.push_back(toRow(row));
        out.has_match = r.match.has_value();
        out.match_index = static_cast<std::uint32_t>(r.match.value_or(0));
        out.object_count_differs = r.objectCountDiffers;
        out.truncated = r.truncated;
    } catch (const std::exception& e) {
        out = ReviewReprocessResult{};
        out.settings = settings;
        out.message = rust::String(std::string("review_reprocess_record: ") + e.what());
    } catch (...) {
        out = ReviewReprocessResult{};
        out.settings = settings;
        out.message = rust::String("review_reprocess_record: unknown error");
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
        for (auto v : s.ringRatio) out.ring_ratio.push_back(v);
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

ReviewResult ReviewBridge::review_export_charts(rust::Str output_dir, rust::Vec<ReviewChartSnapshot> charts) {
    try {
        br::ExportChartsRequest req;
        req.outputDir = toStd(output_dir);
        for (const auto& c : charts) {
            br::ChartSnapshot snap;
            snap.name = std::string(c.name);
            snap.encoded.assign(c.encoded.begin(), c.encoded.end());
            req.charts.push_back(std::move(snap));
        }
        std::string error;
        return impl_->started(impl_->jobs->startExportCharts(req, &error), error, "Export Charts");
    } catch (const std::exception& e) {
        return fail(std::string("review_export_charts: ") + e.what());
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

// review-contract.json `review_abi_version` (ADR 0014): YOFO Review's own
// contract, independent of bridge_abi_version(). v1: ReviewSession-backed
// info/rows/frames/series/thumbnails/scatter/core-record calls, overlay_modes,
// review_pixel_formats and the review event queue. v2 (#717 B1a): the run
// identity in fetch_review_info and review_probe_runs (review_probe). v3
// (#717 B3b): emodulus_lut and profile_id in the identity,
// fetch_review_track_rows (review_track_rows) and review_reprocess_record
// (review_reprocess_record). v4 (#764): review_reprocess_record names the
// E-LUT it used (emodulus_lut, emodulus_lut_sha256): the run's own. v5
// (#783): the identity's emodulus_lut also reads a regenerated file's
// @emodulus_lut_json, named by emodulus_lut_provenance (snapshot | recorded |
// host). v6 (#716 A7a): review_run_statistics, the run quick look's twin.
std::uint32_t review_bridge_abi_version() {
    return 9;
}

namespace {

// The experiment fixture: 10 valid frames, 4 invalid, 3-image series, ROI.
// `snapshot` / `annotation` are written verbatim when non-empty; with
// `provenance`, run accounting and a stored KDE record as well.
bool writeExperimentFixture(const std::string& path, const std::string& snapshot, const std::string& annotation,
                            bool provenance) {
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
        if (!hdf5.openFile(path)) return false;
        if (!hdf5.initializeDatasets()) return false;
        if (!hdf5.appendFrames(valid, invalid)) return false;
        backend::services::ProcessingConfig cfg;
        backend::services::ProcessingService::Roi roi{4, 2, 20, 16};
        if (!hdf5.writeExperimentInfo(1000, 5000, valid.size(), invalid.size(), cfg, roi)) return false;
        if (!snapshot.empty() && !hdf5.writeRunSnapshotJson(snapshot, "{}")) return false;
        if (!annotation.empty() && !hdf5.writeExperimentAnnotationJson(annotation)) return false;
        if (!provenance) {
            hdf5.closeFile();
            return true;
        }
        backend::recording::RecordingAccountingSnapshot a;
        a.admitted = 14;
        a.processed = 10;
        a.scientificallyRejected = 4;
        a.persistenceAdmitted = 14;
        a.persistenceCommitted = 14;
        a.completion = backend::recording::RunCompletionState::Complete;
        a.reconciled = true;
        if (!hdf5.writeRunAccounting(a)) return false;
        // A file from #756 on records its rows' coordinate space; the legacy
        // variant (no provenance) does not, and its ROI is away from (0, 0).
        if (!hdf5.writeObjectCoordinates(Hdf5Service::kObjectCoordinatesImage)) return false;
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

} // namespace

bool review_fixture_write_experiment(rust::Str path) {
    return writeExperimentFixture(toStd(path), "{\"schema\":1,\"pixel_to_micron\":0.25,\"profile_id\":\"fixture\"}", "",
                                  /*provenance=*/true);
}

bool review_fixture_write_run(rust::Str path, rust::Str snapshot_json, rust::Str annotation_json) {
    // A legacy file (no snapshot) predates run accounting and the KDE record too.
    const std::string snapshot = toStd(snapshot_json);
    return writeExperimentFixture(toStd(path), snapshot, toStd(annotation_json), /*provenance=*/!snapshot.empty());
}

bool review_fixture_write_regenerated(rust::Str path, rust::Str emodulus_lut_json) {
    // A regenerated file: no run snapshot, the experiment fixture's frames and,
    // when non-empty, `/experiment_info @emodulus_lut_json` as given (#764).
    const std::string lut = toStd(emodulus_lut_json);
    if (!writeExperimentFixture(toStd(path), "", "", /*provenance=*/false)) return false;
    if (lut.empty()) return true;
    try {
        backend::services::Hdf5Service hdf5;
        if (!hdf5.openFileForUpdate(toStd(path))) return false;
        const bool ok = hdf5.writeEModulusLutJson(lut);
        hdf5.closeFile();
        return ok;
    } catch (...) {
        return false;
    }
}

bool review_fixture_write_quick_look(rust::Str path, std::uint32_t variant) {
    if (variant > static_cast<std::uint32_t>(br::RunStatisticsFixture::RawRecording)) return false;
    return br::writeRunStatisticsFixture(toStd(path), static_cast<br::RunStatisticsFixture>(variant));
}

rust::String review_fixture_quick_look_run_id(std::uint32_t variant) {
    if (variant > static_cast<std::uint32_t>(br::RunStatisticsFixture::RawRecording)) return rust::String();
    return rust::String(br::runStatisticsFixtureRunId(static_cast<br::RunStatisticsFixture>(variant)));
}

bool review_fixture_write_population(rust::Str path, std::uint32_t cells, std::uint64_t seed) {
    try {
        using backend::services::Hdf5Service;
        using backend::services::ProcessedFrame;
        constexpr int kH = 48, kW = 64;
        constexpr double kFactor = 0.25; // µm per px
        if (cells == 0 || cells > 200000) return false;
        std::mt19937_64 rng(seed);
        std::normal_distribution<double> unit(0.0, 1.0);
        std::uniform_real_distribution<double> coin(0.0, 1.0);
        auto make = [&](std::uint64_t idx, bool valid) {
            // Two populations: small stiff cells (70 %) and large soft ones.
            const bool small = coin(rng) < 0.7;
            const double areaUm2 = std::max(20.0, small ? 90.0 + 15.0 * unit(rng) : 150.0 + 22.0 * unit(rng));
            const double deform = std::clamp(small ? 0.035 + 0.010 * unit(rng) : 0.070 + 0.015 * unit(rng), 0.001, 0.3);
            const double radiusPx = std::sqrt(areaUm2 / (kFactor * kFactor) / 3.14159265358979);
            ProcessedFrame f;
            f.index = idx;
            f.timestampNs = (idx + 1) * 1'000'000ULL;
            f.originalImage = cv::Mat(kH, kW, CV_8UC1, cv::Scalar(90));
            f.processedImage = cv::Mat::zeros(kH, kW, CV_8UC1);
            const cv::Point c(kW / 2, kH / 2);
            const cv::Size axes(std::max(2, static_cast<int>(radiusPx * (1.0 + deform))), std::max(2, static_cast<int>(radiusPx * (1.0 - deform))));
            cv::ellipse(f.originalImage, c, axes, 0, 0, 360, cv::Scalar(40), 2);
            cv::ellipse(f.processedImage, c, axes, 0, 0, 360, cv::Scalar(255), -1);
            f.validation.isValid = valid;
            f.validation.isTargetGroup = valid && !small;
            f.validation.hasSingleInnerContour = valid;
            f.validation.objectCount = 1;
            f.validation.area = areaUm2 / (kFactor * kFactor);
            f.validation.deformability = deform;
            f.validation.ringRatio = valid ? 20.0 + 2.5 * unit(rng) : 0.0;
            return f;
        };
        std::vector<ProcessedFrame> valid, invalid;
        std::uint64_t idx = 0;
        for (std::uint32_t i = 0; i < cells; ++i) {
            valid.push_back(make(idx++, true));
            if (i % 10 == 9) invalid.push_back(make(idx++, false));
        }
        Hdf5Service hdf5;
        if (!hdf5.openFile(toStd(path))) return false;
        if (!hdf5.initializeDatasets()) return false;
        if (!hdf5.appendFrames(valid, invalid)) return false;
        backend::services::ProcessingConfig cfg;
        if (!hdf5.writeExperimentInfo(1000, 1000 + idx * 1'000'000ULL, valid.size(), invalid.size(), cfg, {0, 0, kW, kH})) return false;
        if (!hdf5.writeRunSnapshotJson("{\"schema\":1,\"pixel_to_micron\":0.25,\"profile_id\":\"population\"}", "{}")) return false;
        hdf5.closeFile();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace mib_review_bridge

rust::String mib_review_bridge::ReviewBridge::review_analysis_read(rust::Str request) {
    return review_analysis_request(request, false);
}
rust::String mib_review_bridge::ReviewBridge::review_analysis_write(rust::Str request) {
    return review_analysis_request(request, true);
}
rust::String mib_review_bridge::ReviewBridge::review_analysis_request(rust::Str request, bool write) {
    auto source = [this](const nlohmann::json& descriptor) -> nlohmann::json {
        const auto kind = descriptor.at("kind").get<std::string>();
        if (kind == "run-archive-gallery") {
            if (descriptor.size() != 6)
                throw std::invalid_argument("Expected archive path, fingerprints, keys and mask");
            const auto response =
                nlohmann::json::parse(impl_->jobs->analyzePopulation(nlohmann::json{
                    {"expectedPath", descriptor.at("path")},
                    {"feature", "area"},
                    {"minimum", nullptr},
                    {"maximum", nullptr},
                    {"snapshot", true},
                    {"sourceKind", "run-archive"},
                    {"galleryKeys", descriptor.at("keys")},
                    {"expectedFingerprint", descriptor.at("fingerprint")},
                    {"expectedFileFingerprint", descriptor.at("fileFingerprint")},
                    {"mask", descriptor.at("mask")}}.dump()));
            if (!response.value("ok", false))
                throw std::invalid_argument(response.value("message", "Archive gallery refused"));
            return response.at("source");
        }
        if (kind == "stored-gallery") {
            if (descriptor.size() != 4) throw std::invalid_argument("Expected gallery path, fingerprint and keys");
            const auto response = nlohmann::json::parse(impl_->jobs->analyzePopulation(nlohmann::json{
                {"expectedPath", descriptor.at("path")}, {"feature", "area"}, {"minimum", nullptr}, {"maximum", nullptr},
                {"snapshot", true}, {"galleryKeys", descriptor.at("keys")}, {"expectedFingerprint", descriptor.at("fingerprint")}}.dump()));
            if (!response.value("ok", false)) throw std::invalid_argument(response.value("message", "Gallery refused"));
            return response.at("source");
        }

        if ((kind != "stored" && kind != "run-archive") || descriptor.size() != 2)
            throw std::invalid_argument("Review supports stored or run-archive sources only");
        const auto result = nlohmann::json::parse(impl_->jobs->analyzePopulation(nlohmann::json{
            {"expectedPath", descriptor.at("path")}, {"feature", "area"}, {"minimum", nullptr},
            {"maximum", nullptr}, {"snapshot", true}, {"sourceKind", kind}}.dump()));
        if (!result.value("ok", false)) throw std::invalid_argument(result.value("message", "Source refused"));
        return result.at("source");
    };
    return rust::String(backend::analysis::analysisRequest(impl_->analysisDatabase, toStd(request), write, source));
}
