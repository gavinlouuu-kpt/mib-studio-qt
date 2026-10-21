// review_source_elut_test (#764)
//
// The #758 follow-ups that change what reprocessing records:
//  - E-LUT (item 2): reanalysis (runReanalysis), YOFO Review's regenerate job
//    and the one-record reprocess compute E with the source run's own E-LUT,
//    the table whose bytes have the sha256 its run snapshot v3 records, found
//    in the kernel's loaded LUT or the bundled one compiled into the review
//    core; refused with a plain reason when it is not available or has no
//    hash; no E for a run that loaded none; a source that records nothing
//    keeps the kernel's own (Legacy). The reprocess reply names the LUT
//    (review ABI 4).
//  - The px→µm source (item 4): saveMasksToHdf5 records where its factor came
//    from; review takes a fallback factor as not the file's; old regenerated
//    files read as before.
//  - The metrics CSV (item 5): E is written as missing (an empty cell) for a
//    run whose snapshot records that no E-LUT was loaded; otherwise as stored.
// Fixture: one ellipse per frame, one track (deformability inside the bundled
// LUT's range), the default 0.4886 µm/px, and two constant test LUTs (E 5 and 9 kPa).
// Watchdog guarded.

#include "backend/processing/BatchMaskSources.h"
#include "backend/processing/EModulusLut.h"
#include "backend/processing/ProcessingConfigJson.h"
#include "backend/processing/ProcessingCoreSha256.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/HdfExportService.h"
#include "backend/review/Reanalysis.h"
#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"
#include "backend/review/SourceEModulusLut.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <hdf5.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using backend::EModulusLut;
using backend::review::EModulusLutBasis;
using backend::review::ReanalysisRequest;
using backend::review::RegenerateMasksRequest;
using backend::review::RegenerateSource;
using backend::review::ReprocessResult;
using backend::review::ReprocessSettings;
using backend::review::ReviewJobEvent;
using backend::review::ReviewJobs;
using backend::review::ReviewJobState;
using backend::review::ReviewSession;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using backend::services::ProcessingConfig;
using backend::services::ProcessingService;
namespace batch_masks = backend::services::batch_masks;

namespace
{
    constexpr int kRows = 96, kCols = 160, kFrames = 3;
    constexpr double kEA = 5.0, kEB = 9.0;

    cv::Mat ellipseFrame(int frame)
    {
        cv::Mat m(kRows, kCols, CV_8UC1, cv::Scalar(0));
        cv::ellipse(m, {24 + 48 * frame, 48}, {15, 11}, 0.0, 0.0, 360.0, cv::Scalar(200), -1, cv::LINE_8);
        return m;
    }
    cv::Mat black() { return cv::Mat(kRows, kCols, CV_8UC1, cv::Scalar(0)); }

    ProcessingConfig config()
    {
        ProcessingConfig c;
        c.processing_contract_version = 1;
        c.require_single_inner_contour = false; // a filled ellipse: outer-contour objects
        c.enable_ring_ratio_check = false;
        c.enable_border_check = true;
        c.enable_area_range_check = true;
        c.area_threshold_min = 1;
        c.area_threshold_max = 100000;
        c.enable_target_group = false;
        return c;
    }

    std::string sha256Of(const std::string &bytes)
    {
        return backend::processing::processingCoreBytesSha256(reinterpret_cast<const std::uint8_t *>(bytes.data()),
                                                              bytes.size());
    }

    // A constant LUT: four corners with E = `e` (area µm² × deformability).
    std::string constantLut(double e)
    {
        std::ostringstream text;
        text << "# test LUT\n";
        for (double area : {0.0, 5000.0})
            for (double deform : {-1.0, 1.0}) text << area << '\t' << deform << '\t' << e << '\n';
        return text.str();
    }

    void writeText(const fs::path &path, const std::string &text)
    {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    std::string readText(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    std::string lutJson(bool loaded, const std::string &id, const std::string &revision, const std::string &sha)
    {
        nlohmann::json lut{{"loaded", loaded}};
        if (!id.empty()) lut["lut_id"] = id;
        if (!revision.empty()) lut["revision"] = revision;
        if (!sha.empty()) lut["sha256"] = sha;
        return nlohmann::json{{"schema_version", 3}, {"pixel_to_micron", 0.4886}, {"emodulus_lut", lut}}.dump();
    }

    // An experiment file as a capture writes it: the ellipse images as valid
    // rows (stored E `storedE`), the config, a full-frame ROI, a black
    // background and the run snapshot `snapshot` ("" writes none).
    void writeSource(const fs::path &path, const std::string &snapshot, double storedE = 0.0)
    {
        Hdf5Service writer;
        MIB_REQUIRE(writer.openFile(path.string()) && writer.initializeDatasets(), "fixture open " + path.string());
        std::vector<ProcessedFrame> valid;
        for (int i = 0; i < kFrames; ++i)
        {
            ProcessedFrame f;
            f.index = 10 + static_cast<std::uint64_t>(i);
            f.timestampNs = 1000 + 10 * static_cast<std::uint64_t>(i);
            f.originalImage = ellipseFrame(i);
            f.processedImage = cv::Mat::zeros(kRows, kCols, CV_8UC1);
            f.validation.isValid = true;
            f.validation.objectCount = 1;
            f.validation.area = 500.0;
            f.validation.deformability = 0.02;
            f.validation.youngsModulus = storedE;
            f.validation.bboxX = 9 + 48 * i;
            f.validation.bboxY = 37;
            f.validation.bboxWidth = 31;
            f.validation.bboxHeight = 23;
            f.validation.centroidX = 24 + 48 * i;
            f.validation.centroidY = 48;
            valid.push_back(f);
        }
        MIB_REQUIRE(writer.appendFrames(valid, {}), "fixture frames");
        const cv::Mat background = black();
        MIB_REQUIRE(writer.writeExperimentInfo(1000, 2000, valid.size(), 0, config(), {0, 0, kCols, kRows}, &background),
                    "fixture info");
        nlohmann::json doc{{"image_processing", backend::processing::config_json::toJson(config())}};
        MIB_REQUIRE(writer.writeConfigJson(doc.dump()), "fixture config document");
        if (!snapshot.empty()) MIB_REQUIRE(writer.writeRunSnapshotJson(snapshot, "{}"), "fixture snapshot");
        writer.closeFile();
    }

    struct Output
    {
        std::vector<ProcessedFrame> rows;
        std::vector<double> validE; // valid objects' stored E
    };
    Output readOutput(const fs::path &path)
    {
        Output out;
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path.string()), "output opens " + path.string());
        std::vector<ProcessedFrame> valid, invalid;
        reader.readValidMetadata(valid);
        reader.readInvalidMetadata(invalid);
        out.rows = valid;
        out.rows.insert(out.rows.end(), invalid.begin(), invalid.end());
        for (const auto &r : valid)
            if (r.validation.objectCount > 0) out.validE.push_back(r.validation.youngsModulus);
        return out;
    }
    bool allE(const Output &o, double e)
    {
        // The frames' ellipses are one track: processBatch keeps its first record.
        bool ok = !o.validE.empty();
        for (double v : o.validE)
            if (!(std::abs(v - e) < 1e-9)) ok = false;
        if (!ok)
        {
            std::fprintf(stderr, "allE(%g): %zu valid objects:", e, o.validE.size());
            for (double v : o.validE) std::fprintf(stderr, " %g", v);
            std::fprintf(stderr, "\n");
        }
        return ok;
    }

    std::shared_ptr<std::atomic<bool>> flag() { return std::make_shared<std::atomic<bool>>(false); }

    ReanalysisRequest hdfRequest(const fs::path &source, const fs::path &output)
    {
        ReanalysisRequest r;
        r.sourceKind = "hdf";
        r.sourcePath = source.string();
        r.outputPath = output.string();
        r.dataset = "all";
        r.config = config();
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

    bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

    // The E column (the last) of every data line of a metrics CSV.
    std::vector<std::string> eColumn(const fs::path &csv)
    {
        std::vector<std::string> out;
        std::istringstream lines(readText(csv));
        std::string line;
        std::getline(lines, line); // header
        while (std::getline(lines, line))
        {
            if (line.empty()) continue;
            out.push_back(line.substr(line.rfind(',') + 1));
        }
        return out;
    }

    // Replace or delete a string attribute of /experiment_info behind the
    // writer's back (an old file, an unknown value).
    void setExperimentInfoString(const fs::path &file, const char *name, const char *value)
    {
        hid_t f = H5Fopen(file.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        MIB_REQUIRE(f >= 0, "raw open " + file.string());
        hid_t g = H5Gopen2(f, "/experiment_info", H5P_DEFAULT);
        if (H5Aexists(g, name) > 0) H5Adelete(g, name);
        if (value)
        {
            hid_t type = H5Tcopy(H5T_C_S1);
            H5Tset_size(type, H5T_VARIABLE);
            hid_t space = H5Screate(H5S_SCALAR);
            hid_t a = H5Acreate2(g, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
            H5Awrite(a, type, &value);
            H5Aclose(a);
            H5Sclose(space);
            H5Tclose(type);
        }
        H5Gclose(g);
        H5Fclose(f);
    }
} // namespace

int main()
{
    mib::test::Watchdog wd(240);
    mib::test::TempDir td("source_elut");

    const std::string lutA = constantLut(kEA), lutB = constantLut(kEB);
    const fs::path lutAPath = td / "lut_a.txt", lutBPath = td / "lut_b.txt";
    writeText(lutAPath, lutA);
    writeText(lutBPath, lutB);
    const std::string shaA = sha256Of(lutA);
    const std::string bundledSha = backend::review::bundledEModulusLutSha256();
    MIB_REQUIRE(bundledSha == sha256Of(readText(fs::path(__FILE__).parent_path().parent_path().parent_path() / "resources" / "isoelastic_curve" /
                                                "scaled_isoelastic_data_LUT_6.16-4.24.txt")),
                "the compiled-in bundled LUT is the resource's bytes");
    EModulusLut bundled;
    MIB_REQUIRE(bundled.loadFromText(backend::review::bundledEModulusLutText(), "test"), "bundled LUT loads");

    const fs::path srcA = td / "src_lut_a.h5";            // records LUT A
    const fs::path srcBundled = td / "src_lut_bundled.h5"; // records the bundled LUT
    const fs::path srcNone = td / "src_lut_none.h5";       // records that no LUT was loaded
    const fs::path srcNoHash = td / "src_lut_nohash.h5";   // records a LUT without a hash
    const fs::path srcLegacy = td / "src_legacy.h5";       // no run snapshot
    const fs::path srcV2 = td / "src_v2.h5";               // a snapshot without emodulus_lut
    writeSource(srcA, lutJson(true, "lut-a", "r1", shaA));
    writeSource(srcBundled, lutJson(true, "scaled_isoelastic_data_LUT_6.16-4.24", "bundled", bundledSha));
    writeSource(srcNone, lutJson(false, "", "", ""));
    writeSource(srcNoHash, lutJson(true, "lut-x", "r9", ""));
    writeSource(srcLegacy, "");
    writeSource(srcV2, R"({"schema_version":2,"pixel_to_micron":0.4886})");
    const std::string nameA = "lut-a r1 (sha256 " + shaA.substr(0, 12) + ")";

    // ---- processBatch: a per-call table replaces the service's own -----------------
    {
        ProcessingService kernel;
        MIB_REQUIRE(kernel.loadEModulusLut(lutBPath.string()), "kernel loads LUT B");
        EModulusLut a;
        MIB_REQUIRE(a.loadFromText(lutA, "A"), "LUT A");
        ProcessingService::BatchAnalysisInputs inputs;
        inputs.eModulusLut = &a;
        const auto withA = kernel.processBatch({ellipseFrame(0)}, config(), black(), {}, {}, nullptr, inputs);
        inputs.eModulusLut = nullptr;
        const auto withNone = kernel.processBatch({ellipseFrame(0)}, config(), black(), {}, {}, nullptr, inputs);
        const auto own = kernel.processBatch({ellipseFrame(0)}, config(), black());
        MIB_REQUIRE(withA.size() == 1 && withNone.size() == 1 && own.size() == 1, "one object per call");
        MIB_EXPECT(withA[0].validation.isValid && std::abs(withA[0].validation.youngsModulus - kEA) < 1e-9,
                   "the call's table: E 5");
        MIB_EXPECT(withNone[0].validation.youngsModulus == 0.0, "a null table: no E (0, as a run without a LUT)");
        MIB_EXPECT(std::abs(own[0].validation.youngsModulus - kEB) < 1e-9, "unset: the service's own (E 9)");
    }
    wd.mark("processBatch");

    // ---- the reanalysis engine ------------------------------------------------------
    {
        // The source's LUT is the kernel's loaded one: used, named, no note.
        ProcessingService kernel;
        MIB_REQUIRE(kernel.loadEModulusLut(lutAPath.string()), "kernel loads LUT A");
        const fs::path out = td / "engine_a.h5";
        const auto outcome = backend::review::runReanalysis(hdfRequest(srcA, out), kernel, flag(), {});
        MIB_REQUIRE(outcome.published, "source LUT A: " + outcome.error);
        MIB_EXPECT(allE(readOutput(out), kEA), "E with the source's LUT A");
        MIB_EXPECT(outcome.eModulusLutBasis == EModulusLutBasis::Source && outcome.eModulusLut == nameA,
                   "named: " + outcome.eModulusLut);
        MIB_EXPECT(backend::review::reanalysisEModulusLutPhrase(outcome) == "E-LUT " + nameA + " (the source run's)",
                   "phrase: " + backend::review::reanalysisEModulusLutPhrase(outcome));
        MIB_EXPECT(has(backend::review::reanalysisTerminalStatusJson(1, hdfRequest(srcA, out), outcome), "\"warnings\":[]"),
                   "the source's own LUT needs no note");
    }
    {
        // The kernel has another LUT: refused, nothing written, never the live one.
        ProcessingService kernel;
        MIB_REQUIRE(kernel.loadEModulusLut(lutBPath.string()), "kernel loads LUT B");
        const fs::path out = td / "engine_refused.h5";
        const auto outcome = backend::review::runReanalysis(hdfRequest(srcA, out), kernel, flag(), {});
        MIB_EXPECT(!outcome.published && !fs::exists(out), "refused, nothing written");
        MIB_EXPECT(has(outcome.error, "E cannot be recomputed with the source run's E-LUT " + nameA) &&
                       has(outcome.error, "no E-LUT with that hash is available here") &&
                       has(outcome.error, "loaded: " + sha256Of(lutB).substr(0, 12)) &&
                       has(outcome.error, "bundled: " + bundledSha.substr(0, 12)),
                   "the reason names the LUT and what is here: " + outcome.error);

        // The bundled LUT, found compiled in although the kernel loaded LUT B.
        const fs::path outBundled = td / "engine_bundled.h5";
        const auto b = backend::review::runReanalysis(hdfRequest(srcBundled, outBundled), kernel, flag(), {});
        MIB_REQUIRE(b.published, "source bundled LUT: " + b.error);
        ProcessingService oracle;
        ProcessingService::BatchAnalysisInputs inputs;
        inputs.eModulusLut = &bundled;
        std::vector<cv::Mat> frames;
        for (int i = 0; i < kFrames; ++i) frames.push_back(ellipseFrame(i));
        const auto expected = oracle.processBatch(frames, config(), black(), {}, {}, nullptr, inputs);
        const auto o = readOutput(outBundled);
        bool same = !o.validE.empty(), finite = false;
        std::size_t expectedValid = 0;
        for (const auto &f : expected) expectedValid += f.validation.isValid ? 1 : 0;
        same = same && expectedValid == o.validE.size();
        std::size_t k = 0;
        for (const auto &f : expected)
        {
            if (!f.validation.isValid) continue;
            const double want = f.validation.youngsModulus;
            if (k >= o.validE.size()) break;
            const double got = o.validE[k++];
            finite = finite || std::isfinite(want);
            if (!(std::abs(got - want) < 1e-9)) same = false;
            if (std::abs(got - kEB) < 1e-9) same = false;
        }
        MIB_EXPECT(same && finite, "E with the bundled LUT (finite, not the live LUT B's 9)");
        MIB_EXPECT(b.eModulusLutBasis == EModulusLutBasis::Source &&
                       has(b.eModulusLut, "scaled_isoelastic_data_LUT_6.16-4.24 bundled (sha256 " + bundledSha.substr(0, 12)),
                   "named: " + b.eModulusLut);

        // A LUT recorded without a hash cannot be identified: refused.
        const auto noHash = backend::review::runReanalysis(hdfRequest(srcNoHash, td / "engine_nohash.h5"), kernel, flag(), {});
        MIB_EXPECT(!noHash.published && has(noHash.error, "lut-x r9") && has(noHash.error, "records no hash"),
                   "no hash: refused: " + noHash.error);

        // A run that loaded none: no E, said so.
        const fs::path outNone = td / "engine_none.h5";
        const auto n = backend::review::runReanalysis(hdfRequest(srcNone, outNone), kernel, flag(), {});
        MIB_REQUIRE(n.published, "source without a LUT: " + n.error);
        MIB_EXPECT(allE(readOutput(outNone), 0.0), "no E, as the source (E 0, not the live 9)");
        MIB_EXPECT(n.eModulusLutBasis == EModulusLutBasis::NotLoaded &&
                       has(backend::review::reanalysisTerminalStatusJson(2, hdfRequest(srcNone, outNone), n),
                           "\"warnings\":[\"E not computed: the source run loaded no E-LUT\"]"),
                   "the reply says E was not computed");

        // Legacy: no snapshot, or one without emodulus_lut: the kernel's own, as before, said so.
        for (const auto &legacy : {srcLegacy, srcV2})
        {
            const fs::path outLegacy = td / ("engine_legacy_" + legacy.stem().string() + ".h5");
            const auto l = backend::review::runReanalysis(hdfRequest(legacy, outLegacy), kernel, flag(), {});
            MIB_REQUIRE(l.published, "legacy source: " + l.error);
            MIB_EXPECT(allE(readOutput(outLegacy), kEB), "legacy: the kernel's LUT B, as before");
            MIB_EXPECT(l.eModulusLutBasis == EModulusLutBasis::Legacy &&
                           has(l.eModulusLutNote, "the loaded one: the source records no E-LUT") &&
                           has(backend::review::reanalysisTerminalStatusJson(3, hdfRequest(legacy, outLegacy), l),
                               l.eModulusLutNote),
                       "legacy said in the reply: " + l.eModulusLutNote);
        }
        // A folder records nothing: the kernel's own, no note.
        const fs::path folder = td / "folder";
        fs::create_directories(folder);
        for (int i = 0; i < kFrames; ++i)
            MIB_REQUIRE(cv::imwrite((folder / ("f" + std::to_string(i) + ".png")).string(), ellipseFrame(i)), "png");
        ReanalysisRequest f;
        f.sourceKind = "folder";
        f.sourcePath = folder.string();
        f.outputPath = (td / "engine_folder.h5").string();
        f.config = config();
        const auto fo = backend::review::runReanalysis(f, kernel, flag(), {});
        MIB_REQUIRE(fo.published, "folder: " + fo.error);
        MIB_EXPECT(allE(readOutput(f.outputPath), kEB) && fo.eModulusLutNote.empty(), "folder: the kernel's own, no note");
    }
    {
        // Legacy with a kernel that loaded none (YOFO Review): no E, as before.
        ProcessingService kernel;
        const fs::path out = td / "engine_legacy_nolut.h5";
        const auto l = backend::review::runReanalysis(hdfRequest(srcLegacy, out), kernel, flag(), {});
        MIB_REQUIRE(l.published, "legacy, no LUT: " + l.error);
        MIB_EXPECT(allE(readOutput(out), 0.0) &&
                       l.eModulusLutNote == "E not computed: the source records no E-LUT and none is loaded",
                   "legacy, none loaded: " + l.eModulusLutNote);
    }
    wd.mark("engine");

    // ---- YOFO Review's regenerate job: the message names the LUT -------------------
    {
        ProcessingService processing;
        MIB_REQUIRE(processing.loadEModulusLut(lutAPath.string()), "kernel loads LUT A");
        ReviewSession session;
        Sink sink;
        ReviewJobs jobs(session, &processing, std::ref(sink));
        const auto regenerate = [&](const fs::path &source, const fs::path &out, ReviewJobEvent &done) {
            MIB_REQUIRE(session.open(source.string()), "open " + source.string());
            RegenerateMasksRequest req;
            req.source = RegenerateSource::WholeFile;
            req.outputPath = out.string();
            req.useRecordedConfig = true;
            req.roi = {0, 0, kCols, kRows};
            std::string err;
            const auto id = jobs.startRegenerateMasks(req, &err);
            MIB_REQUIRE(id != 0, "start regenerate: " + err);
            MIB_REQUIRE(sink.terminal(id, done), "regenerate terminal");
        };
        ReviewJobEvent done;
        regenerate(srcA, td / "yofo_a.h5", done);
        MIB_EXPECT(done.state == ReviewJobState::Completed && has(done.message, "; E-LUT " + nameA + " (the source run's))"),
                   "message names the source's LUT: " + done.message);
        MIB_EXPECT(allE(readOutput(td / "yofo_a.h5"), kEA), "regenerated with LUT A");
        regenerate(srcNone, td / "yofo_none.h5", done);
        MIB_EXPECT(done.state == ReviewJobState::Completed &&
                       has(done.message, "; E not computed: the source run loaded no E-LUT)"),
                   "message: no E: " + done.message);
    }
    {
        ProcessingService processing; // YOFO Review's kernel: no LUT loaded
        ReviewSession session;
        Sink sink;
        ReviewJobs jobs(session, &processing, std::ref(sink));
        MIB_REQUIRE(session.open(srcA.string()), "open A");
        RegenerateMasksRequest req;
        req.source = RegenerateSource::WholeFile;
        req.outputPath = (td / "yofo_refused.h5").string();
        req.roi = {0, 0, kCols, kRows};
        std::string err;
        const auto id = jobs.startRegenerateMasks(req, &err);
        MIB_REQUIRE(id != 0, "start: " + err);
        ReviewJobEvent done;
        MIB_REQUIRE(sink.terminal(id, done), "terminal");
        MIB_EXPECT(done.state == ReviewJobState::Failed && has(done.message, "no E-LUT with that hash is available here") &&
                       !fs::exists(req.outputPath),
                   "LUT not here: the job fails with the reason: " + done.message);
    }
    wd.mark("regenerate");

    // ---- reprocess one record: the reply names the LUT (review ABI 4) ---------------
    {
        ProcessingService kernel;
        MIB_REQUIRE(kernel.loadEModulusLut(lutAPath.string()), "session kernel loads LUT A");
        ReviewSession s;
        ReviewJobs jobs(s, &kernel, [](const ReviewJobEvent &) {});
        ReprocessResult r;
        std::string err;

        MIB_REQUIRE(s.open(srcA.string()), "open A");
        MIB_REQUIRE(jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err), "reprocess A: " + err);
        MIB_EXPECT(r.emodulusRecomputed && r.emodulusLut == nameA && r.emodulusLutSha256 == shaA,
                   "the reply names LUT A: " + r.emodulusLut);
        MIB_EXPECT(has(r.settingsLabel, "; E-LUT " + nameA + " (the run's)"), "label: " + r.settingsLabel);
        MIB_REQUIRE(r.reprocessed.size() == 1, "one object");
        MIB_EXPECT(std::abs(r.reprocessed[0].youngsModulus - kEA) < 1e-9, "E with LUT A");
        // Default settings: still the run's LUT.
        MIB_REQUIRE(jobs.reprocessRecord(true, 0, ReprocessSettings::Default, r, &err), "default A: " + err);
        MIB_EXPECT(r.emodulusRecomputed && r.emodulusLut == nameA, "default settings: the run's LUT too");

        MIB_REQUIRE(s.open(srcBundled.string()), "open bundled");
        MIB_REQUIRE(jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err), "reprocess bundled: " + err);
        MIB_EXPECT(r.emodulusRecomputed && r.emodulusLutSha256 == bundledSha &&
                       has(r.emodulusLut, "scaled_isoelastic_data_LUT_6.16-4.24 bundled"),
                   "the bundled LUT, named: " + r.emodulusLut);
        MIB_EXPECT(!r.reprocessed.empty() && !(std::abs(r.reprocessed[0].youngsModulus - kEA) < 1e-9),
                   "not the session kernel's LUT A");

        MIB_REQUIRE(s.open(srcNone.string()), "open none");
        MIB_REQUIRE(jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err), "reprocess none: " + err);
        MIB_EXPECT(!r.emodulusRecomputed && r.emodulusLut.empty() && r.emodulusLutSha256.empty() &&
                       has(r.settingsLabel, "; E not recomputed: the run loaded no E-LUT") && !r.reprocessed.empty() &&
                       std::isnan(r.reprocessed[0].youngsModulus),
                   "no LUT: not recomputed, unnamed: " + r.settingsLabel);

        MIB_REQUIRE(s.open(srcLegacy.string()), "open legacy");
        MIB_REQUIRE(jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err), "reprocess legacy: " + err);
        MIB_EXPECT(!r.emodulusRecomputed && r.emodulusLut.empty() &&
                       has(r.settingsLabel, "; E not recomputed: the file records no E-LUT"),
                   "legacy: as before ABI 4: " + r.settingsLabel);

        MIB_REQUIRE(s.open(srcNoHash.string()), "open no hash");
        MIB_EXPECT(!jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err) &&
                       has(err, "Record 0 cannot be reprocessed: E cannot be recomputed") && has(err, "records no hash"),
                   "no hash: refused: " + err);
    }
    {
        ProcessingService kernel;
        MIB_REQUIRE(kernel.loadEModulusLut(lutBPath.string()), "session kernel loads LUT B");
        ReviewSession s;
        ReviewJobs jobs(s, &kernel, [](const ReviewJobEvent &) {});
        ReprocessResult r;
        std::string err;
        MIB_REQUIRE(s.open(srcA.string()), "open A");
        MIB_EXPECT(!jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err) &&
                       has(err, "Record 0 cannot be reprocessed") && has(err, "no E-LUT with that hash") &&
                       r.emodulusLut.empty(),
                   "LUT A not here: refused, never LUT B: " + err);
    }
    wd.mark("reprocess");

    // ---- the px→µm source (item 4) --------------------------------------------------
    {
        std::vector<ProcessedFrame> frames;
        {
            ProcessingService kernel;
            frames = kernel.processBatch({ellipseFrame(0)}, config(), black());
        }
        const auto save = [&](const fs::path &path, batch_masks::PixelToMicronSource source) {
            MIB_REQUIRE(batch_masks::saveMasksToHdf5(frames, path.string(), config(), 0, 0, kCols, kRows, black(), false,
                                                     nullptr, 0.3, source),
                        "save " + path.string());
        };
        const auto read = [](const fs::path &path, std::optional<std::string> &source, double &recorded) {
            Hdf5Service reader;
            MIB_REQUIRE(reader.loadFile(path.string()), "open " + path.string());
            source = reader.readAnalysisPixelToMicronSource();
            recorded = ReviewSession::recordedPixelToMicron(reader);
            MIB_EXPECT(reader.readAnalysisPixelToMicron() == std::optional<double>(0.3), "the factor is written");
        };
        std::optional<std::string> source;
        double recorded = 0.0;

        const fs::path fallback = td / "f_fallback.h5";
        save(fallback, batch_masks::PixelToMicronSource::Fallback);
        read(fallback, source, recorded);
        MIB_EXPECT(source == std::optional<std::string>("fallback") && recorded == 0.0,
                   "a fallback factor is not the file's recorded one");
        {
            ReviewSession s;
            s.setFallbackPixelToMicron(0.4);
            MIB_REQUIRE(s.open(fallback.string()), "open fallback output");
            MIB_EXPECT(!s.metadata().pixelToMicronFromFile && std::abs(s.pixelToMicron() - 0.4) < 1e-12,
                       "review shows it as a fallback (pixel_to_micron_from_file false)");
            Hdf5Service reader;
            MIB_REQUIRE(reader.loadFile(fallback.string()), "reopen");
            const auto document = reader.readScalarStringAttribute("/experiment_info", "config_json");
            MIB_EXPECT(document && nlohmann::json::parse(*document).value("pixel_to_micron_source", "") == "fallback",
                       "the config document says so too");
        }
        const fs::path rec = td / "f_recorded.h5";
        save(rec, batch_masks::PixelToMicronSource::Recorded);
        read(rec, source, recorded);
        MIB_EXPECT(source == std::optional<std::string>("recorded") && recorded == 0.3, "recorded: the file's");
        const fs::path op = td / "f_operator.h5";
        save(op, batch_masks::PixelToMicronSource::Operator);
        read(op, source, recorded);
        MIB_EXPECT(source == std::optional<std::string>("operator") && recorded == 0.3, "operator: the file's");
        {
            ReviewSession s;
            MIB_REQUIRE(s.open(op.string()), "open operator output");
            MIB_EXPECT(s.metadata().pixelToMicronFromFile && std::abs(s.pixelToMicron() - 0.3) < 1e-12,
                       "an operator factor reads as the file's");
        }
        // A file regenerated before #764 (no source attribute) reads as before.
        const fs::path old = td / "f_old.h5";
        save(old, batch_masks::PixelToMicronSource::Recorded);
        setExperimentInfoString(old, Hdf5Service::kPixelToMicronSourceAttribute, nullptr);
        read(old, source, recorded);
        MIB_EXPECT(!source && recorded == 0.3, "an old file without the source: its factor, as before");
        // An unknown source is not taken as the file's.
        setExperimentInfoString(old, Hdf5Service::kPixelToMicronSourceAttribute, "guess");
        read(old, source, recorded);
        MIB_EXPECT(source == std::optional<std::string>("guess") && recorded == 0.0, "an unknown source: not recorded");
        {
            Hdf5Service writer;
            MIB_REQUIRE(writer.openFile((td / "f_bad.h5").string()), "open bad");
            MIB_REQUIRE(writer.writeExperimentInfo(0, 0, 0, 0, config(), {0, 0, 0, 0}, nullptr), "info");
            MIB_EXPECT(!writer.writeAnalysisPixelToMicron(0.3, "guess"), "the writer refuses an unknown source");
            writer.closeFile();
        }

        // The engine: the source's own factor is "recorded", a fallback "fallback", an operator's "operator".
        ProcessingService kernel;
        MIB_REQUIRE(kernel.loadEModulusLut(lutAPath.string()), "kernel LUT A");
        const auto regenerate = [&](const fs::path &src, const fs::path &out, bool explicitFactor) {
            auto r = hdfRequest(src, out);
            r.pixelToMicron = 0.3;
            r.pixelToMicronExplicit = explicitFactor;
            const auto outcome = backend::review::runReanalysis(r, kernel, flag(), {});
            MIB_REQUIRE(outcome.published, "regenerate " + src.string() + ": " + outcome.error);
            Hdf5Service reader;
            MIB_REQUIRE(reader.loadFile(out.string()), "open output");
            return reader.readAnalysisPixelToMicronSource().value_or("");
        };
        MIB_EXPECT(regenerate(srcLegacy, td / "e_fallback.h5", false) == "fallback",
                   "a source without a factor: the fallback is marked fallback");
        {
            ReviewSession s;
            MIB_REQUIRE(s.open((td / "e_fallback.h5").string()), "reopen");
            MIB_EXPECT(!s.metadata().pixelToMicronFromFile, "and reopens as a fallback, not recorded");
        }
        MIB_EXPECT(regenerate(srcLegacy, td / "e_operator.h5", true) == "operator", "an explicit factor: operator");
        MIB_EXPECT(regenerate(srcA, td / "e_recorded.h5", true) == "recorded",
                   "the source's recorded factor wins and is marked recorded");
    }
    wd.mark("px→µm source");

    // ---- the metrics CSV (item 5) ---------------------------------------------------
    {
        const fs::path csvNone = td / "csv_none.h5", csvLoaded = td / "csv_loaded.h5", csvLegacy = td / "csv_legacy.h5";
        writeSource(csvNone, lutJson(false, "", "", ""), 0.0);
        writeSource(csvLoaded, lutJson(true, "lut-a", "r1", shaA), 0.0);
        writeSource(csvLegacy, "", 0.0);
        backend::recording::HdfExportService exporter;
        const auto run = [&](const fs::path &src, backend::recording::HdfExportFormat format) {
            backend::recording::HdfExportRequest req;
            req.sourcePath = src.string();
            req.outputRoot = (td / ("csv_out_" + src.stem().string() + (format == backend::recording::HdfExportFormat::All ? "_all" : ""))).string();
            req.format = format;
            const auto result = exporter.run(req, backend::recording::HdfExportCancelToken{});
            MIB_REQUIRE(result.completed(), "export " + src.string() + ": " + result.error);
            return format == backend::recording::HdfExportFormat::All ? fs::path(result.finalPath) / "metrics.csv"
                                                                      : fs::path(result.finalPath);
        };
        const auto eNone = eColumn(run(csvNone, backend::recording::HdfExportFormat::MetricsCsv));
        MIB_EXPECT(eNone.size() == kFrames && std::all_of(eNone.begin(), eNone.end(), [](const auto &e) { return e.empty(); }),
                   "no E-LUT loaded: E is missing (empty), not 0");
        const auto eAll = eColumn(run(csvNone, backend::recording::HdfExportFormat::All));
        MIB_EXPECT(eAll.size() == kFrames && std::all_of(eAll.begin(), eAll.end(), [](const auto &e) { return e.empty(); }),
                   "Export All: the same");
        const auto eLoaded = eColumn(run(csvLoaded, backend::recording::HdfExportFormat::MetricsCsv));
        MIB_EXPECT(eLoaded.size() == kFrames &&
                       std::all_of(eLoaded.begin(), eLoaded.end(), [](const auto &e) { return e == "0.000"; }),
                   "a LUT was loaded: the stored value");
        const auto eLegacy = eColumn(run(csvLegacy, backend::recording::HdfExportFormat::MetricsCsv));
        MIB_EXPECT(eLegacy.size() == kFrames &&
                       std::all_of(eLegacy.begin(), eLegacy.end(), [](const auto &e) { return e == "0.000"; }),
                   "a legacy file: as stored");
    }
    wd.mark("csv");
    return mib::test::exitCode();
}
