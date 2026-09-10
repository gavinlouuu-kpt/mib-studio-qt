#pragma once

#include "backend/processing/ProcessingTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace backend {
class EModulusLut;
} // namespace backend

namespace backend::processing {

struct ProcessingCoreIdentity {
    std::string version{"0.1.0"};
    uint32_t contractVersion{1};
    uint32_t engineAbiVersion{1};
    std::string artifactSha256;
    std::string releaseTag;
    std::string manifestSha256;
    std::string source{"bundled"};
    std::string buildId;
    std::string runtimeFingerprint;

    bool operator==(const ProcessingCoreIdentity& other) const;
    bool operator!=(const ProcessingCoreIdentity& other) const { return !(*this == other); }
};

enum class BackgroundDifferenceMode {
    DirectionalSubtract,
    AbsoluteDifference,
};

struct KernelConfig {
    int gaussianBlurSize{3};
    int backgroundSubtractThreshold{8};
    int morphologyKernelSize{3};
    int morphologyIterations{1};
    int emptyFramePixelThreshold{100};
    BackgroundDifferenceMode backgroundDifferenceMode{
        BackgroundDifferenceMode::DirectionalSubtract};
};

struct KernelRoi {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

class IProcessingKernel {
public:
    virtual ~IProcessingKernel() = default;

    virtual const ProcessingCoreIdentity& identity() const noexcept = 0;
    virtual bool processMask(const cv::Mat& gray,
                             const cv::Mat& background,
                             const KernelConfig& config,
                             const KernelRoi& roi,
                             cv::Mat& outputMask,
                             std::string* error = nullptr) = 0;
    virtual bool isEmpty(const cv::Mat& gray,
                         const cv::Mat& background,
                         const KernelConfig& config,
                         const KernelRoi& roi,
                         bool& outputIsEmpty,
                         std::string* error = nullptr) = 0;
    virtual bool reset(std::string* error = nullptr) = 0;

    // ---- Version-sensitive science beyond mask generation (A7) ----
    // Contour extraction, per-object metrics/LUT/target gating, and batch
    // track matching. The default implementations execute the shared bundled
    // science (ProcessingScience). ABI v1 dynamic cores inherit them because
    // the C ABI transports only mask/empty decisions; an ABI v2 core
    // overrides them to own the full pipeline across the plugin boundary.
    virtual bool analyzeObjects(const cv::Mat& processedImage,
                                const cv::Rect& roi,
                                const services::ProcessingConfig& config,
                                const cv::Mat& originalImage,
                                double pixelToMicronFactor,
                                const backend::EModulusLut* eModulusLut,
                                std::vector<services::FilterResult>& results,
                                std::string* error = nullptr);
    virtual bool matchTrack(const std::vector<services::BatchTrack>& tracks,
                            const std::vector<bool>& matchedThisFrame,
                            const services::FilterResult& detection,
                            uint64_t frameIndex,
                            int frameWidth,
                            int& matchedTrack,
                            std::string* error = nullptr);
};

std::shared_ptr<IProcessingKernel> makeBundledProcessingKernel();
ProcessingCoreIdentity bundledProcessingCoreIdentity();

} // namespace backend::processing
