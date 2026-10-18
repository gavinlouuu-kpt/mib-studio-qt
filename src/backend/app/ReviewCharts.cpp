// The facade's review charts and saved-image overlay (bridge contract
// `fetch_review_charts_json`, `render_review_overlay`). The JSON, the
// density, the reader-side overlay and the PNG live in mib_review_core
// (backend/review/ReviewCharts.h, #512 slice 4); the facade keeps the policy:
// the loaded file, the live factor and ring-ratio range, and the facade's
// ROI style (RoiStyle::Facade until decision D2 switches it).
#include "backend/app/BackendFacade.h"
#include "backend/app/AppBackend.h"
#include "backend/recording/ReviewChartData.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/ReviewCharts.h"
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
        std::string source;
        {std::scoped_lock lock(reviewMutex_);source=loadedRecordingPath_;}
        if(source.empty())throw std::runtime_error("No HDF file open");
        services::Hdf5Service reader;
        if(!reader.loadFile(source) || reader.isRecordingFile())throw std::runtime_error("Charts require an experiment HDF file");
        std::vector<services::ProcessedFrame> rows;
        const auto present=reader.metadataDatasetPresent(true);
        if(!present || (*present && !reader.readValidMetadata(rows)))throw std::runtime_error("Cannot read valid-frame metrics");
        const auto cfg=backend_.processing().getProcessingConfig();
        const auto factor=backend_.processing().getPixelToMicronFactor();
        return review::reviewChartsJson(rows,source,factor,cfg.ring_ratio_min,cfg.ring_ratio_max);
    }catch(const std::exception& error){return review::reviewChartsErrorJson(error.what());}
}
} // namespace backend::bridge
