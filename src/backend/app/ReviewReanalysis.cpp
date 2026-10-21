// The facade's reanalysis commands (bridge contract `review_reanalysis_json`,
// `fetch_review_reanalysis_preview`). Parsing, the engine and the status JSON
// live in mib_review_core (backend/review/Reanalysis.h, #512 slice 2); the job
// runs on the review lanes' processing lane (ReviewJobs, #512 S7: one
// reanalysis at a time, beside an export; the lane holds the status snapshot).
// The facade keeps the policy: the live processing config as the default and
// the live processing service as the kernel, the tracked operation
// (BackendOperationKind::Reanalysis, through the lanes' host hook). Objects
// are validated at an HDF source's recorded pixel-to-micron factor, the live
// factor when the source records none and for folder / AVI sources (#758);
// the engine passes it per call, so the live service's factor is unchanged.
#include "backend/app/BackendFacade.h"
#include "backend/app/AppBackend.h"
#include "backend/review/Reanalysis.h"
#include "backend/review/ReviewJobs.h"
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
    return reviewJobs_->reanalysisStatusJson();
}

BackendCommandResult BackendFacade::submitReviewReanalysisJson(const std::string& text) {
    if (!initialized_) return {false, BackendCommandType::Review, "Backend not initialized"};
    review::ReanalysisRequest request;
    {
        std::string error;
        if(!review::parseReanalysisRequest(text,backend_.processing().getProcessingConfig(),request,error))
            return {false,BackendCommandType::Review,error};
    }
    // The new file's path is a client path (#762): a relative one lies under the storage root.
    {
        const auto resolved = backend_.resolveClientPath(request.outputPath);
        if (!resolved.ok) return {false, BackendCommandType::Review, "Regenerate output refused: " + resolved.reason};
        request.outputPath = resolved.path;
    }
    request.useRecordedPixelToMicron=true;
    request.pixelToMicron=backend_.processing().getPixelToMicronFactor();
    const auto started=reviewJobs_->startReanalysisJson(request,backend_.processing());
    if (started.operationId==0) return {false,BackendCommandType::Review,started.error};
    if (!started.started) return {false,BackendCommandType::Review,started.error,started.operationId};
    return {true,BackendCommandType::Review,"Reanalysis started",started.operationId};
}
} // namespace backend::bridge
