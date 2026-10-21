// review_reanalysis_pixel_factor_test (#758)
//
// Regenerate / reanalysis validated objects at the kernel's own factor (the
// 0.4886 µm/px default in YOFO Review, the live factor in MIB Studio) instead
// of the source file's recorded one. Fixture (pixel_factor_fixture.h): one
// bright disc per frame, valid and in the target group at 0.25 µm/px, invalid
// at 0.4886. This drives:
//  - processBatch: a per-call factor and channel band replace the live ones
//    and leave them unchanged (BatchAnalysisInputs);
//  - the engine directly (runReanalysis): recorded settings take the
//    source's recorded factor; the fallback applies when it records none and
//    to folder sources; without the flag the given factor (the default) is
//    used; the output records the factor (`/experiment_info
//    @pixel_to_micron`), ReviewSession reads it back as recorded; two sources
//    with different factors run back to back on one kernel each use their
//    own; the kernel's live factor is never set (checked from inside the
//    job, and by a live caller running beside it); the channel band is the
//    one of the run's background, not the kernel's live band;
//  - YOFO Review's regenerate job (ReviewJobs): recorded settings at the
//    file's factor (the session's fallback when it records none), default
//    settings at 0.4886, the completion message naming the factor; two files
//    regenerated in turn on one kernel each at its own factor.
// The facade (`review_reanalysis_json`) is backend.facade_reanalysis_pixel_factor.
// Watchdog guarded.

#include "backend/processing/BatchMaskSources.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/Reanalysis.h"
#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"

#include "review/pixel_factor_fixture.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace pixel_factor_fixture;
using backend::review::ReanalysisOutcome;
using backend::review::ReanalysisRequest;
using backend::review::RegenerateMasksRequest;
using backend::review::RegenerateSource;
using backend::review::ReviewJobEvent;
using backend::review::ReviewJobs;
using backend::review::ReviewJobState;
using backend::review::ReviewSession;

namespace
{
    std::shared_ptr<std::atomic<bool>> flag() { return std::make_shared<std::atomic<bool>>(false); }

    ReanalysisRequest hdfRequest(const fs::path &source, const fs::path &output, const ProcessingConfig &config)
    {
        ReanalysisRequest r;
        r.sourceKind = "hdf";
        r.sourcePath = source.string();
        r.outputPath = output.string();
        r.dataset = "all";
        r.config = config;
        return r;
    }

    struct Sink
    {
        std::mutex m;
        std::condition_variable cv;
        std::vector<ReviewJobEvent> events;
        void operator()(const ReviewJobEvent &e)
        {
            std::scoped_lock lock(m);
            events.push_back(e);
            cv.notify_all();
        }
        bool terminal(std::uint64_t id, ReviewJobEvent &out)
        {
            std::unique_lock lock(m);
            return cv.wait_for(lock, std::chrono::seconds(60), [&] {
                for (const auto &e : events)
                    if (e.operationId == id && (e.state == ReviewJobState::Completed ||
                                                e.state == ReviewJobState::Failed || e.state == ReviewJobState::Cancelled))
                    {
                        out = e;
                        return true;
                    }
                return false;
            });
        }
    };

    bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
} // namespace

int main()
{
    mib::test::Watchdog wd(240);
    mib::test::TempDir td("reanalysis_pixel_factor");
    const double areaPx = discAreaPx();
    const ProcessingConfig flip = flipConfig(areaPx);
    const ProcessingConfig targetOnly = targetOnlyConfig(areaPx);

    const fs::path at25 = td / "recorded_025.h5";      // records 0.25
    const fs::path at4886 = td / "recorded_04886.h5";  // records 0.4886
    const fs::path none = td / "records_none.h5";      // no run snapshot
    const fs::path target25 = td / "target_025.h5";    // records 0.25, target-only config
    writeSource(at25, kRecorded, flip);
    writeSource(at4886, kDefault, flip);
    writeSource(none, 0.0, flip);
    writeSource(target25, kRecorded, targetOnly);
    const fs::path folder = td / "folder";
    writeFolder(folder);

    // ---- processBatch: per-call inputs replace the live state, never change it ----
    {
        ProcessingService kernel; // live factor: the default
        const std::vector<cv::Mat> frames{disc(0)};
        ProcessingService::BatchAnalysisInputs inputs;
        inputs.pixelToMicronFactor = kRecorded;
        const auto atRecorded = kernel.processBatch(frames, flip, blackBackground(), {}, {}, nullptr, inputs);
        const auto atLive = kernel.processBatch(frames, flip, blackBackground());
        MIB_REQUIRE(atRecorded.size() == 1 && atLive.size() == 1, "one object per call");
        MIB_EXPECT(atRecorded[0].validation.isValid && atRecorded[0].validation.isTargetGroup &&
                       near(atRecorded[0].validation.analysisPixelToMicronFactor, kRecorded),
                   "per-call 0.25: valid, target group, stamped 0.25");
        MIB_EXPECT(!atLive[0].validation.isValid && !atLive[0].validation.isTargetGroup &&
                       near(atLive[0].validation.analysisPixelToMicronFactor, kDefault),
                   "no inputs: the live 0.4886 (fixture flips; test is sensitive)");
        MIB_EXPECT(near(kernel.getPixelToMicronFactor(), kDefault), "the per-call factor leaves the live one");
        ProcessingService::BatchAnalysisInputs bad;
        bad.pixelToMicronFactor = 0.0;
        bool refused = false;
        try { kernel.processBatch(frames, flip, blackBackground(), {}, {}, nullptr, bad); }
        catch (const std::invalid_argument &) { refused = true; }
        MIB_EXPECT(refused, "a zero factor is refused");
    }
    wd.mark("processBatch inputs");

    // ---- the engine: recorded settings take the source's factor ------------------
    {
        ProcessingService kernel;
        kernel.setPixelToMicronFactor(0.7); // a live factor neither source records
        const fs::path out = td / "engine_recorded.h5";
        auto r = hdfRequest(at25, out, flip);
        r.pixelToMicron = 0.7; // the fallback (MIB Studio: the live factor)
        // From inside the job: the kernel's live factor is untouched, and a
        // live caller sees it (the factor is passed per call, #758).
        bool liveSeenDuringJob = false, liveFactorDuringJob = true;
        const auto outcome = backend::review::runReanalysis(r, kernel, flag(), [&](std::uint64_t done, std::uint64_t) {
            if (done != 1) return;
            liveFactorDuringJob = near(kernel.getPixelToMicronFactor(), 0.7);
            const auto live = kernel.processBatch({disc(0)}, flip, blackBackground());
            liveSeenDuringJob = live.size() == 1 && near(live[0].validation.analysisPixelToMicronFactor, 0.7);
        });
        MIB_REQUIRE(outcome.published, "recorded run published: " + outcome.error);
        MIB_EXPECT(near(outcome.pixelToMicron, kRecorded) && outcome.pixelToMicronRecorded,
                   "outcome: the recorded 0.25");
        const auto o = readOutput(out);
        MIB_EXPECT(allObjects(o, true, true), "validity and target group at 0.25");
        MIB_EXPECT(o.recordedFactor && near(*o.recordedFactor, kRecorded), "the output records 0.25");
        MIB_EXPECT(liveFactorDuringJob && liveSeenDuringJob, "the live factor stays 0.7 while the job runs");
        MIB_EXPECT(near(kernel.getPixelToMicronFactor(), 0.7), "and after it");
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(out.string()), "reopen");
        MIB_EXPECT(near(ReviewSession::recordedPixelToMicron(reader), kRecorded),
                   "review reads the output's factor as recorded");
    }
    {
        // Target group alone flips (area range wide).
        ProcessingService kernel;
        const fs::path out = td / "engine_target.h5";
        const auto outcome = backend::review::runReanalysis(hdfRequest(target25, out, targetOnly), kernel, flag(), {});
        MIB_REQUIRE(outcome.published, "target run: " + outcome.error);
        MIB_EXPECT(allObjects(readOutput(out), true, true), "target group at 0.25");
        auto def = hdfRequest(target25, td / "engine_target_default.h5", targetOnly);
        def.useRecordedPixelToMicron = false;
        MIB_REQUIRE(backend::review::runReanalysis(def, kernel, flag(), {}).published, "target default run");
        MIB_EXPECT(allObjects(readOutput(td / "engine_target_default.h5"), true, false),
                   "valid but no target group at 0.4886");
    }
    {
        // Default settings: the given factor (the default), labelled in the output.
        ProcessingService kernel;
        const fs::path out = td / "engine_default.h5";
        auto r = hdfRequest(at25, out, flip);
        r.useRecordedPixelToMicron = false;
        const auto outcome = backend::review::runReanalysis(r, kernel, flag(), {});
        MIB_REQUIRE(outcome.published, "default run: " + outcome.error);
        MIB_EXPECT(near(outcome.pixelToMicron, kDefault) && !outcome.pixelToMicronRecorded, "outcome: the default");
        const auto o = readOutput(out);
        MIB_EXPECT(allObjects(o, false, false), "invalid, no target group at 0.4886");
        MIB_EXPECT(o.recordedFactor && near(*o.recordedFactor, kDefault), "the output records 0.4886");
    }
    {
        // A source that records no factor, and a folder: the fallback.
        ProcessingService kernel;
        auto r = hdfRequest(none, td / "engine_none.h5", flip);
        r.pixelToMicron = kRecorded;
        auto outcome = backend::review::runReanalysis(r, kernel, flag(), {});
        MIB_REQUIRE(outcome.published, "no-factor run: " + outcome.error);
        MIB_EXPECT(near(outcome.pixelToMicron, kRecorded) && !outcome.pixelToMicronRecorded, "the fallback, not recorded");
        MIB_EXPECT(allObjects(readOutput(r.outputPath), true, true), "validated at the fallback 0.25");
        ReanalysisRequest f;
        f.sourceKind = "folder";
        f.sourcePath = folder.string();
        f.outputPath = (td / "engine_folder.h5").string();
        f.config = flip;
        f.pixelToMicron = kRecorded;
        outcome = backend::review::runReanalysis(f, kernel, flag(), {});
        MIB_REQUIRE(outcome.published, "folder run: " + outcome.error);
        const auto o = readOutput(f.outputPath);
        MIB_EXPECT(allObjects(o, true, true) && o.recordedFactor && near(*o.recordedFactor, kRecorded),
                   "folder at the given 0.25, recorded in the output");
        ReanalysisRequest invalid = f;
        invalid.outputPath = (td / "engine_bad_factor.h5").string();
        invalid.pixelToMicron = 0.0;
        outcome = backend::review::runReanalysis(invalid, kernel, flag(), {});
        MIB_EXPECT(!outcome.published && outcome.error == "Pixel-to-micron factor must be finite and positive" &&
                       !fs::exists(invalid.outputPath),
                   "a zero factor is refused, nothing written: " + outcome.error);
    }
    wd.mark("engine");

    // ---- the recorded target group: from the config document ---------------------
    {
        // The target group has no processing_config_* attribute; captures
        // record it in `config_json`, and so do outputs now.
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(at25.string()), "open source");
        ProcessingConfig recorded;
        MIB_REQUIRE(reader.readRecordedProcessingConfig(recorded), "recorded config");
        MIB_EXPECT(recorded.enable_target_group && recorded.target_group_area_min == flip.target_group_area_min &&
                       recorded.target_group_area_max == flip.target_group_area_max &&
                       recorded.target_group_deformability_max == flip.target_group_deformability_max,
                   "the source's target group, from its config document");
        reader.closeFile();
        const fs::path legacy = td / "no_document.h5";
        writeSource(legacy, kRecorded, flip, false);
        MIB_REQUIRE(reader.loadFile(legacy.string()), "open legacy");
        ProcessingConfig legacyConfig;
        MIB_REQUIRE(reader.readRecordedProcessingConfig(legacyConfig), "legacy recorded config");
        MIB_EXPECT(!legacyConfig.enable_target_group && legacyConfig.area_threshold_max == flip.area_threshold_max,
                   "no document: the attributes, and no target group, as before");
        reader.closeFile();
        const auto o = readOutput(td / "engine_recorded.h5");
        MIB_EXPECT(o.config.enable_target_group && o.config.target_group_area_min == flip.target_group_area_min &&
                       o.config.target_group_area_max == flip.target_group_area_max,
                   "an output records the target group it applied");
    }

    // ---- a batch of two sources on one kernel: each its own factor ----------------
    {
        ProcessingService kernel;
        const std::vector<std::pair<fs::path, double>> order{{at25, kRecorded}, {at4886, kDefault}, {at25, kRecorded},
                                                             {none, 0.7}};
        int n = 0;
        for (const auto &[source, expected] : order)
        {
            const fs::path out = td / ("batch_" + std::to_string(n++) + ".h5");
            auto r = hdfRequest(source, out, flip);
            r.pixelToMicron = 0.7; // the fallback for the file that records none
            const auto outcome = backend::review::runReanalysis(r, kernel, flag(), {});
            MIB_REQUIRE(outcome.published, "batch run " + out.string() + ": " + outcome.error);
            const auto o = readOutput(out);
            MIB_EXPECT(near(outcome.pixelToMicron, expected) && o.recordedFactor && near(*o.recordedFactor, expected),
                       out.filename().string() + " uses its own source's factor " + std::to_string(expected) +
                           ", got " + std::to_string(outcome.pixelToMicron));
            MIB_EXPECT(allObjects(o, near(expected, kRecorded), near(expected, kRecorded)),
                       out.filename().string() + ": validity follows its own factor");
        }
        MIB_EXPECT(near(kernel.getPixelToMicronFactor(), kDefault), "the kernel's factor is never set");
    }
    wd.mark("batch");

    // ---- a live caller beside the job: never sees the job's factor ----------------
    {
        ProcessingService kernel; // live 0.4886
        std::atomic<bool> stop{false};
        std::atomic<int> wrong{0}, calls{0};
        std::thread live([&] {
            while (!stop.load())
            {
                const auto records = kernel.processBatch({disc(1)}, flip, blackBackground());
                ++calls;
                for (const auto &rec : records)
                    if (!near(rec.validation.analysisPixelToMicronFactor, kDefault)) ++wrong;
            }
        });
        while (calls.load() < 1) std::this_thread::yield(); // the live caller runs before the jobs start
        for (int i = 0; i < 6; ++i)
        {
            const fs::path out = td / ("beside_" + std::to_string(i) + ".h5");
            MIB_REQUIRE(backend::review::runReanalysis(hdfRequest(at25, out, flip), kernel, flag(), {}).published,
                        "job beside a live caller");
            MIB_EXPECT(allObjects(readOutput(out), true, true), "job at 0.25 beside the live caller");
        }
        stop = true;
        live.join();
        MIB_EXPECT(wrong.load() == 0, "the live caller always analysed at 0.4886 (" + std::to_string(wrong.load()) +
                                          " of " + std::to_string(calls.load()) + " calls saw another factor)");
    }
    wd.mark("beside a live caller");

    // ---- the channel band is the run's, not the kernel's live band ----------------
    {
        // Live: auto band on, a walled background whose band (rows ~14-81)
        // excludes a small disc centred on row 9. The run's black background
        // has no walls: a full-frame band, so the disc is in the channel.
        cv::Mat walled(kRows, kCols, CV_8UC1, cv::Scalar(140));
        for (int r = 0; r < 14; ++r) walled.row(r).setTo(200 + (r % 5) * 6);
        for (int r = 82; r < kRows; ++r) walled.row(r).setTo(195 + (r % 5) * 6);
        ProcessingConfig banded = baseConfig();
        banded.auto_roi_from_background = true;
        ProcessingService kernel;
        kernel.setProcessingConfig(banded);
        kernel.setRealtimeBackgroundGray(walled);
        const auto liveBand = kernel.getChannelBand();
        MIB_REQUIRE(liveBand.h > 0 && liveBand.y > 9, "the live band excludes row 9 (test is sensitive)");
        const fs::path source = td / "band_source.h5";
        {
            Hdf5Service writer;
            MIB_REQUIRE(writer.openFile(source.string()) && writer.initializeDatasets(), "band source");
            ProcessedFrame f;
            f.index = 7;
            f.originalImage = disc(1, 6, 9);
            f.processedImage = cv::Mat::zeros(kRows, kCols, CV_8UC1);
            f.validation.isValid = true;
            f.validation.objectCount = 1;
            MIB_REQUIRE(writer.appendFrames({f}, {}), "band frame");
            const cv::Mat bg = blackBackground();
            MIB_REQUIRE(writer.writeExperimentInfo(1, 2, 1, 0, banded, {0, 0, kCols, kRows}, &bg), "band info");
            writer.closeFile();
        }
        const auto live = kernel.processBatch({disc(1, 6, 9)}, banded, blackBackground());
        MIB_REQUIRE(live.size() == 1 && !live[0].validation.inChannel, "the live band rejects the disc (sensitive)");
        const fs::path out = td / "band_out.h5";
        const auto outcome = backend::review::runReanalysis(hdfRequest(source, out, banded), kernel, flag(), {});
        MIB_REQUIRE(outcome.published, "band run: " + outcome.error);
        const auto o = readOutput(out);
        MIB_EXPECT(o.valid == 1 && o.rows.size() == 1 && o.rows[0].validation.inChannel,
                   "the run's own band: the disc is in the channel and valid");
        const auto runBand = ProcessingService::detectChannelBand(blackBackground(), banded);
        MIB_EXPECT(o.config.auto_roi_from_background && o.config.channel_band_y == runBand.y &&
                       o.config.channel_band_h == runBand.h,
                   "the output records the band applied");
        MIB_EXPECT(kernel.getChannelBand().y == liveBand.y && kernel.getChannelBand().h == liveBand.h,
                   "the live band is unchanged");
    }
    wd.mark("channel band");

    // ---- YOFO Review's regenerate job ----------------------------------------------
    {
        ProcessingService processing; // YOFO's bundled kernel: 0.4886, never set
        ReviewSession session;
        session.setFallbackPixelToMicron(kRecorded);
        Sink sink;
        ReviewJobs jobs(session, &processing, std::ref(sink));
        std::string err;
        ReviewJobEvent done;
        const auto regenerate = [&](const fs::path &source, const fs::path &out, bool recorded) {
            MIB_REQUIRE(session.open(source.string()), "open " + source.string());
            RegenerateMasksRequest req;
            req.source = RegenerateSource::WholeFile;
            req.outputPath = out.string();
            req.useRecordedConfig = recorded;
            req.config = flip; // default settings: the same thresholds, so only the factor differs
            req.roi = {0, 0, kCols, kRows};
            const auto id = jobs.startRegenerateMasks(req, &err);
            MIB_REQUIRE(id != 0, "start regenerate: " + err);
            MIB_REQUIRE(sink.terminal(id, done), "regenerate terminal");
            MIB_REQUIRE(done.state == ReviewJobState::Completed, "regenerate completed: " + done.message);
            return readOutput(out);
        };
        auto o = regenerate(at25, td / "yofo_recorded.h5", true);
        MIB_EXPECT(allObjects(o, true, true), "recorded settings: validity and target group at the recorded 0.25");
        MIB_EXPECT(o.recordedFactor && near(*o.recordedFactor, kRecorded), "the output records 0.25");
        MIB_EXPECT(done.message.find("; px→µm 0.2500 recorded)") != std::string::npos, "message: " + done.message);
        {
            ReviewSession reopened;
            MIB_REQUIRE(reopened.open((td / "yofo_recorded.h5").string()), "reopen regenerated");
            MIB_EXPECT(reopened.metadata().pixelToMicronFromFile && near(reopened.pixelToMicron(), kRecorded),
                       "reopened at its recorded 0.25, not the fallback");
        }
        o = regenerate(at25, td / "yofo_default.h5", false);
        MIB_EXPECT(allObjects(o, false, false), "default settings: at 0.4886, invalid");
        MIB_EXPECT(o.recordedFactor && near(*o.recordedFactor, kDefault), "the default output records 0.4886");
        MIB_EXPECT(done.message.find("; px→µm 0.4886 default)") != std::string::npos, "message: " + done.message);
        session.setFallbackPixelToMicron(kRecorded);
        o = regenerate(none, td / "yofo_fallback.h5", true);
        MIB_EXPECT(allObjects(o, true, true) && o.recordedFactor && near(*o.recordedFactor, kRecorded),
                   "a file without a factor: the session's fallback 0.25");
        MIB_EXPECT(done.message.find("; px→µm 0.2500 fallback: the file records none)") != std::string::npos,
                   "message: " + done.message);
        // Two files in turn on one kernel: each its own factor, nothing carried over.
        o = regenerate(at4886, td / "yofo_seq_1.h5", true);
        MIB_EXPECT(allObjects(o, false, false) && o.recordedFactor && near(*o.recordedFactor, kDefault),
                   "a 0.4886 file after a 0.25 one: at 0.4886");
        o = regenerate(at25, td / "yofo_seq_2.h5", true);
        MIB_EXPECT(allObjects(o, true, true) && o.recordedFactor && near(*o.recordedFactor, kRecorded),
                   "then a 0.25 file again: at 0.25");
        MIB_EXPECT(near(processing.getPixelToMicronFactor(), kDefault), "the bundled kernel's factor is never set");
        jobs.shutdown();
    }
    wd.mark("regenerate job");

    return mib::test::exitCode();
}
