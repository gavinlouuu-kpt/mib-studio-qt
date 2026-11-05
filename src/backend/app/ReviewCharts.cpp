// The facade's review charts and saved-image overlay (bridge contract
// `fetch_review_charts_json`, `render_review_overlay`). The JSON, the
// density, the reader-side overlay and the PNG live in mib_review_core
// (backend/review/ReviewCharts.h, #512 slice 4); the facade keeps the policy:
// the loaded file (the valid rows its ReviewSession read at open, #512 S5),
// the factor (the file's recorded pixel_to_micron as Qt charts it, else the
// live one) and the live ring-ratio range, and the facade's ROI style
// (RoiStyle::Facade until decision D2 switches it).
#include "backend/app/BackendFacade.h"
#include "backend/app/AppBackend.h"
#include "backend/recording/ReviewChartData.h"
#include "backend/review/ReviewCharts.h"
#include "backend/review/ReviewSession.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace backend::bridge {
std::vector<uint8_t> BackendFacade::renderReviewOverlayJson(const std::string& text) const {
    return review::renderSavedOverlayPng(text,review::RoiStyle::Facade);
}

std::string BackendFacade::fetchMonitoringChartReferenceJson() const {
    using nlohmann::json;
    static const auto payload=[] {
        json curves=json::array();
        for(const auto& [modulus,points]:recording::bundledIsoelasticCurves())
            curves.push_back({{"modulus",modulus},{"points",points}});
        return json{{"curves",curves},{"curve_source","Bundled scaled_isoelastic_data_6.16-4.24; channel width 30 um, flow rate 0.25, fluid viscosity 4.24"}}.dump();
    }();
    return payload;
}
std::string BackendFacade::fetchReviewChartsJson() const {
    try {
        // The session's rows replace a second reader per fetch; the messages are the ones it gave. One
        // snapshot (one lock): the path, the file type, readability and the rows belong to the same file
        // even while another thread loads a different one.
        const auto valid=review_->validSet();
        const std::string& source=valid.filePath;
        if(!valid.open || source.empty())throw std::runtime_error("No HDF file open");
        if(valid.recordingFile)throw std::runtime_error("Charts require an experiment HDF file");
        if(!valid.metadataReadable)throw std::runtime_error("Cannot read valid-frame metrics");
        const auto& rows=valid.frames;
        const auto cfg=backend_.processing().getProcessingConfig();
        // Qt parity (REVIEW-068): the factor the file recorded, the live one only for a file that
        // records none (#867: the session's fallback is the live factor, so the snapshot's factor is
        // already that). Same snapshot as the rows, so the pair cannot mix two files. The charts'
        // wire spelling of the one source enum: "recorded", or "live" for a Fallback.
        const auto factorSource=valid.pixelToMicronFromFile?review::PixelToMicronSource::Recorded
                                                           :review::PixelToMicronSource::Fallback;
        return review::reviewChartsJson(rows,source,valid.pixelToMicron,cfg.ring_ratio_min,cfg.ring_ratio_max,
                                        factorSource==review::PixelToMicronSource::Recorded?"recorded":"live");
    }catch(const std::exception& error){return review::reviewChartsErrorJson(error.what());}
}
} // namespace backend::bridge
