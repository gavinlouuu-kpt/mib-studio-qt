// The facade's review charts and saved-image overlay (bridge contract
// `fetch_review_charts_json`, `render_review_overlay`). The JSON, the
// density, the reader-side overlay and the PNG live in mib_review_core
// (backend/review/ReviewCharts.h, #512 slice 4); the facade keeps the policy:
// the loaded file (the valid rows its ReviewSession read at open, #512 S5),
// the live factor and ring-ratio range, and the facade's ROI style
// (RoiStyle::Facade until decision D2 switches it).
#include "backend/app/BackendFacade.h"
#include "backend/app/AppBackend.h"
#include "backend/recording/ReviewChartData.h"
#include "backend/review/ReviewCharts.h"
#include "backend/review/ReviewSession.h"
#include <nlohmann/json.hpp>
#include <limits>
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
        // The session's rows replace a second reader per fetch; the messages are the ones it gave.
        const std::string source=review_->filePath();
        if(source.empty())throw std::runtime_error("No HDF file open");
        if(review_->isRecordingFile())throw std::runtime_error("Charts require an experiment HDF file");
        std::vector<services::ProcessedFrame> rows;
        std::uint64_t total=0;
        if(!review_->validMetadataReadable() ||
           !review_->framesPage(true,0,std::numeric_limits<std::uint64_t>::max(),rows,total))
            throw std::runtime_error("Cannot read valid-frame metrics");
        const auto cfg=backend_.processing().getProcessingConfig();
        const auto factor=backend_.processing().getPixelToMicronFactor();
        return review::reviewChartsJson(rows,source,factor,cfg.ring_ratio_min,cfg.ring_ratio_max);
    }catch(const std::exception& error){return review::reviewChartsErrorJson(error.what());}
}
} // namespace backend::bridge
