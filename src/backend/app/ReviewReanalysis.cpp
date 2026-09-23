#include "backend/app/BackendFacade.h"
#include "backend/app/AppBackend.h"
#include "backend/processing/BatchMaskSources.h"
#include "backend/processing/ProcessingConfigJson.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/HdfExportService.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <stdexcept>
#include <algorithm>

namespace backend::bridge {
std::string BackendFacade::fetchReviewReanalysisStatusJson() const {
    std::scoped_lock lock(reanalysisMutex_);
    return reanalysisStatusJson_;
}

BackendCommandResult BackendFacade::submitReviewReanalysisJson(const std::string& text) {
    using nlohmann::json;
    if (!initialized_) return {false, BackendCommandType::Review, "Backend not initialized"};
    std::string source, output, dataset, kind;
    bool synthetic=false;
    services::ProcessingService::Roi customRoi{0,0,0,0};
    bool overrideRoi=false;
    auto config=backend_.processing().getProcessingConfig();
    std::uint64_t start=0, count=0;
    try {
        const auto request=json::parse(text);
        kind=request.value("source_kind",std::string("hdf"));
        if(kind!="hdf" && kind!="folder" && kind!="avi")throw std::runtime_error("Unknown source kind");
        source=request.at("source_path").get<std::string>();
        synthetic=request.value("synthetic_background",false);
        if(request.contains("image_processing")) {
            std::string error;
            if(!processing::config_json::fromJson(request.at("image_processing"),config,&error))throw std::runtime_error(error);
        }
        if(request.contains("roi")) {
            const auto& roi=request.at("roi");
            for(const auto* field:{"x","y","w","h"}) {
                if(!roi.at(field).is_number_integer() || roi.at(field)<0 || roi.at(field)>std::numeric_limits<int>::max())throw std::runtime_error("ROI fields must be nonnegative 32-bit integers");
            }
            customRoi={roi.at("x").get<int>(),roi.at("y").get<int>(),roi.at("w").get<int>(),roi.at("h").get<int>()};
            if(customRoi.x<0 || customRoi.y<0 || customRoi.w<=0 || customRoi.h<=0)throw std::runtime_error("ROI must have nonnegative origin and positive size");
            overrideRoi=true;
        }
        output=request.at("output_path").get<std::string>();
        dataset=request.value("dataset", std::string("/valid_frames/images"));
        const auto rangeValue=[&](const char* name) {
            if(!request.contains(name))return std::uint64_t{0};
            const auto& value=request.at(name);
            if(!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>()<0))
                throw std::runtime_error(std::string(name)+" must be a nonnegative integer");
            return value.get<std::uint64_t>();
        };
        start=rangeValue("start");count=rangeValue("count");
        if (source.empty() || output.empty()) throw std::runtime_error("Source and output paths are required");
        if (dataset!="all" && dataset!="/valid_frames/images" && dataset!="/invalid_frames/images" && dataset!="/recorded_frames/images")
            throw std::runtime_error("Unsupported HDF dataset");
        if (std::filesystem::exists(output)) throw std::runtime_error("Output already exists; choose a new file");
    } catch(const std::exception& error) { return {false,BackendCommandType::Review,error.what()}; }
    {
        std::scoped_lock lock(reanalysisMutex_);
        if (reanalysisActive_) return {false,BackendCommandType::Review,"Reanalysis already active"};
        reanalysisActive_=true;
    }
    if (reanalysisThread_.joinable()) reanalysisThread_.join();
    CancelFlag cancel;
    const auto id=beginOperation(BackendOperationKind::Reanalysis,&cancel,source);
    {
        std::scoped_lock lock(reanalysisMutex_);
        reanalysisStatusJson_=json{{"state","running"},{"operation_id",std::to_string(id)},{"phase","loading"}}.dump();
    }
    try {
        reanalysisThread_=std::thread([this,id,cancel,source,output,dataset,start,count,config,kind,synthetic,customRoi,overrideRoi] {
            std::filesystem::path partial;
            bool published=false;
            std::string error, warning, retained;
            size_t produced=0;
            try {
                const auto checkCancel=[&] { if(cancel->load(std::memory_order_acquire)) throw std::runtime_error("Reanalysis cancelled"); };
                checkCancel();
                std::vector<services::ProcessedFrame> metadata;
                std::vector<cv::Mat> images;
                services::ProcessingService::Roi roi{0,0,0,0};
                cv::Mat background;
                processing::ProcessingCoreIdentity originalCore;
                bool hasOriginalCore=false;
                if(kind=="hdf") {
                    services::Hdf5Service reader;
                    if(!reader.loadFile(source)) throw std::runtime_error("Cannot open HDF source");
                    struct Ref {std::string dataset;size_t row;services::ProcessedFrame metadata;int height,width,channels;};
                    std::vector<Ref> refs;
                    const auto collect=[&](const std::string& path,bool optional) {
                        size_t available=0;int height=0,width=0,channels=0;
                        if(!reader.getDatasetInfo(path,available,height,width,channels)) {
                            if(optional && reader.metadataDatasetPresent(path=="/valid_frames/images")==std::optional<bool>(false))return;
                            throw std::runtime_error("Selected source dataset is missing");
                        }
                        std::vector<services::ProcessedFrame> rows;
                        const bool ok=path=="/valid_frames/images" ? reader.readValidMetadata(rows) :
                            path=="/invalid_frames/images" ? reader.readInvalidMetadata(rows) : reader.readRecordingMetadata(rows);
                        if(!ok || rows.size()<available)throw std::runtime_error("Source frame identities/timestamps are missing");
                        for(size_t i=0;i<available;++i)refs.push_back({path,i,std::move(rows[i]),height,width,channels});
                    };
                    if(dataset=="all") {
                        if(reader.isRecordingFile()) collect("/recorded_frames/images",false);
                        else {collect("/valid_frames/images",true);collect("/invalid_frames/images",true);}
                        const bool hasTime=std::any_of(refs.begin(),refs.end(),[](const auto& ref){return ref.metadata.timestampNs!=0;});
                        std::sort(refs.begin(),refs.end(),[hasTime](const auto& a,const auto& b) {
                            if(hasTime && a.metadata.timestampNs!=b.metadata.timestampNs)return a.metadata.timestampNs<b.metadata.timestampNs;
                            if(a.metadata.index!=b.metadata.index)return a.metadata.index<b.metadata.index;
                            if(a.dataset!=b.dataset)return a.dataset<b.dataset;
                            return a.row<b.row;
                        });
                    } else collect(dataset,false);
                    if(start>=refs.size())throw std::runtime_error("Selected source range is empty");
                    const auto selected=count==0 ? refs.size()-start : count;
                    if(selected>refs.size()-start || selected>4096)throw std::runtime_error("Select a smaller range (maximum 4096 frames)");
                    uint64_t inputBytes=0;
                    for(size_t i=start;i<start+selected;++i) {
                        const auto& ref=refs[i];
                        if(ref.height<=0 || ref.width<=0 || ref.channels!=1)throw std::runtime_error("Source frames must be grayscale");
                        const uint64_t bytes=static_cast<uint64_t>(ref.height)*static_cast<uint64_t>(ref.width);
                        if(bytes>268435456ULL-inputBytes)throw std::runtime_error("Select a smaller range (maximum 256 MiB input)");
                        inputBytes+=bytes;
                    }
                    uint64_t first=0,last=0;size_t valid=0,invalid=0;
                    if(!reader.isRecordingFile() && !reader.readExperimentInfo(first,last,valid,invalid,&roi))
                        throw std::runtime_error("Source ROI metadata is missing");
                    reader.readBackgroundImage(background);
                    hasOriginalCore=reader.readProcessingCoreIdentity(originalCore);
                    images.reserve(selected);metadata.reserve(selected);
                    for(size_t i=start;i<start+selected;++i) {
                        checkCancel();cv::Mat frame;
                        if(!reader.readImageByIndex(refs[i].dataset,refs[i].row,frame))throw std::runtime_error("Failed to read source frame");
                        images.push_back(std::move(frame));metadata.push_back(std::move(refs[i].metadata));
                    }
                } else {
                    services::batch_masks::LoadOptions limits;
                    limits.start=start;limits.count=count;limits.maxFrames=4096;limits.maxBytes=268435456;
                    limits.cancelled=[cancel]{return cancel->load(std::memory_order_acquire);};
                    std::vector<std::string> names, errors;
                    const bool loaded=kind=="folder" ? services::batch_masks::loadFromFolder(source,images,names,errors,limits) :
                        services::batch_masks::loadFromAvi(source,images,names,errors,limits);
                    if(!loaded || !errors.empty() || images.empty())throw std::runtime_error(errors.empty()?"No source images loaded":errors.front());
                    metadata.resize(images.size());
                    for(size_t i=0;i<metadata.size();++i)metadata[i].index=start+i;
                }
                if(overrideRoi)roi=customRoi;
                if(roi.w>0 && (roi.x>images.front().cols-roi.w || roi.y>images.front().rows-roi.h))
                    throw std::runtime_error("ROI exceeds source image dimensions");
                if(synthetic && background.empty())background=services::batch_masks::buildSyntheticBackground(images,[cancel]{return cancel->load(std::memory_order_acquire);});
                const auto selected=images.size();
                processing::ProcessingCoreIdentity usedCore;
                auto frames=backend_.processing().processBatch(images,config,background,roi,
                    [&](const services::ProcessingService::BatchProgress& progress) {
                        checkCancel();
                        {std::scoped_lock lock(reanalysisMutex_);reanalysisStatusJson_=json{{"state","running"},{"operation_id",std::to_string(id)},
                            {"phase","processing"},{"completed",std::to_string(progress.done)},{"total",std::to_string(progress.total)}}.dump();}
                        reportOperationProgress(id,progress.done,progress.total);
                    },&usedCore);
                checkCancel();
                if(hasOriginalCore && originalCore!=usedCore) warning="Source processing core differs; output records the active processing core.";
                for(auto& frame:frames) {
                    const auto local=frame.index;
                    if(local>=selected) throw std::runtime_error("Processing returned an out-of-range source index");
                    const auto& origin=metadata[local];
                    frame.index=origin.index;frame.timestampNs=origin.timestampNs;
                    if(frame.validation.trackId>=0) {
                        if(frame.validation.trackFirstFrame<selected) frame.validation.trackFirstFrame=metadata[frame.validation.trackFirstFrame].index;
                        if(frame.validation.trackLastFrame<selected) frame.validation.trackLastFrame=metadata[frame.validation.trackLastFrame].index;
                    }
                }
                produced=frames.size();
                if(frames.empty()) throw std::runtime_error("Reanalysis produced no output frames");
                partial=std::filesystem::path(output).parent_path() / ("."+std::filesystem::path(output).filename().string()+".partial-"+recording::HdfExportService::newJobId());
                if(!services::batch_masks::saveMasksToHdf5(frames,partial.string(),config,roi.x,roi.y,roi.w,roi.h,background,kind=="hdf",&usedCore))
                    throw std::runtime_error("Failed to save regenerated HDF");
                checkCancel();
                // Atomic no-clobber publication on the same filesystem: unlike rename,
                // this cannot replace an output created after initial validation.
                std::filesystem::create_hard_link(partial,output);
                published=true;
            } catch(const std::exception& failure) {error=failure.what();}
            if(!partial.empty()) {
                std::error_code cleanup;std::filesystem::remove(partial,cleanup);
                if(cleanup) {retained=partial.string();warning+=" Temporary file cleanup failed: "+cleanup.message();}
            }
            const bool cancelled=!published && cancel->load(std::memory_order_acquire);
            {
                std::scoped_lock lock(reanalysisMutex_);
                reanalysisStatusJson_=json{{"state",published?"completed":cancelled?"cancelled":"failed"},{"operation_id",std::to_string(id)},
                    {"final_path",published?output:""},{"retained_partial_path",retained},{"error",error},{"warnings",warning.empty()?json::array():json::array({warning})},
                    {"objects",std::to_string(produced)}}.dump();
            }
            finishOperation(id,published?BackendOperationState::Completed:cancelled?BackendOperationState::Cancelled:BackendOperationState::Failed,error);
            std::scoped_lock lock(reanalysisMutex_);reanalysisActive_=false;
        });
    } catch(const std::exception& error) {
        {std::scoped_lock lock(reanalysisMutex_);reanalysisActive_=false;reanalysisStatusJson_=json{{"state","failed"},{"error",error.what()},{"operation_id",std::to_string(id)}}.dump();}
        finishOperation(id,BackendOperationState::Failed,error.what());
        return {false,BackendCommandType::Review,error.what(),id};
    }
    return {true,BackendCommandType::Review,"Reanalysis started",id};
}
} // namespace backend::bridge
