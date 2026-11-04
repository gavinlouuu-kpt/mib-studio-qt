// monitoring_density_facade_test (#866 PR E, bridge ABI 46)
//
// The live Monitoring density, the reference contour from a file, the E-modulus table and the stats
// telemetry through the BackendFacade surface the bridge calls, with the real AppBackend (mock camera;
// rows enter the monitoring ring through ProcessingService's append seam, which tags them exactly as the
// realtime path does):
//   1. nothing works before initialize (each command refuses in its own form);
//   2. settings: pushed to MonitoringDensityService and echoed as it applies them (clamped: interval,
//      bandwidth factor, core fraction; non-finite axes fall back to 0); a request while disabled is a
//      no-op that says so;
//   3. the result: one density per monitoring row keyed by the row's whole identity (start generation,
//      capture epoch, frame index, object id), across two captures that reuse frame numbers and a frame
//      with two objects, so a frame index alone is never enough; every valid snapshot row is named by a
//      point; densities are in [0, 1] with a maximum of 1; the core contour, bandwidth, grid, timing and
//      refresh are reported; a poll with the generation seen is {valid, generation, unchanged} and nothing
//      more; a turned-over ring (more rows than the 1000 it holds) is bounded to 1000 points and a payload
//      far below what one reply may carry; disabling drops the result (a reply with result null);
//   4. the reference contour from a file: the full-run record is preferred to the live one, a file without
//      a record, a missing file, a file that is not HDF5 and a path the storage rule refuses are each the
//      plain "No reference contour loaded from <path>: <reason>"; nothing is read while an experiment runs;
//   5. the E-modulus table: loaded, its identity (the run snapshot's: sha256|path), the grid only when the
//      identity differs, row-major, null for no value; a reload changes the identity; no table is
//      {loaded: false};
//   6. the stats telemetry: algorithm time and its age (not published until a frame was processed), the
//      realtime mode, the camera session's frame and data rate with their validity (an unknown rate is
//      never a 0).

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/processing/KdeCoreRecord.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"
#include "backend/services/MonitoringDensityService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {
namespace bridge = backend::bridge;
namespace app = backend::app;
namespace mon = backend::monitoring;
using backend::services::ProcessedFrame;
using json = nlohmann::json;

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool contains(const json& text, const std::string& part) { return text.is_string() && contains(text.get<std::string>(), part); }

constexpr std::uint64_t kBase = 2000000000ull; // injected frame indexes, far from the mock camera's

// A valid row as the realtime path appends it: only what the density reads matters (no images).
void injectRow(backend::services::ProcessingService& proc, std::uint64_t frameIndex, int objectId, double area,
               double deformability, bool valid = true)
{
    ProcessedFrame f;
    f.index = frameIndex;
    f.timestampNs = frameIndex * 10;
    f.validation.isValid = valid;
    f.validation.objectId = objectId;
    f.validation.objectCount = 1;
    f.validation.area = area; // pixels; the service converts with the pixel-to-micron factor
    f.validation.deformability = deformability;
    proc.appendMonitoringFrameForTests(f);
}

// A cell cluster with a few sparse outliers, deterministic.
struct Cloud
{
    std::mt19937 rng{20261105};
    std::normal_distribution<double> area{1200.0, 80.0}, deform{0.05, 0.008};
    void add(backend::services::ProcessingService& proc, std::uint64_t first, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i)
            injectRow(proc, first + i, 0, i % 10 == 9 ? 3500.0 + 10.0 * static_cast<double>(i) : area(rng),
                      i % 10 == 9 ? 0.4 : deform(rng));
    }
};

json density(bridge::BackendFacade& facade, std::uint64_t since)
{
    std::string text;
    MIB_REQUIRE(facade.fetchMonitoringDensityJson(since, text), "fetch_monitoring_density");
    return json::parse(text);
}

json setSettings(bridge::BackendFacade& facade, bool enabled, int intervalMs, double factor, double fraction,
                 double x0, double x1, double y0, double y1)
{
    return json::parse(facade.setMonitoringDensitySettingsJson(enabled, intervalMs, factor, fraction, x0, x1, y0, y1));
}

// Wait for a result of exactly `count` points newer than `since`; returns the reply.
json waitForResult(bridge::BackendFacade& facade, std::uint64_t since, std::size_t count)
{
    json reply;
    MIB_REQUIRE(waitFor(
                    [&] {
                        reply = density(facade, since);
                        return reply["unchanged"] == false && reply["result"].is_object() &&
                               reply["result"]["points"]["count"] == count;
                    },
                    std::chrono::seconds(30)),
                "an estimate of " + std::to_string(count) + " points lands: " + reply.dump().substr(0, 400));
    return reply;
}

std::uint64_t generationOf(const json& reply) { return std::stoull(reply["generation"].get<std::string>()); }

using Key = std::tuple<std::string, std::string, std::string, int>;

std::set<Key> pointKeys(const json& result)
{
    const auto& p = result["points"];
    std::set<Key> keys;
    for (std::size_t i = 0; i < p["count"].get<std::size_t>(); ++i)
    {
        const auto& ctx = p["contexts"][p["context"][i].get<std::size_t>()];
        keys.insert(Key{ctx["start_generation"].get<std::string>(), ctx["capture_epoch"].get<std::string>(),
                        p["frame_index"][i].get<std::string>(), p["object_id"][i].get<int>()});
    }
    return keys;
}

void writeKdeFile(const std::string& path, const mon::KdeCoreRecord* live, const mon::KdeCoreRecord* full)
{
    backend::services::Hdf5Service hdf5;
    MIB_REQUIRE(hdf5.openFile(path), "fixture create");
    MIB_REQUIRE(hdf5.initializeDatasets(), "fixture datasets");
    if (live) MIB_REQUIRE(hdf5.writeKdeLiveJson(mon::toJson(*live)), "fixture live record");
    if (full) MIB_REQUIRE(hdf5.writeKdeAnalysisJson(mon::toJson(*full)), "fixture analysis record");
    hdf5.closeFile();
}

std::string writeLut(const std::filesystem::path& path, double scale)
{
    std::ofstream out(path, std::ios::binary);
    out << "# test table\n";
    for (int a = 0; a < 6; ++a)
        for (int d = 0; d < 6; ++d)
            out << 100 + 60 * a << '\t' << 0.01 + 0.02 * d << '\t' << scale * (1.0 + 0.3 * a + 0.1 * d) << '\n';
    return path.string();
}

bridge::BackendCommandResult startRun(bridge::BackendFacade& facade, const std::string& path)
{
    app::ExperimentReadinessSnapshot readiness;
    bridge::ExperimentCommand start;
    start.action = bridge::ExperimentCommandAction::Start;
    start.outputPath = path;
    if (facade.fetchExperimentReadiness(readiness, path)) start.readinessGeneration = readiness.generation;
    return facade.dispatch(start);
}
} // namespace

int main()
{
    mib::test::Watchdog wd(180);
    mib::test::TempDir td("mib_monitoring_density");
    const auto mockDir = td.path() / "mock_frames";
    std::filesystem::create_directories(mockDir);
    MIB_REQUIRE(cv::imwrite((mockDir / "frame_000.tiff").string(), cv::Mat(96, 512, CV_8UC1, cv::Scalar(180))),
                "mock frame fixture");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (td.path() / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    {
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);

        // ---- 1. before initialize ------------------------------------------------------------
        wd.mark("before init");
        {
            std::string text;
            MIB_EXPECT(!facade.fetchMonitoringDensityJson(0, text), "no density before initialize");
            const auto set = setSettings(facade, true, 2000, 1.0, 0.9, 0, 0, 0, 0);
            MIB_EXPECT(set["ok"] == false && set["settings"].is_null(), "settings refused: " + set.dump());
            MIB_EXPECT(!facade.requestMonitoringDensityUpdate().ok, "no update request before initialize");
            const auto ref = json::parse(facade.fetchKdeReferenceFromFileJson("/x.h5"));
            MIB_EXPECT(ref["ok"] == false && contains(ref["error"], "No reference contour loaded from /x.h5: "),
                       "reference refused: " + ref.dump());
            MIB_EXPECT(json::parse(facade.fetchEModulusLutJson(""))["valid"] == false, "no table before initialize");
        }
        MIB_REQUIRE(facade.initialize(td.path().string()), "facade initialize");
        auto& proc = backendApp.processing();
        proc.setPixelToMicronFactor(0.5); // 1200 px^2 -> 300 um^2

        // ---- 2. settings ---------------------------------------------------------------------
        wd.mark("settings");
        {
            // Out-of-range values are clamped by the service; a non-finite axis drops the axes to 0.
            auto set = setSettings(facade, false, 1, 99.0, 0.01, 0.0, 1000.0, 0.0, std::nan(""));
            MIB_EXPECT(set["ok"] == true, set.dump());
            const auto& s = set["settings"];
            MIB_EXPECT(s["enabled"] == false && s["interval_ms"] == 500 && s["bandwidth_factor"] == 5.0 &&
                           s["core_fraction"] == 0.05 && s["x0"] == 0.0 && s["x1"] == 0.0 && s["y0"] == 0.0 &&
                           s["y1"] == 0.0,
                       "clamped as the service applies them: " + s.dump());
            set = setSettings(facade, true, 100000, std::nan(""), std::nan(""), 0.0, 1000.0, 0.0, 1.0);
            MIB_EXPECT(set["settings"]["interval_ms"] == 60000 && set["settings"]["bandwidth_factor"] == 1.0 &&
                           set["settings"]["core_fraction"] == 0.9 && set["settings"]["x1"] == 1000.0,
                       "non-finite values fall back to the defaults, the axes are kept: " + set.dump());
            // A tick only on request (60 s interval): deterministic.
            const auto r = facade.requestMonitoringDensityUpdate();
            MIB_EXPECT(r.ok && contains(r.message, "requested"), r.message);
            auto off = setSettings(facade, false, 60000, 1.0, 0.9, 0.0, 1000.0, 0.0, 1.0);
            MIB_EXPECT(off["settings"]["enabled"] == false, off.dump());
            const auto noop = facade.requestMonitoringDensityUpdate();
            MIB_EXPECT(noop.ok && contains(noop.message, "disabled"), "a request while disabled is a no-op: " + noop.message);
            setSettings(facade, true, 60000, 1.0, 0.9, 0.0, 1000.0, 0.0, 1.0);
        }

        // ---- 3. the result -------------------------------------------------------------------
        wd.mark("result");
        {
            // Nothing in the ring: no estimate. Disabling above advanced the generation (it drops the result), so
            // a poll from 0 is answered with "no result" (the shell clears its contours), and from there on nothing
            // is newer: unchanged, the three members only.
            facade.requestMonitoringDensityUpdate();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto empty = density(facade, 0);
            MIB_EXPECT(empty["valid"] == true && empty["unchanged"] == false && empty["result"].is_null() &&
                           generationOf(empty) >= 1,
                       "an empty ring: no result: " + empty.dump());
            const auto none = density(facade, generationOf(empty));
            MIB_EXPECT(none["valid"] == true && none["unchanged"] == true && none["generation"] == std::to_string(generationOf(empty)) &&
                           none.size() == 3,
                       "then nothing is newer, three members only: " + none.dump());

            // Two captures that reuse frame numbers 1..60, the second inside run 7; frame 5 holds two objects.
            Cloud cloud;
            const auto e1 = proc.monitoringCaptureEpoch();
            cloud.add(proc, 1, 60);
            injectRow(proc, 5, 1, 1210.0, 0.052);
            injectRow(proc, 9000, 0, 20.0, 0.3, /*valid*/ false); // an invalid row is no point
            const auto e2 = proc.requestMonitoringCaptureChange("a test capture change");
            MIB_REQUIRE(e2 > e1 && proc.monitoringCaptureEpoch() == e2, "a new capture");
            proc.setMonitoringRunGeneration(7);
            cloud.add(proc, 1, 60);
            injectRow(proc, 5, 1, 1190.0, 0.049);
            const std::size_t total = 122;

            facade.requestMonitoringDensityUpdate();
            const auto reply = waitForResult(facade, 0, total);
            const auto& result = reply["result"];
            const auto& p = result["points"];
            MIB_EXPECT(p["contexts"].size() == 2 && p["contexts"][0]["start_generation"] == "0" &&
                           p["contexts"][0]["capture_epoch"] == std::to_string(e1) &&
                           p["contexts"][1]["start_generation"] == "7" &&
                           p["contexts"][1]["capture_epoch"] == std::to_string(e2),
                       "the two (run, capture) pairs are listed once each: " + p["contexts"].dump());
            MIB_EXPECT(p["context"].size() == total && p["frame_index"].size() == total && p["object_id"].size() == total &&
                           p["density"].size() == total,
                       "the arrays are parallel");
            const auto keys = pointKeys(result);
            MIB_EXPECT(keys.size() == total, "every point has its own identity: " + std::to_string(keys.size()));
            MIB_EXPECT(keys.count({"0", std::to_string(e1), "1", 0}) && keys.count({"7", std::to_string(e2), "1", 0}) &&
                           keys.count({"0", std::to_string(e1), "5", 1}) && keys.count({"7", std::to_string(e2), "5", 1}) &&
                           keys.count({"7", std::to_string(e2), "5", 0}) && !keys.count({"0", std::to_string(e1), "9000", 0}),
                       "frame 1 of both captures, both objects of frame 5, and no invalid row");

            // Every valid snapshot row is named by a point (the frontend's match).
            bridge::BackendMonitoringSnapshot snap;
            MIB_REQUIRE(facade.fetchMonitoringSnapshot(snap, 4000), "snapshot");
            std::size_t matched = 0, valid = 0;
            for (const auto& row : snap.rows)
            {
                if (!row.valid) continue;
                ++valid;
                matched += keys.count({std::to_string(row.startGeneration), std::to_string(row.captureEpoch),
                                       std::to_string(row.frameIndex), row.objectId});
            }
            MIB_EXPECT(valid == total && matched == total, "snapshot rows match points by identity: " +
                                                               std::to_string(matched) + "/" + std::to_string(valid));

            double maxD = 0.0, minD = 1.0;
            for (const auto& d : p["density"])
            {
                MIB_REQUIRE(d.is_number(), "a density is a number");
                maxD = std::max(maxD, d.get<double>());
                minD = std::min(minD, d.get<double>());
            }
            MIB_EXPECT(maxD == 1.0 && minD >= 0.0, "densities normalised to [0, 1]");
            MIB_EXPECT(result["core_level"].is_number() && result["core_count"].get<int>() >= 100 && !result["contours"].empty(),
                       "the core and its contour: " + result["contours"].dump().substr(0, 120));
            const auto& loop = result["contours"][0];
            MIB_EXPECT(loop.size() >= 4 && loop.front() == loop.back() && loop[0].size() == 2, "a closed loop of [x, y]");
            MIB_EXPECT(result["pixel_to_micron"] == 0.5 && result["bandwidth"]["x"].get<double>() > 0.0 &&
                           result["bandwidth"]["y"].get<double>() > 0.0 && result["bandwidth_factor"] == 1.0 &&
                           result["core_fraction"] == 0.9,
                       "bandwidth and settings of the estimate");
            MIB_EXPECT(result["grid"]["x0"] == 0.0 && result["grid"]["x1"] == 1000.0 && result["grid"]["nx"] == 128 &&
                           result["grid"]["ny"] == 64,
                       "the configured axes are the grid: " + result["grid"].dump());
            MIB_EXPECT(result["compute_ms"].get<int>() >= 0 && result["wall_ms"].get<int>() >= 0 &&
                           result["computed_at_ns"].is_string(),
                       "compute and wall time");
            MIB_EXPECT(reply["stats"]["refresh_ms"].get<int>() >= 60000 && reply["stats"]["next_interval_ms"].get<int>() >= 60000 &&
                           reply["stats"]["priority_lowered"].is_boolean() && reply["settings"]["interval_ms"] == 60000,
                       "refresh: the configured interval at least: " + reply["stats"].dump());
            MIB_EXPECT(reply.dump().size() < 100000, "a 122-point reply stays small: " + std::to_string(reply.dump().size()));

            // The generation seen: nothing newer, three members.
            const auto gen = generationOf(reply);
            MIB_EXPECT(gen >= 1, "a generation");
            const auto same = density(facade, gen);
            MIB_EXPECT(same["unchanged"] == true && same["valid"] == true && same["generation"] == std::to_string(gen) &&
                           same.size() == 3,
                       "unchanged: " + same.dump());
            MIB_EXPECT(density(facade, gen + 5)["unchanged"] == false, "any other generation is answered");

            // The ring turns over: 1100 more rows, the ring holds 1000, the reply is bounded to them.
            wd.mark("turnover");
            for (std::uint64_t i = 0; i < 1100; ++i)
                injectRow(proc, kBase + i, 0, 1100.0 + static_cast<double>(i % 200), 0.04 + 0.0001 * static_cast<double>(i % 90));
            facade.requestMonitoringDensityUpdate();
            const auto big = waitForResult(facade, gen, 1000);
            const std::size_t bytes = big.dump().size();
            MIB_EXPECT(big["result"]["points"]["count"] == 1000 && bytes < 150000,
                       "at most 1000 points, " + std::to_string(bytes) + " bytes");
            MIB_EXPECT(big["result"]["points"]["contexts"].size() == 1 &&
                           big["result"]["points"]["contexts"][0]["capture_epoch"] == std::to_string(e2),
                       "the older capture left the ring: " + big["result"]["points"]["contexts"].dump());

            // Disabling drops the result: a newer generation with result null.
            wd.mark("disable");
            const auto before = generationOf(big);
            setSettings(facade, false, 60000, 1.0, 0.9, 0.0, 1000.0, 0.0, 1.0);
            const auto dropped = density(facade, before);
            MIB_EXPECT(dropped["unchanged"] == false && dropped["result"].is_null() && generationOf(dropped) > before &&
                           dropped["settings"]["enabled"] == false,
                       "disabled: the result is dropped: " + dropped.dump());
            // Enabling again recomputes the unchanged input.
            setSettings(facade, true, 60000, 1.0, 0.9, 0.0, 1000.0, 0.0, 1.0);
            const auto again = waitForResult(facade, generationOf(dropped), 1000);
            MIB_EXPECT(generationOf(again) > generationOf(dropped), "re-enabled: a new estimate");
            // Leave the density off for the phases below (the worker would wake on a run's rows).
            setSettings(facade, false, 60000, 1.0, 0.9, 0.0, 1000.0, 0.0, 1.0);
            proc.setMonitoringRunGeneration(0);
        }

        // ---- 4. the reference contour from a file --------------------------------------------
        wd.mark("reference");
        {
            mon::KdeCoreRecord live;
            live.coreFraction = 0.8;
            live.contours = {{{100, 0.02}, {200, 0.02}, {200, 0.08}, {100, 0.08}, {100, 0.02}}};
            mon::KdeCoreRecord full = live;
            full.provisional = false;
            full.source = "full-run";
            full.coreFraction = 0.9;
            full.contours.push_back({{400, 0.3}, {450, 0.3}, {450, 0.35}, {400, 0.3}});
            mon::KdeCoreRecord empty = live;
            empty.contours.clear();
            const auto liveOnly = (td.path() / "ref_live_only.h5").string();
            const auto both = (td.path() / "ref_both.h5").string();
            const auto none = (td.path() / "ref_none.h5").string();
            const auto emptyContour = (td.path() / "ref_empty.h5").string();
            writeKdeFile(liveOnly, &live, nullptr);
            writeKdeFile(both, &live, &full);
            writeKdeFile(none, nullptr, nullptr);
            writeKdeFile(emptyContour, &empty, nullptr);
            const auto notHdf5 = (td.path() / "not_hdf5.h5").string();
            std::ofstream(notHdf5) << "this is not an HDF5 file";

            auto r = json::parse(facade.fetchKdeReferenceFromFileJson(liveOnly));
            MIB_EXPECT(r["ok"] == true && r["error"].is_null() && r["reason"].is_null() && r["source"] == "live" &&
                           r["core_percent"] == 80 && r["core_fraction"] == 0.8 && r["loops"] == 1 &&
                           r["file_name"] == "ref_live_only.h5" && r["path"] == liveOnly,
                       "a live record is read: " + r.dump());
            MIB_EXPECT(r["contours"][0].size() == 5 && r["contours"][0][1] == json::array({200.0, 0.02}),
                       "the contour as stored (um^2 x deformability): " + r["contours"].dump());
            r = json::parse(facade.fetchKdeReferenceFromFileJson(both));
            MIB_EXPECT(r["ok"] == true && r["source"] == "full-run" && r["core_percent"] == 90 && r["loops"] == 2,
                       "the full-run record is preferred: " + r.dump());

            const auto refuse = [&](const std::string& path, const std::string& reason, const std::string& what) {
                const auto x = json::parse(facade.fetchKdeReferenceFromFileJson(path));
                MIB_EXPECT(x["ok"] == false && x["reason"] == reason && x["contours"].empty() && x["loops"] == 0 &&
                               x["source"].is_null() &&
                               x["error"] == "No reference contour loaded from " + path + ": " + reason,
                           what + ": " + x.dump());
            };
            refuse(none, "the experiment has no stored core contour", "a file without a record");
            refuse(emptyContour, "the stored core contour is empty (too few cells)", "an empty contour");
            refuse((td.path() / "missing.h5").string(), "file not found", "a missing file");
            refuse(notHdf5, "not a readable HDF5 file", "a file that is not HDF5");
            {
                const auto x = json::parse(facade.fetchKdeReferenceFromFileJson(""));
                MIB_EXPECT(x["ok"] == false && contains(x["reason"], "1 to 4096 bytes"), "an empty path: " + x.dump());
                const auto y = json::parse(facade.fetchKdeReferenceFromFileJson(std::string(5000, 'a')));
                MIB_EXPECT(y["ok"] == false && contains(y["reason"], "1 to 4096 bytes") && y["path"].get<std::string>().size() == 256,
                           "a long path is refused and echoed short: " + y["reason"].dump());
            }
        }

        // ---- 5. the E-modulus table ----------------------------------------------------------
        wd.mark("emodulus");
        {
            const auto lutA = writeLut(td.path() / "lut_a.txt", 1.0);
            MIB_REQUIRE(proc.loadEModulusLut(lutA), "load a table");
            const auto identityA = proc.eModulusLutIdentity();
            auto lut = json::parse(facade.fetchEModulusLutJson(""));
            MIB_REQUIRE(lut["valid"] == true && lut["loaded"] == true && lut["unchanged"] == false && lut["lut"].is_object(),
                        "a loaded table with its grid: " + lut.dump().substr(0, 300));
            const std::string keyA = identityA.sha256 + "|" + identityA.path;
            MIB_EXPECT(lut["identity"] == keyA && lut["info"]["sha256"] == identityA.sha256 && lut["info"]["path"] == identityA.path,
                       "the identity is the run snapshot's: " + lut["identity"].dump());
            const auto& g = lut["lut"];
            const auto& table = proc.eModulusLut();
            MIB_EXPECT(g["area_bins"] == 200 && g["deform_bins"] == 200 && g["values"].size() == 40000 &&
                           g["area_min"] == 100.0 && g["deform_min"] == 0.01,
                       "the 200 x 200 grid over the table's range: " + g.dump().substr(0, 200));
            MIB_EXPECT(std::fabs(g["area_step"].get<double>() - table.gridAreaStep()) < 1e-12 &&
                           std::fabs(g["deform_step"].get<double>() - table.gridDeformStep()) < 1e-12,
                       "the steps");
            // Row-major [deform][area], to six significant digits.
            double worst = 0.0;
            for (std::size_t d = 0; d < 200; d += 37)
                for (std::size_t a = 0; a < 200; a += 41)
                {
                    const double v = g["values"][d * 200 + a].get<double>();
                    const double ref = table.grid()[d * 200 + a];
                    worst = std::max(worst, std::fabs(v - ref) / std::fabs(ref));
                }
            MIB_EXPECT(worst < 5e-6, "the values are the table's, row-major: " + std::to_string(worst));
            MIB_EXPECT(lut.dump().size() < 600000, "the table fits one reply: " + std::to_string(lut.dump().size()));

            // The identity the caller holds: no grid.
            const auto same = json::parse(facade.fetchEModulusLutJson(keyA));
            MIB_EXPECT(same["valid"] == true && same["loaded"] == true && same["unchanged"] == true && same["lut"].is_null() &&
                           same["identity"] == keyA,
                       "the same table: " + same.dump());
            // Another identity: the grid again. A reload of other bytes changes it.
            MIB_EXPECT(json::parse(facade.fetchEModulusLutJson("old|/other"))["lut"].is_object(), "a different identity gets the grid");
            MIB_REQUIRE(proc.loadEModulusLut(writeLut(td.path() / "lut_b.txt", 2.0)), "reload another table");
            const auto changed = json::parse(facade.fetchEModulusLutJson(keyA));
            MIB_EXPECT(changed["unchanged"] == false && changed["lut"].is_object() && changed["identity"] != keyA &&
                           changed["lut"]["values"][0].get<double>() > 1.9 * lut["lut"]["values"][0].get<double>(),
                       "a reload is a new identity with the new grid");

            // A value the table does not have is null (the PZ7035 profile table loads grids with holes).
            std::vector<double> holes(4 * 3, 1.5);
            holes[5] = std::nan("");
            MIB_REQUIRE(const_cast<backend::EModulusLut&>(proc.eModulusLut()).loadGrid(10.0, 5.0, 4, 0.01, 0.02, 3, holes),
                        "a grid with a hole");
            const auto withHole = json::parse(facade.fetchEModulusLutJson("x"));
            MIB_EXPECT(withHole["lut"]["values"].size() == 12 && withHole["lut"]["values"][5].is_null() &&
                           withHole["lut"]["values"][4] == 1.5 && withHole["lut"]["area_bins"] == 4 &&
                           withHole["lut"]["deform_bins"] == 3 && withHole["lut"]["area_step"] == 5.0,
                       "NaN is null: " + withHole["lut"].dump());
            // No table: {loaded: false}.
            // (EModulusLut::resolvePath also tries the bundled table relative to the working directory, so
            // a missing file only fails to load when none is found there.)
            if (!proc.loadEModulusLut((td.path() / "no_such_table.txt").string()))
            {
                const auto unloaded = json::parse(facade.fetchEModulusLutJson(keyA));
                MIB_EXPECT(unloaded["valid"] == true && unloaded["loaded"] == false && unloaded["identity"] == "" &&
                               unloaded["lut"].is_null() && unloaded["info"].is_null(),
                           "no table: " + unloaded.dump());
            }
        }

        // ---- 6. the stats telemetry ----------------------------------------------------------
        wd.mark("stats");
        {
            bridge::BackendProcessingStats s;
            MIB_REQUIRE(facade.fetchProcessingStats(s), "processing stats");
            MIB_EXPECT(!s.algoAvgPublished && s.algoAvgUs == 0.0 && s.algoAvgAgeMs == 0.0 && s.realtimeMode == "inline" &&
                           !s.cameraRunning && s.cameraFrameRate.state == "unavailable" && !s.cameraFrameRate.hasValue &&
                           s.cameraDataRateMBps.state == "unavailable" && !s.cameraDataRateMBps.hasValue,
                       "nothing published, nothing running: no value is invented");
            proc.setRealtimeProcessingMode(backend::services::ProcessingService::RealtimeProcessingMode::AsyncBatch);
            MIB_REQUIRE(facade.fetchProcessingStats(s), "processing stats");
            MIB_EXPECT(s.realtimeMode == "async_batch", "the realtime mode is read: " + s.realtimeMode);
            proc.setRealtimeProcessingMode(backend::services::ProcessingService::RealtimeProcessingMode::Inline);

            bridge::CameraCommand configure;
            configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
            configure.mockFrameDirectory = mockDir.string();
            configure.mockFrameIntervalMs = 5;
            configure.mockLoopFiles = true;
            MIB_REQUIRE(facade.dispatch(configure).ok, "mock camera configure");
            bridge::CameraCommand startCapture;
            startCapture.action = bridge::CameraCommandAction::StartCapture;
            MIB_REQUIRE(facade.dispatch(startCapture).ok, "capture start");
            proc.setRealtimeEnabled(true);
            proc.startRealtime(backendApp.getFrameStore());
            const auto states = std::set<std::string>{"valid", "stale", "unavailable", "unsupported", "error"};
            MIB_REQUIRE(waitFor(
                            [&] {
                                return facade.fetchProcessingStats(s) && s.cameraRunning && s.algoAvgPublished;
                            },
                            std::chrono::seconds(30)),
                        "the camera runs and the algorithm time is published");
            MIB_EXPECT(s.algoAvgUs > 0.0 && s.algoAvgAgeMs >= 0.0 && s.algoAvgAgeMs < 10000.0,
                       "the algorithm time and its age: " + std::to_string(s.algoAvgUs) + " us, " + std::to_string(s.algoAvgAgeMs) + " ms");
            for (const auto* m : {&s.cameraFrameRate, &s.cameraDataRateMBps})
            {
                MIB_EXPECT(states.count(m->state) == 1, "a contract state: " + m->state);
                // A value exists exactly for a valid or stale sample (an unknown rate is never a 0).
                MIB_EXPECT(m->hasValue == (m->state == "valid" || m->state == "stale"), "value iff valid or stale: " + m->state);
                MIB_EXPECT(m->hasValue || (m->value == 0.0 && m->ageMs == 0.0), "no value, no number");
            }

            // ---- 4b. nothing is read while an experiment runs -----------------------------------
            wd.mark("reference during a run");
            const auto refFile = (td.path() / "ref_during_run.h5").string();
            mon::KdeCoreRecord rec;
            rec.contours = {{{1, 0.1}, {2, 0.1}, {2, 0.2}, {1, 0.1}}};
            writeKdeFile(refFile, &rec, nullptr);
            MIB_REQUIRE(json::parse(facade.fetchKdeReferenceFromFileJson(refFile))["ok"] == true, "readable while idle");
            const auto probe = (td.path() / "probe.h5").string();
            MIB_REQUIRE(waitFor(
                            [&] {
                                app::ExperimentReadinessSnapshot readiness;
                                return facade.fetchExperimentReadiness(readiness, probe) && readiness.ready;
                            },
                            std::chrono::seconds(15)),
                        "readiness passes with a running mock camera");
            const auto started = startRun(facade, (td.path() / "run1.h5").string());
            MIB_REQUIRE(started.ok, "a run starts: " + started.message);
            const auto during = json::parse(facade.fetchKdeReferenceFromFileJson(refFile));
            MIB_EXPECT(during["ok"] == false && contains(during["reason"], "experiment is running or starting") &&
                           contains(during["error"], "No reference contour loaded from " + refFile + ": "),
                       "no HDF5 file is read while a run is active (HDF5 is not thread-safe): " + during.dump());
            bridge::ExperimentCommand stop;
            stop.action = bridge::ExperimentCommandAction::Stop;
            stop.expectedStartGeneration = backendApp.experiment().status().startGeneration;
            MIB_REQUIRE(facade.dispatch(stop).ok, "stop the run");
            MIB_REQUIRE(waitFor([&] { return backendApp.experiment().status().state == app::ExperimentRunState::Idle; },
                                std::chrono::seconds(30)),
                        "the run finalizes");
            const auto after = json::parse(facade.fetchKdeReferenceFromFileJson(refFile));
            MIB_EXPECT(after["ok"] == true, "readable again once idle: " + after.dump());
        }
        backendApp.shutdown();
    }
    return 0;
}
