// recording_capacity_readiness_test (#833): the coordinator sizes the
// experiment buffer from the camera window and rate, gates Start on the
// camera's maximum rate and the destination's write rate, and applies the
// plan at Start. A camera that reports like the eGrabber camera
// (support/acquisition_report_camera.h) gives the window, the driven rate and
// AcquisitionFrameRateMax; a fake write-rate probe gives the destination.
//  - 512x96 and 1216x208 at 5000 fps on a 150 MB/s destination: ready; Start
//    applies the planned frame and byte caps (10000 frames, 2 s);
//  - 1920x1080 at 5000 fps (camera max 1123): camera.rate fails, Start refused;
//  - a destination slower than the floor stream: storage.throughput fails;
//  - an unknown destination rate or camera maximum: warnings, Start allowed;
//  - a camera without a report: the rate comes from target_fps;
//  - an override: experiment_buffer_max_mb sizes the buffer (1061 frames);
//  - window and rate are readiness inputs (generation changes), the available
//    RAM is not (two evaluations keep their generation); the destination is
//    measured once per directory and frame size, never by the UI view.

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"

#include "support/acquisition_report_camera.h"
#include "support/assert.h"
#include "support/fault_kernel.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using backend::app::ExperimentStartOutcome;
using backend::app::ExperimentStartRequest;
using backend::app::GateStatus;
using backend::services::ProcessingService;
namespace fs = std::filesystem;

namespace {
bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

GateStatus statusOf(const backend::app::ExperimentReadinessSnapshot& r, const char* id)
{
    const auto* g = r.gate(id);
    return g ? g->status : GateStatus::Unavailable;
}

std::string textOf(const backend::app::ExperimentReadinessSnapshot& r, const char* id)
{
    const auto* g = r.gate(id);
    return g ? g->reason + " | " + g->remediation + " | " + g->detail : std::string("(missing)");
}

void dumpGates(const backend::app::ExperimentReadinessSnapshot& r)
{
    std::fprintf(stderr, "readiness gen=%llu ready=%d\n", (unsigned long long)r.generation, r.ready ? 1 : 0);
    for (const auto& g : r.gates)
        std::fprintf(stderr, "  %-22s %-12s %s %s\n", g.id.c_str(), backend::app::toString(g.status), g.reason.c_str(),
                     g.detail.c_str());
}
} // namespace

int main()
{
#ifdef _WIN32
    _putenv_s("MIB_DISABLED_SERVICES", "auto_update,autofocus,trigger,syringe_pump,pulse_generator");
#else
    setenv("MIB_DISABLED_SERVICES", "auto_update,autofocus,trigger,syringe_pump,pulse_generator", 1);
#endif
    mib::test::Watchdog wd(150);
    mib::test::TempDir td("recording_capacity_readiness");
    const fs::path frames = td.path() / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 4, 96, 96), "write mock frames");

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    auto& coord = backend.experiment();
    coord.setApplicationIdentity("test-version", "test-build", "test-os");
    auto& proc = backend.processing();
    auto kernel = std::make_shared<mib::test::FaultKernel>();
    std::string kernelError;
    MIB_REQUIRE(proc.activateProcessingKernel(kernel, &kernelError), "fault kernel active: " + kernelError);
    {
        auto cfg = proc.getProcessingConfig();
        cfg.enable_target_group = false;
        cfg.auto_background_enabled = false;
        proc.setProcessingConfig(cfg);
    }
    proc.setFlushInterval(1000);
    proc.setInvalidFrameSamplingRate(200);
    proc.setRealtimeRoi(ProcessingService::Roi{0, 0, 96, 96});
    proc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
    proc.startRealtime(backend.getFrameStore());

    // The mock camera is the configured source (camera.source warns, as for any
    // simulated run); its frames come from a camera that reports its acquisition.
    camera::mock::MockCameraOptions opts;
    opts.folder = frames;
    opts.frameInterval = std::chrono::microseconds(2000);
    opts.loopFiles = true;
    backend.configureMockCamera(opts);

    std::atomic<double> diskMBps{150.0};
    std::atomic<int> probes{0};
    coord.setWriteRateProbe([&](const std::string&, uint64_t bytesPerFrame) {
        probes.fetch_add(1);
        backend::app::WriteRateMeasurement m;
        m.ok = diskMBps.load() > 0;
        m.bytesPerSecond = diskMBps.load() * 1e6;
        m.directory = td.path().string();
        m.note = m.ok ? "fake probe, " + std::to_string(bytesPerFrame) + " B frames" : "fake probe failed";
        return m;
    });

    auto useCamera = [&](int w, int h, double fps, double maxFps, bool reports = true) {
        backend.capture().stop();
        waitFor([&] { return !backend.capture().isRunning(); }, std::chrono::seconds(5));
        mib::test::AcquisitionReportCamera::Setup setup;
        setup.report = mib::test::mc2066Report(w, h, fps, maxFps);
        setup.reports = reports;
        setup.frameInterval = std::chrono::microseconds(5000); // the rate is reported, not delivered
        backend.capture().setCameraFactory(
            [setup]() { return std::make_unique<mib::test::AcquisitionReportCamera>(setup); });
        MIB_REQUIRE(backend.capture().requestStart() == backend::services::CaptureStartOutcome::Accepted,
                    "capture start accepted");
        MIB_REQUIRE(waitFor([&] {
                        backend::playback::Frame f;
                        return backend.capture().lifecycleSnapshot().cameraReady && backend.getFrameStore() &&
                               backend.getFrameStore()->getLatest(f) && f.width == static_cast<uint64_t>(w) &&
                               f.height == static_cast<uint64_t>(h);
                    },
                            std::chrono::seconds(10)),
                    "frames of the new window arrive");
    };
    int runs = 0;
    auto startAndStop = [&](const backend::app::ExperimentReadinessSnapshot& r, const std::string& out) {
        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = r.generation;
        const auto res = coord.start(req);
        MIB_REQUIRE(res.started(), "start: " + res.message);
        ++runs;
        const size_t frames = proc.getMaxBufferedFrames();
        const uint64_t bytes = proc.getMaxBufferedBytes();
        MIB_EXPECT(coord.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted, "stop accepted");
        MIB_REQUIRE(waitFor([&] { return coord.status().terminal; }, std::chrono::seconds(30)), "run finalizes");
        return std::make_pair(frames, bytes);
    };

    // ---- 512x96 at 5000 fps: supported; Start applies the plan --------------------------------------
    uint64_t gen512 = 0;
    {
        wd.mark("512x96");
        useCamera(512, 96, 5000, 23862);
        const auto out = (td.path() / "w512.h5").string();
        const auto r = coord.evaluateReadiness(out);
        dumpGates(r);
        MIB_EXPECT(statusOf(r, "camera.rate") == GateStatus::Pass, "5000 <= 23862 fps: " + textOf(r, "camera.rate"));
        MIB_EXPECT(statusOf(r, "storage.buffer") == GateStatus::Pass, "buffer: " + textOf(r, "storage.buffer"));
        MIB_EXPECT(statusOf(r, "storage.throughput") == GateStatus::Pass, "disk: " + textOf(r, "storage.throughput"));
        MIB_EXPECT(statusOf(r, "camera.window") == GateStatus::Pass, "no profile window: the script decides");
        MIB_EXPECT(r.ready, "512x96 at 5000 fps is ready");
        MIB_EXPECT(r.bufferPlanValid && r.bufferPlan.maxFrames == 10000 &&
                       r.bufferPlan.maxBytes == 10000ull * 98304 && r.bufferPlan.bytesPerFrame == 98304,
                   "2 s at 5000 fps of 98304-byte frames");
        MIB_EXPECT(r.bufferPlan.requestedFps == 5000.0, "rate from the camera's CycleMinimumPeriod");
        MIB_EXPECT(coord.evaluateReadiness(out).generation == r.generation,
                   "a stable state keeps its generation (the available RAM is not an input)");
        MIB_EXPECT(probes.load() == 1, "the destination was measured once");
        gen512 = r.generation;
        const auto applied = startAndStop(r, out);
        MIB_EXPECT(applied.first == 10000 && applied.second == 10000ull * 98304,
                   "Start applied both caps of the plan together");
    }
    // ---- 1216x208 at 5000 fps (the rig) on the HDD: supported -------------------------------------------
    {
        wd.mark("1216x208");
        useCamera(1216, 208, 5000, 8894);
        const auto out = (td.path() / "w1216.h5").string();
        const auto r = coord.evaluateReadiness(out);
        dumpGates(r);
        MIB_EXPECT(r.generation != gen512, "the window is a readiness input");
        MIB_EXPECT(statusOf(r, "camera.rate") == GateStatus::Pass && statusOf(r, "storage.throughput") == GateStatus::Pass,
                   "rig window passes rate and disk");
        MIB_EXPECT(r.bufferPlan.maxFrames == 10000 && r.bufferPlan.maxBytes == 10000ull * 505856,
                   "10000 frames of 505856 bytes (the 512 MiB cap held 1061)");
        MIB_EXPECT(probes.load() == 2, "a new frame size is measured again");
        const auto view = coord.recordingCapacity(out);
        MIB_EXPECT(view.known && view.inputs.plan.maxFrames == r.bufferPlan.maxFrames &&
                       view.capacity.throughput.status == GateStatus::Pass,
                   "the UI view shows the same plan and the cached destination rate");
        MIB_EXPECT(probes.load() == 2, "the UI view never measures");
        const auto viewNoPath = coord.recordingCapacity();
        MIB_EXPECT(viewNoPath.inputs.diskBytesPerSecond == 150e6, "without a path: the last measured destination");
        MIB_EXPECT(r.ready, "ready");
    }
    // ---- 1920x1080 at 5000 fps: above the camera maximum, Start blocked ---------------------------------
    {
        wd.mark("1920x1080");
        useCamera(1920, 1080, 5000, 1123);
        const auto out = (td.path() / "w1920.h5").string();
        const auto r = coord.evaluateReadiness(out);
        dumpGates(r);
        MIB_EXPECT(statusOf(r, "camera.rate") == GateStatus::Fail, "5000 > 1123 fps");
        MIB_EXPECT(textOf(r, "camera.rate").find("1123 fps") != std::string::npos, "names the maximum");
        MIB_EXPECT(!r.ready, "not ready");
        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = r.generation;
        const auto res = coord.start(req);
        MIB_EXPECT(res.outcome == ExperimentStartOutcome::NotReady && res.message.find("camera.rate") != std::string::npos,
                   "Start refused on camera.rate: " + res.message);
        MIB_EXPECT(!backend.hdf5().isFileOpen() && !fs::exists(out), "nothing opened");
    }
    // ---- rate change at the same window is an input -----------------------------------------------------
    {
        wd.mark("rate");
        useCamera(1216, 208, 5000, 8894);
        const auto out = (td.path() / "rate.h5").string();
        const auto at5000 = coord.evaluateReadiness(out);
        useCamera(1216, 208, 4000, 8894);
        const auto at4000 = coord.evaluateReadiness(out);
        MIB_EXPECT(at4000.generation != at5000.generation, "the requested rate is a readiness input");
        MIB_EXPECT(at4000.bufferPlan.maxFrames == 8000, "the buffer follows the rate (2 s at 4000 fps)");
    }
    // ---- a destination slower than the floor stream ------------------------------------------------------
    {
        wd.mark("slow disk");
        useCamera(1216, 208, 5000, 8894);
        diskMBps.store(10.0);
        coord.setWriteRateProbe(nullptr); // clears the cache ...
        coord.setWriteRateProbe([&](const std::string&, uint64_t) { // ... and installs the slow one
            probes.fetch_add(1);
            backend::app::WriteRateMeasurement m;
            m.ok = true;
            m.bytesPerSecond = diskMBps.load() * 1e6;
            m.directory = td.path().string();
            m.note = "fake slow probe";
            return m;
        });
        const auto out = (td.path() / "slow.h5").string();
        const auto r = coord.evaluateReadiness(out);
        dumpGates(r);
        MIB_EXPECT(statusOf(r, "storage.throughput") == GateStatus::Fail,
                   "12.6 MB/s floor > 10 MB/s: " + textOf(r, "storage.throughput"));
        MIB_EXPECT(!r.ready, "Start blocked by the destination");
        MIB_EXPECT(textOf(r, "storage.throughput").find("NVMe") != std::string::npos, "names the fix");
    }
    // ---- unknown destination rate and camera maximum: warnings only ----------------------------------------
    {
        wd.mark("unknown");
        diskMBps.store(0.0);
        std::atomic<int> failedProbes{0};
        coord.setWriteRateProbe([&](const std::string&, uint64_t) {
            failedProbes.fetch_add(1);
            backend::app::WriteRateMeasurement m;
            m.note = "probe failed";
            return m;
        });
        useCamera(1216, 208, 5000, 0);
        const auto out = (td.path() / "unknown.h5").string();
        const auto r = coord.evaluateReadiness(out);
        dumpGates(r);
        MIB_EXPECT(statusOf(r, "storage.throughput") == GateStatus::Warn, "unknown disk rate warns");
        MIB_EXPECT(statusOf(r, "camera.rate") == GateStatus::Warn, "unknown camera maximum warns");
        MIB_EXPECT(r.ready, "warnings do not block Start");
        (void)coord.evaluateReadiness(out);
        MIB_EXPECT(failedProbes.load() == 1, "a failed measurement is kept, not repeated on every evaluation");
    }
    // ---- a camera without a report: target_fps is the rate --------------------------------------------------
    {
        wd.mark("target_fps");
        diskMBps.store(150.0);
        coord.setWriteRateProbe([&](const std::string&, uint64_t) {
            backend::app::WriteRateMeasurement m;
            m.ok = true;
            m.bytesPerSecond = 150e6;
            m.directory = "fake";
            m.note = "fake";
            return m;
        });
        useCamera(512, 96, 5000, 23862, /*reports=*/false);
        auto noRate = coord.evaluateReadiness((td.path() / "norate.h5").string());
        MIB_EXPECT(statusOf(noRate, "camera.rate") == GateStatus::Warn && noRate.bufferPlan.requestedFps == 0.0,
                   "no report, no target_fps: rate unknown");
        MIB_EXPECT(noRate.bufferPlan.maxFrames == 4000, "unknown rate: the pre-#833 4000 frames");
        backend::app::AcquisitionProfile profile;
        profile.targetFps = 2500;
        backend.setAcquisitionProfile(profile);
        const auto r = coord.evaluateReadiness((td.path() / "norate.h5").string());
        MIB_EXPECT(r.generation != noRate.generation, "target_fps is a readiness input");
        MIB_EXPECT(r.bufferPlan.requestedFps == 2500.0 && r.bufferPlan.maxFrames == 5000,
                   "2 s at the profile's 2500 fps");
        MIB_EXPECT(textOf(r, "camera.rate").find("profile target_fps") != std::string::npos, "says where the rate came from");
        backend.setAcquisitionProfile({});
    }
    // ---- the override sizes the buffer -----------------------------------------------------------------------
    {
        wd.mark("override");
        useCamera(1216, 208, 5000, 8894);
        proc.setExperimentBufferOverrideBytes(512ULL * 1024 * 1024);
        const auto out = (td.path() / "override.h5").string();
        const auto r = coord.evaluateReadiness(out);
        dumpGates(r);
        MIB_EXPECT(statusOf(r, "storage.buffer") == GateStatus::Warn &&
                       textOf(r, "storage.buffer").find("0.21 s") != std::string::npos,
                   "the rig's 512 MiB holds 0.21 s: warn");
        MIB_EXPECT(r.ready, "the override still starts");
        const auto applied = startAndStop(r, out);
        MIB_EXPECT(applied.first == 1061 && applied.second == 1061ull * 505856, "1061 frames, the rig's cap");
        proc.setExperimentBufferOverrideBytes(0);
    }
    // ---- the Overview's planned window (the camera shows another one) -----------------------------------
    {
        wd.mark("planned");
        useCamera(512, 96, 5000, 23862);
        const auto other = coord.recordingCapacity({}, camera::common::CameraWindow{1216, 208, 368, 616});
        MIB_EXPECT(other.planned && other.known && other.inputs.windowWidth == 1216 && other.inputs.windowHeight == 208,
                   "the planned window is described, not the running 512x96");
        MIB_EXPECT(other.inputs.requestedFps == 0.0 && other.inputs.maxFps == 0.0,
                   "another window: the session's rates are not this window's");
        MIB_EXPECT(other.window.status == GateStatus::Pass && other.window.detail.find("planned") != std::string::npos,
                   "camera.window says the window is planned");
        const auto same = coord.recordingCapacity({}, camera::common::CameraWindow{512, 96, 704, 492});
        MIB_EXPECT(same.inputs.requestedFps == 5000.0 && same.inputs.maxFps == 23862.0,
                   "the running window's size: the session's rates apply");
    }
    MIB_EXPECT(runs == 2, "two runs started");
    backend.capture().stop();
    proc.stopRealtime();
    backend.shutdown();
    return mib::test::exitCode();
}
