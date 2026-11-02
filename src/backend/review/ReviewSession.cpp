#include "backend/analysis/EvidenceGallery.h"
#include "backend/review/ReviewSession.h"
#include "backend/analysis/ColumnarEvaluation.h"
#include "backend/analysis/PopulationGates.h"
#include "backend/analysis/PopulationStatistics.h"
#include "backend/analysis/PopulationGraph.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/review/RecordingColumns.h"
#include "backend/processing/ProcessingCoreSha256.h"
#include <atomic>

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
#include <list>
#include <set>
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

        // F-1b: the stored measurements of an experiment file as a columnar
        // source handle (F-source/2). Identity and units are those the
        // F-source/1 row form always had, so saved bindings stay valid.
        nlohmann::json storedColumnarDescriptor(const std::string &path, const std::string &fileHash,
                                                std::uint64_t validRows, std::uint64_t invalidRows)
        {
            using Json = nlohmann::json;
            return Json{{"schema", analysis::kColumnarSourceSchema},
                        {"storage", "columnar"},
                        {"sourceKey", "sha256:" + fileHash},
                        {"fingerprint", "sha256:" + fileHash},
                        {"fingerprintKind", "full-file-sha256"},
                        {"scope", "stored"},
                        {"path", path},
                        {"parameters", {{"area", "pixel2"}, {"deformability", "dimensionless"}, {"youngsModulus", "kPa"}}},
                        {"segments", Json::array({Json{{"set", "valid"}, {"count", std::to_string(validRows)}},
                                                  Json{{"set", "invalid"}, {"count", std::to_string(invalidRows)}}})},
                        {"events", std::to_string(validRows + invalidRows)},
                        {"populationComplete", true},
                        {"finalized", true},
                        {"retention",
                         "file-pinned: columns are re-read from the file and its full-file SHA-256 "
                         "re-verified on every evaluation; no rows are copied"},
                        {"provenance", {{"processing", nullptr}, {"calibration", nullptr}, {"mechanics", nullptr},
                            {"semantics", "stored measurements; no recomputation or compatibility inferred from missing provenance"}}}};
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
        std::filesystem::file_time_type openedWriteTime{};
        std::uintmax_t openedSize{0};
        // F-1b: browse metadata is paged, not held (a 300k-row file cost
        // ~174 MiB of ProcessedFrame rows at open). The row counts are read at
        // open; rows are read on demand in kMetaPageRows pages, the last
        // kMetaPagesCached kept (LRU), and full passes stream page by page.
        static constexpr std::uint64_t kMetaPageRows = 1024;
        static constexpr std::size_t kMetaPagesCached = 8;
        static constexpr std::uint64_t kMetaScanRows = 4096;
        std::uint64_t validRows{0};
        std::uint64_t invalidRows{0};
        struct MetaPage
        {
            bool valid{true};
            std::uint64_t page{0};
            std::vector<services::ProcessedFrame> rows;
        };
        mutable std::list<MetaPage> metaPages;
        bool validMetaReadable{false};
        bool invalidMetaReadable{false};
        std::vector<std::string> validFields, invalidFields;
        bool validFieldsReadable{false}, invalidFieldsReadable{false};
        ReviewMetadata meta;
        double fallbackFactor{0.4886};
        // F-1: the open generation's full-file SHA-256 (streamed, any size),
        // computed once on demand, and the native membership handles of the
        // last evaluations, keyed to source identity + graph revision.
        std::uint64_t openGeneration{0};
        std::string cachedFileHash;
        struct CachedMembership
        {
            std::string key;
            std::shared_ptr<const analysis::MembershipSet> membership;
        };
        std::vector<CachedMembership> memberships;
        static constexpr std::size_t kMembershipCacheEntries = 4;
        // F-1b: full-file SHA-256 of files lent as columns while another file
        // is open (retained file-pinned sources), keyed to path + size + mtime.
        struct ForeignHash
        {
            std::string path;
            std::uintmax_t size{0};
            std::filesystem::file_time_type time{};
            std::string hash;
        };
        std::vector<ForeignHash> foreignHashes;

        std::uint64_t rowCount(bool valid) const { return valid ? validRows : invalidRows; }

        bool readRangeLocked(bool valid, std::uint64_t start, std::uint64_t count,
                             std::vector<services::ProcessedFrame> &out) const
        {
            if (recording) return valid && reader.readRecordingMetadataRange(start, count, out);
            return reader.readMetadataRange(valid, start, count, out);
        }

        // One stored row (no pixels) through the page cache; nullptr when the
        // index is outside the set or its page cannot be read. The pointer is
        // valid until the next rowLocked call that loads a page.
        const services::ProcessedFrame *rowLocked(bool valid, std::uint64_t index) const
        {
            if (index >= rowCount(valid)) return nullptr;
            const std::uint64_t page = index / kMetaPageRows;
            const auto offset = static_cast<std::size_t>(index - page * kMetaPageRows);
            for (auto it = metaPages.begin(); it != metaPages.end(); ++it)
                if (it->valid == valid && it->page == page)
                {
                    metaPages.splice(metaPages.begin(), metaPages, it);
                    return offset < metaPages.front().rows.size() ? &metaPages.front().rows[offset] : nullptr;
                }
            MetaPage loaded;
            loaded.valid = valid;
            loaded.page = page;
            if (!readRangeLocked(valid, page * kMetaPageRows, kMetaPageRows, loaded.rows)) return nullptr;
            metaPages.push_front(std::move(loaded));
            while (metaPages.size() > kMetaPagesCached) metaPages.pop_back();
            return offset < metaPages.front().rows.size() ? &metaPages.front().rows[offset] : nullptr;
        }

        // Every row of a set in order, kMetaScanRows at a time, without
        // caching; false when a page cannot be read or `visit` stops.
        template <typename Visit>
        bool scanRowsLocked(bool valid, Visit &&visit) const
        {
            std::vector<services::ProcessedFrame> chunk;
            const std::uint64_t total = rowCount(valid);
            for (std::uint64_t start = 0; start < total; start += kMetaScanRows)
            {
                if (!readRangeLocked(valid, start, kMetaScanRows, chunk) || chunk.empty()) return false;
                for (std::size_t i = 0; i < chunk.size() && start + i < total; ++i)
                    if (!visit(start + i, chunk[i])) return false;
            }
            return true;
        }

        // The open set's row count, verified readable: the whole compound
        // row type on the first page and every row of the analysis members
        // in bounded chunks (what readValidMetadata's one read proved).
        bool verifyRowsLocked(bool valid, std::uint64_t count) const
        {
            if (count == 0) return true;
            std::vector<services::ProcessedFrame> first;
            if (!reader.readMetadataRange(valid, 0, kMetaPageRows, first) || first.empty()) return false;
            std::uint64_t seen = 0;
            const bool ok = reader.readMetadataColumns(
                valid, 0, [&](const services::Hdf5Service::MetadataColumnRow *, std::size_t n) {
                    seen += n;
                    return true;
                });
            return ok && seen == count;
        }

        // Streaming SHA-256 of the open file (no size cap), fenced against
        // change since open; cached for the open generation.
        // Stat fence: the open file still has the size and mtime recorded
        // at open. Every source-bound read (fingerprint, cached membership,
        // columnar scan) checks it, so an in-place rewrite is never mixed
        // with state derived from the earlier content.
        bool unchangedLocked() const
        {
            std::error_code sizeError, timeError;
            const auto size = std::filesystem::file_size(path, sizeError);
            const auto time = std::filesystem::last_write_time(path, timeError);
            return !sizeError && !timeError && size == openedSize && time == openedWriteTime;
        }

        std::string fileHashLocked()
        {
            const auto unchanged = [&] { return unchangedLocked(); };
            if (!unchanged()) throw std::invalid_argument("Source changed since open; reopen before source-bound analysis");
            if (!cachedFileHash.empty()) return cachedFileHash;
            std::string error;
            auto hash = processing::fileSha256(path, &error, {});
            if (hash.empty()) throw std::invalid_argument("Source fingerprint failed: " + error);
            if (!unchanged()) throw std::invalid_argument("Source changed while fingerprinting");
            cachedFileHash = hash;
            return cachedFileHash;
        }

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
            validRows = 0;
            invalidRows = 0;
            metaPages.clear();
            validMetaReadable = true;
            if (recording)
            {
                std::uint64_t total = 0;
                reader.readRecordingInfo(meta.startTimeNs, meta.endTimeNs, total, meta.filteredFrames,
                                         &meta.multiImageEnabled, &meta.multiImageCount);
                if (meta.multiImageCount == 0) meta.multiImageCount = 1;
                validRows = reader.recordingMetadataRowCount().value_or(0);
                meta.totalValid = total;
            }
            else
            {
                std::size_t totalValid = 0;
                std::size_t totalInvalid = 0;
                reader.readExperimentInfo(meta.startTimeNs, meta.endTimeNs, totalValid, totalInvalid, &meta.roi);
                meta.totalValid = totalValid;
                meta.totalInvalid = totalInvalid;
                // An absent valid set is empty, not unreadable (the facade's
                // charts rule before #512 S5, when they opened their own reader).
                // F-1b: counted and verified readable here; rows are paged.
                const auto readable = [&](bool valid, std::uint64_t &rows) {
                    const auto present = reader.metadataDatasetPresent(valid);
                    if (!present.has_value()) return false;
                    if (!*present) return true;
                    const auto count = reader.metadataRowCount(valid);
                    if (!count || !verifyRowsLocked(valid, *count)) return false;
                    rows = *count;
                    return true;
                };
                validMetaReadable = readable(true, validRows);
                invalidMetaReadable = readable(false, invalidRows);
                validFields.clear(); invalidFields.clear();
                validFieldsReadable = reader.readMetadataFieldNames(true, validFields);
                invalidFieldsReadable = reader.readMetadataFieldNames(false, invalidFields);
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
            meta.eModulusLutRecord = recording::readFileEModulusLut(reader);
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
                const std::uint64_t n = std::min<std::uint64_t>(rowCount(valid), masks.count);
                std::vector<std::size_t> candidates;
                scanRowsLocked(valid, [&](std::uint64_t i, const services::ProcessedFrame &frame) {
                    if (i >= n) return false;
                    const auto &v = frame.validation;
                    if (v.bboxWidth > 0 && v.bboxHeight > 0) candidates.push_back(static_cast<std::size_t>(i));
                    return true;
                });
                for (const std::size_t position : maskCheckSample(candidates.size(), room))
                {
                    const std::size_t index = candidates[position];
                    cv::Mat mask;
                    if (!reader.readImageByIndex(masksPath(valid), index, mask) || mask.empty())
                    {
                        votes.emplace_back(std::nullopt);
                        continue;
                    }
                    const auto *row = rowLocked(valid, index);
                    if (!row)
                    {
                        votes.emplace_back(std::nullopt);
                        continue;
                    }
                    const auto v = row->validation;
                    StoredObject object;
                    object.box = cv::Rect2d(v.bboxX, v.bboxY, v.bboxWidth, v.bboxHeight);
                    object.areaPx = v.area;
                    object.hole = innerContours && v.innerContourCount > 0;
                    votes.push_back(maskPlacementVote(toGray(mask), object, meta.roi.x, meta.roi.y));
                }
            }
            return maskCheckVerdict(votes);
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
            const auto *row = rowLocked(valid, index);
            if (!row) return false;
            const auto validation = row->validation;
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
            out = composeOverlay(original, mask, &validation, recording ? OverlayMode::None : mode);
            if (roiOverlay && !recording) drawRoiOverlay(out, meta.roi, original.cols, original.rows);
            return !out.empty();
        }

        bool readSeriesLocked(std::uint64_t index, std::vector<cv::Mat> &images) const
        {
            images.clear();
            if (index >= validRows) return false;
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
            validRows = 0;
            invalidRows = 0;
            metaPages.clear();
            validMetaReadable = false;
            cachedFileHash.clear();
            memberships.clear();
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
                const auto sizeBefore = std::filesystem::file_size(filePath);
                const auto timeBefore = std::filesystem::last_write_time(filePath);
                if (!reader.loadFile(filePath))
                {
                    if (error) *error = "File not found or not an HDF5 review file: " + filePath;
                    return false;
                }
                open = true;
                path = filePath;
                ++openGeneration;
                cachedFileHash.clear();
                memberships.clear();
                recording = reader.isRecordingFile();
                readMetadataLocked();
                openedSize = std::filesystem::file_size(filePath);
                openedWriteTime = std::filesystem::last_write_time(filePath);
                if (openedSize != sizeBefore || openedWriteTime != timeBefore)
                    throw std::runtime_error("Source changed while opening");
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
                    impl_->recording ? "recording" : "experiment", impl_->validRows,
                    impl_->invalidRows, impl_->factor(),
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
        // objects were validated at (#758), when it is the run's own (#764):
        // recorded from the source or supplied by the operator. A fallback
        // factor (the source recorded none) is not the file's calibration.
        // Files regenerated before #764 have no source attribute and read as
        // before.
        const auto source = reader.readAnalysisPixelToMicronSource();
        if (source && *source != services::Hdf5Service::kPixelToMicronSourceRecorded &&
            *source != services::Hdf5Service::kPixelToMicronSourceOperator)
            return 0.0;
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
        // The E-LUT record of the stored E (review ABI 5, #783): the
        // snapshot's own record wins; else a regenerated file's
        // `@emodulus_lut_json` with its provenance (recorded | host), the
        // order of recording::readFileEModulusLut. A file without either (a
        // capture before v3, a file regenerated before #764) reads absent.
        if (out.eModulusLut)
        {
            out.eModulusLutProvenance = kEModulusLutProvenanceSnapshot;
        }
        else if (const auto attribute = reader.readEModulusLutJson())
        {
            if (auto record = recording::parseEModulusLutRecordJson(*attribute))
            {
                out.eModulusLut = std::move(record->lut);
                out.eModulusLutProvenance = std::move(record->provenance);
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

    RunStatisticsResult ReviewSession::runStatistics(const std::string &path, const std::string &computedAt) const
    {
        // The session's own lock (#722), as probeRuns: no session read, open or
        // close interleaves with this file's HDF5 calls.
        std::scoped_lock lock(impl_->mutex);
        return readRunStatistics(path, computedAt);
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
        return impl_->rowCount(valid);
    }

    void ReviewSession::withRecordingColumns(const std::string& path, const std::string& expectedFingerprint,
                                             const std::function<void(const analysis::EventColumns&)>& use) const {
        const auto refuse = [](const std::string& message) { throw std::invalid_argument(message); };
        std::scoped_lock lock(impl_->mutex);
        if (impl_->open && path == impl_->path) {
            if (impl_->recording) refuse("A raw recording has no stored measurements");
            if (!impl_->validMetaReadable || !impl_->invalidMetaReadable)
                refuse("Object metadata is unreadable; incomplete populations are not analyzed");
            if (impl_->meta.totalValid != impl_->validRows || impl_->meta.totalInvalid != impl_->invalidRows)
                refuse("Stored counts and readable rows disagree; source coverage is incomplete");
            if ("sha256:" + impl_->fileHashLocked() != expectedFingerprint)
                refuse("Source fingerprint changed; refresh the source explicitly");
            RecordingColumns columns(impl_->reader, impl_->validRows, impl_->invalidRows, impl_->validFields,
                                     impl_->invalidFields);
            use(columns);
            (void)impl_->fileHashLocked(); // throws when the file changed during the read
            return;
        }
        // Another file (a retained, file-pinned source): its own reader.
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) refuse("Pinned source file is unavailable: " + path);
        const auto time = std::filesystem::last_write_time(path, ec);
        if (ec) refuse("Pinned source file is unavailable: " + path);
        std::string hash;
        auto& hashes = impl_->foreignHashes;
        for (const auto& entry : hashes)
            if (entry.path == path && entry.size == size && entry.time == time) hash = entry.hash;
        if (hash.empty()) {
            std::string error;
            hash = processing::fileSha256(path, &error, {});
            if (hash.empty()) refuse("Source fingerprint failed: " + error);
            hashes.insert(hashes.begin(), Impl::ForeignHash{path, size, time, hash});
            if (hashes.size() > 8) hashes.pop_back();
        }
        if ("sha256:" + hash != expectedFingerprint)
            refuse("Pinned source file no longer matches its fingerprint: " + path);
        services::Hdf5Service reader;
        struct Close {
            services::Hdf5Service& reader;
            ~Close() { reader.closeFile(); }
        } close{reader};
        if (!reader.loadFile(path)) refuse("Pinned source file cannot be opened: " + path);
        if (reader.isRecordingFile()) refuse("A raw recording has no stored measurements");
        std::uint64_t start = 0, end = 0;
        std::size_t totalValid = 0, totalInvalid = 0;
        reader.readExperimentInfo(start, end, totalValid, totalInvalid);
        const auto validRows = reader.metadataRowCount(true);
        const auto invalidRows = reader.metadataRowCount(false);
        if (!validRows || !invalidRows)
            refuse("Object metadata is unreadable; incomplete populations are not analyzed");
        if (totalValid != *validRows || totalInvalid != *invalidRows)
            refuse("Stored counts and readable rows disagree; source coverage is incomplete");
        std::vector<std::string> validFields, invalidFields;
        if ((*validRows && !reader.readMetadataFieldNames(true, validFields)) ||
            (*invalidRows && !reader.readMetadataFieldNames(false, invalidFields)))
            refuse("Measurement schema is unreadable");
        RecordingColumns columns(reader, *validRows, *invalidRows, validFields, invalidFields);
        use(columns);
        if (std::filesystem::file_size(path, ec) != size || ec ||
            std::filesystem::last_write_time(path, ec) != time || ec)
            refuse("Pinned source file changed during the read");
    }

    std::string ReviewSession::analyzePopulation(const std::string& requestJson) const {
        using namespace backend::analysis;
        using Json = nlohmann::json;
        const auto refuse = [](const std::string& message) {
            return Json{{"ok", false}, {"message", message}}.dump();
        };
        try {
            if (requestJson.size() > 65536) return refuse("Analysis request exceeds 65536 bytes");
            const auto request = Json::parse(requestJson);
            static const std::set<std::string> optional{
                "graph", "galleryOffset", "snapshot", "sourceKind", "galleryKeys",
                "expectedFingerprint", "expectedFileFingerprint", "mask",
                // Review ABI 10 (F-1): bounded columnar results.
                "page", "histogram", "scatter", "fingerprint",
                // Review ABI 10, additive (F-1b): the F-source/2 handle.
                "columnar"};
            if (!request.is_object() || !request.contains("feature") || !request.contains("minimum") ||
                !request.contains("maximum") || !request.contains("expectedPath"))
                return refuse("Expected feature, minimum, maximum and expectedPath");
            for (auto it = request.begin(); it != request.end(); ++it)
                if (it.key() != "feature" && it.key() != "minimum" && it.key() != "maximum" &&
                    it.key() != "expectedPath" && !optional.count(it.key()))
                    return refuse("Expected feature, minimum, maximum and expectedPath");
            static const std::set<std::string> features{"area", "deformability", "youngsModulus"};
            const auto feature = request.at("feature").get<std::string>();
            if (!features.count(feature))
                return refuse("Supported features: stored area (px²), deformability and Young's modulus (kPa)");
            Interval interval;
            if (!request.at("minimum").is_null())
                interval.minimum = request.at("minimum").get<double>();
            if (!request.at("maximum").is_null())
                interval.maximum = request.at("maximum").get<double>();
            validateInterval(interval);
            std::optional<std::uint64_t> galleryOffset;
            if (request.contains("galleryOffset")) {
                if (!request.at("galleryOffset").is_number_unsigned())
                    return refuse("Invalid gallery offset");
                galleryOffset = request.at("galleryOffset").get<std::uint64_t>();
            }
            // The graph (or the top-level interval as a one-node graph) is
            // compiled by the one F graph compiler before any source access.
            const Json graphJson = request.contains("graph")
                ? request.at("graph")
                : Json{{"id", "interval"}, {"revision", "1"}, {"output", "interval"},
                       {"nodes", Json::array({Json{{"id", "interval"}, {"parent", ""}, {"kind", "interval"},
                                                   {"feature", feature}, {"minimum", request.at("minimum")},
                                                   {"maximum", request.at("maximum")}}})}};
            const auto graph = compileGraph(graphJson);
            for (const auto& f : gateFeatures(graph))
                if (!features.count(f)) return refuse("Unsupported graph feature");
            // One lock covers source identity, both sets, reduction and optional
            // image copies through the existing reader. No acquisition calls.
            std::scoped_lock lock(impl_->mutex);
            if (!impl_->open) return refuse("Open a recording before analysis");
            if (request.at("expectedPath").get<std::string>() != impl_->path)
                return refuse("The open source changed; refresh before analyzing");
            if (request.value("sourceKind", std::string("stored")) == "run-archive") {
                const auto archive =
                    impl_->reader.readScalarStringAttribute("/monitoring", "run_evidence_json");
                if (!archive || archive->size() > 4 * 1024 * 1024)
                    return refuse("Run event archive absent or exceeds 4 MiB budget");
                auto source = adaptEvidenceArchive(Json::parse(*archive));
                source["scope"] = "run-archive";
                source["path"] = impl_->path;
                // Streaming full-file SHA-256: any file size (F01).
                source["fileFingerprint"] = "sha256:" + impl_->fileHashLocked();
                if (request.contains("galleryKeys")) {
                    if (request.at("expectedFingerprint") != source.at("fingerprint") ||
                        request.at("expectedFileFingerprint") != source.at("fileFingerprint"))
                        return refuse("Archive gallery source/file fingerprint changed");
                    const bool mask = request.value("mask", false);
                    auto gallery =
                        evidenceGallery(source, request.at("galleryKeys"), mask,
                                        [&](const std::string& set, const std::string& ordinal) {
                                            return impl_->reader.readRunEvidenceImage(
                                                source.at("sourceKey"), set, ordinal, mask);
                                        });
                    if ("sha256:" + impl_->fileHashLocked() != source.at("fileFingerprint"))
                        return refuse("Archive changed during gallery read");
                    gallery["fileFingerprint"] = source.at("fileFingerprint");
                    return Json{{"ok", true}, {"source", gallery}}.dump();
                }
                return Json{{"ok", true}, {"source", source}}.dump();
            }
            if (request.value("sourceKind", std::string("stored")) != "stored")
                return refuse("Unknown source kind");
            if (impl_->recording) return refuse("Open an experiment file with object metadata");
            if (!impl_->validMetaReadable || !impl_->invalidMetaReadable)
                return refuse(
                    "Object metadata is unreadable; incomplete populations are not analyzed");
            // Never certify missing observations as a complete file population.
            if (impl_->meta.totalValid != impl_->validRows ||
                impl_->meta.totalInvalid != impl_->invalidRows)
                return refuse(
                    "Stored counts and readable rows disagree; source coverage is incomplete");
            for (const bool valid : {true, false})
                if (impl_->rowCount(valid) != 0 &&
                    !(valid ? impl_->validFieldsReadable : impl_->invalidFieldsReadable))
                    return refuse("Measurement schema is unreadable");
            // Review ABI 10, additive (F-1b): `columnar` returns the source as an
            // F-source/2 handle (no rows) for the analysis host.
            const bool columnarHandle = request.value("columnar", false);
            const bool freeze = request.value("snapshot", false) || columnarHandle;
            const bool wantFingerprint = freeze || request.value("fingerprint", false);
            std::string fileHash;
            if (wantFingerprint) {
                try {
                    fileHash = impl_->fileHashLocked();
                } catch (const std::invalid_argument& e) {
                    return refuse(e.what());
                }
            }
            if (request.contains("galleryKeys")) {
                if (!freeze || request.at("expectedFingerprint") != "sha256:" + fileHash)
                    return refuse("Gallery source fingerprint changed");
                const auto& keys = request.at("galleryKeys");
                if (!keys.is_array() || keys.size() > 12) return refuse("Gallery requires at most 12 exact keys");
                Json images = Json::array();
                for (const auto& value : keys) {
                    const auto key = value.get<std::string>();
                    const auto ref = Json::parse(key);
                    if (!ref.is_array() || ref.size() != 3 || ref.at(0) != "sha256:" + fileHash ||
                        (ref.at(1) != "valid" && ref.at(1) != "invalid")) return refuse("Foreign gallery event key");
                    const auto ordinalText = ref.at(2).get<std::string>();
                    const auto ordinal = std::stoull(ordinalText);
                    if (std::to_string(ordinal) != ordinalText) return refuse("Invalid gallery ordinal");
                    const bool valid = ref.at(1) == "valid";
                    if (ordinal >= impl_->rowCount(valid)) return refuse("Gallery ordinal outside source");
                    Json item{{"key", key}, {"set", ref.at(1)}, {"ordinal", ordinalText}, {"pixels", nullptr}};
                    cv::Mat image;
                    if (impl_->composeFrameLocked(valid, ordinal, OverlayMode::None, false, image) && !image.empty()) {
                        const auto tile = letterbox(image, 64, 1);
                        item["pixels"] = std::vector<unsigned char>(tile.data, tile.data + tile.total());
                    }
                    images.push_back(std::move(item));
                }
                return Json{{"ok", true}, {"source", {{"schema", "F-gallery/1"},
                    {"fingerprint", "sha256:" + fileHash}, {"images", images}}}}.dump();
            }
            if (columnarHandle)
                return Json{{"ok", true},
                            {"source", storedColumnarDescriptor(impl_->path, fileHash, impl_->validRows,
                                                                impl_->invalidRows)}}.dump();
            if (freeze) {
                // F-source/1 is a JSON row transport with an explicit bound;
                // larger recordings are analysed natively (columnar handle).
                if (impl_->validRows + impl_->invalidRows > kMaxJsonSourceEvents)
                    return refuse(jsonSourceBoundMessage());
                RecordingColumns stored(impl_->reader, impl_->validRows, impl_->invalidRows,
                                        impl_->validFields, impl_->invalidFields);
                return Json{{"ok", true},
                            {"source", materializeColumnarSource(storedColumnarDescriptor(
                                           impl_->path, fileHash, impl_->validRows, impl_->invalidRows),
                                       stored)}}.dump();
            }

            // ---- F-1 columnar evaluation of the whole file ------------------
            // Columns are read from the HDF5 metadata in bounded chunks; the
            // population DAG is evaluated chunk by chunk into two-bit
            // membership; only bounded results leave this function.
            const ResourceBounds bounds{};
            RecordingColumns columns(impl_->reader, impl_->validRows, impl_->invalidRows,
                                     impl_->validFields, impl_->invalidFields);
            const auto graphRevision = contentHash(graphJson);
            const std::string sourceIdentity =
                fileHash.empty()
                    ? "open:" + std::to_string(impl_->openGeneration) + ":" +
                          std::to_string(impl_->openedSize) + ":" +
                          std::to_string(static_cast<long long>(
                              impl_->openedWriteTime.time_since_epoch().count())) +
                          ":" + impl_->path
                    : "sha256:" + fileHash;
            const RevisionKey revision{sourceIdentity,
                                       fileHash.empty() ? std::string("unavailable") : "sha256:" + fileHash,
                                       graphRevision,
                                       "single-file",
                                       "none",
                                       "stored-metadata/1",
                                       "none",
                                       "not-used",
                                       "native-columnar/1"};
            const auto cacheKey = sourceIdentity + "|" + graphRevision;
            // The identity above names the size/mtime recorded at open; re-stat
            // before trusting any cached membership or reading columns. A
            // changed file drops every cached membership of this generation.
            const auto sourceChanged = [&] {
                if (impl_->unchangedLocked()) return false;
                impl_->memberships.clear();
                return true;
            };
            if (sourceChanged())
                return refuse("Source changed since open; reopen before source-bound analysis");
            std::shared_ptr<const MembershipSet> membership;
            bool cached = false;
            auto& cache = impl_->memberships;
            for (auto it = cache.begin(); it != cache.end(); ++it)
                if (it->key == cacheKey) {
                    membership = it->membership;
                    std::rotate(cache.begin(), it, it + 1); // most recent first
                    cached = true;
                    break;
                }
            if (!membership) {
                membership = evaluateColumns(graph, columns, revision, nullptr, bounds);
                cache.insert(cache.begin(), {cacheKey, membership});
                std::size_t held = 0, keep = 0;
                while (keep < cache.size() && keep < Impl::kMembershipCacheEntries &&
                       held + cache[keep].membership->bytes() <= bounds.maxMembershipBytes)
                    held += cache[keep++].membership->bytes();
                cache.resize(std::max<std::size_t>(keep, 1));
            }
            const auto& population = membership->population(graph.output);
            const auto statistics = columnStatistics(population, columns, feature, revision, bounds);
            if (sourceChanged())
                return refuse("Source changed during analysis; reopen before source-bound analysis");
            static std::atomic<std::uint64_t> next{0};
            const auto snapshot = std::string("local-evaluation:") + std::to_string(++next);
            const auto membershipName = [](Truth t) {
                return t == Truth::True ? "selected" : t == Truth::False ? "excluded" : "unknown";
            };
            const auto eventJson = [&](std::uint64_t index) {
                const auto ref = eventReference(membership->segments, index);
                const auto ordinal = std::to_string(ref.second);
                return Json{{"key", snapshot + ":" + ref.first + ":" + ordinal},
                            {"set", ref.first},
                            {"ordinal", ordinal}};
            };
            // A bounded page of event references (replaces every-event rows).
            std::uint64_t pageOffset = 0, pageLimit = 256;
            std::string pageFilter = "all";
            if (request.contains("page")) {
                const auto& page = request.at("page");
                if (!page.is_object() || !page.contains("offset") || !page.contains("limit") ||
                    page.size() != 2 + page.count("membership") ||
                    !page.at("offset").is_number_unsigned() || !page.at("limit").is_number_unsigned())
                    return refuse("page requires unsigned offset and limit, optional membership");
                pageOffset = page.at("offset").get<std::uint64_t>();
                pageLimit = page.at("limit").get<std::uint64_t>();
                pageFilter = page.value("membership", std::string("all"));
                if (pageLimit > 1000) return refuse("page limit is at most 1000 events");
                if (pageFilter != "all" && pageFilter != "selected" && pageFilter != "excluded" &&
                    pageFilter != "unknown")
                    return refuse("page membership is all, selected, excluded or unknown");
            }
            Json rows = Json::array();
            std::uint64_t rowsTotal = membership->events;
            if (pageFilter == "all") {
                for (std::uint64_t i = pageOffset; i < membership->events && rows.size() < pageLimit; ++i) {
                    auto row = eventJson(i);
                    row["membership"] = membershipName(population.get(i));
                    rows.push_back(std::move(row));
                }
            } else {
                const Truth wanted = pageFilter == "selected" ? Truth::True
                                     : pageFilter == "excluded" ? Truth::False
                                                                : Truth::Unknown;
                rowsTotal = population.count(wanted);
                if (pageLimit > 0)
                    if (const auto first = population.find(wanted, pageOffset))
                        for (std::uint64_t i = *first; i < membership->events && rows.size() < pageLimit; ++i)
                            if (population.get(i) == wanted) {
                                auto row = eventJson(i);
                                row["membership"] = pageFilter;
                                rows.push_back(std::move(row));
                            }
            }
            Json gallery = Json::array();
            if (galleryOffset) {
                if (const auto first = population.find(Truth::True, *galleryOffset))
                    for (std::uint64_t i = *first; i < membership->events && gallery.size() < 12; ++i) {
                        if (population.get(i) != Truth::True) continue;
                        auto item = eventJson(i);
                        item["pixels"] = nullptr;
                        const bool valid = item["set"] == "valid";
                        const auto ordinal = eventReference(membership->segments, i).second;
                        cv::Mat image;
                        if (impl_->composeFrameLocked(valid, ordinal, OverlayMode::None, false, image) &&
                            !image.empty()) {
                            const auto tile = letterbox(image, 64, 1);
                            item["pixels"] = std::vector<unsigned char>(tile.data, tile.data + tile.total());
                        }
                        gallery.push_back(std::move(item));
                    }
            }
            const auto number = [](const std::optional<double>& v) -> Json {
                return v ? Json(*v) : Json(nullptr);
            };
            Json populations = Json::array();
            for (const auto& c : populationCounts(*membership))
                populations.push_back({{"id", c.id},
                                       {"parent", c.parent},
                                       {"selected", std::to_string(c.selected)},
                                       {"excluded", std::to_string(c.excluded)},
                                       {"unknown", std::to_string(c.unknown)},
                                       {"parentSelected", std::to_string(c.parentSelected)},
                                       {"percentParent", number(c.percentParent)}});
            Json reply{
                {"ok", true},
                {"schema", "review-population/1"},
                {"snapshot", snapshot},
                {"path", impl_->path},
                {"sourceFingerprint", fileHash.empty() ? Json(nullptr) : Json("sha256:" + fileHash)},
                {"durable", false},
                {"feature", feature},
                {"unit", statistics.unit},
                {"qcPolicy", "none"},
                {"valid", std::to_string(impl_->validRows)},
                {"invalid", std::to_string(impl_->invalidRows)},
                {"selected", std::to_string(statistics.selected)},
                {"excluded", std::to_string(statistics.excluded)},
                {"unknown", std::to_string(statistics.unknownMembership)},
                {"measured", std::to_string(statistics.measured)},
                {"missing", std::to_string(statistics.missing)},
                {"minimum", number(statistics.minimum)},
                {"maximum", number(statistics.maximum)},
                {"mean", number(statistics.mean)},
                {"median", number(statistics.median)},
                {"p05", number(statistics.p05)},
                {"p25", number(statistics.p25)},
                {"p75", number(statistics.p75)},
                {"p95", number(statistics.p95)},
                {"sampleStandardDeviation", number(statistics.sampleStandardDeviation)},
                {"cvPercent", number(statistics.cvPercent)},
                {"geometricMean", number(statistics.geometricMean)},
                {"estimators",
                 {{"quantile", "Hyndman-Fan type 7, exact order statistics of every selected finite value"},
                  {"standardDeviation", "sample; n-1"},
                  {"cvPercent", "100 * sample SD / absolute mean; null for zero mean"},
                  {"geometricMean", "exp(mean(log(x))); null if any measured x <= 0"},
                  {"exact", true}}},
                {"populations", populations},
                {"rows", rows},
                {"rowsOffset", pageOffset},
                {"rowsLimit", pageLimit},
                {"rowsMembership", pageFilter},
                {"rowsTotal", std::to_string(rowsTotal)},
                {"gallery", gallery},
                {"galleryOffset", galleryOffset ? Json(*galleryOffset) : Json(nullptr)},
                {"membershipKey",
                 {{"sourceIdentity", sourceIdentity},
                  {"sourceFingerprint", revision.sourceFingerprint},
                  {"graphRevision", graphRevision},
                  {"cached", cached}}},
                {"evaluation",
                 {{"engine", "native-columnar/1"},
                  {"events", std::to_string(membership->events)},
                  {"chunkEvents", bounds.chunkEvents},
                  {"chunksRead", std::to_string(membership->chunksRead)},
                  {"largestChunk", std::to_string(membership->largestChunk)},
                  {"membershipBytes", std::to_string(membership->bytes())},
                  {"bounds",
                   {{"maxEvents", std::to_string(bounds.maxEvents)},
                    {"maxMembershipBytes", std::to_string(bounds.maxMembershipBytes)},
                    {"maxExactValues", std::to_string(bounds.maxExactValues)}}}}},
                {"measurementSemantics",
                 "Stored metadata values; absent fields and nonfinite measurements are unknown. "
                 "Processing validity is not scientific QC."}};
            if (request.contains("histogram")) {
                const auto& spec = request.at("histogram");
                if (!spec.is_object() || spec.size() != 3 + spec.count("feature") ||
                    !spec.at("bins").is_number_unsigned())
                    return refuse("histogram requires minimum, maximum, bins and optional feature");
                const auto histFeature = spec.value("feature", feature);
                if (!features.count(histFeature)) return refuse("Unsupported histogram feature");
                const auto h = columnHistogram(population, columns, histFeature,
                                               spec.at("minimum").get<double>(),
                                               spec.at("maximum").get<double>(),
                                               spec.at("bins").get<std::size_t>(), bounds);
                Json counts = Json::array();
                for (const auto c : h.counts) counts.push_back(std::to_string(c));
                reply["histogram"] = {{"feature", histFeature},
                                      {"unit", columns.parameters().at(histFeature)},
                                      {"edges", h.edges},
                                      {"counts", counts},
                                      {"underflow", std::to_string(h.underflow)},
                                      {"overflow", std::to_string(h.overflow)},
                                      {"measured", std::to_string(h.measured)},
                                      {"missing", std::to_string(h.missing)},
                                      {"semantics", "exact counts of the output population; bins [left,right), last bin closed"}};
            }
            if (request.contains("scatter")) {
                const auto& spec = request.at("scatter");
                if (!spec.is_object() || spec.size() != 3 || !spec.at("maxPoints").is_number_unsigned())
                    return refuse("scatter requires x, y and maxPoints");
                const auto x = spec.at("x").get<std::string>(), y = spec.at("y").get<std::string>();
                if (!features.count(x) || !features.count(y)) return refuse("Unsupported scatter feature");
                const auto sc = columnScatter(population, columns, x, y,
                                              spec.at("maxPoints").get<std::size_t>(), bounds);
                Json points = Json::array();
                for (const auto& p : sc.points) {
                    auto point = eventJson(p.event);
                    point["x"] = p.x;
                    point["y"] = p.y;
                    points.push_back(std::move(point));
                }
                reply["scatter"] = {{"x", x},
                                    {"y", y},
                                    {"eligible", std::to_string(sc.eligible)},
                                    {"stride", std::to_string(sc.stride)},
                                    {"decimated", sc.stride > 1},
                                    {"points", points},
                                    {"semantics", "display only: every stride-th selected event with both values finite, in event order; never used for counts or statistics"}};
            }
            if (sourceChanged())
                return refuse("Source changed during analysis; reopen before source-bound analysis");
            return reply.dump();
        } catch (const std::exception& e) {
            return refuse(e.what());
        }
    }

    bool ReviewSession::metricsPage(bool valid, std::uint64_t offset, std::uint64_t count,
                                    std::vector<MetricRow>& rows, std::uint64_t& totalOut) const {
        std::scoped_lock lock(impl_->mutex);
        rows.clear();
        totalOut = 0;
        if (!impl_->open) return false;
        const std::uint64_t total = impl_->rowCount(valid);
        totalOut = total;
        if (offset >= total) return true;
        const std::uint64_t end = offset + std::min<std::uint64_t>(total - offset, count);
        std::vector<services::ProcessedFrame> frames;
        if (!impl_->readRangeLocked(valid, offset, end - offset, frames)) return false;
        rows.reserve(frames.size());
        const double f = impl_->factor();
        for (std::size_t k = 0; k < frames.size(); ++k)
            rows.push_back(rowFromFrame(frames[k], offset + k, f));
        return true;
    }

    bool ReviewSession::trackRows(bool valid, int trackId, std::size_t max, TrackRows& out,
                                  std::string* error) const {
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
        const double f = impl_->factor();
        // F-1b: an O(n) pass over the paged rows, chunk by chunk.
        if (!impl_->scanRowsLocked(valid, [&](std::uint64_t i, const services::ProcessedFrame& frame) {
                if (frame.validation.trackId != trackId) return true;
                ++out.total;
                if (out.rows.size() >= max) return true;
                out.positions.push_back(i);
                out.rows.push_back(rowFromFrame(frame, i, f));
                return true;
            })) {
            out = TrackRows{};
            return refuse("The stored records could not be read");
        }
        out.truncated = out.total > out.rows.size();
        return true;
    }

    bool ReviewSession::reprocessInputs(bool valid, std::uint64_t index, ReprocessSettings settings,
                                        ReprocessInputs& out, std::string* error) const {
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
        const std::uint64_t records = impl_->rowCount(valid);
        if (index >= records)
            return refuse("No record " + std::to_string(index) + " in the " + (valid ? "valid" : "invalid") +
                          " set (" + std::to_string(records) + " records)");
        const auto &meta = impl_->meta;
        out.pixelToMicron = impl_->factor();
        const auto *storedRow = impl_->rowLocked(valid, index);
        if (!storedRow) return refuse("Record " + std::to_string(index) + " could not be read");
        out.stored = rowFromFrame(*storedRow, index, out.pixelToMicron);
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
        out.eModulusLut = meta.eModulusLutRecord;
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
        const std::uint64_t total = impl_->rowCount(valid);
        totalOut = total;
        if (offset >= total) return true;
        // Overflow-safe end (offset + count may wrap for a huge count).
        const std::uint64_t remaining = total - offset;
        return impl_->readRangeLocked(valid, offset, std::min<std::uint64_t>(remaining, count), frames);
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
        // The whole set on request (the facade's charts): read now, not held.
        if (impl_->validRows > 0 && !impl_->readRangeLocked(true, 0, impl_->validRows, out.frames))
        {
            out.frames.clear();
            out.metadataReadable = false;
        }
        return out;
    }

    bool ReviewSession::frameMeta(bool valid, std::uint64_t index, services::ProcessedFrame &out) const
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open) return false;
        const auto *row = impl_->rowLocked(valid, index);
        if (!row) return false;
        out = *row;
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
        if (!impl_->open || index >= impl_->validRows) return false;
        if (impl_->recording)
        {
            if (impl_->meta.multiImageEnabled && impl_->meta.multiImageCount > 1)
            {
                const std::uint64_t available = impl_->validRows - index;
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
        const auto *row = impl_->rowLocked(true, index);
        if (!row) return false;
        const auto validation = row->validation;
        cv::Mat composed = composeOverlay(images[k], mask, &validation,
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
        const std::uint64_t records = impl_->rowCount(valid);
        out.valid = true;
        out.offset = offset;
        out.size = size;
        const bool colour = (!impl_->recording && (mode != OverlayMode::None || roiOverlay));
        out.channels = colour ? 3 : 1;
        if (offset >= records) return true;
        const std::uint64_t end = offset + std::min<std::uint64_t>(records - offset, count);
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
        // F-1b: streamed page by page from the file, not from held rows.
        if (!impl_->scanRowsLocked(true, [&](std::uint64_t i, const services::ProcessedFrame &frame) {
                if (!frame.validation.isValid) return true;
                data.frameIndex.push_back(frame.index);
                data.validPosition.push_back(static_cast<std::size_t>(i));
                data.areaUm2.push_back(frame.validation.area * areaFactor);
                data.deformability.push_back(frame.validation.deformability);
                data.targetGroup.push_back(frame.validation.isTargetGroup ? 1 : 0);
                data.ringRatio.push_back(frame.validation.ringRatio);
                return true;
            }))
        {
            ScatterData empty;
            empty.pixelToMicron = f;
            return empty;
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
        if (!impl_->open) return false;
        const auto *row = impl_->rowLocked(valid, index);
        if (!row) return false;
        out = *row;
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
