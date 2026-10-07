// PZ7035 camera modes (#501 P1): the single register writer (LED, cell path, cell capture) and
// AppBackend's Align/Run sequence on a PL-science backend with the mock camera standing in for
// the GenTL producer. The rules under test come from pz7035-imx426 docs/YOFO_HOST_INTERFACE.md:
// - no PL register is touched while the PL is unconfigured (PCFG_DONE);
// - the LED is written off-first (S[0] = 0 ... S[0] = 1);
// - the cell path is never on while the camera stream (the producer) runs: its AcquisitionStart
//   pulses clear the U-Net enable;
// - in Run the live camera stays stopped and the readiness gates ask for Run, not a live camera;
// - raw LED values only in Service mode and within the mode's limits, enforced by the backend.
#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/RecordingTarget.h"
#include "backend/app/SciencePlacement.h"
#include "backend/pz/PzInstrumentControl.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pz = backend::pz;

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int processId() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

constexpr unsigned S0 = 128; // S[i] = P[128 + i]

struct Write {
    unsigned index;
    uint32_t value;
};

// A U-Net cell image: 'CEL2', 100 MHz strobe clock, a finished capture with two listed cells.
struct FakeState {
    std::map<unsigned, uint32_t> live;
    std::vector<Write> writes;
    unsigned reads{0};
    bool configured{true};
    std::function<void(unsigned, uint32_t)> onWrite;
};

class FakeControl final : public pz::IPzControlRegisters {
public:
    explicit FakeControl(FakeState& s) : s_(s) {}
    uint32_t live(unsigned i) override {
        ++s_.reads;
        auto it = s_.live.find(i);
        return it == s_.live.end() ? 0 : it->second;
    }
    void setLive(unsigned i, uint32_t v) override {
        s_.writes.push_back({i, v});
        s_.live[i] = v;
        if (i == S0 + 36) s_.live[S0 + 42] = 1; // capture done at once
        if (s_.onWrite) s_.onWrite(i, v);
    }
    uint32_t window(unsigned w) override {
        ++s_.reads;
        if (w < 12288) return 0x04030201u + w;                       // gray pattern
        if (w < 13824) return 0xFF00FF00u;                            // mask pattern
        const unsigned cell = (w - 13824) / 32, k = (w - 13824) % 32;
        if (k == 15) return (10u + cell) << 16 | (100u + 40 * cell); // y << 16 | x
        if (k == 16) return 20u << 16 | 30u;                         // h << 16 | w
        if (k == 17) return 2u << 24 | cell << 16 | 1u;               // count, rank, valid
        return k;
    }
    bool plConfigured(std::string* why) override {
        if (!s_.configured && why) *why = "PL not configured (DEVCFG PCFG_DONE = 0): load the PL image";
        return s_.configured;
    }
    void sleepUs(unsigned) override {}

private:
    FakeState& s_;
};

void seedCellImage(FakeState& s) {
    s.live[S0 + 41] = 0x43454C32u; // 'CEL2'
    s.live[S0 + 10] = 100000;      // kHz
    s.live[8] = 0x01;              // some other command bit that must survive
    s.live[S0 + 43] = 777;         // capture tag
    s.live[S0 + 44] = 2u | (0x5u << 8) | (3u << 16);
    s.live[S0 + 45] = 4u | (1u << 16);
}

std::vector<Write> strobeWrites(const FakeState& s, size_t from = 0) {
    std::vector<Write> w;
    for (size_t i = from; i < s.writes.size(); ++i)
        if (s.writes[i].index >= S0 && s.writes[i].index <= S0 + 5) w.push_back(s.writes[i]);
    return w;
}

void testControl() {
    FakeState s;
    seedCellImage(s);
    pz::PzInstrumentControl control(std::make_unique<FakeControl>(s));
    std::string err;

    MIB_REQUIRE(control.setLed(pz::kRunLed, &err), "Run LED: " + err);
    const auto w = strobeWrites(s);
    MIB_EXPECT(w.size() == 6 && w.front().index == S0 && w.front().value == 0 && w.back().index == S0 &&
                   w.back().value == 1,
               "LED written off first and enabled last");
    MIB_EXPECT(s.live[S0 + 1] == 700 && s.live[S0 + 2] == 6000, "7/60 µs = 700/6000 cycles at 100 MHz");

    MIB_REQUIRE(control.setCellPath(true, &err), "cell path on");
    MIB_EXPECT(s.live[S0 + 46] == 1 && s.live[8] == 0x11, "S[46] and P[8] bit 4 set, other bits kept");
    MIB_EXPECT(control.cellPathOn(&err), "cell path reads on");

    pz::PzCellCapture capture;
    MIB_REQUIRE(control.captureCell(capture, std::chrono::milliseconds(50), &err), "capture: " + err);
    MIB_EXPECT(capture.frameId == 777 && capture.listed == 2 && capture.flags == 5 && capture.dropped == 3 &&
                   capture.cells == 4 && capture.blemishes == 1,
               "capture listing words");
    MIB_EXPECT(capture.gray.size() == 512 * 96 && capture.gray[0] == 1 && capture.gray[3] == 4 && capture.gray[4] == 2,
               "gray little-endian, pixel 0 in bits 7:0");
    MIB_EXPECT(capture.mask.size() == 6144 && capture.mask[0] == 0x00 && capture.mask[1] == 0xFF, "mask bytes");
    MIB_EXPECT(capture.list.size() == 2 && capture.list[1].x() == 140 && capture.list[1].y() == 11 &&
                   capture.list[1].width() == 30 && capture.list[1].height() == 20 && capture.list[1].valid(),
               "cell bbox {y,x}, {h,w}, validity");

    const auto packet = pz::encodeRunPreview(capture);
    MIB_EXPECT(packet.size() == 32 + 49152 + 6144 + 2 * 72 && std::memcmp(packet.data(), "MIBC", 4) == 0 &&
                   packet[4] == 1 && packet[6] == 32 && packet[8] == 0x09 && packet[9] == 0x03 && packet[16] == 2,
               "run preview packet: header, frame id 777, 2 cells");

    MIB_REQUIRE(control.setCellPath(false, &err), "cell path off");
    MIB_EXPECT(s.live[S0 + 46] == 0 && s.live[8] == 0x01, "cell path off clears only bit 4");
    MIB_EXPECT(!control.captureCell(capture, std::chrono::milliseconds(50), &err) &&
                   err.find("cell path is off") != std::string::npos,
               "no capture with the cell path off");

    // A blank PL: nothing is read or written at all.
    s.configured = false;
    const auto reads = s.reads;
    const auto writes = s.writes.size();
    MIB_EXPECT(!control.ledOff(&err) && err.find("PCFG_DONE") != std::string::npos, "refused while the PL is blank");
    MIB_EXPECT(!control.setCellPath(true, &err) && !control.setLed(pz::kAlignLed, &err), "every write is refused");
    MIB_EXPECT(s.reads == reads && s.writes.size() == writes, "no PL access while the PL is blank");

    // Another image: refused before any write.
    s.configured = true;
    s.live[S0 + 41] = 0;
    MIB_EXPECT(!control.ledOff(&err) && err.find("CEL2") != std::string::npos, "not the U-Net cell image");
    MIB_EXPECT(s.writes.size() == writes, "nothing written on another image");
}

void testLedLimits() {
    using pz::InstrumentMode;
    MIB_EXPECT(pz::checkLed(InstrumentMode::Run, pz::kRunLed).empty(), "Run preset within Run limits");
    MIB_EXPECT(pz::checkLed(InstrumentMode::Align, pz::kAlignLed).empty(), "Align preset within Align limits");
    MIB_EXPECT(!pz::checkLed(InstrumentMode::Run, {7.0, 90.0}).empty(), "Run width above 80 µs refused");
    MIB_EXPECT(!pz::checkLed(InstrumentMode::Align, {0.0, 40.0}).empty(), "Align width below 60 µs refused");
    MIB_EXPECT(!pz::checkLed(InstrumentMode::Run, {-1.0, 60.0}).empty(), "negative delay refused");
    MIB_EXPECT(!pz::checkLed(InstrumentMode::Unknown, pz::kRunLed).empty(), "no LED without a mode");
}

const backend::app::ReadinessGate* gateOf(const backend::app::ExperimentReadinessSnapshot& r, const char* id) {
    return r.gate(id);
}

void testModeSequence(const mib::test::TempDir& td) {
    const auto frames = td.path() / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 8, 512, 96), "mock frames");
    setEnv("MIB_PL_SCIENCE", "1");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", frames.string().c_str());
    setEnv("MIB_DISABLED_SERVICES", "sqlite,hdf5,autofocus,trigger,playback");
    setEnv("MIB_EXECUTION_PROVIDER", "none");
    MIB_REQUIRE(!backend::app::hostProcessingAvailable(), "science on the PL");

    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize((td.path() / "data").string()), "facade initializes");
    MIB_EXPECT(!backend.instrumentControlAvailable(), "no control without the board provider");

    FakeState s;
    seedCellImage(s);
    // The rule the producer imposes: the cell path never goes on while the camera stream runs.
    bool cellOnWhileStreaming = false;
    s.onWrite = [&](unsigned i, uint32_t v) {
        const bool cellOn = (i == S0 + 46 && v == 1) || (i == 8 && (v & 0x10u));
        if (cellOn && backend.capture().lifecycleSnapshot().isActive()) cellOnWhileStreaming = true;
    };
    backend.setInstrumentControlForTesting(std::make_unique<FakeControl>(s));
    MIB_REQUIRE(backend.instrumentControlAvailable(), "control injected");
    MIB_EXPECT(backend.instrumentMode() == pz::InstrumentMode::Unknown && s.writes.empty(),
               "nothing is written until a mode is chosen");

    std::string err;
    // Align: the camera streams, cell path off, LED 0/125.
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Align, 0, 0, &err), "Align: " + err);
    MIB_EXPECT(backend.instrumentMode() == pz::InstrumentMode::Align, "mode Align");
    MIB_EXPECT(backend.capture().lifecycleSnapshot().cameraReady, "the camera streams in Align");
    MIB_EXPECT(s.live[S0 + 46] == 0 && (s.live[8] & 0x10u) == 0, "cell path off in Align");
    MIB_EXPECT(s.live[S0 + 0] == 1 && s.live[S0 + 1] == 0 && s.live[S0 + 2] == 12500, "LED Align 0/125");
    std::vector<uint8_t> preview;
    MIB_EXPECT(!backend.fetchRunPreview(preview, &err), "no run preview in Align");

    const auto out = (td.path() / "run.h5").string();
    auto readiness = backend.experiment().evaluateReadiness(out, "pl");
    MIB_EXPECT(gateOf(readiness, "instrument.mode") &&
                   gateOf(readiness, "instrument.mode")->status == backend::app::GateStatus::Fail,
               "a run needs Run mode");

    // Run at an off-grid window: snapped to x % 8, y % 4; camera stopped; cell path on; LED 7/60.
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Run, 157, 203, &err), "Run: " + err);
    MIB_EXPECT(backend.instrumentMode() == pz::InstrumentMode::Run, "mode Run");
    MIB_EXPECT(backend.instrumentRunOffset() == std::make_pair(152, 200), "window snapped to (152, 200)");
    MIB_EXPECT(!backend.capture().lifecycleSnapshot().isActive(), "the producer stream is stopped in Run");
    MIB_EXPECT(s.live[S0 + 46] == 1 && (s.live[8] & 0x10u) != 0, "cell path on in Run");
    MIB_EXPECT(s.live[S0 + 0] == 1 && s.live[S0 + 1] == 700 && s.live[S0 + 2] == 6000, "LED Run 7/60");
    MIB_EXPECT(!cellOnWhileStreaming, "the cell path never went on while the camera streamed");

    MIB_EXPECT(backend.fetchRunPreview(preview, &err) && preview.size() > 32 + 49152,
               "run preview from the cell capture: " + err);

    backend::bridge::CameraCommand start;
    start.action = backend::bridge::CameraCommandAction::StartCapture;
    const auto refused = facade.dispatch(start);
    MIB_EXPECT(!refused.ok && refused.message.find("Run mode") != std::string::npos, "Start Camera refused in Run");
    MIB_EXPECT(!backend.capture().lifecycleSnapshot().isActive(), "and the stream stays stopped");
    MIB_EXPECT(!backend.setCameraOverview(true, &err), "the overview switch is refused in Run");

    readiness = backend.experiment().evaluateReadiness(out, "pl");
    MIB_EXPECT(gateOf(readiness, "instrument.mode") &&
                   gateOf(readiness, "instrument.mode")->status == backend::app::GateStatus::Pass,
               "Run passes the instrument gate");
    {
        const auto* persistent = gateOf(readiness, "storage.persistent");
        const bool onRam = backend::app::recordingTarget(out).ram;
        MIB_EXPECT(persistent && persistent->status == (onRam ? backend::app::GateStatus::Warn
                                                              : backend::app::GateStatus::Pass),
                   "storage.persistent follows the destination's filesystem");
        std::error_code ec;
        if (std::filesystem::is_directory("/dev/shm", ec) && backend::app::recordingTarget("/dev/shm").ram) {
            const auto ramOut = "/dev/shm/mib_gate_" + std::to_string(processId()) + ".h5";
            const auto ramReadiness = backend.experiment().evaluateReadiness(ramOut, "pl");
            const auto* warn = gateOf(ramReadiness, "storage.persistent");
            MIB_EXPECT(warn && warn->status == backend::app::GateStatus::Warn &&
                           warn->reason.rfind("Recording to RAM: lost on power-off.", 0) == 0,
                       "a RAM destination warns at run start");
            MIB_EXPECT(!warn->blocksStart(), "and does not block the run");
        }
    }
    MIB_EXPECT(!gateOf(readiness, "camera.session") && !gateOf(readiness, "camera.geometry") &&
                   !gateOf(readiness, "camera.deliveryMode"),
               "the live-camera gates do not apply in Run");
    MIB_EXPECT(gateOf(readiness, "telemetry.transportLoss") &&
                   gateOf(readiness, "telemetry.transportLoss")->status == backend::app::GateStatus::Pass,
               "transport loss comes from the PL records in Run");

    // Raw LED: Service mode only, within the Run limits.
    MIB_EXPECT(!backend.setInstrumentLed(5.0, 70.0, &err) && err.find("Service") != std::string::npos,
               "raw LED refused outside Service mode (backend-side)");
    backend.setServiceMode(true);
    MIB_EXPECT(!backend.setInstrumentLed(7.0, 90.0, &err), "a Run width above 80 µs is refused");
    MIB_EXPECT(backend.setInstrumentLed(5.0, 70.0, &err) && s.live[S0 + 1] == 500 && s.live[S0 + 2] == 7000,
               "5/70 µs applied in Service mode: " + err);
    backend.setServiceMode(false);

    // Back to Align: cell path off before the camera starts again.
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Align, 0, 0, &err), "back to Align: " + err);
    MIB_EXPECT(s.live[S0 + 46] == 0 && backend.capture().lifecycleSnapshot().cameraReady, "Align again, streaming");
    MIB_EXPECT(!cellOnWhileStreaming, "still never on while streaming");

    // A blank PL refuses the switch and writes nothing.
    s.configured = false;
    const auto writes = s.writes.size();
    MIB_EXPECT(!backend.setInstrumentMode(pz::InstrumentMode::Run, 0, 0, &err) && err.find("PCFG_DONE") != std::string::npos,
               "mode switch refused with the PL blank");
    MIB_EXPECT(s.writes.size() == writes, "no register written");
    facade.shutdown();
}

// Both states of the persistence warning (#501): a RAM-backed destination (tmpfs, like the JTAG
// RAM root) warns with the operator text; a disk destination does not.
void testRecordingTarget(const mib::test::TempDir& td) {
    namespace fs = std::filesystem;
    const auto disk = backend::app::recordingTarget((td.path() / "runs" / "run.h5").string());
    MIB_EXPECT(disk.exists && disk.writable, "a temp dir on disk is writable: " + disk.path);
    std::error_code ec;
    const fs::path shm = "/dev/shm";
    if (fs::is_directory(shm, ec) && backend::app::recordingTarget(shm.string()).filesystem == "tmpfs") {
        const auto dir = shm / ("mib_target_" + std::to_string(processId()));
        fs::create_directories(dir, ec);
        const auto ram = backend::app::recordingTarget((dir / "run.h5").string());
        const auto warning = backend::app::recordingTargetWarning(ram);
        MIB_EXPECT(ram.ram && ram.writable, "tmpfs is RAM");
        MIB_EXPECT(warning.rfind("Recording to RAM: lost on power-off. Copy data off before shutdown. ", 0) == 0 &&
                       warning.find(" GB free.") != std::string::npos,
                   "the operator warning: " + warning);
        fs::remove_all(dir, ec);
    } else {
        std::fprintf(stderr, "instrument_modes: /dev/shm is not tmpfs here; RAM state checked by construction only\n");
    }
    backend::app::RecordingTarget ram;
    ram.ram = true;
    ram.freeBytes = 2'500'000'000ull;
    MIB_EXPECT(backend::app::recordingTargetWarning(ram) ==
                   "Recording to RAM: lost on power-off. Copy data off before shutdown. 2.5 GB free.",
               "warning wording");
    if (!disk.ram) MIB_EXPECT(backend::app::recordingTargetWarning(disk).empty(), "a disk destination does not warn");
}

} // namespace

int main() {
    mib::test::Watchdog wd(60);
    mib::test::TempDir td("instrument_modes");
    testControl();
    testLedLimits();
    testModeSequence(td);
    testRecordingTarget(td);
    return mib::test::exitCode();
}
