// recording_capacity_gate_test (#833): the readiness rules that block Start
// outside the supported camera window / rate range (app/RecordingCapacity.h).
// Pure: inputs in, gates out. Windows and maxima are the EoSens 2.0MCX12
// (MC2066) table of issue #833; the destination is the rig's D: HDD (about
// 150 MB/s) unless a case says otherwise.
//  - camera.rate:        Fail above AcquisitionFrameRateMax, Warn without it or without a rate;
//  - storage.buffer:     Fail when one series does not fit an override or the minimum hold does
//                        not fit the RAM ceiling; Warn below 1 s if every frame were saved;
//  - storage.throughput: Fail when the floor stream (invalid sampling, no cells) exceeds a
//                        measured destination rate; Warn when the rate is unknown;
//  - camera.window:      the profile's window in the camera, refused/rejected/not in effect
//                        fail; a script override, an unnamed camera or a camera without
//                        window control warn.
// Every message names its numbers and the fix.

#include "backend/app/RecordingCapacity.h"

#include "support/assert.h"

#include <string>

using backend::app::CameraWindowApplyReport;
using backend::app::CameraWindowGateInputs;
using backend::app::GateStatus;
using backend::app::ProfileCameraWindow;
using backend::app::RecordingCapacityInputs;
using backend::app::cameraWindowGate;
using backend::app::evaluateRecordingCapacity;

namespace {
constexpr uint64_t GiB = 1024ULL * 1024 * 1024;
constexpr uint64_t MiB = 1024ULL * 1024;

RecordingCapacityInputs inputs(uint64_t w, uint64_t h, double fps, double maxFps, double diskMBps,
                               uint64_t availableRam = 16 * GiB, uint64_t overrideBytes = 0, size_t sampling = 200)
{
    RecordingCapacityInputs in;
    in.windowWidth = w;
    in.windowHeight = h;
    in.imagesPerFrame = 2;
    in.requestedFps = fps;
    in.requestedFpsSource = fps > 0 ? "camera CycleMinimumPeriod (RC)" : "";
    in.maxFps = maxFps;
    in.overrideBytes = overrideBytes;
    in.invalidSamplingRate = sampling;
    in.diskBytesPerSecond = diskMBps * 1e6;
    in.diskRateNote = diskMBps > 0 ? "measured" : "not measured yet";
    backend::processing::ExperimentBufferSizingInputs s;
    s.bytesPerFrame = backend::processing::savedBytesPerFrame(w, h, false, 1);
    s.requestedFps = fps;
    s.flushInterval = 1000;
    s.availableRamBytes = availableRam;
    s.overrideBytes = overrideBytes;
    in.plan = backend::processing::planExperimentBuffer(s);
    return in;
}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

ProfileCameraWindow profileWindow(int w, int h, int x, int y, const char* model = "MC2066")
{
    ProfileCameraWindow p;
    p.window = {w, h, x, y};
    p.cameraVendor = model[0] ? "SVS-Vistek GmbH" : "";
    p.cameraModel = model;
    return p;
}
} // namespace

int main()
{
    // ---- 512x96 and 1216x208 at 5000 fps on the HDD: supported ---------------------------------------
    {
        const auto c = evaluateRecordingCapacity(inputs(512, 96, 5000, 23862, 150));
        MIB_EXPECT(c.rate.status == GateStatus::Pass, "512x96: 5000 <= 23862 fps");
        MIB_EXPECT(c.buffer.status == GateStatus::Pass && has(c.buffer.detail, "2.00 s"), "512x96 buffer holds 2 s");
        MIB_EXPECT(c.throughput.status == GateStatus::Pass, "512x96 floor stream fits the HDD");
        const auto r = evaluateRecordingCapacity(inputs(1216, 208, 5000, 8894, 150));
        MIB_EXPECT(r.rate.status == GateStatus::Pass && has(r.rate.detail, "8894 fps"), "1216x208: camera max 8894");
        MIB_EXPECT(r.buffer.status == GateStatus::Pass && has(r.buffer.detail, "10000 frames"), "rig window: 10000 frames");
        MIB_EXPECT(r.throughput.status == GateStatus::Pass, "floor 25 fps x 505856 B = 12.6 MB/s < 150 MB/s");
        MIB_EXPECT(has(r.throughput.detail, "2529 MB/s") && has(r.throughput.detail, "5.9%"),
                   "every frame would be 2529 MB/s, so the HDD keeps up with 5.9% of frames: " + r.throughput.detail);
        MIB_EXPECT(r.everyFrameBytesPerSecond == 5000.0 * 505856 && r.floorFps == 25.0, "derived stream numbers");
    }
    // ---- 1920x1080 at 5000 fps: above the camera maximum ---------------------------------------------
    {
        const auto c = evaluateRecordingCapacity(inputs(1920, 1080, 5000, 1123, 150, 64 * GiB));
        MIB_EXPECT(c.rate.status == GateStatus::Fail, "5000 > 1123 fps blocks Start");
        MIB_EXPECT(has(c.rate.reason, "5000 fps") && has(c.rate.reason, "1123 fps") && has(c.rate.reason, "1920x1080"),
                   "reason names the rate, the maximum and the window: " + c.rate.reason);
        MIB_EXPECT(has(c.rate.remediation, "smaller camera window") && has(c.rate.remediation, "lower frame rate") &&
                       has(c.rate.remediation, "891 us"),
                   "remediation names the fix and the period: " + c.rate.remediation);
        const auto atMax = evaluateRecordingCapacity(inputs(1920, 1080, 1123, 1123, 150, 64 * GiB));
        MIB_EXPECT(atMax.rate.status == GateStatus::Pass, "exactly the maximum is allowed");
    }
    // ---- the camera does not report its maximum; no rate at all -------------------------------------------
    {
        const auto c = evaluateRecordingCapacity(inputs(1216, 208, 5000, 0, 150));
        MIB_EXPECT(c.rate.status == GateStatus::Warn && has(c.rate.reason, "AcquisitionFrameRateMax"),
                   "max unavailable warns");
        const auto n = evaluateRecordingCapacity(inputs(1216, 208, 0, 8894, 150));
        MIB_EXPECT(n.rate.status == GateStatus::Warn && has(n.rate.remediation, "target_fps"), "no rate warns");
        MIB_EXPECT(n.throughput.status == GateStatus::Warn, "no rate: the stream cannot be estimated");
        MIB_EXPECT(n.buffer.status == GateStatus::Pass && has(n.buffer.detail, "frame rate unknown"),
                   "no rate: the unknown-rate cap, said in the detail");
    }
    // ---- floor stream above the measured destination rate ------------------------------------------------
    {
        const auto slow = evaluateRecordingCapacity(inputs(1216, 208, 5000, 8894, 10));
        MIB_EXPECT(slow.throughput.status == GateStatus::Fail, "12.6 MB/s floor > 10 MB/s destination blocks Start");
        MIB_EXPECT(has(slow.throughput.reason, "12.6 MB/s") && has(slow.throughput.reason, "10.0 MB/s") &&
                       has(slow.throughput.reason, "1216x208"),
                   "reason names both rates and the window: " + slow.throughput.reason);
        MIB_EXPECT(has(slow.throughput.remediation, "NVMe"), "remediation names a faster destination");
        const auto everyInvalid = evaluateRecordingCapacity(inputs(1216, 208, 5000, 8894, 150, 16 * GiB, 0, 1));
        MIB_EXPECT(everyInvalid.throughput.status == GateStatus::Fail,
                   "saving every invalid frame (2529 MB/s) cannot run on the HDD");
    }
    // ---- unknown destination rate --------------------------------------------------------------------------
    {
        const auto c = evaluateRecordingCapacity(inputs(1216, 208, 5000, 8894, 0));
        MIB_EXPECT(c.throughput.status == GateStatus::Warn && has(c.throughput.reason, "unknown"),
                   "unknown disk rate warns, never passes");
        MIB_EXPECT(has(c.throughput.reason, "12.6 MB/s"), "still names the floor stream");
    }
    // ---- RAM ceiling ------------------------------------------------------------------------------------------
    {
        const auto cut = evaluateRecordingCapacity(inputs(1216, 208, 5000, 8894, 150, 4 * GiB));
        MIB_EXPECT(cut.buffer.status == GateStatus::Warn && has(cut.buffer.reason, "0.47 s") &&
                       has(cut.buffer.reason, "75% of the available memory"),
                   "a buffer cut below 1 s warns: " + cut.buffer.reason);
        const auto none = evaluateRecordingCapacity(inputs(1216, 208, 5000, 8894, 150, 2 * GiB));
        MIB_EXPECT(none.buffer.status == GateStatus::Fail && has(none.buffer.reason, "2000 frames") &&
                       has(none.buffer.remediation, "smaller camera window"),
                   "the minimum hold does not fit: Start blocked: " + none.buffer.reason);
        const auto big = evaluateRecordingCapacity(inputs(1920, 1080, 1000, 1123, 150, 16 * GiB));
        MIB_EXPECT(big.buffer.status == GateStatus::Fail, "1920x1080 floor + queue (23 GiB) does not fit 12 GiB");
    }
    // ---- manual override ---------------------------------------------------------------------------------------
    {
        const auto rig = evaluateRecordingCapacity(inputs(1216, 208, 5000, 8894, 150, 16 * GiB, 512 * MiB));
        MIB_EXPECT(rig.buffer.status == GateStatus::Warn && has(rig.buffer.reason, "0.21 s") &&
                       has(rig.buffer.reason, "experiment_buffer_max_mb"),
                   "the rig's 512 MiB holds 0.21 s and says the override set it: " + rig.buffer.reason);
        const auto tiny = evaluateRecordingCapacity(inputs(1216, 208, 5000, 8894, 150, 16 * GiB, 1));
        MIB_EXPECT(tiny.buffer.status == GateStatus::Fail && has(tiny.buffer.reason, "one full-frame"),
                   "an override below one series blocks Start");
        const auto roomy = evaluateRecordingCapacity(inputs(512, 96, 5000, 23862, 150, 16 * GiB, 2 * GiB));
        MIB_EXPECT(roomy.buffer.status == GateStatus::Pass && has(roomy.buffer.detail, "override"),
                   "a large override passes and is labelled");
    }

    // ---- camera.window -------------------------------------------------------------------------------------------
    {
        CameraWindowGateInputs in;
        in.cameraAppliesWindows = true;
        in.cameraName = "egrabber";
        in.sessionWindowKnown = true;
        in.sessionWindow = {512, 96, 704, 500};
        MIB_EXPECT(cameraWindowGate(in).status == GateStatus::Pass && has(cameraWindowGate(in).detail, "no camera window"),
                   "no profile window: the script decides, Pass");

        in.profile = profileWindow(1216, 208, 368, 616);
        auto g = cameraWindowGate(in);
        MIB_EXPECT(g.status == GateStatus::Fail && has(g.reason, "not in effect") && has(g.reason, "512x96"),
                   "profile window not in the camera: Start blocked: " + g.reason);
        MIB_EXPECT(has(g.remediation, "restart Live View"), "says how to apply it");

        CameraWindowApplyReport refused;
        refused.sequence = 1;
        refused.requested = true;
        refused.profile = *in.profile;
        refused.connectedVendor = "Basler";
        refused.connectedModel = "acA1920";
        refused.refusal = backend::app::cameraModelMismatch(*in.profile, "Basler", "acA1920");
        in.apply = refused;
        g = cameraWindowGate(in);
        MIB_EXPECT(g.status == GateStatus::Fail && has(g.reason, "MC2066") && has(g.reason, "acA1920"),
                   "a window for another camera is refused, not clipped: " + g.reason);

        CameraWindowApplyReport rejected = refused;
        rejected.refusal.clear();
        rejected.error = "GenTL: value out of range";
        in.apply = rejected;
        g = cameraWindowGate(in);
        MIB_EXPECT(g.status == GateStatus::Fail && has(g.reason, "out of range"), "a rejected window blocks Start");

        CameraWindowApplyReport applied;
        applied.sequence = 2;
        applied.requested = true;
        applied.applied = true;
        applied.profile = *in.profile;
        applied.scriptWindow = {512, 96, 704, 500};
        applied.scriptWindowKnown = true;
        in.apply = applied;
        in.sessionWindow = {1216, 208, 368, 616};
        g = cameraWindowGate(in);
        MIB_EXPECT(g.status == GateStatus::Warn && has(g.reason, "512x96 at (704, 500)") &&
                       has(g.reason, "1216x208 at (368, 616)") && has(g.reason, "profile wins"),
                   "a script the profile overrode is said, with both windows: " + g.reason);

        applied.scriptWindow = in.profile->window;
        in.apply = applied;
        g = cameraWindowGate(in);
        MIB_EXPECT(g.status == GateStatus::Pass && has(g.detail, "SVS-Vistek GmbH MC2066"), "agreeing: Pass");

        in.profile = profileWindow(1216, 208, 368, 616, "");
        in.apply.reset();
        g = cameraWindowGate(in);
        MIB_EXPECT(g.status == GateStatus::Warn && has(g.reason, "does not name the camera"),
                   "a window without its camera is flagged");

        // A report for an older profile window is not this window's.
        in.profile = profileWindow(1216, 208, 368, 616);
        in.apply = refused;
        in.apply->profile = profileWindow(512, 96, 704, 500);
        g = cameraWindowGate(in);
        MIB_EXPECT(g.status == GateStatus::Pass, "a stale refusal for another window is ignored");

        CameraWindowGateInputs mock;
        mock.profile = profileWindow(1216, 208, 368, 616);
        mock.cameraName = "mock";
        mock.frameKnown = true;
        mock.frameWidth = 512;
        mock.frameHeight = 96;
        g = cameraWindowGate(mock);
        MIB_EXPECT(g.status == GateStatus::Warn && has(g.reason, "mock"), "the mock camera cannot apply a window");
        mock.frameWidth = 1216;
        mock.frameHeight = 208;
        MIB_EXPECT(cameraWindowGate(mock).status == GateStatus::Pass, "frames that match the window pass");

        CameraWindowGateInputs stopped;
        stopped.profile = profileWindow(1216, 208, 368, 616);
        stopped.cameraAppliesWindows = true;
        MIB_EXPECT(cameraWindowGate(stopped).status == GateStatus::Unavailable, "no window known yet: not Pass");
    }
    return mib::test::exitCode();
}
