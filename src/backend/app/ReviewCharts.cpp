#include "backend/app/BackendFacade.h"
#include "backend/app/AppBackend.h"
#include "backend/recording/ReviewChartData.h"
#include "backend/recording/ProcessingOverlay.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "backend/recording/Hdf5Service.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
namespace backend::bridge {
std::vector<uint8_t> BackendFacade::renderReviewOverlayJson(const std::string& text) const {
    using nlohmann::json;
    const auto request=json::parse(text);
    const auto& value=request.at("index");
    if(!value.is_number_integer() || value<0)throw std::invalid_argument("Image index must be nonnegative integer");
    const auto index=value.get<size_t>();
    const auto mode=request.value("mode",0);
    if(mode<0 || mode>4)throw std::invalid_argument("Invalid overlay mode");
    const bool valid=request.value("valid",true);
    services::Hdf5Service reader;
    if(!reader.loadFile(request.at("source_path").get<std::string>()))throw std::runtime_error("Cannot open overlay source");
    const auto path=valid?"/valid_frames/images":"/invalid_frames/images";
    size_t count=0;int height=0,width=0,channels=0;
    if(!reader.getDatasetInfo(path,count,height,width,channels) || index>=count || height<=0 || width<=0 || channels!=1 || static_cast<uint64_t>(height)*width>64*1024*1024ULL)
        throw std::runtime_error("Overlay source frame unavailable or exceeds 64 MiB");
    cv::Mat image,mask;
    if(!reader.readImageByIndex(path,index,image))throw std::runtime_error("Cannot read overlay source image");
    if(mode && (!reader.readImageByIndex(valid?"/valid_frames/masks":"/invalid_frames/masks",index,mask) || mask.size()!=image.size()))
        throw std::runtime_error("Matching saved mask unavailable");
    std::vector<services::ProcessedFrame> rows;
    services::FilterResult classification;
    classification.isValid=valid;
    const bool metadataOk=valid?reader.readValidMetadata(rows):reader.readInvalidMetadata(rows);
    if(metadataOk && index<rows.size())classification=rows[index].validation;
    auto rgb=recording::renderProcessingOverlay(image,mask,&classification,static_cast<recording::OverlayMode>(mode));
    if(request.value("roi",false)) {
        uint64_t first=0,last=0;size_t nvalid=0,ninvalid=0;services::ProcessingService::Roi roi;
        if(reader.readExperimentInfo(first,last,nvalid,ninvalid,&roi) && roi.w>0 && roi.h>0)
            cv::rectangle(rgb,cv::Rect(roi.x,roi.y,roi.w,roi.h)&cv::Rect(0,0,rgb.cols,rgb.rows),cv::Scalar(255,210,0),1);
    }
    cv::Mat bgr;cv::cvtColor(rgb,bgr,cv::COLOR_RGB2BGR);std::vector<uint8_t> bytes;
    if(!cv::imencode(".png",bgr,bytes))throw std::runtime_error("Could not encode overlay image");
    return bytes;
}

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
