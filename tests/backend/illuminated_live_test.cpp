// Issue #413: saved illuminated profiles must start/stop the complete rig.
#include "backend/camera/mindvision/MindVisionCamera.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/ModbusRtu.h"
#include "support/fake_mindvision_sdk.h"
#include "support/assert.h"
#include "support/watchdog.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>

using backend::services::IlluminationSession;
using backend::services::PulseGeneratorService;
using camera::common::MindVisionCamera;
namespace mv = backend::camera::mindvision;
namespace modbus = backend::services::modbus;

namespace {
struct Wire {
    std::atomic<bool> failWrites{false};
    std::atomic<bool> corruptRead{false};
    std::atomic<int> writes{0};
    std::array<uint16_t, 12> regs{}; // exclusively accessed by the serial worker
};
class Port final : public backend::services::ISerialPort {
    Wire& wire;
    bool opened = false;
    std::vector<uint8_t> rx;

public:
    explicit Port(Wire& w) : wire(w) {}
    bool open(int, int) override {
        opened = true;
        return true;
    }
    bool isOpen() const override { return opened; }
    void close() override { opened = false; }
    int write(const std::vector<uint8_t>& q) override {
        ++wire.writes;
        const int start = (q[2] << 8) | q[3], count = (q[4] << 8) | q[5];
        if (q[1] != 3 && wire.failWrites) {
            rx = {q[0], static_cast<uint8_t>(q[1] | 0x80), 4};
            modbus::appendCrc(rx);
            return static_cast<int>(q.size());
        }
        if (q[1] == 3) {
            rx = {q[0], 3, static_cast<uint8_t>(count * 2)};
            for (int i = 0; i < count; ++i) {
                const auto n = wire.regs.at(start + i);
                rx.push_back(n >> 8);
                rx.push_back(n & 255);
            }
            if (wire.corruptRead) rx[3] ^= 1;
            modbus::appendCrc(rx);
        } else if (q[1] == 6) {
            wire.regs.at(start) = count;
            rx = q;
        } else {
            for (int i = 0; i < count; ++i)
                wire.regs.at(start + i) = (q[7 + i * 2] << 8) | q[8 + i * 2];
            rx = {q[0], q[1], q[2], q[3], q[4], q[5]};
            modbus::appendCrc(rx);
        }
        return static_cast<int>(q.size());
    }
    bool waitForBytesWritten(int) override { return true; }
    bool waitForReadyRead(int ms) override {
        if (rx.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return !rx.empty();
    }
    std::vector<uint8_t> readAll() override {
        auto out = rx;
        rx.clear();
        return out;
    }
    std::string lastError() const override { return {}; }
};
} // namespace
int main() {
    mib::test::Watchdog watchdog(60);
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("mib-live-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    struct Clean {
        std::filesystem::path p;
        ~Clean() { std::filesystem::remove_all(p); }
    } clean{dir};
    const auto path = (dir / "camera.json").string();
    const std::string profile = R"({"width":512,"height":96,"exposure_time_us":100,
      "trigger_mode":2,"ext_trig_signal_type":2,"strobe_mode":1,"strobe_pulse_width_us":100,
      "strobe_polarity":1,"live_view":{"enabled":true,"port":"COM1"}})";
    std::ofstream(path) << profile;
    const auto parsed = mv::parseConfig(profile);
    MIB_REQUIRE(parsed.ok && parsed.config.illuminatedLive, "saved rig round-trip");
    MIB_EXPECT(!mv::parseConfig(R"({"live_view":{"enabled":"yes"}})").ok,
               "invalid ownership flag rejected");
    {
        mib::test::FakeMindVisionSdk fake;
        MindVisionCamera camera(0, path, fake.ops());
        MIB_EXPECT(!camera.start(), "regression: rig cannot start without generator coordinator");
        MIB_EXPECT(camera.lastFailure().code == "mindvision.rig_missing",
                   "actionable missing setup");
        MIB_EXPECT(fake.playCalls == 0, "no camera play without generator preparation");
    }
    // Each failure stage, followed by successful restart. Ordered events,
    // including teardown, not just matching configuration values.
    for (int failure = 0; failure < 6; ++failure) {
        watchdog.mark("ordered lifecycle/fault injection");
        mib::test::FakeMindVisionSdk fake;
        auto sdk = std::make_shared<mv::SdkOps>(*fake.ops());
        std::vector<std::string> events;
        auto session = std::make_shared<IlluminationSession>();
        session->prepare = [&] {
            events.push_back("prepare/off");
            return failure != 1;
        };
        session->enable = [&] {
            events.push_back("enable");
            return failure != 4;
        };
        session->disable = [&] {
            events.push_back("generator off");
            return failure != 5;
        };
        sdk->applyConfig = [&](int, const mv::Config&) {
            events.push_back("configure");
            return failure != 2;
        };
        sdk->armIllumination = [&](int, const mv::Config&) {
            events.push_back("arm/readback");
            return failure != 3;
        };
        const auto play = sdk->play;
        sdk->play = [&](int h) {
            events.push_back("play");
            return play(h);
        };
        const auto stop = sdk->stop;
        sdk->stop = [&](int h) {
            events.push_back("capture stop");
            return stop(h);
        };
        sdk->setOutputIoMode = [&](int, int io, int mode) {
            if (io == 0 && mode == 3) events.push_back("led mode");
            return 0;
        };
        sdk->setIoStateEx = [&](int, int io, unsigned value) {
            if (io == 0 && value == 0) events.push_back("led off");
            return 0;
        };
        MindVisionCamera camera(0, path, sdk, session);
        const bool started = camera.start();
        MIB_EXPECT(started == (failure == 0 || failure == 5),
                   "failed setup prevents capture readiness");
        camera.stop();
        if (failure == 0 || failure == 5) {
            const std::vector<std::string> expected{"prepare/off",  "configure", "play",
                                                    "arm/readback", "enable",    "generator off",
                                                    "led mode",     "led off",   "capture stop"};
            MIB_EXPECT(events == expected,
                       "generator starts after arm; stops before LED and camera");
        }
        if (failure == 5)
            MIB_EXPECT(camera.lastFailure().code == "mindvision.rig_shutdown_unconfirmed",
                       "shutdown failure is retained");
        if (failure < 4 && failure > 0)
            MIB_EXPECT(std::find(events.begin(), events.end(), "enable") == events.end(),
                       "no enable after config/arm failure");
        const auto size = events.size();
        camera.stop();
        MIB_EXPECT(events.size() == size, "stop idempotent");
    }
    // CaptureService fault path releases generator before destroying camera.
    // Five delivered frames plus one explicitly rejected geometry frame.
    for (int cycle = 0; cycle < 10; ++cycle) {
        watchdog.mark("capture pipeline fault/restart");
        mib::test::FakeMindVisionSdk fake;
        for (int n = 0; n < 5; ++n)
            fake.frames.push_back(mib::test::FakeMindVisionSdk::frame(512, 96, 10));
        fake.frames.push_back(mib::test::FakeMindVisionSdk::frame(256, 96, 10));
        auto sdk = std::make_shared<mv::SdkOps>(*fake.ops());
        sdk->armIllumination = [](int, const mv::Config&) { return true; };
        auto session = std::make_shared<IlluminationSession>();
        std::atomic<bool> on{false};
        std::atomic<int> stopped{0}, delivered{0};
        session->prepare = [] { return true; };
        session->enable = [&] {
            on = true;
            return true;
        };
        session->disable = [&] {
            on = false;
            ++stopped;
            return true;
        };
        backend::services::CaptureService capture;
        capture.setCameraFactory(
            [&] { return std::make_unique<MindVisionCamera>(0, path, sdk, session); });
        capture.setFrameCallback(
            [&](const uint8_t*, size_t, uint64_t, uint64_t, uint64_t) { ++delivered; });
        MIB_REQUIRE(capture.start(), "pipeline starts");
        using State = backend::services::CaptureLifecycleState;
        MIB_REQUIRE(capture.waitForState({State::Faulted}, std::chrono::seconds(3)) ==
                        State::Faulted,
                    "geometry fault ends pipeline");
        capture.stop();
        MIB_EXPECT(!on && stopped == 1, "fault teardown gates illumination exactly once");
        MIB_EXPECT(delivered == 5 && fake.releaseCalls == 6,
                   "five frames delivered, one explicit reject; all buffers released");
    }
    // Real service through fake Modbus transport: ownership, wire round-trip,
    // failures and repeated cross-thread stop. No hardware is touched.
    Wire wire;
    backend::services::serialbus::SerialBusManager bus;
    bus.setSerialPortFactory([&] { return std::make_unique<Port>(wire); });
    PulseGeneratorService gen(bus);
    PulseGeneratorService::Config cfg;
    cfg.portName = "COM1";
    backend::services::serialbus::PortInfo adapter;
    adapter.systemName = "COM1";
    adapter.vendorId = 0x1234;
    adapter.productId = 1;
    std::string discoveryError;
    MIB_EXPECT(!gen.discoverLiveView(cfg, {}, &discoveryError), "missing adapter rejected");
    MIB_EXPECT(gen.discoverLiveView(cfg, {adapter}, &discoveryError) && cfg.portName == "COM1",
               "unique compatible generator automatically resolved");
    MIB_EXPECT((wire.regs == std::array<uint16_t, 12>{}), "discovery never changes outputs");
    auto secondAdapter = adapter;
    secondAdapter.systemName = "COM2";
    MIB_EXPECT(!gen.discoverLiveView(cfg, {adapter, secondAdapter}, &discoveryError),
               "ambiguous generators never guessed");
    for (int i = 0; i < 20; ++i) {
        watchdog.mark("generator ownership stress");
        MIB_REQUIRE(gen.beginLiveView(cfg, 0, 5000, 10), "prepare session");
        MIB_EXPECT(!gen.getStatus().channels[0].outputEnabled, "prepared but gated off");
        std::atomic<int> refused{0};
        std::thread manual([&] {
            for (int n = 0; n < 20; ++n) {
                if (!gen.setFrequency(0, 6000)) ++refused;
                gen.disconnect();
            }
        });
        MIB_REQUIRE(gen.enableLiveView(), "enable after camera ready");
        manual.join();
        MIB_EXPECT(refused == 20 && gen.isConnected(), "manual control cannot steal active rig");
        MIB_EXPECT(gen.getStatus().channels[0].frequencyHz == 5000, "manual frequency unchanged");
        std::thread stopper([&] { MIB_EXPECT(gen.endLiveView(), "cross-thread stop"); });
        stopper.join();
        MIB_EXPECT(!gen.getStatus().channels[0].outputEnabled, "stop gate confirmed");
    }
    MIB_REQUIRE(gen.beginLiveView(cfg, 0, 5000, 10) && gen.enableLiveView(),
                "shutdown fault setup");
    wire.failWrites = true;
    MIB_EXPECT(!gen.endLiveView(), "failed OFF not reported success");
    MIB_EXPECT(gen.getStatus().channels[0].outputEnabled, "unknown state not changed to OFF");
    wire.failWrites = false;
    MIB_EXPECT(gen.setOutputEnabled(0, false), "manual recovery after link restored");
    MIB_EXPECT(!gen.beginLiveView(cfg, 0, 5000, 100), "continuous duty rejected");
    int owner = 0, other = 0;
    MIB_REQUIRE(gen.beginLiveView(cfg, 0, 5000, 10, &owner), "identity owner");
    MIB_EXPECT(!gen.beginLiveView(cfg, 0, 5000, 10, &other), "second owner refused");
    MIB_EXPECT(gen.endLiveView(&other) && gen.liveViewOwned(),
               "failed second owner cannot release first");
    wire.corruptRead = true;
    MIB_EXPECT(!gen.enableLiveView(&owner), "readback mismatch rejects acknowledged enable");
    wire.corruptRead = false;
    MIB_EXPECT(gen.endLiveView(&owner), "cleanup after readback failure");
    gen.disconnect();
    return mib::test::exitCode();
}
