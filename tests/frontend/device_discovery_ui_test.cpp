// device_discovery_ui_test (issue #419)
//
// Qt adapter over the backend discovery service (offscreen, real widgets,
// fake providers swapped in for the production ones):
//  - DeviceInitManager::start() keeps the UI responsive while a camera probe
//    blocks (a timer keeps ticking), then ConnectTab lists the camera and
//    reports the automatic selection; the nanopositioner step shows the
//    identifying status and applies the auto-connect result through the real
//    AutofocusService (fake driver factory);
//  - ConnectTab Refresh while a scan runs is coalesced onto the same job and
//    never enumerates on the UI thread; destroying the tab mid-scan is safe;
//  - stop() cancels the owned job promptly and nothing is applied afterwards;
//  - ConfigTabs' pulse-generator Scan/Cancel runs through the service with an
//    explicit port scope (no tab-owned thread) and survives tab destruction.
#include "backend/app/AppBackend.h"
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/discovery/StartupDiscoveryCoordinator.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/CameraControlService.h"
#include "frontend/system/DeviceInitManager.h"
#include "frontend/tabs/ConfigTabs.h"
#include "frontend/tabs/ConnectTab.h"
#include "frontend/tabs/NanopositionerTab.h"
#include "frontend/utils/ApplicationSettings.h"
#include "support/assert.h"
#include "support/fake_discovery_providers.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

using namespace backend::discovery;
using mib::test::makeCandidate;
using mib::test::ScriptedProvider;
using namespace std::chrono_literals;
namespace np = backend::nanopositioner;

namespace {

void settle(int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}

// Pump the event loop until `pred` holds or `ms` elapsed. Returns pred().
template <typename Pred>
bool pumpUntil(Pred pred, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
    return pred();
}

DiscoveredDevice mindVisionCandidate(int index, const std::string& label)
{
    auto d = makeCandidate(DeviceKind::Camera, "cam", "", "", index, -1,
                           IdentificationStatus::Identified, IdentityStrength::SessionLocal);
    d.displayName = label;
    backend::services::DiscoveredCamera cam;
    cam.cameraType = backend::services::CameraType::MindVision;
    cam.cameraIndex = index;
    cam.modelName = "Fake MV";
    cam.label = label;
    d.camera = cam;
    return d;
}

DiscoveredDevice nanoCandidate(const std::string& id, const std::string& path,
                               IdentificationStatus status = IdentificationStatus::Identified)
{
    auto d = makeCandidate(DeviceKind::Nanopositioner, "np", id, path, -1, -1, status);
    np::Endpoint ep;
    ep.backend = np::BackendKind::Oeabt;
    ep.persistentId = id;
    ep.systemPath = path;
    ep.displayName = path;
    ep.knownOeabtCandidate = true;
    d.nanopositioner = ep;
    return d;
}

struct FakeNanoState {
    std::atomic<int> writes{0};
    std::atomic<bool> connected{false};
};

class FakeNanoBackend final : public np::INanopositionerBackend {
public:
    explicit FakeNanoBackend(std::shared_ptr<FakeNanoState> s) : state_(std::move(s)) {}
    np::BackendKind kind() const override { return np::BackendKind::Oeabt; }
    bool connect(const np::Endpoint&, std::string&) override { state_->connected = true; return true; }
    void disconnect() override { state_->connected = false; }
    bool isConnected() const override { return state_->connected.load(); }
    bool readVoltage(double& v, std::string&) override { v = 10.0; return true; }
    bool setVoltage(double, std::string&) override { ++state_->writes; return true; }
    std::optional<double> maximumVoltage() const override { return 100.0; }
    std::string connectedEndpoint() const override { return "COM7"; }

private:
    std::shared_ptr<FakeNanoState> state_;
};

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // No MIB_CAMERA_MODE: the implicit fallback leaves the camera unconfigured
    // so the startup camera step actually runs (#413).
    qputenv("MIB_DISABLED_SERVICES", "auto_update,trigger,yolo,syringe_pump,pulse_generator");
    mib::test::Watchdog watchdog(60);
    QApplication app(argc, argv);
    mib::test::TempDir dir("device_discovery_ui");
    QCoreApplication::setOrganizationName("mib_discovery_ui_test");
    QCoreApplication::setApplicationName("mib_discovery_ui_test");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(dir.path().string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((dir / "data").string()), "backend initializes");
    auto& service = backend.deviceDiscovery();
    for (const auto& id : service.providerIds()) {
        MIB_REQUIRE(service.unregisterProvider(id), "production provider replaced by a fake");
    }
    auto* cam = new ScriptedProvider("cam", DeviceKind::Camera, "camera-sdk");
    auto* npProvider = new ScriptedProvider("np", DeviceKind::Nanopositioner);
    cam->result.candidates = {mindVisionCandidate(0, "Fake MV (index 0)")};
    npProvider->result.candidates = {nanoCandidate("usb-oeabt-7", "COM7"),
                                     nanoCandidate("", "COM3", IdentificationStatus::Unidentified)};
    service.registerProvider(std::unique_ptr<ScriptedProvider>(cam));
    service.registerProvider(std::unique_ptr<ScriptedProvider>(npProvider));
    auto nanoState = std::make_shared<FakeNanoState>();
    MIB_REQUIRE(backend.autofocus().setBackendFactory(
                    [nanoState](np::BackendKind) -> std::unique_ptr<np::INanopositionerBackend> {
                        return std::make_unique<FakeNanoBackend>(nanoState);
                    }),
                "fake nanopositioner driver installed");
    StartupDiscoveryCoordinator::Timing timing;
    timing.cameraDelay = 20ms;
    timing.nanopositionerRetries = 1;
    timing.nanopositionerRetryDelay = 10ms;
    backend.startupDiscovery().setTiming(timing);

    // ---- startup: responsive UI, camera selection, nanopositioner auto-connect ----
    {
        watchdog.mark("startup");
        auto connectTab = std::make_unique<frontend::ConnectTab>(backend);
        auto nanoTab = std::make_unique<frontend::NanopositionerTab>(backend);
        auto* mvList = connectTab->findChild<QListWidget*>("mindVisionList");
        auto* connectStatus = connectTab->findChild<QLabel*>("statusLabel");
        auto* nanoStatus = nanoTab->findChild<QLabel*>("statusLabel");
        auto* nanoCombo = nanoTab->findChild<QComboBox*>("comPortCombo");
        MIB_REQUIRE(mvList && connectStatus && nanoStatus && nanoCombo, "widgets exist");
        // The constructor lists devices through an asynchronous job (never on
        // the UI thread); let it finish before blocking the startup step.
        MIB_REQUIRE(pumpUntil([&] { return mvList->count() == 1; }, 5000),
                    "constructor refresh lists the camera asynchronously");
        MIB_EXPECT(cam->calls.load() == 1, "constructor refresh ran one enumeration");
        MIB_EXPECT(!connectStatus->text().contains("Connected to"),
                   "a plain refresh lists devices but selects nothing");
        cam->block = true;
        cam->reset();

        frontend::DeviceInitManager manager(backend);
        manager.setConnectTab(connectTab.get());
        manager.setNanopositionerTab(nanoTab.get());
        int cameraFinished = 0, nanoFinished = 0;
        QString cameraMessage;
        QObject::connect(&manager, &frontend::DeviceInitManager::cameraInitFinished,
                         [&](bool ok, const QString& msg) { ++cameraFinished; if (ok) cameraMessage = msg; });
        QObject::connect(&manager, &frontend::DeviceInitManager::nanopositionerInitFinished,
                         [&](bool ok) { if (ok) ++nanoFinished; });

        cam->reset();
        manager.start();
        MIB_REQUIRE(pumpUntil([&] { return cam->entered.load(); }, 3000),
                    "camera probe entered after the initial delay");
        // UI responsiveness: a repeating timer keeps ticking while the probe blocks.
        int ticks = 0;
        QTimer ticker;
        ticker.setInterval(5);
        QObject::connect(&ticker, &QTimer::timeout, [&] { ++ticks; });
        ticker.start();
        pumpUntil([&] { return ticks >= 10; }, 2000);
        ticker.stop();
        MIB_EXPECT(ticks >= 10, "event loop keeps running while the camera probe blocks");
        MIB_EXPECT(connectStatus->text().contains("Scanning"), "startup step shows the scanning state");
        MIB_EXPECT(!connectStatus->text().contains("Connected to"), "nothing selected before the probe returns");

        cam->release();
        MIB_REQUIRE(pumpUntil([&] { return connectStatus->text().contains("Connected to"); }, 5000),
                    "auto-selection reported in ConnectTab after release");
        MIB_EXPECT(mvList->count() == 1, "camera listed");
        MIB_EXPECT(backend.cameraSelection().mode ==
                       backend::AppBackend::CameraSelectionSnapshot::Mode::MindVision,
                   "backend selection applied through the policy hook");
        MIB_EXPECT(backend.cameraSelection().label == "Fake MV (index 0)", "selected label");
        MIB_REQUIRE(pumpUntil([&] { return cameraFinished == 1; }, 2000), "cameraInitFinished emitted once");
        MIB_EXPECT(cameraMessage == "Fake MV (index 0)", "success message carries the label");

        MIB_REQUIRE(pumpUntil([&] { return nanoState->connected.load(); }, 5000),
                    "nanopositioner auto-connected through the real AutofocusService");
        MIB_REQUIRE(pumpUntil([&] { return nanoFinished == 1; }, 2000), "nanopositionerInitFinished(true)");
        MIB_EXPECT(backend.autofocus().isConnected(), "service reports connected");
        MIB_EXPECT(nanoState->writes.load() == 0, "no voltage writes during discovery/connect");
        MIB_EXPECT(nanoCombo->count() == 2, "combo lists identified and unidentified endpoints");
        MIB_EXPECT(!nanoStatus->text().contains("Identifying"), "identifying status cleared");
        MIB_EXPECT(npProvider->calls.load() == 1, "one probe pass, no retry needed");

        // ---- manual refresh coalesces; destroying the tab mid-scan is safe ----
        watchdog.mark("refresh");
        cam->reset();
        cam->block = true;
        auto* refresh = connectTab->findChild<QPushButton*>("refreshBtn");
        MIB_REQUIRE(refresh, "refresh button");
        const int callsBefore = cam->calls.load();
        refresh->click();
        MIB_REQUIRE(pumpUntil([&] { return cam->entered.load(); }, 3000), "refresh started a job");
        refresh->click();
        refresh->click();
        settle();
        MIB_EXPECT(service.activeWorkerCount() == 1, "repeated Refresh coalesces onto the running job");
        MIB_EXPECT(connectStatus->text().contains("Scanning"), "refresh shows a scanning state");
        connectTab.reset(); // destroyed while its job is still blocked
        settle();
        cam->release();
        MIB_REQUIRE(pumpUntil([&] { return service.activeWorkerCount() == 0; }, 5000), "job finishes");
        MIB_EXPECT(cam->calls.load() == callsBefore + 1, "the coalesced refreshes ran one enumeration");
        settle();

        // ---- stop(): prompt, cancels the owned job, nothing applied afterwards ----
        watchdog.mark("stop");
        backend.autofocus().disconnect();
        nanoTab->setDiscoveryRunning(false); // re-evaluate enabled state after the disconnect
        npProvider->block = true;
        npProvider->reset();
        auto* nanoRefresh = nanoTab->findChild<QPushButton*>("refreshComPortBtn");
        MIB_REQUIRE(nanoRefresh && nanoRefresh->isEnabled(), "nanopositioner refresh button enabled");
        settle(10); // drain the autofocus disconnect status message first
        nanoRefresh->click();
        // The Started outcome is applied synchronously inside the click.
        MIB_EXPECT(nanoStatus->text().contains("Identifying"), "identifying status shown");
        MIB_REQUIRE(pumpUntil([&] { return npProvider->entered.load(); }, 3000), "nano probe blocked");
        MIB_EXPECT(!nanoRefresh->isEnabled(), "refresh disabled while identifying");
        const auto job = backend.startupDiscovery().nanopositionerJobId();
        QElapsedTimer stopTimer;
        stopTimer.start();
        manager.stop();
        MIB_EXPECT(stopTimer.elapsed() < 1000, "stop() returns promptly while a probe blocks");
        MIB_REQUIRE(service.waitForTerminal(job, 3000ms), "owned job terminates");
        MIB_EXPECT(service.discoverySnapshot(job).state == JobState::Cancelled, "owned job cancelled");
        settle(10);
        MIB_EXPECT(!backend.autofocus().isConnected(), "nothing connected after stop");
        MIB_EXPECT(nanoFinished == 1, "no further outcome after stop");
        npProvider->release();
        settle();
    }

    // ---- ConfigTabs: pulse-generator scan through the service --------------------
    {
        watchdog.mark("pulse-generator-scan");
        auto* pg = new ScriptedProvider("pg", DeviceKind::PulseGenerator);
        pg->script = [](const DiscoveryRequest& req, const ProviderContext&) {
            ProviderResult r;
            MIB_EXPECT(req.serialScope && req.serialScope->portName == "COM9" &&
                           req.serialScope->addressFrom == 1 && req.serialScope->addressTo == 16,
                       "scan carries the explicit port/address scope");
            auto d = makeCandidate(DeviceKind::PulseGenerator, "pg", "COM9@7", "COM9", -1, 7,
                                   IdentificationStatus::Identified, IdentityStrength::SessionLocal);
            backend::services::PulseGeneratorService::ScanHit hit;
            hit.address = 7;
            hit.kind = backend::services::PulseGeneratorService::ScanHit::Kind::PulseGenerator;
            d.pulseGenerator = hit;
            r.candidates.push_back(d);
            return r;
        };
        pg->block = true;
        service.registerProvider(std::unique_ptr<ScriptedProvider>(pg));

        auto tabs = std::make_unique<frontend::ConfigTabs>(backend);
        tabs->setNonInteractiveForTests(true);
        auto* portCombo = tabs->findChild<QComboBox*>("mvGeneratorPort");
        auto* scanBtn = tabs->findChild<QPushButton*>("pgScanBtn");
        auto* addrSpin = tabs->findChild<QSpinBox*>("pgAddrSpin");
        MIB_REQUIRE(portCombo && scanBtn && addrSpin, "pulse-generator controls exist");
        portCombo->clear();
        portCombo->addItem("COM9 (fake)", QStringLiteral("COM9"));
        addrSpin->setValue(1);
        scanBtn->click();
        MIB_REQUIRE(pumpUntil([&] { return pg->entered.load(); }, 3000), "scan job entered the provider");
        MIB_EXPECT(scanBtn->text().contains("Cancel", Qt::CaseInsensitive), "scan button toggles to cancel");
        scanBtn->click(); // cancel
        MIB_REQUIRE(pumpUntil([&] { return service.activeWorkerCount() == 0; }, 3000), "cancelled scan ends");
        settle();
        MIB_EXPECT(!scanBtn->text().contains("Cancel", Qt::CaseInsensitive), "button restored after cancel");
        MIB_EXPECT(addrSpin->value() == 1, "cancelled scan changes nothing");

        pg->reset();
        pg->block = false;
        scanBtn->click();
        MIB_REQUIRE(pumpUntil([&] { return addrSpin->value() == 7; }, 5000),
                    "completed scan auto-fills the first generator address");

        // Destroy the tab while a scan blocks: no crash, job still terminates.
        pg->reset();
        pg->block = true;
        scanBtn->click();
        MIB_REQUIRE(pumpUntil([&] { return pg->entered.load(); }, 3000), "third scan blocked");
        tabs.reset();
        settle();
        pg->release();
        MIB_REQUIRE(pumpUntil([&] { return service.activeWorkerCount() == 0; }, 5000), "orphaned scan ends");
        settle();
    }

    backend.shutdown();
    return mib::test::exitCode();
}
