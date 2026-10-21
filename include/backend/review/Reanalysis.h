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
// Semantics (the facade's, the defaults): sources hdf (dataset "all" in
// timestamp order, or one images dataset) / folder / avi with start, count
// and frame + input-byte budgets; optional ROI, processing-config, background
// frame or clear-background overrides; quiet-tile synthetic background;
// source index, timestamp and track ids preserved; written to
// `.<name>.partial-<job>` beside the output and published with a no-replace
// rename (hard-link fallback), so an existing or concurrently created output
// is never overwritten and a partial file never looks finished.
//
// One engine, two semantics (#512 S8, decision D6): YOFO Review's
// regenerate-masks job (ReviewJobs) runs here too, with the options below set
// to its owner-accepted differences of 2026-10-04 (timestamps rebased to the
// first selected frame, a median-of-32 synthetic background, "all" as valid
// rows then invalid rows, colour HDF frames converted to gray). Request
// parsing never sets them, so the facade's output is unchanged.
//
// Calibration (#758): objects are validated and target-grouped at the
// source's own pixel-to-micron factor (ReanalysisRequest::pixelToMicron),
// passed to processBatch per call with the channel band detected from the
// background in use. The kernel's live factor and band are never read or
// changed, so a reanalysis beside a live run or another job cannot race on
// them and one source's factor never reaches the next. The output records
// the factor used (`/experiment_info @pixel_to_micron`) and where it came
// from (`@pixel_to_micron_source`, #764): recorded, operator or fallback.
//
// E-LUT (#764, SourceEModulusLut.h): an hdf source whose run snapshot records
// its E-LUT is reprocessed with that table (found by its hash), passed per
// call; the run is refused before anything is written when it cannot be
// found. A source that loaded none gets no E, as it had none. A source that
// records nothing (no snapshot, or one before v3; folder and AVI) keeps the
// kernel's own LUT, as before. The outcome names what was used.
#pragma once

#include "backend/processing/ProcessingService.h"
#include "backend/review/SourceEModulusLut.h"

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

    // Output timestamps: the source's (facade) or minus the first selected
    // frame's, clamped at 0 (YOFO Review, the Qt BatchMaskDialog's rule).
    enum class ReanalysisTimestamps
    {
        Preserve,
        Rebase,
    };

    // The synthetic background built when `syntheticBackground` is set and
    // no background is in use: the quietest temporal tiles
    // (batch_masks::buildSyntheticBackground; facade, Qt) or the per-pixel
    // median of the first <= 32 selected frames (YOFO Review).
    enum class ReanalysisSyntheticBackground
    {
        QuietTile,
        MedianOf32,
    };

    // hdf dataset "all": every frame in timestamp order (facade, Qt), or the
    // dataset rows as stored, valid then invalid (YOFO Review's whole file).
    enum class ReanalysisFrameOrder
    {
        Timestamp,
        Datasets,
    };

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
        // D6 options; not part of the JSON request (the facade keeps the defaults).
        ReanalysisTimestamps timestamps{ReanalysisTimestamps::Preserve};
        ReanalysisSyntheticBackground syntheticBackgroundKind{ReanalysisSyntheticBackground::QuietTile};
        ReanalysisFrameOrder frameOrder{ReanalysisFrameOrder::Timestamp};
        // hdf only: frames with more than one channel are refused ("Source
        // frames must be grayscale"; facade) or converted to Mono8 the way
        // batch_masks::loadFromHdf5 does (BGR -> gray; YOFO Review's job,
        // whose pre-S8 reader converted them).
        bool convertColorToGray{false};
        // The pixel-to-micron factor (µm per pixel) for area validity and the
        // target group (#758). With `useRecordedPixelToMicron` an hdf
        // source's recorded factor (ReviewSession::recordedPixelToMicron)
        // wins; `pixelToMicron` applies when the source records none, to
        // folder / avi sources and without the flag. Not part of the JSON
        // request: the facade sets its live factor as the fallback, YOFO
        // Review its fallback (recorded settings) or the default (default
        // settings). Must be finite and positive.
        bool useRecordedPixelToMicron{true};
        double pixelToMicron{services::ProcessingService::kDefaultPixelToMicronFactor};
        // `pixelToMicron` was supplied explicitly by the operator for this
        // run (#764): when it applies, the output records it as "operator"
        // instead of "fallback". No host sets it yet.
        bool pixelToMicronExplicit{false};
    };

    // The refusal for an output path that already exists (the facade's
    // `review_reanalysis_json` and YOFO Review's regenerate masks).
    inline constexpr const char *kReanalysisOutputExists = "Output already exists; choose a new file";

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
        std::size_t valid{0};            // of those, the valid ones
        // The factor the objects were validated at (0 when processing never
        // ran) and whether it is the source's recorded one (#758).
        double pixelToMicron{0.0};
        bool pixelToMicronRecorded{false};
        // The E-LUT E was computed with (#764), set once the source was read:
        // Source (`eModulusLut` names the source run's own), NotLoaded (no
        // E: the source loaded none) or Legacy (the source records none: the
        // kernel's own, which `eModulusLut` names; "" when it has none).
        EModulusLutBasis eModulusLutBasis{EModulusLutBasis::Legacy};
        std::string eModulusLut;
        bool eModulusLutResolved{false};
        // For a reply with a message: what E was computed with, said when the
        // source is an hdf file whose own LUT was not used (NotLoaded, or
        // Legacy); empty otherwise. reanalysisTerminalStatusJson adds it to
        // `warnings` of a published run.
        std::string eModulusLutNote;
    };

    // The E-LUT of an outcome in words, for a completion message, e.g.
    // "E-LUT lut-a r1 (sha256 0123456789ab) (the source run's)" or
    // "E not computed: the source run loaded no E-LUT"; "" when the source
    // was never read.
    std::string reanalysisEModulusLutPhrase(const ReanalysisOutcome &outcome);

    using ReanalysisCancelFlag = std::shared_ptr<std::atomic<bool>>;
    // Kernel progress, called after the cancel check (done, total frames).
    using ReanalysisProgressFn = std::function<void(std::uint64_t done, std::uint64_t total)>;

    // Load → `kernel.processBatch` → write the partial file → publish.
    // Never throws; the outcome carries the terminal state.
    ReanalysisOutcome runReanalysis(const ReanalysisRequest &request, services::ProcessingService &kernel,
                                    const ReanalysisCancelFlag &cancel, const ReanalysisProgressFn &onProgress);
    // What `runReanalysis` returns when the flag is set before it starts
    // (nothing read or written): the review job that a shutdown overtook
    // before it ran (ReviewJobs, #742 review round 2).
    ReanalysisOutcome reanalysisCancelledBeforeStart();

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
