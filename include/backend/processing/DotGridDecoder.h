#pragma once

// Dot-grid decoder: camera frame -> absolute wafer pose (Qt-free, OpenCV only).
//
// Pipeline: dark-blob detection -> lattice basis estimate (unbiased by the
// coded displacements) -> integer lattice indexing with iterative affine refit
// -> per-dot displacement direction (2 bits) -> column/row phase candidates ->
// codebook lookup under all eight dihedral transforms (the camera may view the
// chip through the glass, i.e. mirrored) -> vote -> bit-agreement check ->
// pixel-to-wafer affine. Mirrors scripts/dot_grid/dotgrid/decode.py.

#include "backend/processing/DotGridCodebook.h"

#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <vector>

namespace backend::dotgrid {

struct DecoderConfig {
    double umPerPxHint{0.293}; // sizes the blob detector; the decoder measures the true scale
    int minVotes{3};
    double minAgreement{0.9}; // fraction of dots whose direction matches the codebook
    int minAgreeingDots{12};
    int maxDots{8000}; // guard against noise images producing huge blob sets
};

struct DecodeResult {
    bool ok{false};
    std::string reason;
    double centreXUm{0.0}; // wafer coordinates of the image centre
    double centreYUm{0.0};
    double thetaDeg{0.0}; // rotation of the image x axis in the wafer frame
    double umPerPx{0.0};  // measured scale
    bool mirrored{false}; // image is a mirror image of the mask (viewed through the glass)
    int votes{0};
    int candidates{0};
    int dots{0};
    double agreement{0.0};
    double residualPx{0.0};          // rms lattice fit residual
    std::string chip;                // chip name from the codebook table, empty if none
    double pixelToWafer[6]{};        // row-major 2x3: X = m0*u + m1*v + m2, Y = m3*u + m4*v + m5
    std::vector<cv::Point2f> dotsPx; // detected dot centroids (for overlays)
    double decodeMs{0.0};
};

class Decoder {
public:
    explicit Decoder(std::shared_ptr<const Codebook> codebook);

    const Codebook& codebook() const { return *codebook_; }

    // gray: 8-bit single channel (other depths are normalised to 8-bit).
    DecodeResult decode(const cv::Mat& gray, const DecoderConfig& config) const;

    // Dark, roughly round blobs of about the expected diameter (centroids, px).
    static std::vector<cv::Point2f> detectDots(const cv::Mat& gray, double expectedDiameterPx);

private:
    std::shared_ptr<const Codebook> codebook_;
};

// Synthetic view for tests and the mock camera: renders the dots visible from a pose.
struct ViewPose {
    double centreXUm{0.0};
    double centreYUm{0.0};
    double thetaDeg{0.0};
    double umPerPx{0.293};
    bool mirrored{false};
    int width{1920};
    int height{1200};

    // pixel -> wafer affine (row-major 2x3), same convention as DecodeResult::pixelToWafer
    void matrix(double m[6]) const;
    void inverse(double m[6]) const; // wafer -> pixel
};

struct RenderOptions {
    int background{180};
    int dotLevel{40};
    double blurSigmaPx{1.5};
    double noiseSigma{6.0};
    uint32_t seed{0};
    // Optional dot-free band (models the channel keep-out): a line through
    // (x0, y0)-(x1, y1) in wafer um with half-width bandUm; dots within are skipped
    // and the channel itself is drawn as a dark line of channelWidthUm.
    bool channel{false};
    double channelX0{0}, channelY0{0}, channelX1{0}, channelY1{0};
    double channelWidthUm{30.0};
    double bandUm{0.0};
    // Fraction of dots randomly omitted (fabrication defects).
    double missingFraction{0.0};
};

cv::Mat renderView(const Codebook& codebook, const ViewPose& pose, const RenderOptions& options);

} // namespace backend::dotgrid
