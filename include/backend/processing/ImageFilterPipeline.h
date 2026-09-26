#pragma once

// Deterministic, Qt-free image preprocessing for Processing Contract v2.
//
// An ImageFilterPipeline is an ordered list of Gray8 -> Gray8 stages compiled
// and validated once from a config (unknown stage or invalid parameter fails
// before any processing). It is applied symmetrically to the current and
// background images (the "input" phase) and/or to the difference image (the
// "difference" phase). `buildDifferenceImage` is the single background-
// difference implementation shared by mask generation, empty-frame
// classification, and the other host paths, so processing behavior never
// depends on which caller produced the difference.
//
// See docs/architecture/processing-contract-compatibility.md and ADR 0001.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace backend::processing {

enum class ImageFilterStageKind {
    Identity,        // out = in
    Invert,          // out = 255 - in
    LinearContrast,  // out = saturate(alpha * in + beta)
    Gamma,           // out = 255 * (in/255)^gamma
    Clahe,           // contrast-limited adaptive histogram equalization
};

// One stage plus every parameter any stage kind may use. Unused parameters are
// ignored. Defaults are the identity/no-op values for each kind.
struct ImageFilterStageSpec {
    ImageFilterStageKind kind{ImageFilterStageKind::Identity};
    double alpha{1.0};       // LinearContrast gain
    double beta{0.0};        // LinearContrast bias
    double gamma{1.0};       // Gamma exponent (> 0)
    double clipLimit{2.0};   // Clahe clip limit (> 0)
    int tileGridSize{8};     // Clahe tile grid (>= 1)
};

// Map a stage name (as it appears in a v2 config, e.g. "gamma") to its kind.
// Returns false for an unknown name.
bool parseImageFilterStageKind(const std::string& name, ImageFilterStageKind& out);

// A compiled, ready-to-apply ordered pipeline. An empty pipeline is the
// identity (apply copies input to output unchanged).
class ImageFilterPipeline {
public:
    ImageFilterPipeline() = default;

    // Validate and compile `stages`. Returns nullopt and sets *error on an
    // unknown kind or an out-of-range parameter (e.g. gamma <= 0). `error` may
    // be null.
    static std::optional<ImageFilterPipeline> compile(const std::vector<ImageFilterStageSpec>& stages,
                                                      std::string* error);

    // Apply every stage in order. `input` must be CV_8UC1; `output` is CV_8UC1
    // of the same size. Deterministic. `input` and `output` may not alias.
    void apply(const cv::Mat& input, cv::Mat& output) const;

    bool empty() const noexcept;
    std::size_t size() const noexcept;

private:
    struct Impl;
    // Null means the identity pipeline (apply copies input to output). Shared so
    // the compiled stages (compiled once) are cheap to copy and reuse.
    std::shared_ptr<const Impl> impl_;
};

// The one difference operation shared by every path (kernel mask, empty-frame
// checks, realtime/batch loops): cv::absdiff under Contract 2
// (`absoluteDifference`), saturating cv::subtract under Contract 1.
void differenceImage(const cv::Mat& blurredCurrent,
                     const cv::Mat& blurredBackground,
                     bool absoluteDifference,
                     cv::Mat& outDifference);

// Build the background-difference image over a region of `gray`/`background`.
//
// Order: input stages applied symmetrically to the current and background ROI
// crops -> Gaussian blur -> difference (cv::absdiff when `absoluteDifference`,
// else saturating cv::subtract) -> difference stages. When no background is
// supplied the current image (post input-stages, post-blur, post difference-
// stages) is used directly.
//
// A supplied-but-incompatible background (non-empty, wrong type, or wrong size)
// is a hard error when `absoluteDifference` is set (Contract 2); otherwise it
// falls back to current-only (legacy Contract-1 behavior). Returns false and
// sets *error on failure. OpenCV exceptions propagate to the caller.
bool buildDifferenceImage(const cv::Mat& gray,
                          const cv::Mat& background,
                          const cv::Rect& region,
                          const ImageFilterPipeline& inputStages,
                          const ImageFilterPipeline& differenceStages,
                          int gaussianBlurSize,
                          bool absoluteDifference,
                          cv::Mat& outDifference,
                          std::string* error);

// As above, but both images are already cropped to the ROI (used by the
// ROI-only host empty-frame paths). `backgroundRoi` may be empty (current-only)
// or must match `currentRoi` in type and size.
bool buildDifferenceImageCropped(const cv::Mat& currentRoi,
                                 const cv::Mat& backgroundRoi,
                                 const ImageFilterPipeline& inputStages,
                                 const ImageFilterPipeline& differenceStages,
                                 int gaussianBlurSize,
                                 bool absoluteDifference,
                                 cv::Mat& outDifference,
                                 std::string* error);

} // namespace backend::processing
