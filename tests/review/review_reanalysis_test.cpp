// review_reanalysis_test (#512 slice 2, plan
// docs/exec-plans/active/2026-10-18-one-review-implementation.md)
//
// The reanalysis engine (backend/review/Reanalysis.h) moved out of
// BackendFacade into mib_review_core. This drives it directly — no
// AppBackend, one standalone ProcessingService — so any shell can rely on it:
//  - request parsing: defaults, every field, and the refusals with the
//    messages the facade returns (contract `review_reanalysis_json`);
//  - round trip: "all" in timestamp order, a dataset range, a folder with a
//    synthetic background; source index + timestamp preserved;
//  - fault injection: cancel before start and during processing, frame
//    budget, an output created while the job runs (never overwritten), no
//    partial file left behind;
//  - the status snapshots, byte for byte;
//  - the independent-reader preview;
//  - the D6 options (#512 S8; YOFO Review's regenerate): parsing never sets
//    them (the facade keeps preserve + quiet tile + timestamp order); rebase
//    clamps rows recorded before the first selected one to 0; "all" in
//    dataset order; the median-of-32 background, which differs from the
//    quiet-tile one on the same frames; colour HDF frames refused by default
//    (the facade) and converted BGR -> gray with `convertColorToGray`.
// The facade surface over it is covered by backend.export_bridge_facade.
// Watchdog guarded.

#include "backend/processing/BatchMaskSources.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/Reanalysis.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using backend::review::ReanalysisFrameOrder;
using backend::review::ReanalysisOutcome;
using backend::review::ReanalysisRequest;
using backend::review::ReanalysisSyntheticBackground;
using backend::review::ReanalysisTimestamps;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using nlohmann::json;

namespace
{
    constexpr int kValid = 12, kInvalid = 6;

    std::uint64_t timestampFor(std::uint64_t index) { return 5000 + 7 * (118 - index); } // later index, earlier time

    cv::Mat cellImage(int radius)
    {
        cv::Mat m(32, 32, CV_8UC1, cv::Scalar(90));
        cv::circle(m, {16, 16}, radius, cv::Scalar(20), -1);
        cv::circle(m, {16, 16}, 2, cv::Scalar(90), -1);
        return m;
    }

    void writeFixture(const fs::path &path)
    {
        Hdf5Service writer;
        MIB_REQUIRE(writer.openFile(path.string()) && writer.initializeDatasets(), "fixture open");
        std::vector<ProcessedFrame> valid, invalid;
        for (int i = 0; i < kValid + kInvalid; ++i)
        {
            ProcessedFrame f;
            f.index = 100 + static_cast<std::uint64_t>(i);
            f.timestampNs = timestampFor(f.index);
            f.originalImage = cellImage(6 + i % 4);
            f.processedImage = cv::Mat::zeros(32, 32, CV_8UC1);
            f.validation.isValid = i < kValid;
            f.validation.objectCount = 1;
            (i < kValid ? valid : invalid).push_back(f);
        }
        MIB_REQUIRE(writer.appendFrames(valid, invalid), "fixture frames");
        const cv::Mat background(32, 32, CV_8UC1, cv::Scalar(90));
        MIB_REQUIRE(writer.writeExperimentInfo(1000, 2000, kValid, kInvalid, {}, {0, 0, 32, 32}, &background),
                    "fixture info");
        writer.closeFile();
    }

    bool noPartials(const fs::path &dir)
    {
        for (const auto &e : fs::directory_iterator(dir))
            if (e.path().filename().string().find(".partial-") != std::string::npos) return false;
        return true;
    }

    std::vector<ProcessedFrame> outputFrames(const fs::path &path)
    {
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path.string()), "output opens");
        std::vector<ProcessedFrame> valid, invalid;
        reader.readValidMetadata(valid);
        reader.readInvalidMetadata(invalid);
        valid.insert(valid.end(), invalid.begin(), invalid.end());
        return valid;
    }

    std::shared_ptr<std::atomic<bool>> flag(bool value = false)
    {
        return std::make_shared<std::atomic<bool>>(value);
    }
} // namespace

int main()
{
    mib::test::Watchdog wd(120);
    mib::test::TempDir td("review_reanalysis");
    const fs::path source = td.path() / "source.h5";
    writeFixture(source);
    backend::services::ProcessingService kernel;
    backend::services::ProcessingConfig defaults;
    defaults.ring_ratio_min = 3.25; // marks "the host's config" in parse results
    std::string error;

    // ---- parsing ---------------------------------------------------------------
    {
        ReanalysisRequest r;
        MIB_REQUIRE(backend::review::parseReanalysisRequest(
                        json{{"source_path", source.string()}, {"output_path", (td.path() / "new.h5").string()}}.dump(),
                        defaults, r, error),
                    "minimal request: " + error);
        MIB_EXPECT(r.sourceKind == "hdf" && r.dataset == "/valid_frames/images", "default kind and dataset");
        MIB_EXPECT(r.start == 0 && r.count == 0 && r.maxFrames == 4096 && r.maxInputBytes == 268435456ULL,
                   "default range and budgets");
        MIB_EXPECT(!r.overrideRoi && !r.backgroundIndex && !r.syntheticBackground && !r.clearBackground,
                   "no overrides by default");
        MIB_EXPECT(r.config.ring_ratio_min == 3.25, "defaults config used without image_processing");
        MIB_EXPECT(r.backgroundDataset == "/valid_frames/images", "default background dataset");

        const json full{{"source_kind", "folder"},
                        {"source_path", "/in"},
                        {"output_path", (td.path() / "full.h5").string()},
                        {"dataset", "all"},
                        {"start", 2},
                        {"count", 3},
                        {"max_frames", 10},
                        {"max_input_mib", 2},
                        {"synthetic_background", true},
                        {"clear_background", true},
                        {"background_index", 4},
                        {"background_dataset", "/invalid_frames/images"},
                        {"roi", {{"x", 1}, {"y", 2}, {"w", 3}, {"h", 4}}},
                        {"image_processing", {{"ring_ratio_min", 7.5}}}};
        MIB_REQUIRE(backend::review::parseReanalysisRequest(full.dump(), defaults, r, error), "full request: " + error);
        MIB_EXPECT(r.sourceKind == "folder" && r.sourcePath == "/in" && r.dataset == "all", "kind, source, dataset");
        MIB_EXPECT(r.start == 2 && r.count == 3 && r.maxFrames == 10 && r.maxInputBytes == 2ULL * 1048576,
                   "range and budgets");
        MIB_EXPECT(r.syntheticBackground && r.clearBackground && r.backgroundIndex && *r.backgroundIndex == 4 &&
                       r.backgroundDataset == "/invalid_frames/images",
                   "background options");
        MIB_EXPECT(r.overrideRoi && r.roi.x == 1 && r.roi.y == 2 && r.roi.w == 3 && r.roi.h == 4, "ROI override");
        MIB_EXPECT(r.config.ring_ratio_min == 7.5, "image_processing overrides the defaults");
        MIB_EXPECT(r.timestamps == ReanalysisTimestamps::Preserve &&
                       r.syntheticBackgroundKind == ReanalysisSyntheticBackground::QuietTile &&
                       r.frameOrder == ReanalysisFrameOrder::Timestamp,
                   "parsing keeps the facade's semantics (D6 options are not in the JSON)");

        const std::string out = (td.path() / "x.h5").string();
        const std::vector<std::pair<json, std::string>> refusals{
            {{{"source_kind", "tiff"}, {"source_path", "/in"}, {"output_path", out}}, "Unknown source kind"},
            {{{"source_path", ""}, {"output_path", out}}, "Source and output paths are required"},
            {{{"source_path", "/in"}, {"output_path", out}, {"roi", {{"x", -1}, {"y", 0}, {"w", 4}, {"h", 4}}}},
             "ROI fields must be nonnegative 32-bit integers"},
            {{{"source_path", "/in"}, {"output_path", out}, {"roi", {{"x", 1.5}, {"y", 0}, {"w", 4}, {"h", 4}}}},
             "ROI fields must be nonnegative 32-bit integers"},
            {{{"source_path", "/in"}, {"output_path", out}, {"roi", {{"x", 0}, {"y", 0}, {"w", 0}, {"h", 4}}}},
             "ROI must have nonnegative origin and positive size"},
            {{{"source_path", "/in"}, {"output_path", out}, {"start", 1.5}}, "start must be a nonnegative integer"},
            {{{"source_path", "/in"}, {"output_path", out}, {"start", -3}}, "start must be a nonnegative integer"},
            {{{"source_path", "/in"}, {"output_path", out}, {"count", "3"}}, "count must be a nonnegative integer"},
            {{{"source_path", "/in"}, {"output_path", out}, {"max_frames", 0}},
             "Input budget must be 1–1000000 frames and 1–16384 MiB"},
            {{{"source_path", "/in"}, {"output_path", out}, {"max_input_mib", 20000}},
             "Input budget must be 1–1000000 frames and 1–16384 MiB"},
            {{{"source_path", "/in"}, {"output_path", out}, {"dataset", "/foo"}}, "Unsupported HDF dataset"},
            {{{"source_path", "/in"}, {"output_path", source.string()}}, "Output already exists; choose a new file"},
            {{{"source_path", "/in"}, {"output_path", out}, {"background_index", -1}},
             "background_index must be a nonnegative integer"},
        };
        for (const auto &[request, message] : refusals)
        {
            error.clear();
            MIB_EXPECT(!backend::review::parseReanalysisRequest(request.dump(), defaults, r, error) && error == message,
                       "refusal '" + message + "', got '" + error + "'");
        }
        for (const std::string bad : {std::string("{bad"), json{{"output_path", out}}.dump(),
                                      json{{"source_path", "/in"}}.dump(),
                                      json{{"source_path", "/in"}, {"output_path", out},
                                           {"image_processing", {{"ring_ratio_min", "x"}}}}
                                          .dump()})
        {
            error.clear();
            MIB_EXPECT(!backend::review::parseReanalysisRequest(bad, defaults, r, error) && !error.empty(),
                       "malformed request refused: " + bad);
        }
    }
    wd.mark("parse");

    const auto parse = [&](const json &request) {
        ReanalysisRequest r;
        MIB_REQUIRE(backend::review::parseReanalysisRequest(request.dump(), backend::services::ProcessingConfig{}, r, error),
                    "parse: " + error);
        return r;
    };

    // ---- round trip: whole file in timestamp order ------------------------------
    {
        const fs::path out = td.path() / "all.h5";
        std::uint64_t lastDone = 0, lastTotal = 0;
        const auto outcome = backend::review::runReanalysis(
            parse({{"source_path", source.string()}, {"output_path", out.string()}, {"dataset", "all"}}), kernel, flag(),
            [&](std::uint64_t done, std::uint64_t total) { lastDone = done; lastTotal = total; });
        MIB_REQUIRE(outcome.published, "all published: " + outcome.error);
        MIB_EXPECT(!outcome.cancelled && outcome.error.empty() && outcome.retainedPartialPath.empty(), "clean outcome");
        MIB_EXPECT(lastTotal == kValid + kInvalid && lastDone == lastTotal, "progress reaches the whole source");
        const auto frames = outputFrames(out);
        MIB_EXPECT(!frames.empty() && frames.size() == outcome.produced, "output holds every produced frame");
        for (const auto &f : frames)
            MIB_EXPECT(f.index >= 100 && f.index < 100 + kValid + kInvalid && f.timestampNs == timestampFor(f.index),
                       "source index and timestamp preserved");
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(out.string()), "reopen");
        backend::processing::ProcessingCoreIdentity identity;
        MIB_EXPECT(reader.readProcessingCoreIdentity(identity), "active core identity recorded");
        MIB_EXPECT(noPartials(td.path()), "no partial residue");
    }
    wd.mark("all");

    // ---- one dataset, a range -------------------------------------------------
    {
        const fs::path out = td.path() / "range.h5";
        const auto outcome = backend::review::runReanalysis(
            parse({{"source_path", source.string()},
                   {"output_path", out.string()},
                   {"dataset", "/valid_frames/images"},
                   {"start", 2},
                   {"count", 5}}),
            kernel, flag(), {});
        MIB_REQUIRE(outcome.published, "range published: " + outcome.error);
        for (const auto &f : outputFrames(out))
            MIB_EXPECT(f.index >= 102 && f.index <= 106, "only the selected valid rows");
    }

    // ---- folder with a synthetic background --------------------------------------
    {
        const fs::path folder = td.path() / "folder";
        fs::create_directory(folder);
        for (int i = 0; i < 5; ++i)
            MIB_REQUIRE(cv::imwrite((folder / ("f" + std::to_string(i) + ".png")).string(), cellImage(5 + i)), "png");
        const fs::path out = td.path() / "folder.h5";
        const auto outcome = backend::review::runReanalysis(parse({{"source_kind", "folder"},
                                                                   {"source_path", folder.string()},
                                                                   {"output_path", out.string()},
                                                                   {"start", 1},
                                                                   {"count", 3},
                                                                   {"synthetic_background", true}}),
                                                            kernel, flag(), {});
        MIB_REQUIRE(outcome.published, "folder published: " + outcome.error);
        for (const auto &f : outputFrames(out)) MIB_EXPECT(f.index >= 1 && f.index <= 3, "folder indices are start + i");
        Hdf5Service reader;
        cv::Mat background;
        MIB_EXPECT(reader.loadFile(out.string()) && reader.readBackgroundImage(background) && !background.empty(),
                   "synthetic background persisted");

        cv::Mat preview;
        std::uint64_t index = 0;
        MIB_EXPECT(backend::review::loadReanalysisPreview(
                       json{{"source_kind", "folder"}, {"source_path", folder.string()}, {"index", 1}}.dump(), preview,
                       index) &&
                       index == 1 && preview.size() == cv::Size(32, 32) && cv::norm(preview, cellImage(6), cv::NORM_INF) == 0,
                   "preview reads the requested folder frame");
        MIB_EXPECT(!backend::review::loadReanalysisPreview(
                       json{{"source_path", source.string()}, {"dataset", "/valid_frames/images"}, {"index", 99}}.dump(),
                       preview, index),
                   "out-of-range preview refused");
        MIB_EXPECT(!backend::review::loadReanalysisPreview(json{{"source_path", source.string()}, {"index", -1}}.dump(),
                                                           preview, index),
                   "negative preview index refused");
        MIB_EXPECT(!backend::review::loadReanalysisPreview("{bad", preview, index), "malformed preview refused");
    }
    wd.mark("folder");

    // ---- D6 options (#512 S8): YOFO Review's regenerate semantics ------------------
    {
        // "all" in dataset order (valid rows 100..111, then invalid 112..117)
        // with timestamps minus the first selected row's: the fixture records
        // later indices EARLIER, so row 100 is the latest and every other row
        // clamps to 0 (the pre-S8 YOFO rule).
        ReanalysisRequest yofo = parse(
            {{"source_path", source.string()}, {"output_path", (td.path() / "yofo-all.h5").string()}, {"dataset", "all"}});
        yofo.timestamps = ReanalysisTimestamps::Rebase;
        yofo.frameOrder = ReanalysisFrameOrder::Datasets;
        const auto outcome = backend::review::runReanalysis(yofo, kernel, flag(), {});
        MIB_REQUIRE(outcome.published, "dataset order published: " + outcome.error);
        std::size_t valid = 0;
        const auto yofoFrames = outputFrames(yofo.outputPath);
        for (const auto &f : yofoFrames)
        {
            valid += f.validation.isValid ? 1 : 0;
            MIB_EXPECT(f.timestampNs == 0 && f.index >= 100 && f.index < 100 + kValid + kInvalid,
                       "rebased to row 100 and clamped: index " + std::to_string(f.index));
        }
        MIB_EXPECT(!yofoFrames.empty() && outcome.valid == valid, "outcome counts the valid records");

        // Rebase alone (timestamp order): offsets from the earliest row, 117.
        ReanalysisRequest rebased = parse(
            {{"source_path", source.string()}, {"output_path", (td.path() / "rebased.h5").string()}, {"dataset", "all"}});
        rebased.timestamps = ReanalysisTimestamps::Rebase;
        MIB_REQUIRE(backend::review::runReanalysis(rebased, kernel, flag(), {}).published, "rebased published");
        for (const auto &f : outputFrames(rebased.outputPath))
            MIB_EXPECT(f.timestampNs == timestampFor(f.index) - timestampFor(117),
                       "rebased offset for index " + std::to_string(f.index));

        // Dataset order vs timestamp order: the first output row of the
        // invalid set differs (112 as stored vs 117, the earliest).
        const auto firstInvalid = [&](const fs::path &path) {
            Hdf5Service reader;
            MIB_REQUIRE(reader.loadFile(path.string()), "reopen");
            std::vector<ProcessedFrame> rows;
            reader.readInvalidMetadata(rows);
            return rows.empty() ? std::uint64_t{0} : rows.front().index;
        };
        const auto defaultOrder = firstInvalid(td.path() / "all.h5");
        const auto datasetOrder = firstInvalid(yofo.outputPath);
        MIB_EXPECT(defaultOrder != datasetOrder && defaultOrder != 0 && datasetOrder != 0,
                   "dataset order keeps the rows as stored (" + std::to_string(datasetOrder) + " vs timestamp order " +
                       std::to_string(defaultOrder) + ")");

        // Median of the first <= 32 frames, the upper median; quiet tile on
        // the same frames gives another background.
        const fs::path folder = td.path() / "median";
        fs::create_directory(folder);
        std::vector<cv::Mat> frames;
        for (int i = 0; i < 6; ++i)
        {
            cv::Mat m(32, 32, CV_8UC1);
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x) m.at<uint8_t>(y, x) = static_cast<uint8_t>((x * 7 + y * 3 + i * 41) % 251);
            frames.push_back(m);
            MIB_REQUIRE(cv::imwrite((folder / ("m" + std::to_string(i) + ".png")).string(), m), "median png");
        }
        cv::Mat oracle(32, 32, CV_8UC1);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
            {
                std::vector<uint8_t> column;
                for (const auto &m : frames) column.push_back(m.at<uint8_t>(y, x));
                std::sort(column.begin(), column.end());
                oracle.at<uint8_t>(y, x) = column[column.size() / 2];
            }
        ReanalysisRequest median = parse({{"source_kind", "folder"},
                                          {"source_path", folder.string()},
                                          {"output_path", (td.path() / "median.h5").string()},
                                          {"synthetic_background", true}});
        median.syntheticBackgroundKind = ReanalysisSyntheticBackground::MedianOf32;
        MIB_REQUIRE(backend::review::runReanalysis(median, kernel, flag(), {}).published, "median published");
        Hdf5Service reader;
        cv::Mat background;
        MIB_REQUIRE(reader.loadFile(median.outputPath) && reader.readBackgroundImage(background), "median background");
        MIB_EXPECT(cv::norm(background, oracle, cv::NORM_INF) == 0, "median-of-32 background");
        MIB_EXPECT(cv::norm(oracle, backend::services::batch_masks::buildSyntheticBackground(frames), cv::NORM_INF) > 0,
                   "quiet tile differs on these frames (test is sensitive)");
    }
    {
        // Colour HDF frames: the facade refuses them; YOFO's option converts
        // them the way batch_masks::loadFromHdf5 does.
        const fs::path colour = td.path() / "colour.h5";
        std::vector<cv::Mat> bgr;
        {
            Hdf5Service writer;
            MIB_REQUIRE(writer.openFile(colour.string()) && writer.initializeDatasets(), "colour open");
            std::vector<ProcessedFrame> valid;
            for (int i = 0; i < 3; ++i)
            {
                cv::Mat m(32, 32, CV_8UC3, cv::Scalar(20 + i, 90, 160));
                cv::circle(m, {16, 16}, 6 + i, cv::Scalar(240, 10, 60), -1);
                bgr.push_back(m);
                ProcessedFrame f;
                f.index = 300 + static_cast<std::uint64_t>(i);
                f.timestampNs = 9000 + static_cast<std::uint64_t>(i);
                f.originalImage = m;
                f.processedImage = cv::Mat::zeros(32, 32, CV_8UC1);
                f.validation.isValid = true;
                valid.push_back(f);
            }
            MIB_REQUIRE(writer.appendFrames(valid, {}), "colour frames");
            MIB_REQUIRE(writer.writeExperimentInfo(1000, 2000, 3, 0, {}, {0, 0, 32, 32}, nullptr), "colour info");
            writer.closeFile();
        }
        ReanalysisRequest refused =
            parse({{"source_path", colour.string()}, {"output_path", (td.path() / "colour-refused.h5").string()}});
        const auto no = backend::review::runReanalysis(refused, kernel, flag(), {});
        MIB_EXPECT(!no.published && no.error == "Source frames must be grayscale", "facade refuses colour: " + no.error);
        ReanalysisRequest converted = refused;
        converted.outputPath = (td.path() / "colour-gray.h5").string();
        converted.convertColorToGray = true;
        const auto yes = backend::review::runReanalysis(converted, kernel, flag(), {});
        MIB_REQUIRE(yes.published, "converted colour published: " + yes.error);
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(converted.outputPath), "colour output");
        std::vector<ProcessedFrame> rows;
        reader.readValidMetadata(rows);
        std::vector<ProcessedFrame> invalidRows;
        reader.readInvalidMetadata(invalidRows);
        std::size_t checked = 0;
        for (const bool valid : {true, false})
        {
            const auto &set = valid ? rows : invalidRows;
            for (std::size_t i = 0; i < set.size(); ++i)
            {
                cv::Mat image, expected;
                MIB_REQUIRE(reader.readImageByIndex(valid ? "/valid_frames/images" : "/invalid_frames/images", i, image),
                            "colour output image");
                cv::cvtColor(bgr.at(set[i].index - 300), expected, cv::COLOR_BGR2GRAY);
                MIB_EXPECT(image.type() == CV_8UC1 && cv::norm(image, expected, cv::NORM_INF) == 0,
                           "output frame is the BGR -> gray source");
                ++checked;
            }
        }
        MIB_EXPECT(checked > 0, "colour output holds records");
    }
    wd.mark("d6 options");

    // ---- fault injection ----------------------------------------------------------
    {
        const fs::path out = td.path() / "cancel-before.h5";
        const auto outcome = backend::review::runReanalysis(
            parse({{"source_path", source.string()}, {"output_path", out.string()}}), kernel, flag(true), {});
        MIB_EXPECT(!outcome.published && outcome.cancelled && outcome.error == "Reanalysis cancelled",
                   "cancel before start");
        MIB_EXPECT(!fs::exists(out) && noPartials(td.path()), "cancel before start writes nothing");
    }
    {
        const fs::path out = td.path() / "cancel-during.h5";
        auto cancel = flag();
        const auto outcome = backend::review::runReanalysis(
            parse({{"source_path", source.string()}, {"output_path", out.string()}, {"dataset", "all"}}), kernel, cancel,
            [&](std::uint64_t, std::uint64_t) { cancel->store(true); });
        MIB_EXPECT(!outcome.published && outcome.cancelled, "cancel during processing");
        MIB_EXPECT(!fs::exists(out) && noPartials(td.path()), "cancel during processing publishes nothing");
    }
    {
        const auto outcome = backend::review::runReanalysis(
            parse({{"source_path", source.string()},
                   {"output_path", (td.path() / "budget.h5").string()},
                   {"dataset", "all"},
                   {"max_frames", 2}}),
            kernel, flag(), {});
        MIB_EXPECT(!outcome.published && !outcome.cancelled &&
                       outcome.error == "Selected range exceeds configured frame budget",
                   "frame budget refused: " + outcome.error);
    }
    {
        const auto outcome = backend::review::runReanalysis(
            parse({{"source_path", (td.path() / "missing.h5").string()}, {"output_path", (td.path() / "m.h5").string()}}),
            kernel, flag(), {});
        MIB_EXPECT(!outcome.published && outcome.error == "Cannot open HDF source", "missing source fails");
    }
    {
        const fs::path out = td.path() / "collision.h5";
        const auto outcome = backend::review::runReanalysis(
            parse({{"source_path", source.string()}, {"output_path", out.string()}, {"dataset", "all"}}), kernel, flag(),
            [&](std::uint64_t, std::uint64_t) {
                if (!fs::exists(out)) std::ofstream(out) << "preserve concurrent output";
            });
        MIB_EXPECT(!outcome.published && !outcome.cancelled && !outcome.error.empty(), "late collision fails");
        std::ifstream existing(out);
        std::string contents;
        std::getline(existing, contents);
        MIB_EXPECT(contents == "preserve concurrent output", "concurrent output never overwritten");
        MIB_EXPECT(noPartials(td.path()), "collision leaves no partial");
    }
    wd.mark("faults");

    // ---- status snapshots ------------------------------------------------------------
    {
        MIB_EXPECT(backend::review::reanalysisLoadingStatusJson(7) == R"({"operation_id":"7","phase":"loading","state":"running"})",
                   "loading status");
        MIB_EXPECT(backend::review::reanalysisProgressStatusJson(7, 3, 9) ==
                       R"({"completed":"3","operation_id":"7","phase":"processing","state":"running","total":"9"})",
                   "progress status");
        ReanalysisRequest r;
        r.outputPath = "/out/new.h5";
        ReanalysisOutcome done;
        done.published = true;
        done.produced = 18;
        done.valid = 5;
        MIB_EXPECT(backend::review::reanalysisTerminalStatusJson(7, r, done) ==
                       R"({"error":"","final_path":"/out/new.h5","objects":"18","operation_id":"7","retained_partial_path":"","state":"completed","valid_objects":"5","warnings":[]})",
                   "completed status");
        ReanalysisOutcome cancelled;
        cancelled.cancelled = true;
        cancelled.error = "Reanalysis cancelled";
        MIB_EXPECT(backend::review::reanalysisTerminalStatusJson(8, r, cancelled) ==
                       R"({"error":"Reanalysis cancelled","final_path":"","objects":"0","operation_id":"8","retained_partial_path":"","state":"cancelled","valid_objects":"0","warnings":[]})",
                   "cancelled status");
        ReanalysisOutcome failed;
        failed.error = "Failed to save regenerated HDF";
        failed.warning = " Temporary file cleanup failed: x";
        failed.retainedPartialPath = "/out/.new.h5.partial-1";
        failed.produced = 2;
        MIB_EXPECT(backend::review::reanalysisTerminalStatusJson(9, r, failed) ==
                       R"({"error":"Failed to save regenerated HDF","final_path":"","objects":"2","operation_id":"9","retained_partial_path":"/out/.new.h5.partial-1","state":"failed","valid_objects":"0","warnings":[" Temporary file cleanup failed: x"]})",
                   "failed status with warning");
        MIB_EXPECT(backend::review::reanalysisStartFailedStatusJson(10, "no thread") ==
                       R"({"error":"no thread","operation_id":"10","state":"failed"})",
                   "start-failed status");
    }

    return mib::test::exitCode();
}
