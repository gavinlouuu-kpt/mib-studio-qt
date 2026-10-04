// A7 seam proof: a spy kernel overriding the science virtuals observes every
// mask, object-analysis, and track-matching decision made by the desktop
// batch/offline pipeline, and the outputs stay identical to the golden
// bundled behavior. Realtime paths share the same ProcessingService wrappers
// (processMaskWithActiveKernel / filterProcessedObjects /
// matchTrackWithActiveKernel), so this covers the routing for every desktop
// path that can run without camera hardware.
#include "backend/processing/IProcessingKernel.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "support/assert.h"

#include <opencv2/imgproc.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace {

class SpyScienceKernel final : public backend::processing::IProcessingKernel {
public:
    SpyScienceKernel() : bundled_(backend::processing::makeBundledProcessingKernel()) {
        identity_.version = "7.7.7-science-spy";
        identity_.contractVersion = 1;
        identity_.engineAbiVersion = 1;
        identity_.source = "test";
    }

    const backend::processing::ProcessingCoreIdentity& identity() const noexcept override {
        return identity_;
    }

    bool processMask(const cv::Mat& gray, const cv::Mat& background,
                     const backend::processing::KernelConfig& config,
                     const backend::processing::KernelRoi& roi, cv::Mat& mask,
                     std::string* error) override {
        maskCalls.fetch_add(1, std::memory_order_relaxed);
        return bundled_->processMask(gray, background, config, roi, mask, error);
    }

    bool isEmpty(const cv::Mat& gray, const cv::Mat& background,
                 const backend::processing::KernelConfig& config,
                 const backend::processing::KernelRoi& roi, bool& empty,
                 std::string* error) override {
        emptyCalls.fetch_add(1, std::memory_order_relaxed);
        return bundled_->isEmpty(gray, background, config, roi, empty, error);
    }

    bool reset(std::string*) override { return true; }

    bool analyzeObjects(const cv::Mat& processedImage, const cv::Rect& roi,
                        const backend::services::ProcessingConfig& config,
                        const cv::Mat& originalImage, double pixelToMicronFactor,
                        const backend::EModulusLut* eModulusLut,
                        std::vector<backend::services::FilterResult>& results,
                        std::string* error, const cv::Mat& background) override {
        analyzeCalls.fetch_add(1, std::memory_order_relaxed);
        return IProcessingKernel::analyzeObjects(processedImage, roi, config, originalImage,
                                                 pixelToMicronFactor, eModulusLut, results,
                                                 error, background);
    }

    bool matchTrack(const std::vector<backend::services::BatchTrack>& tracks,
                    const std::vector<bool>& matchedThisFrame,
                    const backend::services::FilterResult& detection, uint64_t frameIndex,
                    int frameWidth, int& matchedTrack, std::string* error) override {
        trackCalls.fetch_add(1, std::memory_order_relaxed);
        return IProcessingKernel::matchTrack(tracks, matchedThisFrame, detection, frameIndex,
                                             frameWidth, matchedTrack, error);
    }

    std::atomic<int> maskCalls{0};
    std::atomic<int> emptyCalls{0};
    std::atomic<int> analyzeCalls{0};
    std::atomic<int> trackCalls{0};

private:
    backend::processing::ProcessingCoreIdentity identity_;
    std::shared_ptr<backend::processing::IProcessingKernel> bundled_;
};

cv::Mat makeRingFrame(int leftShift) {
    cv::Mat image(96, 160, CV_8UC1, cv::Scalar(0));
    cv::circle(image, cv::Point(48 + leftShift, 48), 22, cv::Scalar(200), cv::FILLED);
    cv::circle(image, cv::Point(48 + leftShift, 48), 11, cv::Scalar(40), cv::FILLED);
    cv::circle(image, cv::Point(112, 48), 18, cv::Scalar(255), cv::FILLED);
    cv::circle(image, cv::Point(112, 48), 9, cv::Scalar(0), cv::FILLED);
    return image;
}

} // namespace

int main() {
    backend::services::ProcessingService service;
    service.setPixelToMicronFactor(0.5);

    auto spy = std::make_shared<SpyScienceKernel>();
    std::string error;
    MIB_REQUIRE(service.activateProcessingKernel(spy, &error), error);

    backend::services::ProcessingConfig config;
    config.gaussian_blur_size = 1;
    config.bg_subtract_threshold = 127;
    config.morph_kernel_size = 1;
    config.morph_iterations = 1;
    config.enable_area_range_check = false;
    config.enable_ring_ratio_check = false;
    config.enable_deformability_range_check = false;
    config.enable_area_ratio_check = false;
    config.require_single_inner_contour = false;

    std::vector<cv::Mat> frames{makeRingFrame(0), makeRingFrame(6), makeRingFrame(12)};
    const auto records = service.processBatch(frames, config);

    MIB_EXPECT(spy.get() != nullptr, "spy alive");
    MIB_EXPECT(spy->maskCalls.load() == 3, "every batch frame's mask came from the kernel");
    // computeProcessedFrame analyzes once per frame and processBatch analyzes
    // the same mask again for per-object records: 2 kernel calls per frame.
    MIB_EXPECT(spy->analyzeCalls.load() == 6,
               "every batch frame's object analysis came from the kernel");
    MIB_EXPECT(spy->trackCalls.load() >= 4,
               "every valid detection's track match came from the kernel");

    // Behavior through an overriding kernel matches the golden baseline.
    MIB_REQUIRE(records.size() == 2, "one record per tracked object");
    MIB_EXPECT(records[0].validation.trackId == 1 &&
                   records[0].validation.trackObservationCount == 3 &&
                   records[0].validation.trackLastFrame == 2,
               "drifting ring tracked identically through the spy kernel");
    MIB_EXPECT(records[1].validation.trackId == 2 &&
                   records[1].validation.trackObservationCount == 3,
               "static ring tracked identically through the spy kernel");

    // The recording empty-frame decision also crosses the seam.
    backend::playback::Frame frame;
    frame.width = 32;
    frame.height = 32;
    frame.linePitch = 32;
    frame.data.assign(32 * 32, 0);
    (void)service.isFrameEmptyWithActiveKernel(frame, config,
                                               backend::services::ProcessingService::Roi{0, 0, 32, 32}, nullptr);
    MIB_EXPECT(spy->emptyCalls.load() == 1, "recording empty check came from the kernel");

    return mib::test::exitCode();
}
