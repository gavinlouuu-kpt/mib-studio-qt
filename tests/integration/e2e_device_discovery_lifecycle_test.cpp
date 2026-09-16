// e2e_device_discovery_lifecycle_test (issue #419)
//
// End-to-end lifecycle over a real AppBackend (mock camera, fake serial
// factory, fake nanopositioner driver), exercising the shared discovery
// service exactly the way the desktop and the headless facade do:
//  1. startup policy: the configured mock camera is skipped, the unique
//     identified nanopositioner is connected through the real AutofocusService,
//     one hook call each;
//  2. explicit pulse-generator scan over the real PulseGeneratorService and
//     SerialBusManager against a fake Modbus bench: generator, foreign
//     device and corrupt responder classified, zero write function codes,
//     port released afterwards;
//  3. a cancelled scan leaves the established nanopositioner connected;
//  4. candidate accounting is conserved (identified + unidentified +
//     ambiguous == listed) on every snapshot;
//  5. shutdown with a blocked job in flight terminates it before the shared
//     serial port is released, and nothing is applied afterwards.
// Watchdog-guarded; no naked joins.
#include "backend/app/AppBackend.h"
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/discovery/StartupDiscoveryCoordinator.h"
#include "backend/discovery/providers/NanopositionerProvider.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/SerialBus.h"
#include "support/assert.h"
#include "support/fake_discovery_providers.h"
#include "support/fake_modbus_bench.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace backend::discovery;
using mib::test::ScriptedProvider;
using namespace std::chrono_literals;
namespace np = backend::nanopositioner;

namespace {

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

struct FakeNanoState {
    std::atomic<int> writes{0};
    std::atomic<int> connects{0};
    std::atomic<bool> connected{false};
};

class FakeNanoBackend final : public np::INanopositionerBackend {
public:
    explicit FakeNanoBackend(std::shared_ptr<FakeNanoState> s) : state_(std::move(s)) {}
    np::BackendKind kind() const override { return np::BackendKind::Oeabt; }
    bool connect(const np::Endpoint&, std::string&) override
    {
        ++state_->connects;
        state_->connected = true;
        return true;
    }
    void disconnect() override { state_->connected = false; }
    bool isConnected() const override { return state_->connected.load(); }
    bool readVoltage(double& v, std::string&) override { v = 10.0; return true; }
    bool setVoltage(double, std::string&) override { ++state_->writes; return true; }
    std::optional<double> maximumVoltage() const override { return 100.0; }
    std::string connectedEndpoint() const override { return "COM7"; }

private:
    std::shared_ptr<FakeNanoState> state_;
};

np::Endpoint endpoint(np::BackendKind backend, const std::string& id, const std::string& path)
{
    np::Endpoint ep;
    ep.backend = backend;
    ep.persistentId = id;
    ep.systemPath = path;
    ep.displayName = path;
    ep.knownOeabtCandidate = backend == np::BackendKind::Oeabt;
    return ep;
}

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout = 10000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

// Candidate accounting: every listed candidate has exactly one status.
bool accountingConserved(const DiscoverySnapshot& s)
{
    std::size_t identified = 0, unidentified = 0, ambiguous = 0, unsupported = 0;
    for (const auto& c : s.candidates) {
        switch (c.identification) {
        case IdentificationStatus::Identified: ++identified; break;
        case IdentificationStatus::Unidentified: ++unidentified; break;
        case IdentificationStatus::Ambiguous: ++ambiguous; break;
        case IdentificationStatus::Unsupported: ++unsupported; break;
        }
    }
    return identified + unidentified + ambiguous + unsupported == s.candidates.size() &&
           s.candidates.size() <= kMaxCandidates;
}

} // namespace

int main()
{
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_DISABLED_SERVICES", "yolo,auto_update,trigger");
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir dir("e2e_device_discovery");

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize(dir.path().string()), "backend initializes");
    auto& service = backend.deviceDiscovery();
    auto& coordinator = backend.startupDiscovery();

    // Fake serial bench behind the real SerialBusManager / PulseGeneratorService.
    mib::test::FakeModbusBench bench;
    mib::test::FakeModbusWire generator;
    generator.address = 3;
    mib::test::makeGeneratorLike(generator);
    mib::test::FakeModbusWire foreign;
    foreign.address = 5;
    foreign.exceptionOnRead = true;
    mib::test::FakeModbusWire garbage;
    garbage.address = 9;
    garbage.corruptRead = true;
    bench.attach("fake-bus", &generator);
    bench.attach("fake-bus", &foreign);
    bench.attach("fake-bus", &garbage);
    std::mutex timelineMutex;
    std::vector<std::string> timeline;
    auto record = [&](std::string event) {
        std::lock_guard<std::mutex> lk(timelineMutex);
        timeline.push_back(std::move(event));
    };
    bench.onClose = [&](const std::string& name) { record("port-closed:" + name); };
    backend.serialBus().setSerialPortFactory([&] { return std::make_unique<mib::test::FakeModbusPort>(bench); });

    // Fake nanopositioner driver behind the real AutofocusService; the real
    // NanopositionerProvider with a fake enumeration/probe seam.
    auto nanoState = std::make_shared<FakeNanoState>();
    MIB_REQUIRE(backend.autofocus().setBackendFactory(
                    [nanoState](np::BackendKind) -> std::unique_ptr<np::INanopositionerBackend> {
                        return std::make_unique<FakeNanoBackend>(nanoState);
                    }),
                "fake nanopositioner driver installed");
    MIB_REQUIRE(service.unregisterProvider("nanopositioner"), "production nanopositioner provider swapped");
    const auto stage = endpoint(np::BackendKind::Oeabt, "usb-oeabt-7", "COM7");
    const auto adapter = endpoint(np::BackendKind::Oeabt, "usb-ftdi-3", "COM3");
    std::atomic<int> probes{0};
    service.registerProvider(std::make_unique<NanopositionerProvider>(
        [&] { return std::vector{adapter, stage}; },
        [&](const np::Endpoint& ep) {
            ++probes;
            return ep.systemPath == "COM7";
        }));
    // Camera providers stay real: the configured mock camera skips the step
    // before any SDK is touched.

    std::mutex outcomesMutex;
    std::vector<StartupDiscoveryCoordinator::CameraOutcome> cameraOutcomes;
    std::vector<StartupDiscoveryCoordinator::NanopositionerOutcome> nanoOutcomes;
    coordinator.setCameraListener([&](const auto& o) {
        std::lock_guard<std::mutex> lk(outcomesMutex);
        cameraOutcomes.push_back(o);
    });
    coordinator.setNanopositionerListener([&](const auto& o) {
        std::lock_guard<std::mutex> lk(outcomesMutex);
        nanoOutcomes.push_back(o);
    });
    StartupDiscoveryCoordinator::Timing timing;
    timing.cameraDelay = 10ms;
    timing.nanopositionerRetries = 3;
    timing.nanopositionerRetryDelay = 10ms;
    coordinator.setTiming(timing);

    // ---- 1. startup policy ---------------------------------------------------------
    watchdog.mark("startup");
    coordinator.start();
    MIB_REQUIRE(waitFor([&] { return backend.autofocus().isConnected(); }),
                "startup connects the unique identified nanopositioner through AutofocusService");
    MIB_REQUIRE(waitFor([&] { return !coordinator.nanopositionerStepRunning(); }), "startup step ends");
    MIB_EXPECT(nanoState->connects.load() == 1, "exactly one connection attempt");
    MIB_EXPECT(nanoState->writes.load() == 0, "no voltage writes during discovery or connect");
    MIB_EXPECT(probes.load() == 2, "both endpoints probed once, no retry needed");
    {
        std::lock_guard<std::mutex> lk(outcomesMutex);
        bool skipped = false, connected = false;
        for (const auto& o : cameraOutcomes) skipped |= o.kind == StartupDiscoveryCoordinator::CameraOutcome::Kind::Skipped;
        for (const auto& o : nanoOutcomes) {
            if (o.kind == StartupDiscoveryCoordinator::NanopositionerOutcome::Kind::Connected) {
                connected = true;
                MIB_EXPECT(o.endpoint && o.endpoint->systemPath == "COM7", "connected outcome names COM7");
                MIB_EXPECT(accountingConserved(o.snapshot), "nanopositioner snapshot accounting conserved");
                MIB_EXPECT(o.snapshot.candidates.size() == 2, "identified stage and unidentified adapter listed");
            }
        }
        MIB_EXPECT(skipped, "configured mock camera skips the camera step (no SDK touched)");
        MIB_EXPECT(connected, "nanopositioner connected outcome delivered");
    }
    MIB_EXPECT(!coordinator.runNanopositionerStep(), "connected: manual step refused");

    // ---- 2. explicit pulse-generator scan over the real service graph ---------------
    watchdog.mark("pulse-generator");
    DiscoveryRequest scan;
    scan.kinds = {DeviceKind::PulseGenerator};
    scan.serialScope = SerialScanScope{};
    scan.serialScope->portName = "fake-bus";
    scan.serialScope->settings.baudRate = 9600;
    scan.serialScope->addressFrom = 1;
    scan.serialScope->addressTo = 10;
    scan.serialScope->perAddressTimeoutMs = 20;
    scan.origin = "e2e";
    auto job = service.startDiscovery(scan);
    MIB_REQUIRE(job.accepted && service.waitForTerminal(job.jobId, 15000ms), "scan terminates");
    auto snap = service.discoverySnapshot(job.jobId);
    MIB_EXPECT(snap.state == JobState::Completed && snap.complete, "scan completed with full coverage");
    MIB_EXPECT(accountingConserved(snap), "scan accounting conserved");
    MIB_REQUIRE(snap.candidates.size() == 3, "generator, foreign device and corrupt responder listed");
    std::size_t identifiedGenerators = 0;
    for (const auto& c : snap.candidates) {
        if (c.endpoint.busAddress == 3) {
            identifiedGenerators += c.identification == IdentificationStatus::Identified;
        } else {
            MIB_EXPECT(c.identification == IdentificationStatus::Unidentified,
                       "foreign/corrupt responders are never identified as generators");
        }
    }
    MIB_EXPECT(identifiedGenerators == 1, "exactly the generator address is identified");
    MIB_EXPECT(generator.writeCommands.load() == 0 && foreign.writeCommands.load() == 0 &&
                   garbage.writeCommands.load() == 0,
               "discovery issued no write function codes on the bus");
    MIB_EXPECT(!backend.pulseGenerator().isConnected(), "discovery did not connect the generator");
    MIB_EXPECT(waitFor([&] { return bench.openPorts.load() == 0; }, 2000ms), "scan released the shared port");

    // ---- 3. a cancelled scan leaves the established connection alone ----------------
    watchdog.mark("cancel");
    auto* blocker = new ScriptedProvider("blocker", DeviceKind::PulseGenerator, "serial-probe");
    blocker->block = true;
    service.registerProvider(std::unique_ptr<ScriptedProvider>(blocker));
    DiscoveryRequest blocked = scan;
    blocked.providers = {"blocker"};
    job = service.startDiscovery(blocked);
    MIB_REQUIRE(job.accepted && blocker->waitUntilEntered(2000ms), "blocked scan entered");
    service.cancelDiscovery(job.jobId);
    MIB_REQUIRE(service.waitForTerminal(job.jobId, 5000ms), "cancelled scan terminates");
    MIB_EXPECT(service.discoverySnapshot(job.jobId).state == JobState::Cancelled, "scan cancelled");
    MIB_EXPECT(backend.autofocus().isConnected() && nanoState->connected.load(),
               "cancelling a scan never disconnects the established nanopositioner");
    MIB_EXPECT(nanoState->writes.load() == 0, "still no motion commands");

    // ---- 4. real generator connection shares the bus with a later scan ----------------
    watchdog.mark("shared-bus");
    MIB_REQUIRE(backend.pulseGenerator().connect("fake-bus", 9600, 3), "generator connects on the shared bus");
    DiscoveryRequest liveScan = scan;
    liveScan.providers = {"pulse-generator"}; // the blocking fake registered above must not join
    job = service.startDiscovery(liveScan);
    MIB_REQUIRE(job.accepted && service.waitForTerminal(job.jobId, 15000ms), "scan with a live session terminates");
    snap = service.discoverySnapshot(job.jobId);
    MIB_EXPECT(snap.state == JobState::Completed && snap.candidates.size() == 3,
               "scan reuses the connected session instead of reporting Busy");
    MIB_EXPECT(generator.writeCommands.load() == 0, "connected scan still read-only");
    MIB_EXPECT(backend.pulseGenerator().isConnected(), "scan did not disturb the live generator session");

    // ---- 5. shutdown with a blocked job in flight -----------------------------------
    watchdog.mark("shutdown");
    blocker->reset();
    job = service.startDiscovery(blocked);
    MIB_REQUIRE(job.accepted && blocker->waitUntilEntered(2000ms), "job blocked before shutdown");
    const auto observer = service.addObserver([&](const DiscoverySnapshot& s) {
        if (isTerminal(s.state)) record("job-terminal:" + std::to_string(s.jobId));
    });
    const int connectsBefore = nanoState->connects.load();
    backend.shutdown();
    MIB_EXPECT(service.isShutdown() && coordinator.isStopped(), "shutdown stopped discovery and the policy");
    MIB_EXPECT(service.activeWorkerCount() == 0, "no discovery worker survives shutdown");
    MIB_EXPECT(service.discoverySnapshot(job.jobId).state == JobState::Cancelled, "in-flight job cancelled");
    MIB_EXPECT(!backend.autofocus().isConnected() && !backend.pulseGenerator().isConnected(),
               "hardware released by shutdown (#417)");
    {
        std::lock_guard<std::mutex> lk(timelineMutex);
        std::size_t terminalAt = timeline.size(), closedAt = timeline.size();
        for (std::size_t i = 0; i < timeline.size(); ++i) {
            if (timeline[i] == "job-terminal:" + std::to_string(job.jobId) && terminalAt == timeline.size()) terminalAt = i;
            if (timeline[i].rfind("port-closed:", 0) == 0 && i > terminalAt && closedAt == timeline.size()) closedAt = i;
        }
        MIB_EXPECT(terminalAt < timeline.size(), "job end observed");
        MIB_EXPECT(closedAt < timeline.size() && terminalAt < closedAt,
                   "discovery drained before the shared serial port was released");
    }
    std::this_thread::sleep_for(50ms);
    MIB_EXPECT(nanoState->connects.load() == connectsBefore, "no connection attempt after shutdown");
    MIB_EXPECT(!coordinator.runCameraStep() && !coordinator.runNanopositionerStep(),
               "stopped policy refuses new steps");
    service.removeObserver(observer);
    backend.shutdown(); // idempotent
    return mib::test::exitCode();
}
