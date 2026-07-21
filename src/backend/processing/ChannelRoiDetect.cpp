#include "backend/processing/ChannelRoiDetect.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace backend::processing {

namespace {

float medianOf(std::vector<float> values) {
    if (values.empty()) {
        return 0.0f;
    }
    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    return values[mid];
}

} // namespace

ChannelRoi detectChannelRoi(const cv::Mat& backgroundGray, const ChannelRoiParams& params) {
    const ChannelRoi fullFrame{0, 0, backgroundGray.cols, backgroundGray.rows};

    // Need enough rows to distinguish a central band from wall bands.
    if (backgroundGray.empty() || backgroundGray.rows < 8 || backgroundGray.cols < 1) {
        return fullFrame;
    }

    cv::Mat gray;
    if (backgroundGray.type() == CV_8UC1) {
        gray = backgroundGray;
    } else if (backgroundGray.channels() == 1) {
        backgroundGray.convertTo(gray, CV_8UC1);
    } else {
        // Unexpected multi-channel input: don't guess a luminance, fail safe.
        return fullFrame;
    }

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(3, 3), 0);

    // Mean magnitude of the vertical gradient per row: walls (horizontal
    // high-contrast bands) light up; the flat central channel stays low.
    cv::Mat gradientY;
    cv::Sobel(blurred, gradientY, CV_32F, 0, 1, 3);
    gradientY = cv::abs(gradientY);

    cv::Mat rowProfileMat; // rows x 1, CV_32F
    cv::reduce(gradientY, rowProfileMat, 1, cv::REDUCE_AVG, CV_32F);

    const int rows = gray.rows;
    std::vector<float> rowProfile(static_cast<size_t>(rows));
    for (int r = 0; r < rows; ++r) {
        rowProfile[static_cast<size_t>(r)] = rowProfileMat.at<float>(r, 0);
    }

    // Baseline: gradient level of the central third (assumed to be channel).
    const int centreLo = rows / 3;
    const int centreHi = std::max(centreLo + 1, (2 * rows) / 3);
    std::vector<float> centre(rowProfile.begin() + centreLo, rowProfile.begin() + centreHi);
    float baseline = medianOf(centre);
    if (baseline <= 1e-6f) {
        baseline = medianOf(rowProfile); // near-flat centre: fall back to overall median
    }

    float threshold = static_cast<float>(baseline * params.wallGradientRatio);

    // Degenerate case: a (near-)flat channel makes the ratio test meaningless
    // (baseline ~ 0). Sharp walls still show up as isolated gradient peaks at
    // their edges, so split relative to the strongest gradient present. Real
    // backgrounds carry channel texture and never reach this branch.
    if (baseline <= 1e-3f) {
        double peak = 0.0;
        cv::minMaxLoc(rowProfileMat, nullptr, &peak);
        if (peak <= 1e-6) {
            return fullFrame; // truly flat image: no walls detectable
        }
        threshold = static_cast<float>(peak * 0.5);
    }

    // Largest contiguous run of non-wall rows = the channel band.
    int bestStart = -1;
    int bestLen = 0;
    int currentStart = -1;
    int currentLen = 0;
    for (int r = 0; r < rows; ++r) {
        const bool isWall = rowProfile[static_cast<size_t>(r)] > threshold;
        if (isWall) {
            currentLen = 0;
            currentStart = -1;
            continue;
        }
        if (currentLen == 0) {
            currentStart = r;
        }
        ++currentLen;
        if (currentLen > bestLen) {
            bestLen = currentLen;
            bestStart = currentStart;
        }
    }

    if (bestStart < 0 || bestLen <= 0) {
        return fullFrame; // everything looked like a wall: don't crop
    }

    const int margin = std::max(0, params.marginRows);
    const int top = bestStart + margin;
    const int bottom = bestStart + bestLen - 1 - margin; // inclusive
    if (bottom < top) {
        return fullFrame;
    }
    const int height = bottom - top + 1;

    // Reject an implausibly thin band, and skip cropping when no wall was
    // actually trimmed (band spans the whole frame).
    const int minHeight = std::max(1, static_cast<int>(std::lround(rows * params.minBandFraction)));
    if (height < minHeight || height >= rows) {
        return fullFrame;
    }

    return ChannelRoi{0, top, gray.cols, height};
}

} // namespace backend::processing
