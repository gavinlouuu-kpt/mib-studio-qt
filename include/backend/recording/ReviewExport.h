#pragma once

#include "backend/processing/ProcessingService.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace backend::review
{

    // Qt-free frame/object-metrics CSV export (BE-6, issue #276). Produces the
    // exact column set and numeric precision of the Qt HdfReviewTab export so
    // outputs stay scientifically equivalent.
    //
    // `progress(done, total)` is called periodically; returning false cancels
    // the export. On cancellation or failure the partial output file is
    // removed — the source recording is never touched (read-only inputs).
    bool writeMetricsCsv(
        const std::string &filePath,
        const std::vector<services::ProcessedFrame> &validFrames,
        const std::vector<services::ProcessedFrame> &invalidFrames,
        double pixelToMicronFactor,
        std::string *errorOut = nullptr,
        const std::function<bool(std::uint64_t, std::uint64_t)> &progress = {});

} // namespace backend::review
