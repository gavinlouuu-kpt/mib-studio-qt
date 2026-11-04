// dot_grid_facade_test (CONNECT-054, bridge ABI 45)
//
// Wafer Grid through the BackendFacade surface the bridge calls, with the real AppBackend and
// DotGridService (no camera: frames are pushed into the backend's FrameStore):
//   1. the design registry compiled into mib_backend is the shipped
//      resources/defaults/dot_grid/registry.json (same fingerprint), so a shell without the
//      Qt resource system decodes against the registry the Qt build reads;
//   2. before initialize the commands refuse and the pose document says unavailable;
//   3. the facade starts the service paused: enabling loads the bundled registry (one design),
//      sizes the detector hint from the processing pixel-to-micron factor, and nothing decodes
//      while paused;
//   4. unpausing decodes the newest frame: a valid pose names the design, carries the dots and
//      the image size; since_sequence == sequence returns only the state block (unchanged);
//   5. a frame without a grid is an invalid pose with a reason and no dots;
//   6. pausing stops decoding again; disabling hides the pose and keeps the registry; switching
//      on again starts without the earlier pose.
#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/DotGridDecoder.h"
#include "backend/processing/DotGridRegistry.h"
#include "backend/services/DotGridService.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/opencv_tsan.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

namespace {
namespace bridge = backend::bridge;
namespace dg = backend::dotgrid;
using json = nlohmann::json;

constexpr uint64_t kMono8 = 0x01080001;

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

void push(backend::playback::FrameStore& store, const cv::Mat& img, uint64_t ts)
{
    store.pushFrame(img.data, img.total(), static_cast<uint64_t>(img.cols), static_cast<uint64_t>(img.rows),
                    static_cast<size_t>(img.step), kMono8, ts);
}

json pose(const bridge::BackendFacade& facade, uint64_t since = 0)
{
    return json::parse(facade.fetchDotGridPoseJson(since));
}
} // namespace

int main(int argc, char** argv)
{
    mib::test::serializeOpenCvUnderTsan();
    mib::test::Watchdog wd(120);
    MIB_REQUIRE(argc >= 2, "usage: dot_grid_facade_test <resources/defaults/dot_grid/registry.json>");

    // 1. The compiled-in registry is the shipped file.
    wd.mark("bundled registry");
    dg::Registry shipped;
    std::string err;
    MIB_REQUIRE(dg::Registry::loadFile(argv[1], shipped, &err), "shipped registry loads: " + err);
    const auto bundled = backend::services::DotGridService::bundledRegistry(&err);
    MIB_REQUIRE(bundled && !bundled->empty(), "bundled registry parses: " + err);
    MIB_EXPECT(bundled->fingerprint() == shipped.fingerprint(),
               "the compiled-in registry is resources/defaults/dot_grid/registry.json");
    MIB_EXPECT(backend::services::DotGridService::bundledRegistry() == bundled, "parsed once, shared");
    const dg::Design* design = bundled->find("wafer-sort-rt");
    MIB_REQUIRE(design != nullptr, "the Wafer_soRT design ships");

    mib::test::TempDir td("mib_dot_grid_facade");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (td.path() / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    {
        backend::AppBackend app;
        bridge::BackendFacade facade(app);

        // 2. Before initialize.
        wd.mark("before initialize");
        {
            const auto early = pose(facade);
            MIB_EXPECT(early["available"] == false && early["enabled"] == false && early["has_pose"] == false,
                       "unavailable before initialize: " + early.dump());
            const auto r = facade.setDotGridEnabled(true);
            MIB_EXPECT(!r.ok && !r.message.empty(), "enable refused before initialize");
            MIB_EXPECT(!facade.setDotGridPaused(false).ok, "unpause refused before initialize");
        }
        MIB_REQUIRE(facade.initialize(td.path().string()), "facade initialize");
        auto& service = app.dotGrid();

        // 3. Starts paused and disabled; enabling loads the bundled registry.
        wd.mark("enable");
        {
            const auto idle = pose(facade);
            MIB_EXPECT(idle["available"] == true, "the service thread runs: " + idle.dump());
            MIB_EXPECT(idle["enabled"] == false && idle["paused"] == true && idle["designs"] == 0 &&
                           idle["has_pose"] == false && idle["sequence"] == 0,
                       "paused and off at start: " + idle.dump());
        }
        const double factor = app.processing().getPixelToMicronFactor();
        const auto on = facade.setDotGridEnabled(true);
        MIB_REQUIRE(on.ok, "enable: " + on.message);
        {
            const auto state = pose(facade);
            MIB_EXPECT(state["enabled"] == true && state["paused"] == true, "on but paused: " + state.dump());
            MIB_EXPECT(state["designs"] == bundled->size(), "the bundled registry is the codebook source");
            MIB_EXPECT(state["has_pose"] == false, "no pose yet");
        }
        MIB_EXPECT(service.getConfig().registry == bundled, "the service holds the shared bundled registry");
        if (factor > 0.0) {
            MIB_EXPECT(std::abs(service.getConfig().umPerPxHint - factor) < 1e-12,
                       "the detector hint follows the processing pixel-to-micron factor");
        }

        // One frame while paused: nothing decodes.
        dg::ViewPose view;
        view.centreXUm = 40000.0;
        view.centreYUm = 52000.0;
        view.thetaDeg = 12.0;
        view.umPerPx = 0.293;
        view.mirrored = true;
        dg::RenderOptions render;
        render.seed = 1;
        const cv::Mat gridFrame = dg::renderView(*design->codebook, view, render);
        auto store = app.getFrameStore();
        push(*store, gridFrame, 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(600)); // > two sample intervals
        MIB_EXPECT(service.decodeAttempts() == 0, "a paused service decodes nothing");
        MIB_EXPECT(pose(facade)["sequence"] == 0, "no pose published while paused");

        // 4. Unpause: the newest frame is decoded.
        wd.mark("unpause");
        MIB_REQUIRE(facade.setDotGridPaused(false).ok, "unpause");
        MIB_REQUIRE(waitFor([&] { return pose(facade)["sequence"].get<uint64_t>() > 0; }, std::chrono::seconds(20)),
                    "a pose is published after unpausing");
        const json first = pose(facade);
        MIB_EXPECT(first["paused"] == false && first["has_pose"] == true, first.dump());
        MIB_EXPECT(first["valid"] == true, "valid pose: " + first.dump());
        if (first["valid"] == true) {
            MIB_EXPECT(std::abs(first["centre_x_um"].get<double>() - 40000.0) < 1.0 &&
                           std::abs(first["centre_y_um"].get<double>() - 52000.0) < 1.0,
                       "pose position: " + first.dump());
            MIB_EXPECT(std::abs(first["theta_deg"].get<double>() - 12.0) < 0.5, "pose rotation");
            MIB_EXPECT(first["mirrored"] == true, "mirror flag");
            MIB_EXPECT(first["design_id"] == "wafer-sort-rt" && !first["design_name"].get<std::string>().empty(),
                       "the pose names the design: " + first.dump());
            MIB_EXPECT(first["chip"].is_string(), "chip is always a string (empty outside a chip)");
            MIB_EXPECT(first["image_width"] == gridFrame.cols && first["image_height"] == gridFrame.rows,
                       "image size");
            MIB_EXPECT(first["dots_px"].is_array() && !first["dots_px"].empty() &&
                           first["dots_px"].size() == first["dots_px_total"].get<size_t>() &&
                           first["dots_px"][0].is_array() && first["dots_px"][0].size() == 2,
                       "detected dots as [x, y] pixel pairs");
            MIB_EXPECT(first["dots"].get<int>() >= static_cast<int>(first["dots_px"].size()) ||
                           first["dots"].get<int>() > 0,
                       "dot count");
            MIB_EXPECT(first["votes"].get<int>() >= 3 && first["decode_ms"].get<double>() > 0.0, "votes and timing");
        }

        // since_sequence == sequence: only the state block.
        const uint64_t seq = first["sequence"].get<uint64_t>();
        {
            const json same = pose(facade, seq);
            if (same["sequence"].get<uint64_t>() == seq) {
                MIB_EXPECT(same["unchanged"] == true && !same.contains("dots_px") && !same.contains("centre_x_um") &&
                               same["enabled"] == true,
                           "unchanged poll returns the state block only: " + same.dump());
            } // else a newer pose was published in between: the full document is correct
            const json stale = pose(facade, seq == 1 ? 999999 : seq - 1);
            MIB_EXPECT(stale["unchanged"] == false && stale.contains("valid"), "an older sequence gets the pose");
        }

        // 5. A frame without a grid: an invalid pose with a reason, no dots.
        wd.mark("blank frame");
        push(*store, cv::Mat(gridFrame.rows, gridFrame.cols, CV_8UC1, cv::Scalar(180)), 2000);
        MIB_REQUIRE(waitFor([&] { return pose(facade)["sequence"].get<uint64_t>() > seq; }, std::chrono::seconds(20)),
                    "a pose is published for the blank frame");
        {
            const json blank = pose(facade);
            MIB_EXPECT(blank["has_pose"] == true && blank["valid"] == false, blank.dump());
            MIB_EXPECT(!blank["reason"].get<std::string>().empty(), "a failure carries its reason: " + blank.dump());
            MIB_EXPECT(!blank.contains("dots_px") && !blank.contains("centre_x_um"),
                       "an invalid pose has no dots or position");
        }

        // 6. Pause stops decoding; disabling hides the pose and keeps the registry.
        wd.mark("pause and disable");
        MIB_REQUIRE(facade.setDotGridPaused(true).ok, "pause");
        const uint64_t attempts = service.decodeAttempts();
        push(*store, gridFrame, 3000);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        MIB_EXPECT(service.decodeAttempts() == attempts, "a paused service decodes nothing");
        MIB_REQUIRE(facade.setDotGridEnabled(false).ok, "disable");
        {
            const json off = pose(facade);
            MIB_EXPECT(off["enabled"] == false && off["paused"] == true && off["has_pose"] == false &&
                           off["designs"] == bundled->size(),
                       "off: no pose, registry kept: " + off.dump());
        }
        MIB_REQUIRE(facade.setDotGridEnabled(true).ok, "re-enable");
        MIB_EXPECT(service.getConfig().registry == bundled, "re-enabling does not reload the registry");
        {
            // Paused, so nothing has decoded since: the pose of the earlier session is forgotten.
            const json again = pose(facade);
            MIB_EXPECT(again["enabled"] == true && again["has_pose"] == false,
                       "switching on forgets the earlier pose: " + again.dump());
        }

        facade.shutdown();
    }
    return mib::test::exitCode();
}
