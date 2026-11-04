#include "backend/app/RecordingCapacity.h"

#include <cmath>
#include <cstdio>

namespace backend::app {
namespace {

using processing::ExperimentBufferSizing;
using processing::formatBufferBytes;

ReadinessGate makeGate(const char* id, GateStatus status, std::string reason = {}, std::string remediation = {},
                       std::string detail = {})
{
    ReadinessGate g;
    g.id = id;
    g.status = status;
    g.reason = std::move(reason);
    g.remediation = std::move(remediation);
    g.detail = std::move(detail);
    return g;
}

std::string fps(double value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.0f fps", value);
    return text;
}

std::string seconds(double value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.2f s", value);
    return text;
}

std::string size(uint64_t w, uint64_t h)
{
    return std::to_string(w) + "x" + std::to_string(h);
}

bool sameProfileWindow(const ProfileCameraWindow& a, const ProfileCameraWindow& b)
{
    return a.window == b.window && a.cameraModel == b.cameraModel && a.cameraVendor == b.cameraVendor;
}

constexpr const char* kFixWindowOrRate =
    "use a smaller camera window or a lower frame rate";

} // namespace

std::string formatRate(double bytesPerSecond)
{
    char text[48];
    const double mb = bytesPerSecond / 1e6;
    std::snprintf(text, sizeof text, mb >= 100 ? "%.0f MB/s" : "%.1f MB/s", mb);
    return text;
}

ReadinessGate cameraWindowGate(const CameraWindowGateInputs& in)
{
    std::string running;
    if (in.sessionWindowKnown)
        running = describeWindow(in.sessionWindow);
    else if (in.frameKnown)
        running = size(in.frameWidth, in.frameHeight) + " frames";

    if (!in.profile) {
        return makeGate("camera.window", GateStatus::Pass, {}, {},
                        (running.empty() ? std::string("camera window unknown") : running) +
                            "; the profile has no camera window, so the camera script sets it "
                            "(Save Profile records it in the profile)");
    }
    const auto& profile = *in.profile;
    const std::string wanted = describeWindow(profile.window);
    const bool frameMatches = in.frameKnown && in.frameWidth == static_cast<uint64_t>(profile.window.width) &&
                              in.frameHeight == static_cast<uint64_t>(profile.window.height);

    if (!in.cameraAppliesWindows) {
        if (in.frameKnown && !frameMatches)
            return makeGate("camera.window", GateStatus::Warn,
                            "the " + (in.cameraName.empty() ? std::string("selected") : in.cameraName) +
                                " camera does not apply camera windows: frames are " +
                                size(in.frameWidth, in.frameHeight) + ", the profile's window is " + wanted,
                            "expected for the simulated camera; on the instrument the eGrabber camera applies it");
        return makeGate("camera.window", GateStatus::Pass, {}, {}, wanted + " (frames match the profile's window)");
    }

    // The last apply, when it was for this window (a later profile change makes it stale).
    const CameraWindowApplyReport* apply =
        in.apply && in.apply->requested && sameProfileWindow(in.apply->profile, profile) ? &*in.apply : nullptr;
    if (apply && !apply->refusal.empty())
        return makeGate("camera.window", GateStatus::Fail, apply->refusal + ", so it was not applied",
                        "load a profile made for this camera, or set the window on this camera and save the "
                        "profile again",
                        wanted);
    if (apply && !apply->error.empty())
        return makeGate("camera.window", GateStatus::Fail,
                        "the camera rejected the profile's camera window " + wanted + ": " + apply->error,
                        "choose a window inside the sensor, in the camera's size and offset steps", wanted);

    const bool inEffect = in.sessionWindowKnown ? in.sessionWindow == profile.window : frameMatches;
    if (running.empty())
        return makeGate("camera.window", GateStatus::Unavailable, "the camera has not reported its window yet",
                        "start Live View", wanted);
    if (!inEffect)
        return makeGate("camera.window", GateStatus::Fail,
                        "the profile's camera window " + wanted + " is not in effect: the camera runs " + running,
                        "apply the camera script (switch to the Experiment tab, or Apply in the camera script "
                        "editor), then restart Live View",
                        wanted);

    if (apply && apply->applied && apply->scriptWindowKnown && apply->scriptWindow != profile.window)
        return makeGate("camera.window", GateStatus::Warn,
                        "the camera script sets " + describeWindow(apply->scriptWindow) +
                            "; the profile's camera window " + wanted + " is applied instead (the profile wins)",
                        "save the profile again so its camera script matches, or remove the Width/Height/Offset "
                        "lines from the script",
                        wanted);
    if (!profile.namesCamera())
        return makeGate("camera.window", GateStatus::Warn,
                        "the profile's camera window " + wanted + " does not name the camera it was made for",
                        "save the profile again with the camera connected (Save Profile records the camera)",
                        wanted);
    return makeGate("camera.window", GateStatus::Pass, {}, {},
                    wanted + " from the profile, for " + describeCamera(profile.cameraVendor, profile.cameraModel));
}

RecordingCapacity evaluateRecordingCapacity(const RecordingCapacityInputs& in)
{
    using S = ExperimentBufferSizing;
    RecordingCapacity c;
    const auto& plan = in.plan;
    c.bytesPerFrame = plan.bytesPerFrame;
    const bool windowKnown = in.windowWidth > 0 && in.windowHeight > 0;
    // `shape` in reasons, `window` (with what a saved frame takes) in details.
    const std::string shape = windowKnown ? size(in.windowWidth, in.windowHeight) : std::string("unknown");
    const std::string window =
        windowKnown ? shape + " (" + std::to_string(in.imagesPerFrame) + " images per saved frame, " +
                          formatBufferBytes(plan.bytesPerFrame) + ")"
                    : std::string("unknown window");
    const bool rateKnown = in.requestedFps > 0;
    if (rateKnown && plan.bytesPerFrame > 0) {
        c.everyFrameBytesPerSecond = in.requestedFps * static_cast<double>(plan.bytesPerFrame);
        c.floorFps = in.requestedFps / static_cast<double>(in.invalidSamplingRate ? in.invalidSamplingRate : 1);
        c.floorBytesPerSecond = c.floorFps * static_cast<double>(plan.bytesPerFrame);
    }

    // --- camera.rate ------------------------------------------------------
    const std::string requested =
        rateKnown ? fps(in.requestedFps) + (in.requestedFpsSource.empty() ? std::string()
                                                                           : " (" + in.requestedFpsSource + ")")
                  : std::string();
    if (!rateKnown) {
        c.rate = makeGate("camera.rate", GateStatus::Warn,
                          "the requested frame rate is unknown: the camera does not report it and the profile "
                          "has no target_fps",
                          "set target_fps in the profile",
                          in.maxFps > 0 ? "camera max " + fps(in.maxFps) + " at " + window : std::string());
    } else if (in.maxFps > 0 && in.requestedFps > in.maxFps * 1.0001) {
        char period[64];
        std::snprintf(period, sizeof period, "%.0f us", std::ceil(1e6 / in.maxFps));
        c.rate = makeGate("camera.rate", GateStatus::Fail,
                          "the camera is driven at " + requested + " but allows at most " + fps(in.maxFps) +
                              " at the " + shape + " window",
                          std::string(kFixWindowOrRate) + " (at this window: at most " + fps(in.maxFps) +
                              ", a cycle period of at least " + period + ")",
                          requested + "; camera max " + fps(in.maxFps));
    } else if (in.maxFps <= 0) {
        c.rate = makeGate("camera.rate", GateStatus::Warn,
                          "this camera does not report its maximum frame rate for the window "
                          "(AcquisitionFrameRateMax), so " + requested + " cannot be checked",
                          "check the rate against the camera's specification for the " + shape + " window",
                          requested);
    } else {
        c.rate = makeGate("camera.rate", GateStatus::Pass, {}, {},
                          requested + "; camera max " + fps(in.maxFps) + " at " + window);
    }

    // --- storage.buffer -----------------------------------------------------
    const std::string planText = plan.summary() + " (" + processing::toString(plan.source) + ")";
    if (windowKnown && plan.bytesPerFrame == 0) {
        c.buffer = makeGate("storage.buffer", GateStatus::Fail,
                            "the camera window " + size(in.windowWidth, in.windowHeight) +
                                " is too large to size the recording buffer",
                            "use a smaller camera window");
    } else if (!windowKnown) {
        c.buffer = makeGate("storage.buffer", GateStatus::Unavailable,
                            "the camera window is not known yet, so the buffer cannot be sized",
                            "start Live View and wait for the first frame", planText);
    } else if (in.overrideBytes > 0 && in.overrideBytes < plan.bytesPerFrame) {
        c.buffer = makeGate("storage.buffer", GateStatus::Fail,
                            "one full-frame image/mask series (" + formatBufferBytes(plan.bytesPerFrame) +
                                ") cannot fit the recording byte budget experiment_buffer_max_mb (" +
                                formatBufferBytes(in.overrideBytes) + ")",
                            "remove experiment_buffer_max_mb (the buffer is then sized automatically) or "
                            "increase it; or reduce the frame size or series length");
    } else if (!plan.minimumFitsRam) {
        c.buffer = makeGate(
            "storage.buffer", GateStatus::Fail,
            "the buffer the " + shape + " window needs (" + std::to_string(plan.floorFrames) +
                " frames, 2 x the flush interval: " + formatBufferBytes(plan.minimumPeakBytes) +
                " with the write queue) does not fit 75% of the available memory (" +
                formatBufferBytes(plan.ramCeilingBytes) + ")",
            "use a smaller camera window or a smaller buffer_threshold, or close other programs to free memory",
            planText);
    } else if (rateKnown && plan.seconds + 1e-9 < S::kWarnBacklogSeconds) {
        char target[32];
        std::snprintf(target, sizeof target, "%.0f s", S::kTargetBacklogSeconds);
        c.buffer = makeGate("storage.buffer", GateStatus::Warn,
                            "the buffer holds " + seconds(plan.seconds) + " at " + fps(in.requestedFps) +
                                " if every frame were saved (" + plan.summary() + "; target " + target +
                                (plan.clampedByRam ? ", cut to 75% of the available memory" : "") +
                                (plan.source == processing::ExperimentBufferSource::Override
                                     ? ", set by experiment_buffer_max_mb"
                                     : "") +
                                ")",
                            std::string(kFixWindowOrRate) +
                                (plan.source == processing::ExperimentBufferSource::Override
                                     ? ", free memory, or remove experiment_buffer_max_mb"
                                     : ", or free memory"),
                            planText);
    } else if (plan.source == processing::ExperimentBufferSource::Override && plan.maxFrames < plan.flushInterval) {
        c.buffer = makeGate("storage.buffer", GateStatus::Warn,
                            "byte-pressure flushing will run before the configured frame threshold "
                            "(experiment_buffer_max_mb holds " + std::to_string(plan.maxFrames) +
                                " frames, buffer_threshold is " + std::to_string(plan.flushInterval) + ")",
                            "leave capacity headroom; runtime overflow will fail the experiment explicitly",
                            planText);
    } else {
        c.buffer = makeGate("storage.buffer", GateStatus::Pass, {}, {}, planText);
    }

    // --- storage.throughput -------------------------------------------------
    if (!rateKnown || plan.bytesPerFrame == 0) {
        c.throughput = makeGate("storage.throughput", GateStatus::Warn,
                                "the save stream cannot be estimated without the frame rate and the window",
                                "set target_fps in the profile; start Live View",
                                in.diskBytesPerSecond > 0 ? "destination " + formatRate(in.diskBytesPerSecond)
                                                          : in.diskRateNote);
    } else {
        char sampling[96];
        std::snprintf(sampling, sizeof sampling, "%.0f fps (every %zu%s invalid frame at %.0f fps)", c.floorFps,
                      in.invalidSamplingRate,
                      in.invalidSamplingRate == 1 ? "st" : in.invalidSamplingRate == 2 ? "nd" : "th",
                      in.requestedFps);
        const std::string floor = formatRate(c.floorBytesPerSecond) + ": " + sampling;
        const std::string every = formatRate(c.everyFrameBytesPerSecond);
        if (in.diskBytesPerSecond <= 0) {
            c.throughput = makeGate("storage.throughput", GateStatus::Warn,
                                    "the destination's sustained write rate is unknown" +
                                        (in.diskRateNote.empty() ? std::string() : " (" + in.diskRateNote + ")") +
                                        "; a run with no valid cells saves " + floor +
                                        ", every frame would be " + every,
                                    "choose the destination at Start: its write rate is measured then",
                                    "floor " + floor);
        } else if (c.floorBytesPerSecond > in.diskBytesPerSecond) {
            c.throughput = makeGate("storage.throughput", GateStatus::Fail,
                                    "even a run with no valid cells saves " + floor + " of " + shape +
                                        " frames; the destination sustains only " +
                                        formatRate(in.diskBytesPerSecond),
                                    "save to a faster destination such as an NVMe drive, or " +
                                        std::string(kFixWindowOrRate),
                                    in.diskRateNote);
        } else {
            char share[48];
            std::snprintf(share, sizeof share, "%.1f%%",
                          100.0 * std::min(1.0, in.diskBytesPerSecond / c.everyFrameBytesPerSecond));
            c.throughput = makeGate("storage.throughput", GateStatus::Pass, {}, {},
                                    "destination " + formatRate(in.diskBytesPerSecond) + " (" + in.diskRateNote +
                                        "); floor " + floor + "; every frame " + every +
                                        ", so the disk keeps up while at most " + share +
                                        " of frames are saved");
        }
    }
    return c;
}

} // namespace backend::app
