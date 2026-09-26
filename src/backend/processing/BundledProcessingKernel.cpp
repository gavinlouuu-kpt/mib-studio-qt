#include "backend/processing/IProcessingKernel.h"
#include "backend/processing/ImageFilterPipeline.h"
#include "backend/processing/ProcessingCoreAbi.h"
#include "backend/processing/ProcessingScience.h"

#include <algorithm>
#include <exception>

#include <opencv2/imgproc.hpp>

#ifndef MIB_PROCESSING_CORE_VERSION
#define MIB_PROCESSING_CORE_VERSION "0.1.0"
#endif

namespace backend::processing {
namespace {

int oddAtLeastOne(int value) {
    value = std::max(1, value);
    return (value % 2 == 0) ? value + 1 : value;
}

cv::Rect normalizedRoi(const cv::Mat& gray, const KernelRoi& requested) {
    if (gray.empty()) {
        return {};
    }
    int x = requested.x;
    int y = requested.y;
    int width = requested.width;
    int height = requested.height;
    if (width <= 0 || height <= 0) {
        return {0, 0, gray.cols, gray.rows};
    }
    x = std::max(0, std::min(x, gray.cols - 1));
    y = std::max(0, std::min(y, gray.rows - 1));
    width = std::max(1, std::min(width, gray.cols - x));
    height = std::max(1, std::min(height, gray.rows - y));
    return {x, y, width, height};
}

bool validateGray(const cv::Mat& gray, std::string* error) {
    if (gray.empty()) {
        if (error) *error = "input image is empty";
        return false;
    }
    if (gray.type() != CV_8UC1) {
        if (error) *error = "input image must be CV_8UC1";
        return false;
    }
    return true;
}

std::string runtimeFingerprint() {
#if defined(_MSC_VER)
    return "windows-x86_64-msvc" + std::to_string(_MSC_VER) + "-md-cxx17";
#elif defined(_WIN32) && defined(__x86_64__) && defined(__clang__)
    return "windows-x86_64-clang" + std::to_string(__clang_major__) + "-cxx17";
#elif defined(_WIN32) && defined(__x86_64__) && defined(__GNUC__)
    return "windows-x86_64-gcc" + std::to_string(__GNUC__) + "-cxx17";
#elif defined(__linux__) && defined(__x86_64__) && defined(__clang__)
    return "linux-x86_64-clang" + std::to_string(__clang_major__) + "-cxx17";
#elif defined(__linux__) && defined(__x86_64__) && defined(__GNUC__)
    return "linux-x86_64-gcc" + std::to_string(__GNUC__) + "-cxx17";
#elif defined(__linux__) && defined(__aarch64__) && defined(__clang__)
    return "linux-aarch64-clang" + std::to_string(__clang_major__) + "-cxx17";
#elif defined(__linux__) && defined(__aarch64__) && defined(__GNUC__)
    return "linux-aarch64-gcc" + std::to_string(__GNUC__) + "-cxx17";
#elif defined(__APPLE__) && defined(__aarch64__) && defined(__clang__)
    return "macos-aarch64-clang" + std::to_string(__clang_major__) + "-cxx17";
#elif defined(__APPLE__) && defined(__x86_64__) && defined(__clang__)
    return "macos-x86_64-clang" + std::to_string(__clang_major__) + "-cxx17";
#else
    return "unknown-platform-cxx17";
#endif
}

class BundledProcessingKernel final : public IProcessingKernel {
public:
    BundledProcessingKernel() : identity_(bundledProcessingCoreIdentity()) {}

    const ProcessingCoreIdentity& identity() const noexcept override { return identity_; }

    bool processMask(const cv::Mat& gray,
                     const cv::Mat& background,
                     const KernelConfig& config,
                     const KernelRoi& roi,
                     cv::Mat& outputMask,
                     std::string* error) override {
        try {
            if (!validateGray(gray, error)) return false;
            const cv::Rect region = normalizedRoi(gray, roi);
            outputMask = cv::Mat::zeros(gray.rows, gray.cols, CV_8UC1);

            // One shared difference implementation for mask + empty-frame paths.
            // Contract 1 (flag off) uses saturating subtraction; Contract 2
            // (flag on) uses cv::absdiff. Filter pipelines are identity here
            // until an ABI-v2 core / v2 config supplies stages.
            cv::Mat processingInput;
            if (!buildDifferenceImage(gray, background, region, inputStages_, differenceStages_,
                                      config.gaussianBlurSize, config.absoluteBackgroundDifference,
                                      processingInput, error)) {
                outputMask.release();
                return false;
            }

            cv::Mat thresholded;
            cv::threshold(processingInput, thresholded,
                          std::max(0, config.backgroundSubtractThreshold), 255,
                          cv::THRESH_BINARY);
            const int morphologySize = oddAtLeastOne(config.morphologyKernelSize);
            const cv::Mat morphologyKernel = cv::getStructuringElement(
                cv::MORPH_CROSS, cv::Size(morphologySize, morphologySize));
            cv::Mat destination = outputMask(region);
            cv::morphologyEx(thresholded, destination, cv::MORPH_CLOSE, morphologyKernel,
                             cv::Point(-1, -1), std::max(1, config.morphologyIterations));
            cv::morphologyEx(destination, destination, cv::MORPH_OPEN, morphologyKernel,
                             cv::Point(-1, -1), std::max(1, config.morphologyIterations));
            return true;
        } catch (const cv::Exception& ex) {
            if (error) *error = ex.what();
            outputMask.release();
            return false;
        } catch (const std::exception& ex) {
            if (error) *error = ex.what();
            outputMask.release();
            return false;
        }
    }

    bool isEmpty(const cv::Mat& gray,
                 const cv::Mat& background,
                 const KernelConfig& config,
                 const KernelRoi& roi,
                 bool& outputIsEmpty,
                 std::string* error) override {
        try {
            if (!validateGray(gray, error)) {
                outputIsEmpty = true;
                return false;
            }
            const cv::Rect region = normalizedRoi(gray, roi);
            cv::Mat difference;
            if (!buildDifferenceImage(gray, background, region, inputStages_, differenceStages_,
                                      config.gaussianBlurSize, config.absoluteBackgroundDifference,
                                      difference, error)) {
                outputIsEmpty = true;
                return false;
            }
            cv::Mat thresholded;
            cv::threshold(difference, thresholded,
                          std::max(0, config.backgroundSubtractThreshold), 255,
                          cv::THRESH_BINARY);
            outputIsEmpty =
                cv::countNonZero(thresholded) < std::max(0, config.emptyFramePixelThreshold);
            return true;
        } catch (const cv::Exception& ex) {
            if (error) *error = ex.what();
            outputIsEmpty = true;
            return false;
        }
    }

    bool reset(std::string*) override { return true; }

private:
    ProcessingCoreIdentity identity_;
    // Identity (no-op) preprocessing until an ABI-v2 core / v2 config supplies
    // stages. Owned by the kernel so stages compile once per context.
    ImageFilterPipeline inputStages_;
    ImageFilterPipeline differenceStages_;
};

} // namespace

bool ProcessingCoreIdentity::operator==(const ProcessingCoreIdentity& other) const {
    return version == other.version && contractVersion == other.contractVersion &&
           engineAbiVersion == other.engineAbiVersion &&
           artifactSha256 == other.artifactSha256 && releaseTag == other.releaseTag &&
           manifestSha256 == other.manifestSha256 && source == other.source &&
           buildId == other.buildId && runtimeFingerprint == other.runtimeFingerprint;
}

ProcessingCoreIdentity bundledProcessingCoreIdentity() {
    ProcessingCoreIdentity identity;
    identity.version = MIB_PROCESSING_CORE_VERSION;
    identity.contractVersion = MIB_PROCESSING_CONTRACT_VERSION;
    identity.engineAbiVersion = MIB_PROCESSING_ENGINE_ABI_VERSION;
    identity.source = "bundled";
    identity.buildId = "mib-processing-" MIB_PROCESSING_CORE_VERSION;
    identity.runtimeFingerprint = runtimeFingerprint();
    return identity;
}

std::shared_ptr<IProcessingKernel> makeBundledProcessingKernel() {
    return std::make_shared<BundledProcessingKernel>();
}

// Default science implementations: every kernel executes the shared bundled
// pipeline unless it overrides these (ABI v2 dynamic cores will).
bool IProcessingKernel::analyzeObjects(const cv::Mat& processedImage,
                                       const cv::Rect& roi,
                                       const services::ProcessingConfig& config,
                                       const cv::Mat& originalImage,
                                       double pixelToMicronFactor,
                                       const backend::EModulusLut* eModulusLut,
                                       std::vector<services::FilterResult>& results,
                                       std::string* error) {
    try {
        results = science::filterProcessedObjects(processedImage, roi, config, originalImage,
                                                  pixelToMicronFactor, eModulusLut);
        return true;
    } catch (const std::exception& ex) {
        results.clear();
        if (error) *error = ex.what();
        return false;
    }
}

bool IProcessingKernel::matchTrack(const std::vector<services::BatchTrack>& tracks,
                                   const std::vector<bool>& matchedThisFrame,
                                   const services::FilterResult& detection,
                                   uint64_t frameIndex,
                                   int frameWidth,
                                   int& matchedTrack,
                                   std::string* error) {
    try {
        matchedTrack =
            science::findMatchingTrack(tracks, matchedThisFrame, detection, frameIndex, frameWidth);
        return true;
    } catch (const std::exception& ex) {
        matchedTrack = -1;
        if (error) *error = ex.what();
        return false;
    }
}

} // namespace backend::processing
