#include "backend/review/ReviewSession.h"

#include "backend/processing/ProcessingContract.h"
#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/RunProvenanceRecord.h"
#include "backend/review/OverlayCompose.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace backend::review
{

    namespace
    {
        const char *datasetPath(ReviewDataset dataset)
        {
            switch (dataset)
            {
            case ReviewDataset::ValidImage: return "/valid_frames/images";
            case ReviewDataset::InvalidImage: return "/invalid_frames/images";
            case ReviewDataset::RecordedImage: return "/recorded_frames/images";
            case ReviewDataset::ValidMask: return "/valid_frames/masks";
            case ReviewDataset::InvalidMask: return "/invalid_frames/masks";
            }
            return nullptr;
        }

        bool isMaskDataset(ReviewDataset dataset)
        {
            return dataset == ReviewDataset::ValidMask || dataset == ReviewDataset::InvalidMask;
        }

        cv::Mat toGray(const cv::Mat &image)
        {
            if (image.empty() || image.channels() == 1) return image;
            cv::Mat gray;
            cv::extractChannel(image, gray, 0);
            return gray;
        }

        // A frame with more than one channel becomes Mono8 exactly as the
        // regenerate engine converts HDF input (Reanalysis.cpp,
        // grayLikeLoadFromHdf5), so a one-record reprocess sees what a
        // regenerate of the file would.
        cv::Mat grayLikeRegenerate(cv::Mat frame)
        {
            if (frame.empty() || frame.type() == CV_8UC1) return frame;
            cv::Mat gray;
            if (frame.channels() == 3) cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
            else frame.convertTo(gray, CV_8UC1);
            return gray;
        }

        MetricRow rowFromFrame(const services::ProcessedFrame &frame, std::uint64_t position,
                               double pixelToMicron)
        {
            const auto &v = frame.validation;
            MetricRow row;
            row.frameIndex = frame.index;
            row.timestampNs = frame.timestampNs;
            row.valid = v.isValid;
            row.targetGroup = v.isTargetGroup;
            row.touchesBorder = v.touchesBorder;
            row.hasSingleInnerContour = v.hasSingleInnerContour;
            row.inRange = v.inRange;
            row.inChannel = v.inChannel;
            row.innerContourCount = v.innerContourCount;
            row.objectId = v.objectId;
            row.objectCount = v.objectCount;
            row.trackId = v.trackId;
            row.trackFirstFrame = v.trackFirstFrame;
            row.trackLastFrame = v.trackLastFrame;
            row.trackObservationCount = v.trackObservationCount;
            row.bboxX = v.bboxX;
            row.bboxY = v.bboxY;
            row.bboxWidth = v.bboxWidth;
            row.bboxHeight = v.bboxHeight;
            row.centroidX = v.centroidX;
            row.centroidY = v.centroidY;
            row.area = v.area;
            row.areaUm2 = v.area * pixelToMicron * pixelToMicron;
            row.deformability = v.deformability;
            row.areaRatio = v.areaRatio;
            row.ringRatio = v.ringRatio;
            row.laplacianVariance = v.laplacianVariance;
            row.youngsModulus = v.youngsModulus;
            row.brightnessQ1 = v.brightness.q1;
            row.brightnessQ2 = v.brightness.q2;
            row.brightnessQ3 = v.brightness.q3;
            row.brightnessQ4 = v.brightness.q4;
            (void)position;
            return row;
        }

        // Letterbox `image` into a size×size tile (black borders), keeping
        // the aspect ratio as the Qt thumbnail does.
        cv::Mat letterbox(const cv::Mat &image, int size, int channels)
        {
            cv::Mat tile = cv::Mat::zeros(size, size, channels == 3 ? CV_8UC3 : CV_8UC1);
            if (image.empty()) return tile;
            cv::Mat src = image;
            if (channels == 3 && src.channels() == 1) cv::cvtColor(src, src, cv::COLOR_GRAY2RGB);
            else if (channels == 1 && src.channels() == 3) cv::cvtColor(src, src, cv::COLOR_RGB2GRAY);
            const double scale = std::min(static_cast<double>(size) / src.cols,
                                          static_cast<double>(size) / src.rows);
            const int w = std::max(1, static_cast<int>(src.cols * scale));
            const int h = std::max(1, static_cast<int>(src.rows * scale));
            cv::Mat scaled;
            cv::resize(src, scaled, cv::Size(w, h), 0, 0, cv::INTER_AREA);
            scaled.copyTo(tile(cv::Rect((size - w) / 2, (size - h) / 2, w, h)));
            return tile;
        }

        // One file of the identity probe (#717 B1a), with its own short-lived
        // reader: header attributes only. The caller serializes it with the
        // session's HDF5 access (ReviewSession::probeRuns).
        RunProbe probeOne(const std::string &path)
        {
            RunProbe probe;
            probe.path = path;
            auto failed = [&path](std::string message) {
                RunProbe out;
                out.path = path;
                out.message = std::move(message);
                return out;
            };
            if (path.empty()) return failed("No file path");
            std::error_code ec;
            const auto status = std::filesystem::status(std::filesystem::u8path(path), ec);
            if (status.type() == std::filesystem::file_type::not_found) return failed("No such file: " + path);
            if (ec) return failed("Cannot read " + path + ": " + ec.message());
            if (status.type() != std::filesystem::file_type::regular) return failed("Not a file: " + path);
            services::Hdf5Service reader;
            try
            {
                if (!reader.loadFile(path))
                {
                    reader.closeFile();
                    return failed("Not an HDF5 file, or it cannot be opened (locked or unreadable): " + path);
                }
                probe.recordingFile = reader.isRecordingFile();
                // Each attribute on its own, presence-aware (#717 B1a review):
                // readExperimentInfo / readRecordingInfo default a missing
                // count to 0, which would probe as a recorded 0.
                const char *group = probe.recordingFile ? "/recording_info" : "/experiment_info";
                if (probe.recordingFile)
                {
                    probe.totalValid = reader.readScalarUintAttribute(group, "total_recorded_frames");
                }
                else
                {
                    probe.totalValid = reader.readScalarUintAttribute(group, "total_valid_frames");
                    probe.totalInvalid = reader.readScalarUintAttribute(group, "total_invalid_frames");
                }
                // A recorded 0 time is the writers' "no wall clock" (regenerating
                // from frame timestamps writes start 0), not 1970: not recorded.
                const auto start = reader.readScalarUintAttribute(group, "start_time_ns");
                const auto end = reader.readScalarUintAttribute(group, "end_time_ns");
                if (start && *start != 0) probe.startTimeNs = start;
                if (end && *end != 0) probe.endTimeNs = end;
                recording::RecordingAccountingSnapshot accounting;
                if (reader.readRunAccounting(accounting)) probe.completion = accounting.completion;
                processing::ProcessingCoreIdentity core;
                if (reader.readProcessingCoreIdentity(core) && !core.version.empty()) probe.coreVersion = core.version;
                probe.identity = ReviewSession::readRunIdentity(reader);
                probe.ok = true;
            }
            catch (const std::exception &e)
            {
                reader.closeFile();
                return failed("Failed to read " + path + ": " + e.what());
            }
            catch (...)
            {
                reader.closeFile();
                return failed("Failed to read " + path);
            }
            reader.closeFile();
            return probe;
        }
    } // namespace

    struct ReviewSession::Impl
    {
        mutable std::mutex mutex;
        services::Hdf5Service reader;
        bool open{false};
        std::string path;
        bool recording{false};
        std::vector<services::ProcessedFrame> validMeta;
        std::vector<services::ProcessedFrame> invalidMeta;
        bool validMetaReadable{false};
        ReviewMetadata meta;
        double fallbackFactor{0.4886};

        double factor() const
        {
            return meta.pixelToMicronFromFile && meta.pixelToMicron > 0.0 ? meta.pixelToMicron
                                                                            : fallbackFactor;
        }

        void readMetadataLocked()
        {
            meta = ReviewMetadata{};
            meta.fileOpen = true;
            meta.filePath = path;
            meta.recordingFile = recording;
            validMeta.clear();
            invalidMeta.clear();
            validMetaReadable = true;
            if (recording)
            {
                std::uint64_t total = 0;
                reader.readRecordingInfo(meta.startTimeNs, meta.endTimeNs, total, meta.filteredFrames,
                                         &meta.multiImageEnabled, &meta.multiImageCount);
                if (meta.multiImageCount == 0) meta.multiImageCount = 1;
                reader.readRecordingMetadata(validMeta);
                meta.totalValid = total;
            }
            else
            {
                std::size_t totalValid = 0;
                std::size_t totalInvalid = 0;
                reader.readExperimentInfo(meta.startTimeNs, meta.endTimeNs, totalValid, totalInvalid, &meta.roi);
                meta.totalValid = totalValid;
                meta.totalInvalid = totalInvalid;
                const bool validRead = reader.readValidMetadata(validMeta);
                // An absent valid set is empty, not unreadable (the facade's
                // charts rule before #512 S5, when they opened their own reader).
                const auto validPresent = reader.metadataDatasetPresent(true);
                validMetaReadable = validPresent.has_value() && (!*validPresent || validRead);
                reader.readInvalidMetadata(invalidMeta);
                std::size_t seriesFrames = 0;
                std::size_t seriesCount = 0;
                int h = 0;
                int w = 0;
                if (reader.getSeriesImageInfo(seriesFrames, seriesCount, h, w) && seriesCount > 1)
                {
                    meta.hasSeries = true;
                    meta.seriesCount = seriesCount;
                }
            }

            backend::processing::ProcessingCoreIdentity identity;
            if (reader.readProcessingCoreIdentity(identity))
            {
                meta.hasCoreIdentity = true;
                meta.coreVersion = identity.version;
                meta.coreSource = identity.source;
                meta.coreReleaseTag = identity.releaseTag;
            }
            {
                cv::Mat bg;
                meta.hasBackground = reader.readBackgroundImage(bg) && !bg.empty();
            }
            auto fillInfo = [this](const char *p, DatasetInfo &info) {
                std::size_t count = 0;
                info.present = reader.getDatasetInfo(p, count, info.height, info.width, info.channels);
                info.count = count;
            };
            fillInfo("/valid_frames/images", meta.validImages);
            fillInfo("/invalid_frames/images", meta.invalidImages);
            fillInfo("/valid_frames/masks", meta.validMasks);
            fillInfo("/invalid_frames/masks", meta.invalidMasks);
            fillInfo("/recorded_frames/images", meta.recordedImages);

            meta.hasAccounting = reader.readRunAccounting(meta.accounting);

            // TD-17: the factor the run was recorded with.
            if (const double f = ReviewSession::recordedPixelToMicron(reader); f > 0.0)
            {
                meta.pixelToMicron = f;
                meta.pixelToMicronFromFile = true;
            }
            if (!meta.pixelToMicronFromFile) meta.pixelToMicron = fallbackFactor;

            {
                services::ProcessingConfig recorded;
                meta.hasRecordedConfig = reader.readRecordedProcessingConfig(recorded);
                meta.ringRatioMin = meta.hasRecordedConfig ? recorded.ring_ratio_min : services::ProcessingConfig{}.ring_ratio_min;
                meta.ringRatioMax = meta.hasRecordedConfig ? recorded.ring_ratio_max : services::ProcessingConfig{}.ring_ratio_max;
            }
            reader.readKdeAnalysisJson(meta.kdeAnalysisJson);
            reader.readKdeLiveJson(meta.kdeLiveJson);
            meta.runIdentity = ReviewSession::readRunIdentity(reader);
            if (!recording) meta.objectCoordinates = resolveObjectCoordinatesLocked();
        }

        // The file's object coordinate space (#756 review 2), in the order of
        // ObjectCoordinates.h. Runs at open under the session's mutex: the
        // mask check reads HDF5 (#722).
        ObjectCoordinates resolveObjectCoordinatesLocked() const
        {
            ObjectSpaceEvidence evidence;
            evidence.attribute =
                reader.readScalarStringAttribute("/experiment_info", services::Hdf5Service::kObjectCoordinatesAttribute);
            evidence.recordedRoi = meta.roi;
            std::string snapshot;
            if (reader.readRunSnapshotJson(snapshot))
            {
                const auto doc = nlohmann::json::parse(snapshot, nullptr, false);
                if (doc.is_object())
                {
                    if (const auto v = doc.find("schema_version"); v != doc.end() && v->is_number_unsigned())
                        evidence.snapshotVersion = v->get<std::uint32_t>();
                    if (const auto r = doc.find("roi"); r != doc.end() && r->is_object())
                    {
                        const auto member = [&](const char *name, int &value) {
                            const auto it = r->find(name);
                            if (it == r->end() || !it->is_number_integer()) return false;
                            value = it->get<int>();
                            return true;
                        };
                        services::ProcessingService::Roi roi{};
                        if (member("x", roi.x) && member("y", roi.y) && member("w", roi.w) && member("h", roi.h))
                            evidence.snapshotRoi = roi;
                    }
                    if (const auto p = doc.find("science_placement"); p != doc.end() && p->is_string())
                        evidence.snapshotPlScience = p->get<std::string>() == "pl";
                    if (const auto m = doc.find("realtime_mode"); m != doc.end() && m->is_string())
                        evidence.snapshotRealtimeMode = m->get<std::string>();
                }
            }
            return resolveObjectCoordinates(evidence, [this] { return maskCheckLocked(); });
        }

        // Where the stored objects are in the stored full-frame masks, over a
        // bounded sample of the records that have both: the valid set spread
        // over, the invalid set only to fill what the valid set lacks.
        std::optional<ObjectSpace> maskCheckLocked() const
        {
            // Contract 1 objects are inner contours: the area is the hole's
            // hull and the box the outline's (ProcessingScience), whenever the
            // frame had an inner contour; otherwise outlines.
            services::ProcessingConfig recorded;
            const bool innerContours = processing::contract::contractObjectsAreInnerContours(
                reader.readRecordedProcessingConfig(recorded) ? recorded.processing_contract_version
                                                              : services::ProcessingConfig{}.processing_contract_version);
            std::vector<std::optional<ObjectSpace>> votes;
            for (const bool valid : {true, false})
            {
                const std::size_t room = kObjectSpaceMaskCheckMaxRecords - votes.size();
                const DatasetInfo &masks = valid ? meta.validMasks : meta.invalidMasks;
                if (room == 0 || !masks.present) continue;
                const auto &frames = set(valid);
                const std::size_t n = std::min<std::size_t>(frames.size(), static_cast<std::size_t>(masks.count));
                std::vector<std::size_t> candidates;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const auto &v = frames[i].validation;
                    if (v.bboxWidth > 0 && v.bboxHeight > 0) candidates.push_back(i);
                }
                for (const std::size_t position : maskCheckSample(candidates.size(), room))
                {
                    const std::size_t index = candidates[position];
                    cv::Mat mask;
                    if (!reader.readImageByIndex(masksPath(valid), index, mask) || mask.empty())
                    {
                        votes.emplace_back(std::nullopt);
                        continue;
                    }
                    const auto &v = frames[index].validation;
                    StoredObject object;
                    object.box = cv::Rect2d(v.bboxX, v.bboxY, v.bboxWidth, v.bboxHeight);
                    object.areaPx = v.area;
                    object.hole = innerContours && v.innerContourCount > 0;
                    votes.push_back(maskPlacementVote(toGray(mask), object, meta.roi.x, meta.roi.y));
                }
            }
            return maskCheckVerdict(votes);
        }

        const std::vector<services::ProcessedFrame> &set(bool valid) const
        {
            return valid ? validMeta : invalidMeta;
        }

        std::string imagesPath(bool valid) const
        {
            if (recording) return "/recorded_frames/images";
            return valid ? "/valid_frames/images" : "/invalid_frames/images";
        }

        std::string masksPath(bool valid) const
        {
            if (recording) return {};
            return valid ? "/valid_frames/masks" : "/invalid_frames/masks";
        }

        // Image + optional mask + overlay/ROI for one frame of a set.
        bool composeFrameLocked(bool valid, std::uint64_t index, OverlayMode mode, bool roiOverlay,
                                cv::Mat &out) const
        {
            const auto &frames = set(valid);
            if (index >= frames.size()) return false;
            cv::Mat original;
            if (!reader.readImageByIndex(imagesPath(valid), static_cast<std::size_t>(index), original) ||
                original.empty())
            {
                return false;
            }
            cv::Mat mask;
            const std::string maskPath = masksPath(valid);
            if (mode != OverlayMode::None && !maskPath.empty())
            {
                reader.readImageByIndex(maskPath, static_cast<std::size_t>(index), mask);
                mask = toGray(mask);
            }
            out = composeOverlay(original, mask, &frames[index].validation, recording ? OverlayMode::None : mode);
            if (roiOverlay && !recording) drawRoiOverlay(out, meta.roi, original.cols, original.rows);
            return !out.empty();
        }

        bool readSeriesLocked(std::uint64_t index, std::vector<cv::Mat> &images) const
        {
            images.clear();
            if (index >= validMeta.size()) return false;
            if (recording)
            {
                if (!meta.multiImageEnabled || meta.multiImageCount <= 1) return true;
                std::vector<cv::Mat> window;
                if (!reader.readImagesRange("/recorded_frames/images", static_cast<std::size_t>(index),
                                            static_cast<std::size_t>(meta.multiImageCount), window))
                {
                    return false;
                }
                if (window.size() > 1) images = std::move(window);
                return true;
            }
            if (!meta.hasSeries) return true;
            return reader.readSeriesImagesByIndex(static_cast<std::size_t>(index), images);
        }

        // Closed, with nothing of the last file left: also a reader handle
        // whose open did not complete. Allocates nothing and does not throw
        // (closeFile never does), so it is safe in a catch block.
        void resetLocked()
        {
            reader.closeFile();
            open = false;
            path.clear();
            recording = false;
            meta = ReviewMetadata{};
            validMeta.clear();
            invalidMeta.clear();
            validMetaReadable = false;
        }

        // All or nothing (#512 S5): either `filePath` is open with its
        // metadata read, or the session is closed. A read that throws after
        // the reader opened (an allocation failure, an OpenCV exception from a
        // malformed dataset) closes the reader again, so no host sees an open
        // reader it was not told about (the facade's D1 flag mirrors isOpen()).
        bool loadLocked(const std::string &filePath, std::string *error)
        {
            try
            {
                if (!reader.loadFile(filePath))
                {
                    if (error) *error = "File not found or not an HDF5 review file: " + filePath;
                    return false;
                }
                open = true;
                path = filePath;
                recording = reader.isRecordingFile();
                readMetadataLocked();
                return true;
            }
            catch (const std::exception &e)
            {
                resetLocked();
                noteFailure(filePath, e.what(), error);
            }
            catch (...)
            {
                resetLocked();
                noteFailure(filePath, nullptr, error);
            }
            return false;
        }

        // After the rollback: a log line and the error text, never a throw.
        static void noteFailure(const std::string &filePath, const char *what, std::string *error) noexcept
        {
            try
            {
                SPDLOG_ERROR("ReviewSession: reading {} failed: {}", filePath, what ? what : "unknown error");
                if (error) *error = "Failed to read " + filePath + (what ? std::string(": ") + what : std::string());
            }
            catch (...)
            {
            }
        }
    };

    ReviewSession::ReviewSession() : impl_(std::make_unique<Impl>()) {}

    ReviewSession::~ReviewSession() { close(); }

    bool ReviewSession::open(const std::string &path, std::string *error)
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->resetLocked();
        if (path.empty())
        {
            if (error) *error = "No file path";
            return false;
        }
        if (!impl_->loadLocked(path, error)) return false;
        SPDLOG_INFO("ReviewSession: opened {} ({} file, valid {}, invalid {}, factor {}{})", path,
                    impl_->recording ? "recording" : "experiment", impl_->validMeta.size(),
                    impl_->invalidMeta.size(), impl_->factor(),
                    impl_->meta.pixelToMicronFromFile ? " from file" : " fallback");
        return true;
    }

    void ReviewSession::close()
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->resetLocked();
    }

    bool ReviewSession::isOpen() const
    {
        std::scoped_lock lock(impl_->mutex);
        // Fail closed (#512 S5 review): a reader handle the session did not
        // finish opening still counts, so the facade's D1 flag keeps Start
        // refused until close() or the next open() releases it.
        return impl_->open || impl_->reader.holdsFileHandle();
    }

    services::Hdf5Service &ReviewSessionTestAccess::reader(ReviewSession &session)
    {
        return session.impl_->reader;
    }

    std::string ReviewSession::filePath() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->path;
    }

    void ReviewSession::setFallbackPixelToMicron(double factor)
    {
        std::scoped_lock lock(impl_->mutex);
        if (factor > 0.0) impl_->fallbackFactor = factor;
        if (impl_->open && !impl_->meta.pixelToMicronFromFile) impl_->meta.pixelToMicron = impl_->fallbackFactor;
    }

    double ReviewSession::pixelToMicron() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->factor();
    }

    double ReviewSession::fallbackPixelToMicron() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->fallbackFactor;
    }

    double ReviewSession::recordedPixelToMicron(const services::Hdf5Service &reader)
    {
        std::string snapshot;
        if (reader.readRunSnapshotJson(snapshot) && !snapshot.empty())
        {
            const auto j = nlohmann::json::parse(snapshot, nullptr, false);
            if (j.is_object() && j.contains("pixel_to_micron") && j["pixel_to_micron"].is_number())
            {
                const double f = j["pixel_to_micron"].get<double>();
                if (std::isfinite(f) && f > 0.0) return f;
            }
        }
        // A regenerated / reanalysed file has no run snapshot: the factor its
        // objects were validated at (#758).
        return reader.readAnalysisPixelToMicron().value_or(0.0);
    }

    MetricRow ReviewSession::metricRow(const services::ProcessedFrame &frame, double pixelToMicron)
    {
        return rowFromFrame(frame, 0, pixelToMicron);
    }

    RunIdentity ReviewSession::readRunIdentity(const services::Hdf5Service &reader)
    {
        RunIdentity out;
        std::string snapshot;
        if (reader.readRunSnapshotJson(snapshot))
        {
            recording::RecordedRunProvenance parsed;
            if (recording::parseRunProvenance(snapshot, parsed))
            {
                out.hasSnapshot = true;
                out.snapshotVersion = parsed.schemaVersion;
                out.runId = parsed.v3.runId;
                out.declared = parsed.v3.identity;
                out.eModulusLut = parsed.v3.eModulusLut;
                // profile_id is not part of the typed record: read it from
                // the same document, a non-empty string only (#717 B3b).
                const auto doc = nlohmann::json::parse(snapshot, nullptr, false);
                if (doc.is_object())
                {
                    const auto it = doc.find("profile_id");
                    if (it != doc.end() && it->is_string() && !it->get<std::string>().empty())
                        out.profileId = it->get<std::string>();
                }
            }
            else
            {
                out.snapshotUnreadable = true;
            }
        }
        std::string annotation;
        if (reader.readExperimentAnnotationJson(annotation))
        {
            out.annotation = recording::parseExperimentAnnotation(annotation);
            if (!out.annotation)
            {
                out.annotationUnreadable = true;
            }
            else
            {
                // Presence-aware: readExperimentAnnotationJson reports a missing
                // version as 0, so a recorded 0 and a missing one differ only here.
                const auto version =
                    reader.readScalarUintAttribute("/experiment_annotation", "annotation_schema_version");
                if (version && *version <= 0xFFFFFFFFull)
                    out.annotationSchemaVersion = static_cast<std::uint32_t>(*version);
            }
        }
        return out;
    }

    bool ReviewSession::probeRuns(const std::vector<std::string> &paths, std::vector<RunProbe> &out,
                                  std::string *error) const
    {
        out.clear();
        if (paths.size() > kReviewProbeMaxPaths)
        {
            if (error)
            {
                *error = "Too many files to probe at once: " + std::to_string(paths.size()) + " (at most " +
                         std::to_string(kReviewProbeMaxPaths) + ")";
            }
            return false;
        }
        out.reserve(paths.size());
        for (const auto &path : paths)
        {
            // The session's own lock, per file (#722): no session read, open or
            // close interleaves with the probe's HDF5 calls, and the open file
            // stays as it was.
            std::scoped_lock lock(impl_->mutex);
            out.push_back(probeOne(path));
        }
        return true;
    }

    ReviewMetadata ReviewSession::metadata() const
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open) return ReviewMetadata{};
        ReviewMetadata m = impl_->meta;
        m.pixelToMicron = impl_->factor();
        return m;
    }

    bool ReviewSession::isRecordingFile() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->open && impl_->recording;
    }

    std::uint64_t ReviewSession::frameCount(bool valid) const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->set(valid).size();
    }

    bool ReviewSession::metricsPage(bool valid, std::uint64_t offset, std::uint64_t count,
                                    std::vector<MetricRow> &rows, std::uint64_t &totalOut) const
    {
        std::scoped_lock lock(impl_->mutex);
        rows.clear();
        totalOut = 0;
        if (!impl_->open) return false;
        const auto &source = impl_->set(valid);
        totalOut = source.size();
        if (offset >= source.size()) return true;
        const std::uint64_t end = std::min<std::uint64_t>(source.size(), offset + count);
        rows.reserve(static_cast<std::size_t>(end - offset));
        const double f = impl_->factor();
        for (std::uint64_t i = offset; i < end; ++i) rows.push_back(rowFromFrame(source[i], i, f));
        return true;
    }

    bool ReviewSession::trackRows(bool valid, int trackId, std::size_t max, TrackRows &out,
                                  std::string *error) const
    {
        out = TrackRows{};
        auto refuse = [error](std::string message) {
            if (error) *error = std::move(message);
            return false;
        };
        if (max > kReviewTrackRowsMax)
            return refuse("Too many rows requested: " + std::to_string(max) + " (at most " +
                          std::to_string(kReviewTrackRowsMax) + ")");
        if (trackId < 0) return refuse("This record is not tracked, so it has no linked records");
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open) return refuse("No review file is open");
        if (impl_->recording) return refuse("A raw recording has no records");
        const auto &source = impl_->set(valid);
        const double f = impl_->factor();
        for (std::uint64_t i = 0; i < source.size(); ++i)
        {
            if (source[i].validation.trackId != trackId) continue;
            ++out.total;
            if (out.rows.size() >= max) continue;
            out.positions.push_back(i);
            out.rows.push_back(rowFromFrame(source[i], i, f));
        }
        out.truncated = out.total > out.rows.size();
        return true;
    }

    bool ReviewSession::reprocessInputs(bool valid, std::uint64_t index, ReprocessSettings settings,
                                        ReprocessInputs &out, std::string *error) const
    {
        out = ReprocessInputs{};
        auto refuse = [error](std::string message) {
            if (error) *error = std::move(message);
            return false;
        };
        if (settings != ReprocessSettings::Recorded && settings != ReprocessSettings::Default)
            return refuse("Unknown reprocess settings");
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open) return refuse("No review file is open");
        if (impl_->recording) return refuse("A raw recording has no stored records to reprocess");
        const auto &source = impl_->set(valid);
        if (index >= source.size())
            return refuse("No record " + std::to_string(index) + " in the " + (valid ? "valid" : "invalid") +
                          " set (" + std::to_string(source.size()) + " records)");
        const auto &meta = impl_->meta;
        out.pixelToMicron = impl_->factor();
        out.stored = rowFromFrame(source[static_cast<std::size_t>(index)], index, out.pixelToMicron);
        cv::Mat image;
        if (!impl_->reader.readImageByIndex(impl_->imagesPath(valid), static_cast<std::size_t>(index), image) ||
            image.empty())
        {
            return refuse("Record " + std::to_string(index) + " has no stored image to reprocess");
        }
        out.image = grayLikeRegenerate(std::move(image));
        out.pixelToMicronFromFile = meta.pixelToMicronFromFile && meta.pixelToMicron > 0.0;
        // The stored record is matched in image coordinates (#756 review 2):
        // a file whose space nothing decides is refused, never matched by a
        // guess.
        out.objectCoordinates = meta.objectCoordinates;
        if (out.objectCoordinates.space == ObjectSpace::Unknown)
            return refuse("Record " + std::to_string(index) +
                          " cannot be matched: the file does not record whether its bounding boxes are relative to "
                          "its ROI (which starts at " + std::to_string(meta.roi.x) + ", " + std::to_string(meta.roi.y) +
                          ") or to the image, and its masks do not tell");
        if (settings == ReprocessSettings::Recorded)
        {
            // The recorded calibration is a recorded processing input: area
            // validity and the target group are in µm² (#756 review).
            out.processingPixelToMicron = out.pixelToMicron;
            services::ProcessingConfig recorded;
            if (impl_->reader.readRecordedProcessingConfig(recorded))
            {
                out.config = recorded;
                out.roi = meta.roi;
                cv::Mat background;
                if (impl_->reader.readBackgroundImage(background) && !background.empty())
                    out.background = grayLikeRegenerate(std::move(background));
                out.usedRecordedSettings = true;
                out.settingsLabel = out.background.empty() ? "recorded settings (config, ROI; the file records no background)"
                                                           : "recorded settings (config, ROI, background)";
            }
            else
            {
                out.settingsLabel = "default settings: the file records none";
            }
        }
        else
        {
            out.settingsLabel = "default settings (whole image, no background)";
        }
        if (!out.background.empty() && out.background.size() != out.image.size())
        {
            // As the regenerate engine refuses it: a background of another size.
            return refuse("The recorded background does not match the record's image size");
        }
        return true;
    }

    bool ReviewSession::framesPage(bool valid, std::uint64_t offset, std::uint64_t count,
                                   std::vector<services::ProcessedFrame> &frames, std::uint64_t &totalOut) const
    {
        std::scoped_lock lock(impl_->mutex);
        frames.clear();
        totalOut = 0;
        if (!impl_->open) return false;
        const auto &source = impl_->set(valid);
        totalOut = source.size();
        if (offset >= source.size()) return true;
        // Overflow-safe end (offset + count may wrap for a huge count).
        const std::uint64_t remaining = source.size() - offset;
        const std::uint64_t end = offset + std::min<std::uint64_t>(remaining, count);
        frames.assign(source.begin() + static_cast<std::ptrdiff_t>(offset),
                      source.begin() + static_cast<std::ptrdiff_t>(end));
        return true;
    }

    ReviewSession::ValidSet ReviewSession::validSet() const
    {
        std::scoped_lock lock(impl_->mutex);
        ValidSet out;
        if (!impl_->open) return out;
        out.open = true;
        out.filePath = impl_->path;
        out.recordingFile = impl_->recording;
        out.metadataReadable = impl_->validMetaReadable;
        out.frames = impl_->validMeta;
        return out;
    }

    bool ReviewSession::frameMeta(bool valid, std::uint64_t index, services::ProcessedFrame &out) const
    {
        std::scoped_lock lock(impl_->mutex);
        const auto &source = impl_->set(valid);
        if (!impl_->open || index >= source.size()) return false;
        out = source[index];
        return true;
    }

    bool ReviewSession::readRaw(ReviewDataset dataset, std::uint64_t index, cv::Mat &out) const
    {
        std::scoped_lock lock(impl_->mutex);
        out = cv::Mat();
        if (!impl_->open) return false;
        const char *path = datasetPath(dataset);
        if (!path) return false;
        cv::Mat image;
        if (!impl_->reader.readImageByIndex(path, static_cast<std::size_t>(index), image) || image.empty())
        {
            return false;
        }
        out = toGray(image);
        return true;
    }

    bool ReviewSession::fetchImage(ReviewDataset dataset, std::uint64_t index, OverlayMode mode,
                                   bool roiOverlay, ReviewImage &out) const
    {
        out = ReviewImage{};
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open || datasetPath(dataset) == nullptr) return false;
        cv::Mat composed;
        if (isMaskDataset(dataset) || dataset == ReviewDataset::RecordedImage)
        {
            const char *path = datasetPath(dataset);
            if (!path) return false;
            cv::Mat image;
            if (!impl_->reader.readImageByIndex(path, static_cast<std::size_t>(index), image) || image.empty())
            {
                return false;
            }
            composed = toGray(image).clone();
            if (roiOverlay && !isMaskDataset(dataset) && !impl_->recording)
            {
                drawRoiOverlay(composed, impl_->meta.roi, image.cols, image.rows);
            }
        }
        else
        {
            const bool valid = dataset == ReviewDataset::ValidImage;
            if (!impl_->composeFrameLocked(valid, index, mode, roiOverlay, composed)) return false;
        }
        packImage(composed, index, out);
        return true;
    }

    bool ReviewSession::seriesInfo(std::uint64_t index, std::uint64_t &count) const
    {
        std::scoped_lock lock(impl_->mutex);
        count = 0;
        if (!impl_->open || index >= impl_->validMeta.size()) return false;
        if (impl_->recording)
        {
            if (impl_->meta.multiImageEnabled && impl_->meta.multiImageCount > 1)
            {
                const std::uint64_t available = impl_->validMeta.size() - index;
                count = std::min<std::uint64_t>(impl_->meta.multiImageCount, available);
                if (count <= 1) count = 0;
            }
            return true;
        }
        if (impl_->meta.hasSeries) count = impl_->meta.seriesCount;
        return true;
    }

    bool ReviewSession::fetchSeriesImage(std::uint64_t index, std::uint64_t k, OverlayMode mode,
                                         bool roiOverlay, ReviewImage &out) const
    {
        out = ReviewImage{};
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open) return false;
        std::vector<cv::Mat> images;
        if (!impl_->readSeriesLocked(index, images) || k >= images.size()) return false;
        cv::Mat mask;
        const std::string maskPath = impl_->masksPath(true);
        if (mode != OverlayMode::None && !maskPath.empty())
        {
            impl_->reader.readImageByIndex(maskPath, static_cast<std::size_t>(index), mask);
            mask = toGray(mask);
        }
        cv::Mat composed = composeOverlay(images[k], mask, &impl_->validMeta[index].validation,
                                          impl_->recording ? OverlayMode::None : mode);
        if (roiOverlay && !impl_->recording) drawRoiOverlay(composed, impl_->meta.roi, images[k].cols, images[k].rows);
        packImage(composed, index, out);
        return !out.data.empty();
    }

    bool ReviewSession::thumbnails(bool valid, std::uint64_t offset, std::uint64_t count, std::uint32_t size,
                                   OverlayMode mode, bool roiOverlay, ThumbnailStrip &out) const
    {
        out = ThumbnailStrip{};
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open || size == 0 || size > 1024) return false;
        const auto &frames = impl_->set(valid);
        out.valid = true;
        out.offset = offset;
        out.size = size;
        const bool colour = (!impl_->recording && (mode != OverlayMode::None || roiOverlay));
        out.channels = colour ? 3 : 1;
        if (offset >= frames.size()) return true;
        const std::uint64_t end = std::min<std::uint64_t>(frames.size(), offset + count);
        const std::size_t tileBytes = static_cast<std::size_t>(size) * size * out.channels;
        out.data.resize(tileBytes * static_cast<std::size_t>(end - offset));
        for (std::uint64_t i = offset; i < end; ++i)
        {
            cv::Mat composed;
            cv::Mat tile;
            if (impl_->composeFrameLocked(valid, i, mode, roiOverlay, composed))
            {
                tile = letterbox(composed, static_cast<int>(size), static_cast<int>(out.channels));
            }
            else
            {
                tile = cv::Mat::zeros(static_cast<int>(size), static_cast<int>(size),
                                      out.channels == 3 ? CV_8UC3 : CV_8UC1);
            }
            std::memcpy(out.data.data() + static_cast<std::size_t>(i - offset) * tileBytes, tile.data, tileBytes);
            ++out.count;
        }
        return true;
    }

    ScatterData ReviewSession::scatter() const
    {
        std::scoped_lock lock(impl_->mutex);
        ScatterData data;
        if (!impl_->open || impl_->recording) return data;
        const double f = impl_->factor();
        data.pixelToMicron = f;
        const double areaFactor = f * f;
        for (std::size_t i = 0; i < impl_->validMeta.size(); ++i)
        {
            const auto &frame = impl_->validMeta[i];
            if (!frame.validation.isValid) continue;
            data.frameIndex.push_back(frame.index);
            data.validPosition.push_back(i);
            data.areaUm2.push_back(frame.validation.area * areaFactor);
            data.deformability.push_back(frame.validation.deformability);
            data.targetGroup.push_back(frame.validation.isTargetGroup ? 1 : 0);
            data.ringRatio.push_back(frame.validation.ringRatio);
        }
        return data;
    }

    std::string ReviewSession::accountingSummary() const
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open) return {};
        if (!impl_->meta.hasAccounting) return " · accounting: not recorded (legacy file)";
        const auto &a = impl_->meta.accounting;
        if (!a.readError.empty()) return " · run unknown — " + a.completionReason;
        const std::uint64_t storeLoss = a.storeOverwritten + a.storeNotCommitted + a.storeMalformed;
        std::ostringstream text;
        text << " · run " << recording::toString(a.completion);
        if (!a.reconciled) text << " (accounting does not reconcile)";
        text << " — empty " << a.empty << ", rejected " << a.scientificallyRejected << ", processing failed "
             << a.processingFailed << ", store loss " << storeLoss << ", persisted " << a.persistenceCommitted
             << "/" << a.persistenceAdmitted << ", persistence failed " << a.persistenceFailed;
        return text.str();
    }

    bool ReviewSession::saveCoreRecordJson(const std::string &json, bool overwrite, std::string *error)
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open)
        {
            if (error) *error = "No file open";
            return false;
        }
        if (impl_->recording)
        {
            if (error) *error = "Recording files carry no analysis record";
            return false;
        }
        if (!overwrite && !impl_->meta.kdeAnalysisJson.empty())
        {
            if (error) *error = "A full-run core record already exists";
            return false;
        }
        const std::string path = impl_->path;
        impl_->reader.closeFile();
        impl_->open = false;
        bool ok = false;
        {
            services::Hdf5Service updater;
            if (!updater.openFileForUpdate(path))
            {
                if (error) *error = "File is read-only or in use: " + path;
            }
            else
            {
                ok = updater.writeKdeAnalysisJson(json);
                if (!ok && error) *error = "Failed to write the core record";
                updater.closeFile();
            }
        }
        // Reopen all or nothing, as open().
        if (!impl_->loadLocked(path, nullptr))
        {
            impl_->resetLocked();
            if (error) *error = "Failed to reopen " + path;
            return false;
        }
        return ok;
    }

    bool ReviewSession::loadFrameForDisplay(bool valid, std::uint64_t index, services::ProcessedFrame &out) const
    {
        std::scoped_lock lock(impl_->mutex);
        const auto &frames = impl_->set(valid);
        if (!impl_->open || index >= frames.size()) return false;
        out = frames[index];
        cv::Mat original;
        if (impl_->reader.readImageByIndex(impl_->imagesPath(valid), static_cast<std::size_t>(index), original))
        {
            out.originalImage = original;
        }
        const std::string maskPath = impl_->masksPath(valid);
        if (!maskPath.empty())
        {
            cv::Mat mask;
            if (impl_->reader.readImageByIndex(maskPath, static_cast<std::size_t>(index), mask)) out.processedImage = mask;
        }
        if (valid)
        {
            std::vector<cv::Mat> series;
            if (impl_->readSeriesLocked(index, series) && !series.empty()) out.seriesImages = std::move(series);
        }
        return !out.originalImage.empty();
    }

} // namespace backend::review
