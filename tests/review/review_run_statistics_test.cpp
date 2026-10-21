// review_run_statistics_test (#716 A7a; bridge ABI 44 run_quick_look, review ABI 6 review_run_statistics)
//
// The run quick look's one shared function (review/RunStatistics.h) on the parity fixture
// (review/RunStatisticsFixture.h, ruling 9).
//  Pure:
//  - median: odd, even, one value, unsorted input; the calibration record's median is this one;
//  - the numbers of the V3WithE fixture by hand: counts (set, valid, finite, excluded), area µm² at
//    the recorded factor, deformability, E over finite values > 0 with out_of_domain, the declared
//    target group, the event rate on the declared clock, the accounting, the flags;
//  - legacy (nothing recorded reads null, never 0), LUT not loaded (no_lut), even n, an empty
//    valid set, a raw recording (not available), a missing or corrupt file (an error);
//  - the deltas run - reference and the withheld / flagged rules; the reference's run_id is
//    confirmed (a mismatch gives no deltas);
//  - the strict request parser.
//  Parity (the claim): for every fixture the column path (Hdf5Service::readMetadataColumns, any
//  chunk size) gives byte-equal statistics to ReviewSession's row path (its metadata rows).
//  No writes: a read leaves the file's bytes and mtime as they were.
//  Concurrency (#722; TSan lane): the LRU of 64 under threads (insert, find, stale stamps,
//  eviction); ReviewJobs::runStatistics is deferred while a review job holds a lane and computed
//  after it; ReviewSession::runStatistics leaves the open file as it was.
// Watchdog guarded.

#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"
#include "backend/review/RunStatistics.h"
#include "backend/review/RunStatisticsFixture.h"
#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace backend::review;
using json = nlohmann::json;

namespace
{
    bool near(double a, double b, double tol = 1e-12) { return std::fabs(a - b) <= tol; }
    bool near(const json &v, double b, double tol = 1e-12) { return v.is_number() && near(v.get<double>(), b, tol); }

    std::string bytesOf(const fs::path &p)
    {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    json statsOf(const RunStatisticsResult &r)
    {
        MIB_REQUIRE(r.status == RunStatisticsStatus::Computed && r.stats, r.path + ": " + r.message);
        return json::parse(runStatisticsJson(*r.stats));
    }

    // The row path: ReviewSession's metadata rows, the header from the same file.
    std::string rowPathJson(const fs::path &p)
    {
        ReviewSession s;
        std::string err;
        MIB_REQUIRE(s.open(p.string(), &err), "open " + p.string() + ": " + err);
        const auto set = s.validSet();
        MIB_REQUIRE(set.open && set.metadataReadable, "the valid set reads");
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(p.string()), "header reader");
        const auto header = readRunStatisticsHeader(reader);
        reader.closeFile();
        return runStatisticsJson(computeRunStatistics(runStatisticsRowsFromFrames(set.frames), header));
    }

    // The column path at a chunk size.
    std::string columnPathJson(const fs::path &p, std::size_t chunk)
    {
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(p.string()), "column reader");
        const auto header = readRunStatisticsHeader(reader);
        RunStatisticsRows rows;
        std::string why;
        MIB_REQUIRE(readRunStatisticsRows(reader, rows, &why, chunk), "columns: " + why);
        reader.closeFile();
        return runStatisticsJson(computeRunStatistics(std::move(rows), header));
    }

    bool hasFlag(const json &stats, const char *flag)
    {
        for (const auto &f : stats["flags"])
            if (f == flag) return true;
        return false;
    }
} // namespace

int main()
{
    mib::test::Watchdog wd(180);
    mib::test::TempDir td("review_run_statistics");
    const auto file = [&](const char *name) { return td.path() / name; };
    const fs::path v3 = file("v3.h5"), legacy = file("legacy.h5"), noLut = file("nolut.h5"), even = file("even.h5"),
                   empty = file("empty.h5"), raw = file("raw.h5"), corrupt = file("corrupt.h5");
    struct Fx
    {
        fs::path path;
        RunStatisticsFixture variant;
    };
    const std::vector<Fx> fixtures{{v3, RunStatisticsFixture::V3WithE},
                                   {legacy, RunStatisticsFixture::Legacy},
                                   {noLut, RunStatisticsFixture::LutNotLoaded},
                                   {even, RunStatisticsFixture::V3WithEEven},
                                   {empty, RunStatisticsFixture::EmptyValidSet},
                                   {raw, RunStatisticsFixture::RawRecording}};
    for (const auto &f : fixtures)
    {
        std::string why;
        MIB_REQUIRE(writeRunStatisticsFixture(f.path.string(), f.variant, &why), "fixture: " + why);
    }
    {
        std::ofstream out(corrupt, std::ios::binary);
        out << "not an HDF5 file";
    }

    // ---- median ------------------------------------------------------------------------------
    wd.mark("median");
    MIB_EXPECT(median({3.0, 1.0, 2.0}) == 2.0, "odd");
    MIB_EXPECT(median({4.0, 1.0, 3.0, 2.0}) == 2.5, "even: the mean of the two middle values");
    MIB_EXPECT(median({7.0}) == 7.0, "one value");
    MIB_EXPECT(median({5.0, 5.0, 1.0, 9.0, 9.0, 9.0}) == 7.0, "duplicates");

    // ---- V3WithE by hand -----------------------------------------------------------------------
    wd.mark("v3 numbers");
    {
        const auto r = readRunStatistics(v3.string(), "on_demand");
        const auto s = statsOf(r);
        MIB_EXPECT(r.computedAt == "on_demand", "the origin is carried");
        MIB_EXPECT(s["counts"] == json({{"set_rows", "25"}, {"valid", "21"}, {"finite", "19"}, {"excluded", "2"}}),
                   s["counts"].dump());
        // Area at 0.25 µm/px: 20 finite values, the middle two rows 11 and 13 (244, 252 px²).
        MIB_EXPECT(s["area_um2"]["n"] == "20" && near(s["area_um2"]["median"], 15.5) &&
                       near(s["area_um2"]["pixel_to_micron"], 0.25),
                   s["area_um2"].dump());
        MIB_EXPECT(s["deformability"]["n"] == "20" && near(s["deformability"]["median"], 0.056),
                   s["deformability"].dump());
        const auto &e = s["emodulus"];
        MIB_EXPECT(e["status"] == "computed" && e["n"] == "14" && near(e["median_kpa"], 3.5) &&
                       e["out_of_domain"] == "4",
                   "E over finite > 0 (zeros are neither), NaN out of domain: " + e.dump());
        MIB_EXPECT(e["lut"]["loaded"] == true && e["lut"]["sha256"] == std::string(64, 'a') &&
                       e["lut"]["provenance"] == "snapshot" && e["lut"]["lut_id"] == "fixture_lut",
                   e["lut"].dump());
        const auto &tg = s["target_group"];
        MIB_EXPECT(tg["declared"] == true && tg["n"] == "8" && tg["of"] == "21" &&
                       near(tg["percent"], 100.0 * 8.0 / 21.0) && tg["bounds"]["area_um2_min"] == 10 &&
                       tg["bounds"]["emodulus_gated"] == false,
                   tg.dump());
        const auto &rate = s["event_rate"];
        MIB_EXPECT(rate["status"] == "computed" && near(rate["span_s"], 0.048) && near(rate["per_s"], 21.0 / 0.048, 1e-9) &&
                       rate["ticks_per_second"] == "1000000",
                   rate.dump());
        MIB_EXPECT(s["accounting"]["completion"] == "intentionallyPartial" && s["accounting"]["admitted"] == "30" &&
                       s["accounting"]["cancelled_by_policy"] == "5",
                   s["accounting"].dump());
        MIB_EXPECT(s["flags"] == json({"low_n", "loss", "e_out_of_domain"}), s["flags"].dump());
        const auto &id = s["identity"];
        MIB_EXPECT(id["run_id"] == runStatisticsFixtureRunId(RunStatisticsFixture::V3WithE) &&
                       id["snapshot_version"] == 3 && id["profile_id"] == "fixture" && id["run_index"] == 1 &&
                       id["sample_ref"] == "S1" && id["core_version"] == "fixture-core-1" &&
                       id["contract_version"] == 2 && id["start_generation"] == "7" &&
                       id["config_canonical_sha256"] == std::string(64, 'c') && id["method_revision_id"].is_null() &&
                       id["start_time_ns"] == "5000000000",
                   id.dump());
        const auto &ov = s["overlay"];
        MIB_EXPECT(ov["stride"] == "1" && ov["points"] == "19" && ov["area_px"].size() == 19 &&
                       ov["area_um2"].size() == 19 && ov["target_group"].size() == 19,
                   "every finite row while under the cap: " + ov.dump());
    }

    // ---- other fixtures --------------------------------------------------------------------------
    wd.mark("fixtures");
    {
        const auto s = statsOf(readRunStatistics(legacy.string(), "on_demand"));
        MIB_EXPECT(s["area_um2"]["n"].is_null() && s["area_um2"]["median"].is_null() &&
                       s["area_um2"]["pixel_to_micron"].is_null(),
                   "no recorded factor: area not recorded, never at a fallback: " + s["area_um2"].dump());
        MIB_EXPECT(s["deformability"]["n"] == "20", "deformability needs no factor");
        MIB_EXPECT(s["emodulus"]["status"] == "not_recorded" && s["emodulus"]["n"].is_null() &&
                       s["emodulus"]["lut"].is_null(),
                   s["emodulus"].dump());
        MIB_EXPECT(s["target_group"]["declared"].is_null() && s["target_group"]["n"].is_null(),
                   "no config document: whether a group was declared is not recorded");
        MIB_EXPECT(s["event_rate"]["status"] == "not_recorded" && s["accounting"].is_null(), "no clock, no accounting");
        MIB_EXPECT(s["flags"] == json({"low_n", "legacy", "no_accounting", "factor_not_recorded"}), s["flags"].dump());
        MIB_EXPECT(s["identity"]["run_id"].is_null() && s["identity"]["has_snapshot"] == false &&
                       s["identity"]["snapshot_version"].is_null() && s["identity"]["core_version"] == "fixture-core-1",
                   s["identity"].dump());
    }
    {
        const auto s = statsOf(readRunStatistics(noLut.string(), "on_demand"));
        MIB_EXPECT(s["emodulus"]["status"] == "no_lut" && s["emodulus"]["median_kpa"].is_null() &&
                       s["emodulus"]["out_of_domain"].is_null() && s["emodulus"]["lut"]["loaded"] == false,
                   s["emodulus"].dump());
        MIB_EXPECT(!hasFlag(s, "e_out_of_domain"), "no E, no out-of-domain flag");
    }
    {
        // The E status follows the identity's E-LUT record and provenance (review ABI 5, #783): a
        // regenerated file (no snapshot) with `@emodulus_lut_json` is computed and names its
        // provenance; the snapshot's own record wins over the attribute.
        wd.mark("regenerated E-LUT");
        const auto withAttribute = [&](const fs::path &from, const char *name, const std::string &lut) {
            const fs::path to = file(name);
            fs::copy_file(from, to, fs::copy_options::overwrite_existing);
            backend::services::Hdf5Service w;
            MIB_REQUIRE(w.openFileForUpdate(to.string()) && w.writeEModulusLutJson(lut), "attribute");
            w.closeFile();
            return to;
        };
        const auto lut = [](const char *provenance, bool loaded) {
            return loaded ? json({{"loaded", true}, {"lut_id", "lut-v2"}, {"revision", "2"},
                                  {"source", "r2-public-catalog"}, {"sha256", std::string(64, 'd')},
                                  {"catalog_sha256", nullptr}, {"checksum_status", "verified"},
                                  {"provenance", provenance}})
                                .dump()
                          : json({{"loaded", false}, {"provenance", provenance}}).dump();
        };
        const auto host = withAttribute(legacy, "regen_host.h5", lut("host", true));
        auto s = statsOf(readRunStatistics(host.string(), "on_demand"));
        MIB_EXPECT(s["emodulus"]["status"] == "computed" && s["emodulus"]["lut"]["provenance"] == "host" &&
                       s["emodulus"]["lut"]["sha256"] == std::string(64, 'd') && s["emodulus"]["n"] == "14",
                   "a regenerated file's host record: " + s["emodulus"].dump());
        MIB_EXPECT(s["identity"]["has_snapshot"] == false, "regenerated: no snapshot");
        MIB_EXPECT(runStatisticsJson(*readRunStatistics(host.string(), "x").stats) == rowPathJson(host),
                   "the column path and the row path agree on a regenerated file");
        const auto recorded = withAttribute(legacy, "regen_recorded.h5", lut("recorded", false));
        s = statsOf(readRunStatistics(recorded.string(), "on_demand"));
        MIB_EXPECT(s["emodulus"]["status"] == "no_lut" && s["emodulus"]["lut"]["provenance"] == "recorded",
                   "a regenerated file whose source recorded no LUT: " + s["emodulus"].dump());
        const auto unknown = withAttribute(legacy, "regen_unknown.h5", lut("guessed", true));
        s = statsOf(readRunStatistics(unknown.string(), "on_demand"));
        MIB_EXPECT(s["emodulus"]["status"] == "not_recorded" && s["emodulus"]["lut"].is_null(),
                   "an unknown provenance is not a record: " + s["emodulus"].dump());
        const auto both = withAttribute(v3, "v3_attribute.h5", lut("host", true));
        s = statsOf(readRunStatistics(both.string(), "on_demand"));
        MIB_EXPECT(s["emodulus"]["lut"]["provenance"] == "snapshot" &&
                       s["emodulus"]["lut"]["sha256"] == std::string(64, 'a'),
                   "the snapshot record wins: " + s["emodulus"]["lut"].dump());
    }
    {
        const auto s = statsOf(readRunStatistics(even.string(), "on_demand"));
        MIB_EXPECT(s["counts"]["valid"] == "22" && s["area_um2"]["n"] == "21" && near(s["area_um2"]["median"], 15.75) &&
                       near(s["deformability"]["median"], 0.059) && s["emodulus"]["n"] == "15" &&
                       near(s["emodulus"]["median_kpa"], 3.75),
                   "odd counts: the middle value");
    }
    {
        const auto s = statsOf(readRunStatistics(empty.string(), "on_demand"));
        MIB_EXPECT(s["counts"] == json({{"set_rows", "0"}, {"valid", "0"}, {"finite", "0"}, {"excluded", "0"}}),
                   s["counts"].dump());
        MIB_EXPECT(s["area_um2"]["n"] == "0" && s["area_um2"]["median"].is_null() &&
                       s["deformability"]["median"].is_null() && s["emodulus"]["n"] == "0" &&
                       s["emodulus"]["median_kpa"].is_null() && s["target_group"]["percent"].is_null() &&
                       s["event_rate"]["status"] == "too_short" && s["overlay"]["points"] == "0",
                   "an empty valid set: counts 0, no medians: " + s.dump());
    }
    {
        const auto r = readRunStatistics(raw.string(), "on_demand");
        MIB_EXPECT(r.status == RunStatisticsStatus::NotAvailable && r.message == kRunStatisticsRawMessage && !r.stats,
                   "a raw recording: not available until exported");
        const auto c = readRunStatistics(corrupt.string(), "on_demand");
        MIB_EXPECT(c.status == RunStatisticsStatus::Error && !c.message.empty(), "a corrupt file is an error");
        const auto m = readRunStatistics(file("missing.h5").string(), "on_demand");
        MIB_EXPECT(m.status == RunStatisticsStatus::Error, "a missing file is an error");
        RunFileStamp stamp;
        std::string why;
        MIB_EXPECT(!stampRunFile(file("missing.h5").string(), stamp, why) && why.find("No such file") == 0, why);
        MIB_EXPECT(!stampRunFile(td.path().string(), stamp, why) && why.find("Not a file") == 0, why);
    }

    // ---- parity: the column path is byte-equal to the ReviewSession row path ---------------------
    wd.mark("parity");
    for (const auto &f : fixtures)
    {
        if (f.variant == RunStatisticsFixture::RawRecording) continue;
        const auto rows = rowPathJson(f.path);
        for (const std::size_t chunk : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{25}})
            MIB_EXPECT(columnPathJson(f.path, chunk) == rows,
                       f.path.filename().string() + " chunk " + std::to_string(chunk) + ":\n" +
                           columnPathJson(f.path, chunk) + "\n" + rows);
        MIB_EXPECT(runStatisticsJson(*readRunStatistics(f.path.string(), "x").stats) == rows,
                   "readRunStatistics is the column path");
    }
    {
        // A larger file across the default chunk (65536 rows) and the overlay cap.
        wd.mark("parity large");
        const fs::path big = file("big.h5");
        backend::services::Hdf5Service w;
        MIB_REQUIRE(w.openFile(big.string()) && w.initializeDatasets(), "big file");
        std::vector<backend::services::ProcessedFrame> batch;
        for (int i = 0; i < 70000; ++i)
        {
            backend::services::ProcessedFrame f;
            f.index = static_cast<std::uint64_t>(i);
            f.timestampNs = static_cast<std::uint64_t>(i) * 200;
            f.originalImage = cv::Mat(4, 4, CV_8UC1, cv::Scalar(0));
            f.processedImage = cv::Mat(4, 4, CV_8UC1, cv::Scalar(0));
            f.validation.isValid = i % 11 != 0;
            f.validation.area = 100.0 + std::fmod(i * 7.31, 97.0);
            f.validation.deformability = std::fmod(i * 0.0137, 0.4);
            f.validation.youngsModulus = i % 13 == 0 ? std::numeric_limits<double>::quiet_NaN() : 0.5 + std::fmod(i * 0.071, 5.0);
            f.validation.isTargetGroup = i % 5 == 0;
            batch.push_back(std::move(f));
            if (batch.size() == 10000)
            {
                MIB_REQUIRE(w.appendFrames(batch, {}), "append big");
                batch.clear();
            }
        }
        w.closeFile();
        const auto rows = rowPathJson(big);
        MIB_EXPECT(columnPathJson(big, 0) == rows, "70000 rows, default chunk");
        MIB_EXPECT(columnPathJson(big, 4099) == rows, "70000 rows, odd chunk");
        const auto s = json::parse(rows);
        MIB_EXPECT(s["overlay"]["points"].get<std::string>() == std::to_string((63636 + 31) / 32) &&
                       s["overlay"]["stride"] == "32",
                   "at most 2000 overlay points at a deterministic stride: " + s["overlay"]["points"].dump());
        MIB_EXPECT(!hasFlag(s, "low_n"), "n above 200");
    }

    // ---- no writes ---------------------------------------------------------------------------
    wd.mark("no writes");
    {
        const auto before = bytesOf(v3);
        const auto mtime = fs::last_write_time(v3);
        for (int i = 0; i < 3; ++i) (void)readRunStatistics(v3.string(), "on_demand");
        MIB_EXPECT(bytesOf(v3) == before && fs::last_write_time(v3) == mtime, "the file is never written");
    }

    // ---- deltas and the reference ------------------------------------------------------------------
    wd.mark("deltas");
    {
        auto run = readRunStatistics(v3.string(), "close");
        auto ref = readRunStatistics(even.string(), "close");
        RunQuickLookReference req{even.string(), runStatisticsFixtureRunId(RunStatisticsFixture::V3WithEEven), "8"};
        const auto reply = json::parse(runQuickLookReplyJson({run, readRunStatistics(legacy.string(), "close"),
                                                              readRunStatistics(raw.string(), "close")},
                                                             req, ref));
        MIB_EXPECT(reply["ok"] == true && reply["reference"]["confirmed"] == true &&
                       reply["reference"]["requested"]["start_generation"] == "8",
                   reply["reference"].dump());
        const auto &d = reply["runs"][0]["delta"];
        MIB_EXPECT(near(d["area_um2_median"]["value"], -0.25) && d["area_um2_median"]["withheld"].is_null(), d.dump());
        MIB_EXPECT(near(d["deformability_median"]["value"], 0.056 - 0.059, 1e-12), d.dump());
        MIB_EXPECT(d["emodulus_median_kpa"]["value"].is_null() && d["emodulus_median_kpa"]["withheld"] == "lut_differs",
                   "different LUT hashes: E withheld");
        MIB_EXPECT(d["differs"] == json({{"profile", "differs"}, {"method_revision", "same"}, {"core", "same"},
                                         {"contract", "same"}}),
                   "flagged, not withheld: " + d["differs"].dump());
        const auto &l = reply["runs"][1]["delta"];
        MIB_EXPECT(l["area_um2_median"]["withheld"] == "factor_not_recorded" &&
                       l["emodulus_median_kpa"]["withheld"] == "no_emodulus" &&
                       l["deformability_median"]["withheld"].is_null() && l["differs"]["profile"] == "unknown",
                   l.dump());
        MIB_EXPECT(reply["runs"][2]["status"] == "not_available" && reply["runs"][2]["delta"].is_null() &&
                       reply["runs"][2]["statistics"].is_null(),
                   reply["runs"][2].dump());
        // The same LUT on both sides: E's delta is given.
        const auto self = json::parse(runQuickLookReplyJson(
            {run}, RunQuickLookReference{v3.string(), runStatisticsFixtureRunId(RunStatisticsFixture::V3WithE), {}}, run));
        MIB_EXPECT(near(self["runs"][0]["delta"]["emodulus_median_kpa"]["value"], 0.0) &&
                       self["runs"][0]["delta"]["differs"]["profile"] == "same",
                   self.dump());
        // A run_id the file does not record: not confirmed, no deltas.
        const auto wrong = json::parse(runQuickLookReplyJson({run}, RunQuickLookReference{even.string(), "nope", {}}, ref));
        MIB_EXPECT(wrong["reference"]["confirmed"] == false &&
                       wrong["reference"]["message"].get<std::string>().find("run_id mismatch") == 0 &&
                       wrong["runs"][0]["delta"].is_null(),
                   wrong["reference"].dump());
        const auto deferred = json::parse(runQuickLookReplyJson({run}, req, deferredRunStatistics(even.string())));
        MIB_EXPECT(deferred["reference"]["confirmed"] == false && deferred["runs"][0]["delta"].is_null() &&
                       deferred["reference"]["run"]["status"] == "deferred" &&
                       deferred["reference"]["run"]["message"] == kRunStatisticsDeferredMessage,
                   deferred.dump());
        const auto none = json::parse(runQuickLookReplyJson({run}, std::nullopt, std::nullopt));
        MIB_EXPECT(none["reference"].is_null() && none["runs"][0]["delta"].is_null(), "no reference until chosen");
    }

    // ---- the request parser ---------------------------------------------------------------------
    wd.mark("parser");
    {
        RunQuickLookRequest req;
        std::string err;
        MIB_EXPECT(parseRunQuickLookRequest(R"({"paths":["/a.h5","b.h5"]})", req, err) && req.paths.size() == 2 &&
                       !req.reference,
                   err);
        MIB_EXPECT(parseRunQuickLookRequest(R"({"paths":["/a.h5"],"reference":{"path":"/r.h5","run_id":null}})", req,
                                            err) &&
                       req.reference && !req.reference->runId,
                   err);
        json many = {{"paths", json::array()}};
        for (int i = 0; i < 33; ++i) many["paths"].push_back("/f" + std::to_string(i) + ".h5");
        for (const auto &[text, key] : std::vector<std::pair<std::string, std::string>>{
                 {R"({"paths":[]})", "paths"},
                 {many.dump(), "paths"},
                 {R"({"paths":[""]})", "paths[0]"},
                 {R"({"paths":[1]})", "paths[0]"},
                 {R"({"paths":["/a"],"paths":["/b"]})", "duplicate"},
                 {R"({"paths":["/a"],"extra":1})", "unknown key"},
                 {R"({"paths":["/a"],"reference":{"path":"/r"}})", "reference.run_id"},
                 {R"({"paths":["/a"],"reference":{"run_id":"x"}})", "reference.path"},
                 {R"({"paths":["/a"],"reference":{"path":"/r","run_id":"x","start_generation":"-1"}})",
                  "start_generation"},
                 {R"({"paths":["/a"],"reference":{"path":"/r","run_id":"x","more":1}})", "unknown key"},
                 {R"([1])", "object"},
                 {R"({)", "JSON"},
                 {R"({})", "paths: required"},
             })
            MIB_EXPECT(!parseRunQuickLookRequest(text, req, err) && err.find(key) != std::string::npos,
                       text + " -> " + err);
        const auto refused = json::parse(runQuickLookRefusedJson("why"));
        MIB_EXPECT(refused["ok"] == false && refused["message"] == "why" && refused["runs"].empty(), refused.dump());
    }

    // ---- the cache -----------------------------------------------------------------------------
    wd.mark("cache");
    {
        RunStatisticsCache cache;
        MIB_EXPECT(cache.capacity() == kRunStatisticsCacheEntries && kRunStatisticsCacheEntries == 64, "64 entries");
        const auto stats = readRunStatistics(v3.string(), "close").stats;
        RunFileStamp stamp;
        std::string why;
        MIB_REQUIRE(stampRunFile(v3.string(), stamp, why), why);
        cache.insert(stamp, {stats, "close"});
        const auto hit = cache.find(stamp);
        MIB_EXPECT(hit && hit->stats == stats && hit->computedAt == "close", "a hit");
        auto changed = stamp;
        changed.mtimeNs += 1;
        MIB_EXPECT(!cache.find(changed) && cache.size() == 0, "a changed file is a miss and drops the stale entry");
        for (int i = 0; i < 70; ++i)
        {
            RunFileStamp s{"/p/" + std::to_string(i), 1, 1};
            cache.insert(s, {stats, "on_demand"});
        }
        MIB_EXPECT(cache.size() == 64, "bounded at 64");
        MIB_EXPECT(!cache.find({"/p/0", 1, 1}) && cache.find({"/p/69", 1, 1}), "least recently used evicted first");
        MIB_EXPECT(cache.find({"/p/6", 1, 1}).has_value(), "touch 6");
        cache.insert({"/p/70", 1, 1}, {stats, "on_demand"});
        MIB_EXPECT(cache.find({"/p/6", 1, 1}) && !cache.find({"/p/7", 1, 1}), "a find refreshes an entry");

        // Threads (TSan): inserts, finds, stale stamps and evictions at once.
        RunStatisticsCache shared;
        std::atomic<bool> go{false};
        std::atomic<int> hits{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 6; ++t)
            threads.emplace_back([&, t] {
                while (!go.load()) std::this_thread::yield();
                for (int i = 0; i < 4000; ++i)
                {
                    // A few paths, two stamps each (a changed file): hits, stale drops and evictions.
                    const RunFileStamp s{"/run/" + std::to_string((i + t) % 80), 10, (i / 7) % 2};
                    if (i % 2 == 0) shared.insert(s, {stats, "close"});
                    else if (const auto e = shared.find(s); e && e->stats == stats) hits.fetch_add(1);
                    if (i % 97 == 0) (void)shared.size();
                }
            });
        go.store(true);
        for (auto &th : threads) th.join();
        MIB_EXPECT(shared.size() <= 64, "bounded under threads: " + std::to_string(shared.size()));
        MIB_EXPECT(hits.load() > 0, "hits under threads");
    }

    // ---- the session and the lanes (#722) ---------------------------------------------------------
    wd.mark("lanes");
    {
        ReviewSession session;
        MIB_REQUIRE(session.open(even.string()), "open a file for review");
        const auto metaBefore = session.metadata();
        const auto r = session.runStatistics(v3.string(), "on_demand");
        MIB_EXPECT(r.status == RunStatisticsStatus::Computed, r.message);
        const auto metaAfter = session.metadata();
        MIB_EXPECT(session.isOpen() && session.filePath() == even.string() &&
                       metaAfter.totalValid == metaBefore.totalValid && session.frameCount(true) == 26,
                   "the open file is left as it was");
        ReviewImage before;
        MIB_REQUIRE(session.fetchImage(ReviewDataset::ValidImage, 3, OverlayMode::None, false, before), "image before");
        const auto self = session.runStatistics(even.string(), "on_demand");
        MIB_EXPECT(self.status == RunStatisticsStatus::Computed && session.isOpen(), "also the open file itself");
        ReviewImage image;
        MIB_EXPECT(session.fetchImage(ReviewDataset::ValidImage, 3, OverlayMode::None, false, image) &&
                       image.data == before.data,
                   "the session still reads its file after reading it again with another reader");
        std::vector<MetricRow> page;
        std::uint64_t total = 0;
        MIB_EXPECT(session.metricsPage(true, 0, 5, page, total) && total == 26 && page.size() == 5, "rows still served");

        std::mutex m;
        std::condition_variable cv;
        bool started = false, release = false;
        ReviewJobs jobs(session, nullptr, [](const ReviewJobEvent &) {});
        std::string err;
        const auto id = ReviewJobsTestAccess::start(
            jobs, ReviewJobKind::ExportMetrics,
            [&](std::uint64_t, const std::shared_ptr<std::atomic<bool>> &, const ReviewJobsTestAccess::Progress &) {
                std::unique_lock lock(m);
                started = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                return ReviewJobsTestAccess::Result{};
            },
            &err);
        MIB_REQUIRE(id != 0, err);
        {
            std::unique_lock lock(m);
            MIB_REQUIRE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return started; }), "job started");
        }
        const auto deferred = jobs.runStatistics(v3.string(), "on_demand");
        MIB_EXPECT(deferred.status == RunStatisticsStatus::Deferred && deferred.message == kRunStatisticsDeferredMessage &&
                       !deferred.stats,
                   "deferred while a review job holds a lane, never an error");
        {
            std::scoped_lock lock(m);
            release = true;
        }
        cv.notify_all();
        MIB_REQUIRE(ReviewJobsTestAccess::waitIdle(jobs, std::chrono::seconds(30)), "job done");
        const auto after = jobs.runStatistics(v3.string(), "on_demand");
        MIB_EXPECT(after.status == RunStatisticsStatus::Computed, "computed once the lanes are free");
    }

    return mib::test::exitCode();
}
