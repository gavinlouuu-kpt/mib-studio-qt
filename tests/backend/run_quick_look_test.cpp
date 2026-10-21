// run_quick_look_test (#716 A7a, bridge ABI 44): the run quick look through the facade with the
// real coordinator, review lanes and mock camera.
//
//  #722 (HDF5 is not thread-safe):
//  - the coordinator's pass runs in finalization step 6b, before the terminal status: when the
//    terminal ExperimentStatusEvent arrives, the run is already in the cache ("close");
//  - an on-demand miss is `deferred` while a run is active and while a review job holds a lane,
//    never an error; a cache hit is served during a run (no HDF5 read);
//  - a review job holding a lane at close defers the close pass too (nothing cached, the numbers
//    come on demand once the lane is free).
//  No actuation: run_quick_look never changes the experiment state, the readiness generation, the
//  processing config or factor, and never writes the files it reads.
//  The reference: confirmed by its run_id; deltas per median; a wrong run_id gives none.
//  Refusals: a malformed request, a refused relative path, a missing file (per-file error).

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/processing/ProcessingService.h"
#include "backend/review/ReviewJobs.h"
#include "backend/review/RunStatistics.h"
#include "backend/review/RunStatisticsFixture.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

namespace app = backend::app;
namespace bridge = backend::bridge;
namespace review = backend::review;
using json = nlohmann::json;

namespace {

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

app::ExperimentReadinessSnapshot readiness(bridge::BackendFacade& facade, const std::string& path)
{
    app::ExperimentReadinessSnapshot r;
    facade.fetchExperimentReadiness(r, path);
    return r;
}

bool startRun(bridge::BackendFacade& facade, const std::string& path)
{
    bridge::ExperimentCommand start;
    start.action = bridge::ExperimentCommandAction::Start;
    start.outputPath = path;
    start.readinessGeneration = readiness(facade, path).generation;
    return facade.dispatch(start).ok;
}

bool stopRun(bridge::BackendFacade& facade, backend::AppBackend& b)
{
    bridge::ExperimentCommand stop;
    stop.action = bridge::ExperimentCommandAction::Stop;
    if (!facade.dispatch(stop).ok) return false;
    return waitFor([&] { return b.experiment().status().terminal && b.experiment().state() == app::ExperimentRunState::Idle; },
                   std::chrono::seconds(20));
}

json quickLook(bridge::BackendFacade& facade, const json& request)
{
    return json::parse(facade.runQuickLookJson(request.dump()));
}

std::string bytesOf(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A review job that holds its lane until released.
struct HeldJob {
    std::mutex m;
    std::condition_variable cv;
    bool started{false}, release{false};
    std::uint64_t id{0};
    void start(review::ReviewJobs& jobs)
    {
        std::string err;
        id = review::ReviewJobsTestAccess::start(
            jobs, review::ReviewJobKind::ExportMetrics,
            [this](std::uint64_t, const std::shared_ptr<std::atomic<bool>>&, const review::ReviewJobsTestAccess::Progress&) {
                std::unique_lock lock(m);
                started = true;
                cv.notify_all();
                cv.wait(lock, [this] { return release; });
                return review::ReviewJobsTestAccess::Result{};
            },
            &err);
        MIB_REQUIRE(id != 0, "held job: " + err);
        std::unique_lock lock(m);
        MIB_REQUIRE(cv.wait_for(lock, std::chrono::seconds(30), [this] { return started; }), "held job started");
    }
    void finish(review::ReviewJobs& jobs)
    {
        {
            std::scoped_lock lock(m);
            release = true;
        }
        cv.notify_all();
        MIB_REQUIRE(review::ReviewJobsTestAccess::waitIdle(jobs, std::chrono::seconds(30)), "held job done");
    }
};

} // namespace

int main()
{
    mib::test::Watchdog wd(170);
    mib::test::TempDir td("mib_run_quick_look");
    const auto mockDir = td.path() / "mock_frames";
    std::filesystem::create_directories(mockDir);
    MIB_REQUIRE(cv::imwrite((mockDir / "frame_000.tiff").string(), cv::Mat(96, 128, CV_8UC1, cv::Scalar(180))),
                "mock frame fixture");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (td.path() / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");
    const auto v3 = (td.path() / "fixture_v3.h5").string();
    const auto even = (td.path() / "fixture_even.h5").string();
    const auto legacy = (td.path() / "fixture_legacy.h5").string();
    MIB_REQUIRE(review::writeRunStatisticsFixture(v3, review::RunStatisticsFixture::V3WithE), "fixture v3");
    MIB_REQUIRE(review::writeRunStatisticsFixture(even, review::RunStatisticsFixture::V3WithEEven), "fixture even");
    MIB_REQUIRE(review::writeRunStatisticsFixture(legacy, review::RunStatisticsFixture::Legacy), "fixture legacy");

    backend::AppBackend backendApp;
    bridge::BackendFacade facade(backendApp);
    MIB_EXPECT(quickLook(facade, {{"paths", {v3}}})["ok"] == false, "refused before initialize");
    MIB_REQUIRE(facade.initialize(td.path().string()), "facade initialize");
    auto& cache = backendApp.runStatisticsCache();
    auto& jobs = bridge::BackendFacadeTestAccess::reviewJobs(facade);

    // The terminal status, as the facade's event stream carries it: the cache size at that moment.
    std::atomic<int> cacheAtTerminal{-1};
    facade.setEventSink([&](const bridge::BackendEvent& e) {
        if (const auto* s = std::get_if<bridge::ExperimentStatusEvent>(&e); s && s->status.terminal)
            cacheAtTerminal.store(static_cast<int>(cache.size()));
    });

    // ---- refusals and idle on-demand reads -------------------------------------------------
    {
        wd.mark("idle");
        const auto bad = quickLook(facade, {{"paths", json::array()}});
        MIB_EXPECT(bad["ok"] == false && bad["message"].get<std::string>().find("paths") != std::string::npos, bad.dump());
        const auto r = quickLook(facade, {{"paths", {v3, "../outside.h5", (td.path() / "missing.h5").string()}}});
        MIB_EXPECT(r["ok"] == true && r["runs"].size() == 3, r.dump());
        MIB_EXPECT(r["runs"][0]["status"] == "computed" && r["runs"][0]["computed_at"] == "on_demand" &&
                       r["runs"][0]["from_cache"] == false && r["runs"][0]["statistics"]["counts"]["valid"] == "21",
                   r["runs"][0].dump());
        MIB_EXPECT(r["runs"][1]["status"] == "error" &&
                       r["runs"][1]["message"].get<std::string>().find("Path refused") == 0,
                   "a relative path that leaves the storage root: " + r["runs"][1].dump());
        MIB_EXPECT(r["runs"][2]["status"] == "error" &&
                       r["runs"][2]["message"].get<std::string>().find("No such file") == 0,
                   r["runs"][2].dump());
        const auto again = quickLook(facade, {{"paths", {v3}}});
        MIB_EXPECT(again["runs"][0]["from_cache"] == true && again["runs"][0]["computed_at"] == "on_demand" &&
                       again["runs"][0]["statistics"] == r["runs"][0]["statistics"],
                   "the second read is the cache's");
        // The reference: confirmed by run_id, deltas per median.
        const auto withRef = quickLook(facade, {{"paths", {v3, legacy}},
                                                {"reference", {{"path", even},
                                                               {"run_id", review::runStatisticsFixtureRunId(
                                                                              review::RunStatisticsFixture::V3WithEEven)},
                                                               {"start_generation", "8"}}}});
        MIB_EXPECT(withRef["reference"]["confirmed"] == true &&
                       withRef["runs"][0]["delta"]["emodulus_median_kpa"]["withheld"] == "lut_differs" &&
                       withRef["runs"][1]["delta"]["area_um2_median"]["withheld"] == "factor_not_recorded",
                   withRef.dump());
        const auto wrongRef = quickLook(facade, {{"paths", {v3}}, {"reference", {{"path", even}, {"run_id", "other"}}}});
        MIB_EXPECT(wrongRef["reference"]["confirmed"] == false && wrongRef["runs"][0]["delta"].is_null(),
                   wrongRef["reference"].dump());
    }

    // ---- no actuation ------------------------------------------------------------------------
    {
        wd.mark("no actuation");
        auto& proc = backendApp.processing();
        const auto factor = proc.getPixelToMicronFactor();
        const auto configVersion = proc.getConfigVersion();
        const auto generation = backendApp.experiment().readinessGeneration();
        const auto before = bytesOf(v3);
        for (int i = 0; i < 5; ++i) (void)quickLook(facade, {{"paths", {v3, even, legacy}}});
        MIB_EXPECT(proc.getPixelToMicronFactor() == factor && proc.getConfigVersion() == configVersion &&
                       backendApp.experiment().readinessGeneration() == generation &&
                       backendApp.experiment().state() == app::ExperimentRunState::Idle && bytesOf(v3) == before,
                   "reads change nothing and write nothing");
    }

    // ---- a run: the close pass before the terminal status; deferred during the run -------------
    {
        bridge::CameraCommand configure;
        configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
        configure.mockFrameDirectory = mockDir.string();
        configure.mockFrameIntervalMs = 5;
        configure.mockLoopFiles = true;
        MIB_REQUIRE(facade.dispatch(configure).ok, "mock camera configure");
        bridge::CameraCommand startCapture;
        startCapture.action = bridge::CameraCommandAction::StartCapture;
        MIB_REQUIRE(facade.dispatch(startCapture).ok, "capture start");
    }
    const auto run1 = (td.path() / "run1.h5").string();
    MIB_REQUIRE(waitFor([&] { return readiness(facade, run1).ready; }, std::chrono::seconds(15)), "ready");
    {
        wd.mark("run 1");
        cache.clear();
        const auto cachedBefore = quickLook(facade, {{"paths", {even}}});
        MIB_REQUIRE(cachedBefore["runs"][0]["status"] == "computed", "even cached before the run");
        MIB_REQUIRE(startRun(facade, run1), "start run 1");
        MIB_REQUIRE(waitFor([&] { return backendApp.experiment().state() == app::ExperimentRunState::Active; },
                            std::chrono::seconds(10)),
                    "active");
        const auto during = quickLook(facade, {{"paths", {v3, even, run1}}});
        MIB_EXPECT(during["runs"][0]["status"] == "deferred" &&
                       during["runs"][0]["message"] == review::kRunStatisticsDeferredMessage &&
                       during["runs"][0]["statistics"].is_null(),
                   "a miss during a run is deferred: " + during["runs"][0].dump());
        MIB_EXPECT(during["runs"][1]["status"] == "computed" && during["runs"][1]["from_cache"] == true,
                   "a cache hit is served during a run: " + during["runs"][1].dump());
        MIB_EXPECT(during["runs"][2]["status"] == "deferred", "the active run itself: numbers at close");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cacheAtTerminal.store(-1);
        MIB_REQUIRE(stopRun(facade, backendApp), "stop run 1");
        MIB_REQUIRE(waitFor([&] { return cacheAtTerminal.load() >= 0; }, std::chrono::seconds(10)), "terminal event");
        MIB_EXPECT(cacheAtTerminal.load() == 2, "the close pass ran before the terminal status (even + run1): " +
                                                    std::to_string(cacheAtTerminal.load()));
        const auto after = quickLook(facade, {{"paths", {run1}}});
        MIB_EXPECT(after["runs"][0]["status"] == "computed" && after["runs"][0]["computed_at"] == "close" &&
                       after["runs"][0]["from_cache"] == true,
                   "the run's numbers come from the close pass: " + after["runs"][0].dump());
        const auto& s = after["runs"][0]["statistics"];
        MIB_EXPECT(s["identity"]["has_snapshot"] == true && !s["identity"]["run_id"].is_null() &&
                       s["accounting"].is_object() && !s["flags"].empty(),
                   "a real run's identity and accounting: " + s.dump());
    }

    // ---- a review job: on-demand deferred; at close deferred too -------------------------------
    {
        wd.mark("review job");
        cache.clear();
        HeldJob held;
        held.start(jobs);
        const auto busy = quickLook(facade, {{"paths", {v3}}});
        MIB_EXPECT(busy["runs"][0]["status"] == "deferred", "deferred while a review job runs: " + busy.dump());
        const auto run2 = (td.path() / "run2.h5").string();
        MIB_REQUIRE(waitFor([&] { return readiness(facade, run2).ready; }, std::chrono::seconds(15)), "ready 2");
        MIB_REQUIRE(startRun(facade, run2), "start run 2");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cacheAtTerminal.store(-1);
        MIB_REQUIRE(stopRun(facade, backendApp), "stop run 2");
        MIB_REQUIRE(waitFor([&] { return cacheAtTerminal.load() >= 0; }, std::chrono::seconds(10)), "terminal 2");
        MIB_EXPECT(cacheAtTerminal.load() == 0, "a held lane defers the close pass: nothing read beside the job");
        const auto stillBusy = quickLook(facade, {{"paths", {run2}}});
        MIB_EXPECT(stillBusy["runs"][0]["status"] == "deferred", stillBusy.dump());
        held.finish(jobs);
        const auto free = quickLook(facade, {{"paths", {run2, v3}}});
        MIB_EXPECT(free["runs"][0]["status"] == "computed" && free["runs"][0]["computed_at"] == "on_demand" &&
                       free["runs"][1]["status"] == "computed",
                   "computed on demand once the lanes are free: " + free.dump());
    }

    facade.shutdown();
    return mib::test::exitCode();
}
