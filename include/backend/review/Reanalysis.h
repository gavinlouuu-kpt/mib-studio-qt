// Reanalysis — regenerate masks from an HDF5 file, an image folder or an AVI
// into a NEW HDF5 file. The engine behind the backend bridge's
// `review_reanalysis_json` / `fetch_review_reanalysis_preview` (MIB Studio,
// YOFO Studio), moved out of BackendFacade into mib_review_core so every
// review shell can share it (#512 slice 2, plan
// docs/exec-plans/active/2026-10-18-one-review-implementation.md).
//
// Qt-free, built on mib_processing only. `runReanalysis` runs on the
// caller's thread; the host owns single flight, the operation id, progress
// events and the status snapshot (the facade keeps all of that). Request
// parsing and the status JSON follow bridge-contract.json
// `review_reanalysis_json` exactly; the facade's messages are these.
//
// Semantics (unchanged from the facade): sources hdf (dataset "all" in
// timestamp order, or one images dataset) / folder / avi with start, count
// and frame + input-byte budgets; optional ROI, processing-config, background
// frame or clear-background overrides; quiet-tile synthetic background;
// source index, timestamp and track ids preserved; written to
// `.<name>.partial-<job>` beside the output and published with a no-replace
// rename (hard-link fallback), so an existing or concurrently created output
// is never overwritten and a partial file never looks finished.
#pragma once

#include "backend/processing/ProcessingService.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace cv { class Mat; }

namespace backend::review
{

    struct ReanalysisRequest
    {
        std::string sourceKind{"hdf"}; // hdf | folder | avi
        std::string sourcePath;
        std::string outputPath; // a new file; never replaced
        // hdf only: "all" or one of /valid_frames/images, /invalid_frames/images,
        // /recorded_frames/images.
        std::string dataset{"/valid_frames/images"};
        std::uint64_t start{0};
        std::uint64_t count{0}; // 0 = the rest of the source
        std::uint64_t maxFrames{4096};
        std::uint64_t maxInputBytes{268435456};
        services::ProcessingConfig config{};
        bool syntheticBackground{false};
        bool clearBackground{false};
        std::optional<std::uint64_t> backgroundIndex;
        std::string backgroundDataset{"/valid_frames/images"};
        bool overrideRoi{false};
        services::ProcessingService::Roi roi{0, 0, 0, 0};
    };

    // Parses and validates a `review_reanalysis_json` request. `defaults` is
    // the processing config used when the request carries no
    // `image_processing` (the facade passes the live config). Refuses an
    // output path that already exists. On failure returns false and `error`
    // is the message the bridge returns.
    bool parseReanalysisRequest(const std::string &json, const services::ProcessingConfig &defaults,
                                ReanalysisRequest &out, std::string &error);

    struct ReanalysisOutcome
    {
        bool published{false};
        // Not published and the cancel flag was set when the run ended.
        bool cancelled{false};
        std::string error;
        std::string warning;
        std::string retainedPartialPath; // a partial file that could not be removed
        std::size_t produced{0};         // frames the kernel returned
    };

    using ReanalysisCancelFlag = std::shared_ptr<std::atomic<bool>>;
    // Kernel progress, called after the cancel check (done, total frames).
    using ReanalysisProgressFn = std::function<void(std::uint64_t done, std::uint64_t total)>;

    // Load → `kernel.processBatch` → write the partial file → publish.
    // Never throws; the outcome carries the terminal state.
    ReanalysisOutcome runReanalysis(const ReanalysisRequest &request, services::ProcessingService &kernel,
                                    const ReanalysisCancelFlag &cancel, const ReanalysisProgressFn &onProgress);

    // `fetch_review_reanalysis_preview`: one Mono8 source frame read by an
    // independent reader (source_kind / source_path / dataset / index).
    // False for a malformed request or an unreadable frame.
    bool loadReanalysisPreview(const std::string &json, cv::Mat &image, std::uint64_t &index);

    // Status snapshots of `review_reanalysis_json` (status vocabulary of
    // `review_export_json`; operation ids as decimal strings).
    std::string reanalysisLoadingStatusJson(std::uint64_t operationId);
    std::string reanalysisProgressStatusJson(std::uint64_t operationId, std::uint64_t done, std::uint64_t total);
    std::string reanalysisTerminalStatusJson(std::uint64_t operationId, const ReanalysisRequest &request,
                                             const ReanalysisOutcome &outcome);
    std::string reanalysisStartFailedStatusJson(std::uint64_t operationId, const std::string &error);

} // namespace backend::review
