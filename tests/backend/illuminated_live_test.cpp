// Issue #413: saved illuminated profiles must start/stop the complete rig.
#include "backend/camera/mindvision/MindVisionCamera.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/ModbusRtu.h"
#include "support/fake_mindvision_sdk.h"
#include "support/assert.h"
#include "support/watchdog.h"
#include <atomic>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>

using backend::services::IlluminationSession;
using backend::services::PulseGeneratorService;
using camera::common::MindVisionCamera;
namespace mv = backend::camera::mindvision;
namespace modbus = backend::services::modbus;

namespace {
// One fake Modbus slave at address 1 behind a named serial port.
struct Wire {
    std::atomic<bool> failWrites{false};
    std::atomic<bool> corruptRead{false};
    std::atomic<int> writes{0};        // every request frame
    std::atomic<int> writeCommands{0}; // FC06/FC16 only: must stay 0 for discovery
    std::array<uint16_t, 12> regs{}; // exclusively accessed by the serial worker
    // Registers outside the 12-register generator map. The real module answers
    // 0 for any such register (observed on the rig); a pump serves real values.
    std::map<uint16_t, uint16_t> extra;
    uint16_t read(int index) const {
        if (index >= 0 && index < 12) return regs.at(static_cast<size_t>(index));
        const auto it = extra.find(static_cast<uint16_t>(index));
        return it == extra.end() ? 0 : it->second;
    }
    uint16_t& reg(int index) {
        if (index >= 0 && index < 12) return regs.at(static_cast<size_t>(index));
        return extra[static_cast<uint16_t>(index)];
    }
};
// Port-name -> slave registry shared by every fake port the bus creates;
// names not present open like an unplugged adapter, `busy` names like an
// adapter held by another program (the platform's EACCES/ACCESS_DENIED path).
struct Bench {
    std::map<std::string, Wire*> wires;
    std::set<std::string> busy;
};
class Port final : public backend::services::ISerialPort {
    const Bench& bench;
    Wire* wire = nullptr;
    bool opened = false;
    int systemError = 0;
    std::vector<uint8_t> rx;

public:
    explicit Port(const Bench& b) : bench(b) {}
    bool open(int n, int) override {
        return openNamed("COM" + std::to_string(n), {});
    }
    bool openNamed(const std::string& name, const backend::services::SerialSettings&) override {
        if (bench.busy.count(name)) {
#ifdef _WIN32
            systemError = 5; // ERROR_ACCESS_DENIED
#else
            systemError = EACCES;
#endif
            return false;
        }
        const auto it = bench.wires.find(name);
        if (it == bench.wires.end()) {
            systemError = 2; // no such device
            return false;
        }
        wire = it->second;
        opened = true;
        return true;
    }
    int lastSystemError() const override { return systemError; }
    bool isOpen() const override { return opened; }
    void close() override { opened = false; }
    int write(const std::vector<uint8_t>& q) override {
        ++wire->writes;
        if (q[1] != 3) ++wire->writeCommands;
        const int start = (q[2] << 8) | q[3], count = (q[4] << 8) | q[5];
        if (q[1] != 3 && wire->failWrites) {
            rx = {q[0], static_cast<uint8_t>(q[1] | 0x80), 4};
            modbus::appendCrc(rx);
            return static_cast<int>(q.size());
        }
        if (q[1] == 3) {
            rx = {q[0], 3, static_cast<uint8_t>(count * 2)};
            for (int i = 0; i < count; ++i) {
                const auto n = wire->read(start + i);
                rx.push_back(n >> 8);
                rx.push_back(n & 255);
            }
            if (wire->corruptRead) rx[3] ^= 1;
            modbus::appendCrc(rx);
        } else if (q[1] == 6) {
            wire->reg(start) = count;
            rx = q;
        } else {
            for (int i = 0; i < count; ++i)
                wire->reg(start + i) = (q[7 + i * 2] << 8) | q[8 + i * 2];
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
    // Stop issued while the generator is still being prepared (discovery can
    // take over a second): the camera must never be opened or played and the
    // generator must be released exactly once. Probabilistic only in the
    // 100 ms margin between the stopper's flag and its stop() call.
    {
        watchdog.mark("stop during preparation cancels before camera open");
        mib::test::FakeMindVisionSdk fake;
        auto sdk = std::make_shared<mv::SdkOps>(*fake.ops());
        sdk->armIllumination = [](int, const mv::Config&) { return true; };
        auto session = std::make_shared<IlluminationSession>();
        std::atomic<bool> preparing{false}, stopIssued{false};
        std::atomic<int> enabled{0}, disabled{0};
        session->prepare = [&] {
            preparing = true;
            while (!stopIssued) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return true;
        };
        session->enable = [&] {
            ++enabled;
            return true;
        };
        session->disable = [&] {
            ++disabled;
            return true;
        };
        MindVisionCamera camera(0, path, sdk, session);
        std::thread stopper([&] {
            while (!preparing) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            stopIssued = true;
            camera.stop();
        });
        const bool started = camera.start();
        stopper.join();
        MIB_EXPECT(!started && camera.lastFailure().code == "mindvision.rig_cancelled",
                   "cancelled start reports cancellation, not readiness");
        MIB_EXPECT(fake.playCalls == 0 && fake.unInitCalls == 0 && enabled == 0,
                   "cancelled start never plays the camera or enables the generator");
        MIB_EXPECT(disabled == 1, "prepared generator released exactly once");
    }
    // Shared timing/connection rules: the same parse gates Save and Play.
    {
        watchdog.mark("live_view timing validation");
        auto profileWith = [](const std::string& extra, double hz, const std::string& liveExtra) {
            return "{\"width\":512,\"height\":96,\"exposure_time_us\":100,\"trigger_mode\":2,"
                   "\"ext_trig_signal_type\":2,\"strobe_mode\":1,\"strobe_pulse_width_us\":100,"
                   "\"strobe_polarity\":1" +
                   extra + ",\"live_view\":{\"enabled\":true,\"port\":\"auto\",\"frequency_hz\":" +
                   std::to_string(hz) + liveExtra + "}}";
        };
        auto ok = mv::parseConfig(profileWith("", 1000, ""));
        MIB_EXPECT(ok.ok && ok.config.liveView.port == "auto" &&
                       ok.config.liveView.channel == 1 && ok.config.liveView.address == 1 &&
                       ok.config.liveView.dutyPercent == 2.0 &&
                       ok.config.liveView.parity == 'N' && ok.config.liveView.baud == 9600,
                   "preset defaults fill missing live_view keys");
        MIB_EXPECT(std::fabs(ok.config.liveView.triggerPulseUs() - 20.0) < 1e-9,
                   "preset trigger pulse is 20 us");
        auto exposure = mv::parseConfig(profileWith("", 40000, ""));
        MIB_EXPECT(!exposure.ok && exposure.error.find("exposure") != std::string::npos &&
                       exposure.error.find("25 us") != std::string::npos,
                   "FPS whose period is shorter than exposure is refused with the period");
        auto strobe = mv::parseConfig(profileWith("", 10000, ""));
        MIB_EXPECT(!strobe.ok && strobe.error.find("strobe") != std::string::npos,
                   "strobe delay + width must be shorter than the period");
        MIB_EXPECT(mv::parseConfig(profileWith("", 9000, "")).ok,
                   "9000 FPS fits 100 us exposure and strobe");
        MIB_EXPECT(!mv::parseConfig(profileWith("", 5000, ",\"duty_percent\":100")).ok,
                   "continuous duty refused");
        MIB_EXPECT(!mv::parseConfig(profileWith("", 5000, ",\"channel\":5")).ok,
                   "channel beyond the module refused");
        MIB_EXPECT(!mv::parseConfig(profileWith("", 5000, ",\"address\":\"1\"")).ok,
                   "wrong JSON type is an error, not a silent default");
        MIB_EXPECT(!mv::parseConfig(profileWith("", 5000, ",\"port\":\"\"")).ok,
                   "empty port refused");
        MIB_EXPECT(!mv::parseConfig(profileWith("", 5000, ",\"parity\":\"X\"")).ok,
                   "unknown parity refused");
        MIB_EXPECT(mv::parseConfig(profileWith("", 300, "")).error.find("400") !=
                       std::string::npos,
                   "FPS below the generator range names the range");
    }
    // Real service through fake Modbus transport: ownership, wire round-trip,
    // failures and repeated cross-thread stop. No hardware is touched.
    // Register images are what the rig PC's ports answered on 2026-09-14/15.
    Wire wire;     // the generator: channel 1 set once (5000 Hz / 50 %), channels 2–4 0 Hz
    Wire foreign;  // a second, never-configured module: identical shape, all zeros
    Wire pumpLike; // a dLSP pump left channel-enabled at address 1: reg0 = 1,
                   // syringe volume 10 at 0x0061 (generator shape on channel 1)
    wire.regs[0] = 0x0007;
    wire.regs[1] = 0xA120; // 500000 = 5000.00 Hz
    wire.regs[2] = 5000;   // 50.00 %
    pumpLike.regs[0] = 1;
    pumpLike.extra[PulseGeneratorService::SYRINGE_PUMP_VOLUME_REGISTER] = 10;
    const auto generatorRegs = wire.regs;
    Bench bench;
    bench.wires = {{"COM1", &wire}, {"COM2", &wire}, {"COM4", &foreign}, {"COM8", &pumpLike}};
    bench.busy = {"COM6"};
    backend::services::serialbus::SerialBusManager bus;
    bus.setSerialPortFactory([&] { return std::make_unique<Port>(bench); });
    PulseGeneratorService gen(bus);
    PulseGeneratorService::Config cfg;
    cfg.portName = "COM1";
    auto usbAdapter = [](const std::string& name) {
        backend::services::serialbus::PortInfo info;
        info.systemName = name;
        info.systemLocation = "\\\\.\\" + name;
        info.vendorId = 0x1a86; // CH344
        info.productId = 0x55d5;
        return info;
    };
    const auto adapter = usbAdapter("COM1");
    const auto secondAdapter = usbAdapter("COM2");
    const auto foreignAdapter = usbAdapter("COM4");
    const auto busyAdapter = usbAdapter("COM6");
    const auto pumpAdapter = usbAdapter("COM8");
    const auto unpluggedAdapter = usbAdapter("COM9");
    std::string discoveryError;
    MIB_EXPECT(!gen.discoverLiveView(cfg, 0, {}, &discoveryError), "missing adapter rejected");
    MIB_EXPECT(gen.discoverLiveView(cfg, 0, {adapter}, &discoveryError) && cfg.portName == "COM1",
               "unique compatible generator automatically resolved");
    MIB_EXPECT(wire.regs == generatorRegs && wire.writeCommands == 0 && wire.extra.empty(),
               "discovery never changes outputs");
    MIB_EXPECT(!gen.discoverLiveView(cfg, 0, {adapter, secondAdapter}, &discoveryError),
               "ambiguous generators never guessed");
    MIB_EXPECT(discoveryError.find("COM1") != std::string::npos &&
                   discoveryError.find("COM2") != std::string::npos,
               "ambiguity names both adapters");
    // Regression (rig PC 2026-09-14): a zero-register module answering at the
    // configured address was a lenient "generator", making discovery ambiguous
    // or, with the real generator unavailable, adopting the wrong device.
    // Regression (rig PC 2026-09-15): the real generator keeps 0 Hz on channels
    // it has never set, so "all channels configured" rejected the real rig.
    cfg.portName = "auto";
    MIB_EXPECT(gen.discoverLiveView(cfg, 0, {foreignAdapter, adapter, unpluggedAdapter, pumpAdapter},
                                    &discoveryError) &&
                   cfg.portName == "COM1",
               "unconfigured module and pump-like device do not make discovery ambiguous");
    MIB_EXPECT(foreign.writeCommands == 0 && foreign.writes > 0 && pumpLike.writeCommands == 0 &&
                   pumpLike.writes > 0,
               "other devices were only read, never written");
    cfg.portName = "auto";
    MIB_EXPECT(!gen.discoverLiveView(cfg, 0, {foreignAdapter, busyAdapter, pumpAdapter},
                                     &discoveryError) &&
                   cfg.portName == "auto",
               "unconfigured module and pump alone are never adopted");
    MIB_EXPECT(discoveryError.find("COM6") != std::string::npos &&
                   discoveryError.find("in use by another program") != std::string::npos,
               "busy adapter is named as the likely cause");
    MIB_EXPECT(discoveryError.find("COM4") != std::string::npos &&
                   discoveryError.find("channel 1 has never been set") != std::string::npos,
               "never-configured module is named with the remedy");
    MIB_EXPECT(discoveryError.find("COM8") != std::string::npos &&
                   discoveryError.find("not a pulse generator") != std::string::npos,
               "pump-like device is named as not a generator");
    MIB_EXPECT(foreign.writeCommands == 0 && pumpLike.writeCommands == 0,
               "other devices still never written");
    // Channel matters: the same generator is unconfigured for channel 2.
    cfg.portName = "auto";
    MIB_EXPECT(!gen.discoverLiveView(cfg, 1, {adapter}, &discoveryError) &&
                   discoveryError.find("channel 2 has never been set") != std::string::npos,
               "requested channel must itself be configured");
    std::vector<uint8_t> zeros(24, 0);
    std::vector<uint8_t> rigImage = {0x00, 0x07, 0xA1, 0x20, 0x13, 0x88};
    rigImage.resize(24, 0);
    MIB_EXPECT(PulseGeneratorService::identityLooksLikeGenerator(zeros) &&
                   !PulseGeneratorService::identityChannelConfigured(zeros, 0),
               "manual scan stays lenient; automatic adoption requires a configured channel");
    MIB_EXPECT(PulseGeneratorService::identityChannelConfigured(rigImage, 0) &&
                   !PulseGeneratorService::identityChannelConfigured(rigImage, 1) &&
                   PulseGeneratorService::identityFrequencyRaw(rigImage, 0) == 500000,
               "rig image: channel 1 configured at 5000 Hz, channel 2 not");
    cfg.portName = "COM1";
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
