// The facade's reanalysis commands (bridge contract `review_reanalysis_json`,
// `fetch_review_reanalysis_preview`). Parsing, the engine and the status JSON
// live in mib_review_core (backend/review/Reanalysis.h, #512 slice 2); the
// facade keeps the policy: the live processing config as the default, one
// reanalysis at a time, the tracked operation (BackendOperationKind::
// Reanalysis) and the status snapshot.
#include "backend/app/BackendFacade.h"
#include "backend/app/AppBackend.h"
#include "backend/review/Reanalysis.h"
#include <opencv2/core.hpp>
#include <cstring>

namespace backend::bridge {
bool BackendFacade::fetchReviewReanalysisPreviewJson(const std::string& text,BackendFrame& out) const {
    cv::Mat image;
    std::uint64_t index=0;
    if(!review::loadReanalysisPreview(text,image,index))return false;
    out={};out.frameIndex=index;out.width=image.cols;out.height=image.rows;out.strideBytes=image.cols;
    out.data.resize(image.total());
    for(int row=0;row<image.rows;++row)std::memcpy(out.data.data()+static_cast<size_t>(row)*image.cols,image.ptr(row),image.cols);
    return true;
}

std::string BackendFacade::fetchReviewReanalysisStatusJson() const {
    std::scoped_lock lock(reanalysisMutex_);
    return reanalysisStatusJson_;
}

BackendCommandResult BackendFacade::submitReviewReanalysisJson(const std::string& text) {
    if (!initialized_) return {false, BackendCommandType::Review, "Backend not initialized"};
    review::ReanalysisRequest request;
    {
        std::string error;
        if(!review::parseReanalysisRequest(text,backend_.processing().getProcessingConfig(),request,error))
            return {false,BackendCommandType::Review,error};
    }
    {
        std::scoped_lock lock(reanalysisMutex_);
        if (reanalysisActive_) return {false,BackendCommandType::Review,"Reanalysis already active"};
        reanalysisActive_=true;
    }
    if (reanalysisThread_.joinable()) reanalysisThread_.join();
    CancelFlag cancel;
    const auto id=beginOperation(BackendOperationKind::Reanalysis,&cancel,request.sourcePath);
    {
        std::scoped_lock lock(reanalysisMutex_);
        reanalysisStatusJson_=review::reanalysisLoadingStatusJson(id);
    }
    try {
        reanalysisThread_=std::thread([this,id,cancel,request] {
            const auto outcome=review::runReanalysis(request,backend_.processing(),cancel,
                [&](std::uint64_t done,std::uint64_t total) {
                    {std::scoped_lock lock(reanalysisMutex_);reanalysisStatusJson_=review::reanalysisProgressStatusJson(id,done,total);}
                    reportOperationProgress(id,done,total);
                });
            {
                std::scoped_lock lock(reanalysisMutex_);
                reanalysisStatusJson_=review::reanalysisTerminalStatusJson(id,request,outcome);
            }
            finishOperation(id,outcome.published?BackendOperationState::Completed:outcome.cancelled?BackendOperationState::Cancelled:BackendOperationState::Failed,outcome.error);
            std::scoped_lock lock(reanalysisMutex_);reanalysisActive_=false;
        });
    } catch(const std::exception& error) {
        {std::scoped_lock lock(reanalysisMutex_);reanalysisActive_=false;reanalysisStatusJson_=review::reanalysisStartFailedStatusJson(id,error.what());}
        finishOperation(id,BackendOperationState::Failed,error.what());
        return {false,BackendCommandType::Review,error.what(),id};
    }
    return {true,BackendCommandType::Review,"Reanalysis started",id};
}
} // namespace backend::bridge
