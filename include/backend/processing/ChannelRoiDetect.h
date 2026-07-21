#pragma once

// Channel-wall detection for auto-fitting the processing ROI.
//
// A captured background of a microfluidic channel shows the channel walls as
// high-contrast horizontal bands at the top and bottom of the frame. Including
// those wall rows in the processing ROI injects noise (spurious contours) and
// defeats the empty-frame fast path, while cells flow in a central band. This
// detector locates the wall rows from the background's vertical gradient and
// returns an ROI that spans the full width but excludes the walls, so the
// operator can keep the full sensor frame and let the wall band be trimmed
// automatically.
//
// Pure and Qt-free (depends only on OpenCV core + imgproc) so it is unit
// testable without the service or the frontend. It fails safe: on empty,
// degenerate, or ambiguous input it returns the full frame, so callers can
// apply the result unconditionally.

#include <opencv2/core.hpp>

namespace backend::processing {

struct ChannelRoi {
    int x{0};
    int y{0};
    int w{0};
    int h{0};
};

struct ChannelRoiParams {
    // A row is treated as a channel wall when its mean |vertical gradient|
    // exceeds this multiple of the central-channel baseline gradient.
    double wallGradientRatio{2.5};
    // Extra rows trimmed inward from each detected wall edge, for margin.
    int marginRows{1};
    // Reject the detection (return the full frame) when the surviving channel
    // band is thinner than this fraction of the frame height — an over-thin
    // band is more likely a misdetection than a real channel.
    double minBandFraction{0.25};
};

// Detect the horizontal channel band in a captured background image and return
// an ROI (full width, wall rows excluded). Returns the full frame on
// empty/degenerate/ambiguous input.
ChannelRoi detectChannelRoi(const cv::Mat& backgroundGray, const ChannelRoiParams& params = {});

} // namespace backend::processing
