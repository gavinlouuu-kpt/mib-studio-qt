#include "backend/app/BackendFacade.h"
#include "backend/app/AppBackend.h"
#include "backend/recording/ReviewChartData.h"
#include "backend/recording/Hdf5Service.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
namespace backend::bridge {
std::string BackendFacade::fetchReviewChartsJson() const {
    using nlohmann::json;
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
        const auto data=recording::makeReviewChartData(rows,factor,cfg.ring_ratio_min,cfg.ring_ratio_max);
        constexpr int resolution=128;
        std::array<uint64_t,resolution*resolution> density{};
        const auto bin=[](double value,double min,double max) {return std::clamp(static_cast<int>((static_cast<long double>(value)-min)/(static_cast<long double>(max)-min)*resolution),0,resolution-1);};
        for(const auto& [area,deform]:data.points)++density[bin(deform,data.deformMin,data.deformMax)*resolution+bin(area,data.areaMin,data.areaMax)];
        json cells=json::array();for(int y=0;y<resolution;++y)for(int x=0;x<resolution;++x)if(density[y*resolution+x])cells.push_back({x,y,std::to_string(density[y*resolution+x])});
        json bins=json::array();for(const auto count:data.bins)bins.push_back(std::to_string(count));
        json curves=json::array();for(const auto& [modulus,points]:recording::bundledIsoelasticCurves())curves.push_back({{"modulus",modulus},{"points",points}});
        return json{{"valid",true},{"source_path",source},{"pixel_to_micron",factor},{"rows",std::to_string(rows.size())},
            {"finite_points",std::to_string(data.points.size())},{"excluded_nonfinite",std::to_string(data.excludedNonfinite)},
            {"area_range",{data.areaMin,data.areaMax}},{"deform_range",{data.deformMin,data.deformMax}},
            {"resolution",resolution},{"density",cells},{"histogram",bins},{"ring_range",{data.ringMin,data.ringMax}},
            {"histogram_samples",std::to_string(data.ringRatios.size())},{"curves",curves},
            {"curve_source","Bundled scaled_isoelastic_data_6.16-4.24; channel width 30 um, flow rate 0.25, fluid viscosity 4.24"}}.dump();
    }catch(const std::exception& error){return json{{"valid",false},{"error",error.what()}}.dump();}
}
} // namespace backend::bridge
