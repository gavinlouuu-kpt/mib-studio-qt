// timestamp_telemetry_test (issue #368)
//
//  - TimestampValue: MindVision 0.1 ms ticks and EGrabber µs convert to ns
//    only through the checked boundary; cross-domain and cross-session
//    subtraction is rejected; overflow is rejected; counter wrap is detected.
//  - Adapter descriptors: MindVision (device ticks, native 10 kHz, capture),
//    mock (host steady ns, synthetic), undeclared default (Unsupported).
//  - CaptureService telemetry: each metric carries its own validity; a
//    backend that exposes no queue telemetry yields Unsupported (never a
//    zero); a valid zero loss counter is distinguishable from unavailable;
//    Stale is derived from freshness; a restart resets every metric to
//    Unavailable and re-tags the session generation.
//  - HDF5: descriptor + telemetry round-trip; a legacy file reads back as
//    Unsupported with the documented legacy interpretation, never rewritten.

#include "backend/camera/common/TimestampValue.h"
#include "backend/camera/mindvision/MindVisionCamera.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"
#include "backend/services/TelemetrySample.h"

#include "support/assert.h"
#include "support/fake_lifecycle_camera.h"
#include "support/fake_mindvision_sdk.h"
#include "support/queue_camera.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

using namespace camera::common;
using backend::services::AcquisitionTelemetrySnapshot;
using backend::services::CaptureLifecycleState;
using backend::services::CaptureService;
using backend::services::MetricValidity;

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

TimestampDescriptor mindVisionDescriptor()
{
    TimestampDescriptor d;
    d.domain = ClockDomain::DeviceTicks;
    d.ticksPerSecond = 10'000; // raw 0.1 ms ticks
    d.semantic = TimestampSemantic::DeviceCapture;
    d.validity = TimestampValidity::Valid;
    d.counterBits = 32;
    d.sessionGeneration = 1;
    return d;
}
} // namespace

int main()
{
    mib::test::Watchdog wd(40);

    // ---- 1. Checked conversions ----------------------------------------------
    {
        wd.mark("conversions");
        TimestampValue mv{12345, mindVisionDescriptor()};
        auto ns = mv.toNanoseconds();
        MIB_REQUIRE(ns.has_value(), "mindvision ticks convert");
        MIB_EXPECT(*ns == 12345ULL * 100'000ULL, "0.1 ms ticks -> ns");

        TimestampDescriptor eg;
        eg.domain = ClockDomain::HostMonotonicUs;
        eg.ticksPerSecond = 1'000'000;
        eg.semantic = TimestampSemantic::TransportReceipt;
        eg.validity = TimestampValidity::Valid;
        TimestampValue e{5'000, eg};
        MIB_EXPECT(e.toNanoseconds().value_or(0) == 5'000'000ULL, "µs -> ns");

        TimestampValue undeclared{99, TimestampDescriptor{}};
        MIB_EXPECT(!undeclared.toNanoseconds().has_value(), "undeclared unit never converts");

        TimestampValue overflow{UINT64_MAX / 10, mindVisionDescriptor()};
        MIB_EXPECT(!overflow.toNanoseconds().has_value(), "overflow rejected");

        // Same domain + session: difference allowed.
        TimestampValue a{1000, mindVisionDescriptor()}, b{400, mindVisionDescriptor()};
        auto d = differenceNs(a, b);
        MIB_REQUIRE(d.has_value(), "same-session device diff allowed");
        MIB_EXPECT(*d == 600LL * 100'000LL, "difference in ns");
        MIB_EXPECT(differenceNs(b, a).value_or(0) == -600LL * 100'000LL, "signed");
        // Different sessions of a device counter: rejected.
        TimestampValue c{1000, mindVisionDescriptor()};
        c.descriptor.sessionGeneration = 2;
        MIB_EXPECT(!differenceNs(a, c).has_value(), "cross-session device ticks rejected");
        // Cross domain: rejected.
        MIB_EXPECT(!differenceNs(a, e).has_value(), "cross-domain subtraction rejected");
        // Host monotonic across sessions: allowed (one host clock).
        TimestampValue e2{7'000, eg};
        e2.descriptor.sessionGeneration = 9;
        MIB_EXPECT(differenceNs(e2, e).value_or(0) == 2'000'000LL, "host clock comparable across sessions");

        MIB_EXPECT(detectCounterWrap(0xFFFFFFF0ULL, 5, 32), "32-bit wrap detected");
        MIB_EXPECT(!detectCounterWrap(100, 90, 32), "small decrease is not a wrap");
        MIB_EXPECT(detectCounterWrap(100, 90, 0), "unknown width: any decrease flagged");
        MIB_EXPECT(!describe(mindVisionDescriptor()).empty(), "describe");
        MIB_EXPECT(std::string(legacyTimestampInterpretation()).find("MindVision") != std::string::npos,
                   "legacy interpretation documented");
    }

    // ---- 2. Adapter descriptors ---------------------------------------------
    {
        wd.mark("adapters");
        mib::test::FakeMindVisionSdk sdk;
        sdk.frames.push_back(mib::test::FakeMindVisionSdk::frame(512, 96, 1, /*ts=*/4242));
        MindVisionCamera cam(0, {}, sdk.ops());
        const auto d = cam.timestampDescriptor();
        MIB_EXPECT(d.domain == ClockDomain::DeviceTicks && d.ticksPerSecond == 1'000'000'000ULL &&
                       d.nativeTicksPerSecond == 10'000 && d.semantic == TimestampSemantic::DeviceCapture &&
                       d.validity == TimestampValidity::Valid,
                   "mindvision descriptor");
        MIB_REQUIRE(cam.start(), "start");
        Frame f;
        MIB_REQUIRE(cam.grabFrame(f), "grab");
        MIB_EXPECT(f.rawDeviceTicks == 4242 && f.timestamp == 4242ULL * 100'000ULL,
                   "raw ticks preserved alongside the normalized value");
        cam.stop();

        camera::mock::MockCameraOptions opts;
        camera::mock::MockCamera mock(opts);
        const auto md = mock.timestampDescriptor();
        MIB_EXPECT(md.domain == ClockDomain::HostSteadyNs && md.semantic == TimestampSemantic::Synthetic,
                   "mock descriptor");

        mib::test::FakeLifecycleCamera::Observations obs;
        mib::test::FakeLifecycleCamera undeclared({}, &obs);
        MIB_EXPECT(undeclared.timestampDescriptor().validity == TimestampValidity::Unsupported,
                   "undeclared adapter is Unsupported, not assumed ns");
    }

    // ---- 3. CaptureService per-metric validity ------------------------------
    {
        wd.mark("telemetry unsupported backend");
        mib::test::FakeLifecycleCamera::Observations obs;
        CaptureService cap;
        mib::test::FakeLifecycleCamera::Script script;
        script.produceInterval = std::chrono::microseconds(200);
        cap.setCameraFactory([&]() {
            obs.destroyed = false;
            return std::unique_ptr<ICamera>(new mib::test::FakeLifecycleCamera(script, &obs));
        });
        const auto idle = cap.telemetrySnapshot();
        MIB_EXPECT(!idle.sessionActive && idle.transportLostFrames.validity == MetricValidity::Unavailable &&
                       idle.captureFrameRate.validity == MetricValidity::Unavailable,
                   "before any session everything is Unavailable");
        MIB_REQUIRE(cap.start(), "start");
        MIB_REQUIRE(cap.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5)) ==
                        CaptureLifecycleState::Running, "running");
        // The stats poll runs 1 s after start; wait for it.
        MIB_REQUIRE(waitFor([&] { return cap.telemetrySnapshot().transportLostFrames.validity != MetricValidity::Unavailable; },
                            std::chrono::seconds(5)),
                    "first stats poll landed");
        const auto t = cap.telemetrySnapshot();
        MIB_EXPECT(t.sessionActive && t.sessionGeneration == 1, "session tagged");
        MIB_EXPECT(t.framesDelivered.validity == MetricValidity::Valid && t.framesDelivered.value > 0, "frames valid");
        MIB_EXPECT(t.transportLostFrames.validity == MetricValidity::Unsupported, "no queue telemetry -> Unsupported, not 0");
        MIB_EXPECT(t.sdkCompletedQueueDepth.validity == MetricValidity::Unsupported, "queue depth Unsupported");
        MIB_EXPECT(t.captureFrameRate.validity == MetricValidity::Valid, "pollStats supported -> fps Valid");
        MIB_EXPECT(t.frameAgeUs.validity != MetricValidity::Valid, "no host-comparable stamps -> frame age not Valid");
        MIB_EXPECT(!t.transportLostFrames.hasValue(), "unsupported metric has no value");
        MIB_EXPECT(t.timestampDescriptor.sessionGeneration == 1 &&
                       t.timestampDescriptor.validity == TimestampValidity::Unsupported,
                   "descriptor tagged with session");
        cap.stop();
        const auto after = cap.telemetrySnapshot();
        MIB_EXPECT(!after.sessionActive, "inactive after stop");
        MIB_EXPECT(after.timestampDescriptor.validity == TimestampValidity::Unavailable, "descriptor cleared on release");
    }
    {
        wd.mark("telemetry valid zero vs stale + restart");
        CaptureService cap;
        cap.setCameraFactory([]() {
            mib::test::QueueBackedTestCamera::Options o;
            o.produceInterval = std::chrono::microseconds(500);
            return std::unique_ptr<ICamera>(new mib::test::QueueBackedTestCamera(o));
        });
        MIB_REQUIRE(cap.start(), "start");
        MIB_REQUIRE(waitFor([&] { return cap.telemetrySnapshot().transportLostFrames.validity == MetricValidity::Valid; },
                            std::chrono::seconds(5)),
                    "queue telemetry valid");
        const auto t = cap.telemetrySnapshot();
        MIB_EXPECT(t.transportLostFrames.validity == MetricValidity::Valid && t.transportLostFrames.value == 0,
                   "a measured zero loss is Valid — distinguishable from Unsupported");
        MIB_EXPECT(t.frameAgeUs.validity == MetricValidity::Valid, "host-comparable stamps -> frame age Valid");
        MIB_EXPECT(t.intentionallyDiscardedFrames.validity == MetricValidity::Valid, "discards valid");
        // Freshness: with a 1 µs window the 1 s-old poll is Stale.
        const auto stale = cap.telemetrySnapshot(/*freshnessWindowUs=*/1);
        MIB_EXPECT(stale.transportLostFrames.validity == MetricValidity::Stale && stale.transportLostFrames.hasValue(),
                   "old sample reported Stale (value retained, distinct from current)");
        const uint64_t gen1 = t.sessionGeneration;
        cap.stop();
        MIB_REQUIRE(cap.start(), "restart");
        const auto fresh = cap.telemetrySnapshot();
        MIB_EXPECT(fresh.sessionGeneration == gen1 + 1, "new session generation");
        MIB_EXPECT(fresh.transportLostFrames.validity == MetricValidity::Unavailable &&
                       fresh.captureFrameRate.validity == MetricValidity::Unavailable,
                   "restart resets every metric to Unavailable (no stale carry-over)");
        MIB_EXPECT(fresh.framesDelivered.validity == MetricValidity::Unavailable ||
                       fresh.framesDelivered.sessionGeneration == gen1 + 1,
                   "frames metric belongs to the new session");
        cap.stop();
    }

    // ---- 4. HDF5 provenance round trip + legacy ------------------------------
    {
        wd.mark("hdf5");
        mib::test::TempDir td("timestamp_telemetry");
        const std::string path = (td.path() / "prov.h5").string();
        AcquisitionTelemetrySnapshot t;
        t.sessionGeneration = 5;
        t.transportLostFrames.value = 0;
        t.transportLostFrames.validity = MetricValidity::Valid;
        t.transportLostFrames.sampleHostTimeUs = 123456;
        t.sdkCompletedQueueDepth.validity = MetricValidity::Unsupported;
        t.frameAgeUs.value = 77;
        t.frameAgeUs.validity = MetricValidity::Stale;
        auto d = mindVisionDescriptor();
        d.ticksPerSecond = 1'000'000'000ULL;
        d.nativeTicksPerSecond = 10'000;
        d.sessionGeneration = 5;
        {
            backend::services::Hdf5Service w;
            MIB_REQUIRE(w.openFile(path), "open");
            MIB_REQUIRE(w.initializeRecordingDatasets(), "init");
            MIB_REQUIRE(w.writeRecordingInfo(1, 2, 0, 0), "info");
            MIB_REQUIRE(w.writeAcquisitionProvenance(d, t), "write provenance");
            w.closeFile();
        }
        {
            backend::services::Hdf5Service r;
            MIB_REQUIRE(r.loadFile(path), "reload");
            TimestampDescriptor rd;
            AcquisitionTelemetrySnapshot rt;
            MIB_REQUIRE(r.readAcquisitionProvenance(rd, rt), "read provenance");
            MIB_EXPECT(rd.domain == d.domain && rd.ticksPerSecond == d.ticksPerSecond &&
                           rd.nativeTicksPerSecond == d.nativeTicksPerSecond && rd.semantic == d.semantic &&
                           rd.validity == d.validity && rd.counterBits == d.counterBits &&
                           rd.sessionGeneration == 5,
                       "descriptor round-trips");
            MIB_EXPECT(rt.transportLostFrames.validity == MetricValidity::Valid && rt.transportLostFrames.value == 0 &&
                           rt.transportLostFrames.sampleHostTimeUs == 123456,
                       "valid zero loss round-trips as valid zero");
            MIB_EXPECT(rt.sdkCompletedQueueDepth.validity == MetricValidity::Unsupported, "unsupported round-trips");
            MIB_EXPECT(rt.frameAgeUs.validity == MetricValidity::Stale && rt.frameAgeUs.value == 77, "stale round-trips");
            r.closeFile();
        }
        const std::string legacy = (td.path() / "legacy.h5").string();
        {
            backend::services::Hdf5Service w;
            MIB_REQUIRE(w.openFile(legacy), "open legacy");
            MIB_REQUIRE(w.initializeRecordingDatasets(), "init legacy");
            MIB_REQUIRE(w.writeRecordingInfo(1, 2, 0, 0), "legacy info");
            w.closeFile();
        }
        {
            backend::services::Hdf5Service r;
            MIB_REQUIRE(r.loadFile(legacy), "reload legacy");
            TimestampDescriptor rd;
            AcquisitionTelemetrySnapshot rt;
            MIB_EXPECT(!r.readAcquisitionProvenance(rd, rt), "legacy file has no provenance");
            MIB_EXPECT(rd.validity == TimestampValidity::Unsupported && rd.domain == ClockDomain::Unknown,
                       "legacy descriptor is Unsupported/Unknown, not silently nanoseconds");
            MIB_EXPECT(rt.transportLostFrames.validity == MetricValidity::Unavailable,
                       "legacy telemetry Unavailable, never 0");
            r.closeFile();
        }
    }

    return mib::test::exitCode();
}
