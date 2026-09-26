#include "backend/processing/ImageFilterPipeline.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <opencv2/imgproc.hpp>

namespace backend::processing {

namespace {

int oddAtLeastOne(int value) {
    value = std::max(1, value);
    return (value % 2 == 0) ? value + 1 : value;
}

struct CompiledStage {
    ImageFilterStageKind kind{ImageFilterStageKind::Identity};
    double alpha{1.0};
    double beta{0.0};
    cv::Mat gammaLut;         // 1x256 CV_8U, for Gamma
    cv::Ptr<cv::CLAHE> clahe; // for Clahe
};

void applyStage(const CompiledStage& stage, const cv::Mat& src, cv::Mat& dst) {
    switch (stage.kind) {
    case ImageFilterStageKind::Identity:
        src.copyTo(dst);
        return;
    case ImageFilterStageKind::Invert:
        cv::bitwise_not(src, dst); // 255 - src for CV_8U
        return;
    case ImageFilterStageKind::LinearContrast:
        src.convertTo(dst, CV_8U, stage.alpha, stage.beta); // saturate(alpha*src + beta)
        return;
    case ImageFilterStageKind::Gamma:
        cv::LUT(src, stage.gammaLut, dst);
        return;
    case ImageFilterStageKind::Clahe:
        stage.clahe->apply(src, dst);
        return;
    }
    src.copyTo(dst); // unreachable for valid compiled stages
}

} // namespace

struct ImageFilterPipeline::Impl {
    std::vector<CompiledStage> stages;
};

bool parseImageFilterStageKind(const std::string& name, ImageFilterStageKind& out) {
    if (name == "identity") { out = ImageFilterStageKind::Identity; return true; }
    if (name == "invert") { out = ImageFilterStageKind::Invert; return true; }
    if (name == "linear_contrast") { out = ImageFilterStageKind::LinearContrast; return true; }
    if (name == "gamma") { out = ImageFilterStageKind::Gamma; return true; }
    if (name == "clahe") { out = ImageFilterStageKind::Clahe; return true; }
    return false;
}

std::optional<ImageFilterPipeline> ImageFilterPipeline::compile(
    const std::vector<ImageFilterStageSpec>& stages, std::string* error) {
    const auto fail = [error](const std::string& message) -> std::optional<ImageFilterPipeline> {
        if (error != nullptr) *error = message;
        return std::nullopt;
    };

    auto impl = std::make_shared<Impl>();
    impl->stages.reserve(stages.size());

    for (std::size_t i = 0; i < stages.size(); ++i) {
        const ImageFilterStageSpec& spec = stages[i];
        CompiledStage compiled;
        compiled.kind = spec.kind;
        compiled.alpha = spec.alpha;
        compiled.beta = spec.beta;

        switch (spec.kind) {
        case ImageFilterStageKind::Identity:
        case ImageFilterStageKind::Invert:
            break;
        case ImageFilterStageKind::LinearContrast:
            if (!std::isfinite(spec.alpha) || !std::isfinite(spec.beta)) {
                return fail("linear_contrast stage " + std::to_string(i) +
                            " requires finite alpha and beta.");
            }
            break;
        case ImageFilterStageKind::Gamma: {
            if (!std::isfinite(spec.gamma) || spec.gamma <= 0.0) {
                return fail("gamma stage " + std::to_string(i) + " requires gamma > 0.");
            }
            compiled.gammaLut = cv::Mat(1, 256, CV_8U);
            uchar* lut = compiled.gammaLut.ptr<uchar>();
            for (int v = 0; v < 256; ++v) {
                lut[v] = cv::saturate_cast<uchar>(255.0 * std::pow(v / 255.0, spec.gamma));
            }
            break;
        }
        case ImageFilterStageKind::Clahe:
            if (!std::isfinite(spec.clipLimit) || spec.clipLimit <= 0.0) {
                return fail("clahe stage " + std::to_string(i) + " requires clipLimit > 0.");
            }
            if (spec.tileGridSize < 1) {
                return fail("clahe stage " + std::to_string(i) + " requires tileGridSize >= 1.");
            }
            compiled.clahe = cv::createCLAHE(spec.clipLimit,
                                             cv::Size(spec.tileGridSize, spec.tileGridSize));
            break;
        default:
            return fail("unknown image filter stage at index " + std::to_string(i) + ".");
        }

        impl->stages.push_back(std::move(compiled));
    }

    ImageFilterPipeline pipeline;
    pipeline.impl_ = std::move(impl);
    return pipeline;
}

void ImageFilterPipeline::apply(const cv::Mat& input, cv::Mat& output) const {
    if (!impl_ || impl_->stages.empty()) {
        input.copyTo(output);
        return;
    }
    cv::Mat src = input;
    cv::Mat dst;
    for (const CompiledStage& stage : impl_->stages) {
        applyStage(stage, src, dst);
        src = dst;
        dst = cv::Mat();
    }
    output = src;
}

bool ImageFilterPipeline::empty() const noexcept {
    return !impl_ || impl_->stages.empty();
}

std::size_t ImageFilterPipeline::size() const noexcept {
    return impl_ ? impl_->stages.size() : 0;
}

namespace {

// The shared post-crop difference core. `currentRoi` is CV_8UC1; `backgroundRoi`
// is empty (current-only) or matches currentRoi in type and size.
bool differenceCore(const cv::Mat& currentRoi,
                    const cv::Mat& backgroundRoi,
                    const ImageFilterPipeline& inputStages,
                    const ImageFilterPipeline& differenceStages,
                    int gaussianBlurSize,
                    bool absoluteDifference,
                    cv::Mat& outDifference,
                    std::string* error) {
    if (currentRoi.empty() || currentRoi.type() != CV_8UC1) {
        if (error) *error = "current image must be a non-empty CV_8UC1 image";
        return false;
    }

    const int blur = oddAtLeastOne(gaussianBlurSize);
    const cv::Size blurSize(blur, blur);

    cv::Mat currentFiltered;
    inputStages.apply(currentRoi, currentFiltered);
    cv::Mat blurredCurrent;
    cv::GaussianBlur(currentFiltered, blurredCurrent, blurSize, 0);

    bool useBackground = !backgroundRoi.empty();
    if (useBackground &&
        (backgroundRoi.type() != CV_8UC1 || backgroundRoi.size() != currentRoi.size())) {
        // A supplied-but-incompatible background: an error under Contract 2,
        // otherwise a silent current-only fallback (legacy Contract-1 behavior).
        if (absoluteDifference) {
            if (error) *error = "background image is incompatible with the current image";
            return false;
        }
        useBackground = false;
    }

    cv::Mat difference;
    if (useBackground) {
        cv::Mat backgroundFiltered;
        inputStages.apply(backgroundRoi, backgroundFiltered);
        cv::Mat blurredBackground;
        cv::GaussianBlur(backgroundFiltered, blurredBackground, blurSize, 0);
        differenceImage(blurredCurrent, blurredBackground, absoluteDifference, difference);
    } else {
        difference = blurredCurrent;
    }

    differenceStages.apply(difference, outDifference);
    return true;
}

} // namespace

void differenceImage(const cv::Mat& blurredCurrent,
                     const cv::Mat& blurredBackground,
                     bool absoluteDifference,
                     cv::Mat& outDifference) {
    if (absoluteDifference) {
        cv::absdiff(blurredCurrent, blurredBackground, outDifference);
    } else {
        cv::subtract(blurredCurrent, blurredBackground, outDifference);
    }
}

bool buildDifferenceImageCropped(const cv::Mat& currentRoi,
                                 const cv::Mat& backgroundRoi,
                                 const ImageFilterPipeline& inputStages,
                                 const ImageFilterPipeline& differenceStages,
                                 int gaussianBlurSize,
                                 bool absoluteDifference,
                                 cv::Mat& outDifference,
                                 std::string* error) {
    return differenceCore(currentRoi, backgroundRoi, inputStages, differenceStages,
                          gaussianBlurSize, absoluteDifference, outDifference, error);
}

bool buildDifferenceImage(const cv::Mat& gray,
                          const cv::Mat& background,
                          const cv::Rect& region,
                          const ImageFilterPipeline& inputStages,
                          const ImageFilterPipeline& differenceStages,
                          int gaussianBlurSize,
                          bool absoluteDifference,
                          cv::Mat& outDifference,
                          std::string* error) {
    if (gray.empty() || gray.type() != CV_8UC1) {
        if (error) *error = "current image must be a non-empty CV_8UC1 image";
        return false;
    }

    cv::Mat backgroundRoi; // empty unless a compatible background is supplied
    if (!background.empty()) {
        if (background.type() == CV_8UC1 && background.size() == gray.size()) {
            backgroundRoi = background(region);
        } else if (absoluteDifference) {
            // Incompatible under Contract 2 -> hard error (never crop it).
            if (error) *error = "background image is incompatible with the current image";
            return false;
        }
        // else: incompatible under Contract 1 -> current-only fallback.
    }

    return differenceCore(gray(region), backgroundRoi, inputStages, differenceStages,
                          gaussianBlurSize, absoluteDifference, outDifference, error);
}

} // namespace backend::processing
