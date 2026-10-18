// ReviewCharts — the chart snapshot JSON and the saved-image overlay PNG of
// a review file: the builders behind the backend bridge's
// `fetch_review_charts_json` and `render_review_overlay` (MIB Studio, YOFO
// Studio), moved out of BackendFacade into mib_review_core so every review
// shell can share them (#512 slice 4, plan
// docs/exec-plans/active/2026-10-18-one-review-implementation.md).
//
// Qt-free, built on mib_processing only. The host keeps the policy: which
// file the charts describe, the pixel-to-micron factor and the ring-ratio
// range (the facade: the loaded file, the live factor and processing
// config), and the ROI style of the overlay. The JSON and the messages
// follow bridge-contract.json exactly; the facade's are these.
#pragma once

#include "backend/processing/ProcessingTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace backend::review
{

    // `fetch_review_charts_json` for `rows`, a file's valid-frame metadata
    // (recording::makeReviewChartData keeps the valid rows): 128 × 128
    // density cells that conserve every finite calibrated point, the exact
    // ring-ratio histogram over [ringMin, ringMax], the bundled isoelastic
    // curves; counters as decimal strings.
    std::string reviewChartsJson(const std::vector<services::ProcessedFrame> &rows, const std::string &sourcePath,
                                 double pixelToMicron, double ringMin, double ringMax);

    // `fetch_review_charts_json` when no snapshot can be built.
    std::string reviewChartsErrorJson(const std::string &error);

    // How render_review_overlay draws the recorded ROI.
    enum class RoiStyle
    {
        // Yellow (255, 210, 0), 1 px, in source pixels, clipped to the image:
        // the facade's render_review_overlay (until decision D2 switches it).
        Facade,
        // Red, 3 px, scaled to the image (drawRoiOverlay): the Qt tab and
        // ReviewSession.
        Review,
    };

    // `render_review_overlay`: source_path / valid / index / mode (0..4) /
    // roi → PNG bytes, read by an independent reader (64 MiB source-frame
    // limit; image, saved mask and classification from the same file),
    // drawn by recording::renderProcessingOverlay. Throws a std::exception
    // carrying the bridge's message for a malformed request or an
    // unavailable frame or mask.
    std::vector<std::uint8_t> renderSavedOverlayPng(const std::string &requestJson, RoiStyle roiStyle);

} // namespace backend::review
