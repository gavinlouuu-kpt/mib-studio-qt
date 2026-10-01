#include "backend/review/ReviewSession.h"

#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/OverlayCompose.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
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
                reader.readValidMetadata(validMeta);
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
            std::string snapshot;
            if (reader.readRunSnapshotJson(snapshot) && !snapshot.empty())
            {
                const auto j = nlohmann::json::parse(snapshot, nullptr, false);
                if (j.is_object() && j.contains("pixel_to_micron") && j["pixel_to_micron"].is_number())
                {
                    const double f = j["pixel_to_micron"].get<double>();
                    if (f > 0.0)
                    {
                        meta.pixelToMicron = f;
                        meta.pixelToMicronFromFile = true;
                    }
                }
            }
            if (!meta.pixelToMicronFromFile) meta.pixelToMicron = fallbackFactor;

            reader.readKdeAnalysisJson(meta.kdeAnalysisJson);
            reader.readKdeLiveJson(meta.kdeLiveJson);
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
    };

    ReviewSession::ReviewSession() : impl_(std::make_unique<Impl>()) {}

    ReviewSession::~ReviewSession() { close(); }

    bool ReviewSession::open(const std::string &path, std::string *error)
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->open)
        {
            impl_->reader.closeFile();
            impl_->open = false;
        }
        impl_->path.clear();
        impl_->meta = ReviewMetadata{};
        impl_->validMeta.clear();
        impl_->invalidMeta.clear();
        if (path.empty())
        {
            if (error) *error = "No file path";
            return false;
        }
        if (!impl_->reader.loadFile(path))
        {
            if (error) *error = "File not found or not an HDF5 review file: " + path;
            return false;
        }
        impl_->open = true;
        impl_->path = path;
        impl_->recording = impl_->reader.isRecordingFile();
        impl_->readMetadataLocked();
        SPDLOG_INFO("ReviewSession: opened {} ({} file, valid {}, invalid {}, factor {}{})", path,
                    impl_->recording ? "recording" : "experiment", impl_->validMeta.size(),
                    impl_->invalidMeta.size(), impl_->factor(),
                    impl_->meta.pixelToMicronFromFile ? " from file" : " fallback");
        return true;
    }

    void ReviewSession::close()
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->open)
        {
            impl_->reader.closeFile();
            impl_->open = false;
        }
        impl_->path.clear();
        impl_->recording = false;
        impl_->meta = ReviewMetadata{};
        impl_->validMeta.clear();
        impl_->invalidMeta.clear();
    }

    bool ReviewSession::isOpen() const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->open;
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
        }
        return data;
    }

    std::string ReviewSession::accountingSummary() const
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open) return {};
        if (!impl_->meta.hasAccounting) return " · accounting: not recorded (legacy file)";
        const auto &a = impl_->meta.accounting;
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
        if (!impl_->reader.loadFile(path))
        {
            impl_->open = false;
            if (error) *error = "Failed to reopen " + path;
            return false;
        }
        impl_->readMetadataLocked();
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
