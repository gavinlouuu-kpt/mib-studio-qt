#include "backend/app/BackendFacade.h"
#include "backend/app/AppBackend.h"
#include "backend/processing/BatchMaskSources.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/HdfExportService.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <stdexcept>

namespace backend::bridge {
std::string BackendFacade::fetchReviewReanalysisStatusJson() const {
    std::scoped_lock lock(reanalysisMutex_);
    return reanalysisStatusJson_;
}

BackendCommandResult BackendFacade::submitReviewReanalysisJson(const std::string& text) {
    using nlohmann::json;
    if (!initialized_) return {false, BackendCommandType::Review, "Backend not initialized"};
    std::string source, output, dataset;
    std::uint64_t start=0, count=0;
    try {
        const auto request=json::parse(text);
        source=request.at("source_path").get<std::string>();
        output=request.at("output_path").get<std::string>();
        dataset=request.value("dataset", std::string("/valid_frames/images"));
        start=request.value("start", std::uint64_t{0});
        count=request.value("count", std::uint64_t{0});
        if (source.empty() || output.empty()) throw std::runtime_error("Source and output paths are required");
        if (dataset!="/valid_frames/images" && dataset!="/invalid_frames/images" && dataset!="/recorded_frames/images")
            throw std::runtime_error("Unsupported HDF dataset");
        if (std::filesystem::exists(output)) throw std::runtime_error("Output already exists; choose a new file");
    } catch(const std::exception& error) { return {false,BackendCommandType::Review,error.what()}; }
    const auto config=backend_.processing().getProcessingConfig();
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
        reanalysisThread_=std::thread([this,id,cancel,source,output,dataset,start,count,config] {
            std::filesystem::path partial;
            bool published=false;
            std::string error, warning, retained;
            size_t produced=0;
            try {
                const auto checkCancel=[&] { if(cancel->load(std::memory_order_acquire)) throw std::runtime_error("Reanalysis cancelled"); };
                checkCancel();
                services::Hdf5Service reader;
                if(!reader.loadFile(source)) throw std::runtime_error("Cannot open HDF source");
                size_t available=0;int height=0,width=0,channels=0;
                if(!reader.getDatasetInfo(dataset,available,height,width,channels) || start>=available)
                    throw std::runtime_error("Selected source dataset/range is empty");
                const auto selected=count==0 ? available-start : count;
                if(selected>available-start || selected>4096 || height<=0 || width<=0 || channels!=1 ||
                    static_cast<uint64_t>(height)*static_cast<uint64_t>(width)>268435456ULL/selected)
                    throw std::runtime_error("Select a smaller grayscale range (maximum 4096 frames / 256 MiB input)");
                std::vector<services::ProcessedFrame> metadata;
                const bool metadataOk=dataset=="/valid_frames/images" ? reader.readValidMetadata(metadata) :
                    dataset=="/invalid_frames/images" ? reader.readInvalidMetadata(metadata) : reader.readRecordingMetadata(metadata);
                if(!metadataOk || metadata.size()<start+selected) throw std::runtime_error("Source frame identities/timestamps are missing");
                services::ProcessingService::Roi roi{0,0,0,0};
                uint64_t first=0,last=0;size_t valid=0,invalid=0;
                if(dataset!="/recorded_frames/images" && !reader.readExperimentInfo(first,last,valid,invalid,&roi))
                    throw std::runtime_error("Source ROI metadata is missing");
                cv::Mat background;
                reader.readBackgroundImage(background);
                processing::ProcessingCoreIdentity originalCore;
                const bool hasOriginalCore=reader.readProcessingCoreIdentity(originalCore);
                std::vector<cv::Mat> images;images.reserve(selected);
                for(size_t i=0;i<selected;++i) {
                    checkCancel();cv::Mat frame;
                    if(!reader.readImageByIndex(dataset,start+i,frame)) throw std::runtime_error("Failed to read source frame");
                    images.push_back(std::move(frame));
                }
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
                    const auto& origin=metadata[start+local];
                    frame.index=origin.index;frame.timestampNs=origin.timestampNs;
                    if(frame.validation.trackId>=0) {
                        if(frame.validation.trackFirstFrame<selected) frame.validation.trackFirstFrame=metadata[start+frame.validation.trackFirstFrame].index;
                        if(frame.validation.trackLastFrame<selected) frame.validation.trackLastFrame=metadata[start+frame.validation.trackLastFrame].index;
                    }
                }
                produced=frames.size();
                if(frames.empty()) throw std::runtime_error("Reanalysis produced no output frames");
                partial=std::filesystem::path(output).parent_path() / ("."+std::filesystem::path(output).filename().string()+".partial-"+recording::HdfExportService::newJobId());
                if(!services::batch_masks::saveMasksToHdf5(frames,partial.string(),config,roi.x,roi.y,roi.w,roi.h,background,true,&usedCore))
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
