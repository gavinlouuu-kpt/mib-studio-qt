// camera_autostart_discovery_test (issue #905)
//
// On the MIB rig's first launch, the Overview camera auto-start opened the Coaxlink device while
// the Connect tab's discovery job was still probing it, and the open failed with "operation not
// allowed" (the eGrabber probe holds the grabber for the length of the probe). The operator had to
// press Start Live View.
//
// What this test pins, through a real MainWindow on a mock camera. A fake camera discovery provider
// (resource class "camera-sdk", like the eGrabber providers) holds the device until the test
// releases it, and the camera refuses to open while the device is held:
// 1. Showing the Overview tab while a camera discovery job probes the device does not open the
//    camera. Once the job completes, the auto-start opens it, with no failed open.
// 2. Leaving the Overview tab before the probe ends drops the deferred auto-start: the camera stays
//    off.
// 3. With no camera discovery in flight, the auto-start opens the camera right away.

#include "backend/app/AppBackend.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/discovery/IDeviceDiscoveryProvider.h"
#include "backend/services/CaptureService.h"
#include "frontend/core/MainWindow.h"
#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QMessageBox>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
constexpr int kConnectTab = 0;
constexpr int kOverviewTab = 1;

// The camera device both the probe and the camera open.
struct Device {
    std::mutex mutex;
    std::condition_variable cv;
    bool held{false};    // a probe has the device open
    bool release{false}; // the test lets the probe finish
    std::atomic<int> opens{0};
    std::atomic<int> failedOpens{0};

    bool isHeld()
    {
        std::lock_guard<std::mutex> lk(mutex);
        return held;
    }
    void hold()
    {
        std::lock_guard<std::mutex> lk(mutex);
        release = false;
    }
    void letGo()
    {
        {
            std::lock_guard<std::mutex> lk(mutex);
            release = true;
        }
        cv.notify_all();
    }
};

// Holds the device for the length of discover(), like EGrabberDiscovery in the eGrabber providers.
class HoldingProbe final : public backend::discovery::IDeviceDiscoveryProvider {
public:
    explicit HoldingProbe(Device& device) : device_(device) {}
    std::string id() const override { return "test-holding-probe"; }
    backend::discovery::DeviceKind kind() const override { return backend::discovery::DeviceKind::Camera; }
    std::string resourceClass() const override { return "camera-sdk"; }
    bool cancellable() const override { return false; }
    backend::discovery::ProviderResult discover(const backend::discovery::DiscoveryRequest&,
                                                const backend::discovery::ProviderContext&) override
    {
        std::unique_lock<std::mutex> lk(device_.mutex);
        device_.held = true;
        device_.cv.notify_all();
        device_.cv.wait_for(lk, std::chrono::seconds(30), [this] { return device_.release; });
        device_.held = false;
        return {};
    }

private:
    Device& device_;
};

// A mock camera that cannot open while a probe holds the device.
class ExclusiveCamera final : public camera::mock::MockCamera {
public:
    ExclusiveCamera(camera::mock::MockCameraOptions options, Device& device)
        : MockCamera(std::move(options)), device_(device)
    {
    }
    bool start() override
    {
        if (device_.isHeld()) {
            ++device_.failedOpens;
            failure_ = {"access-denied", "operation not allowed"};
            std::fprintf(stderr, "ExclusiveCamera: open refused, a discovery probe holds the device\n");
            return false;
        }
        ++device_.opens;
        return MockCamera::start();
    }
    camera::common::CameraFailure lastFailure() const override { return failure_; }

private:
    Device& device_;
    camera::common::CameraFailure failure_;
};

bool waitFor(const std::function<bool()>& condition, std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition()) return true;
        std::this_thread::yield();
    }
    return condition();
}

void pump(std::chrono::milliseconds duration)
{
    waitFor([] { return false; }, duration);
}
} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL",
            QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,syringe_pump,pulse_generator"));
    mib::test::Watchdog watchdog(120);
    QApplication app(argc, argv); // widgets: a real MainWindow and its tab widget

    mib::test::TempDir dir("camera_autostart_discovery");
    QCoreApplication::setOrganizationName("mib_camera_autostart_test");
    QCoreApplication::setApplicationName("mib_camera_autostart_test");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString(dir.path().string()));
    const auto frames = dir / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 8, 96, 96), "mock frames");

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((dir / "data").string()), "initialize");
    auto& discovery = backend.deviceDiscovery();
    for (const auto& id : discovery.providerIds())
        MIB_REQUIRE(discovery.unregisterProvider(id), "disable hardware discovery");

    camera::mock::MockCameraOptions options;
    options.folder = frames;
    options.loopFiles = true;
    options.frameInterval = std::chrono::milliseconds(5);
    backend.configureMockCamera(options);

    MainWindow window(backend);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("tabs"));
    MIB_REQUIRE(tabs && tabs->count() == 4, "the window's Connect/Overview/Experiment/Review tabs");

    // The window's own launch jobs (the Connect tab refresh, the startup nanopositioner step, which
    // retries) run without providers; cancel them so only this test's jobs run from here on.
    pump(std::chrono::milliseconds(200));
    for (std::uint64_t job = 1; job <= 8; ++job) discovery.cancelDiscovery(job);
    MIB_REQUIRE(waitFor([&] { return discovery.activeWorkerCount() == 0; }), "launch discovery jobs finished");

    Device device;
    discovery.registerProvider(std::make_unique<HoldingProbe>(device));
    backend.capture().setCameraFactory(
        [options, &device]() { return std::make_unique<ExclusiveCamera>(options, device); });

    // Whatever the window shows on a failed start is closed.
    QTimer dialogCloser;
    dialogCloser.setInterval(10);
    QObject::connect(&dialogCloser, &QTimer::timeout, &window, [] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->close();
    });
    dialogCloser.start();

    auto& capture = backend.capture();
    auto cameraReady = [&] { return capture.lifecycleSnapshot().cameraReady; };
    auto startProbe = [&](const char* origin) {
        device.hold();
        backend::discovery::DiscoveryRequest request;
        request.kinds = {backend::discovery::DeviceKind::Camera, backend::discovery::DeviceKind::Framegrabber};
        request.origin = origin;
        const auto start = discovery.startDiscovery(request);
        MIB_REQUIRE(start.accepted && !start.coalesced, std::string("start the ") + origin + " probe");
        MIB_REQUIRE(waitFor([&] { return device.isHeld(); }), std::string("the ") + origin + " probe holds the device");
        return start.jobId;
    };
    auto finishProbe = [&](std::uint64_t job) {
        device.letGo();
        MIB_REQUIRE(discovery.waitForTerminal(job, std::chrono::seconds(10)), "the probe job ends");
    };
    auto showConnect = [&] {
        tabs->setCurrentIndex(kConnectTab);
        capture.stop();
        MIB_REQUIRE(!capture.isRunning(), "the camera is off on the Connect tab");
    };

    MIB_REQUIRE(tabs->currentIndex() == kConnectTab, "the window opens on the Connect tab");
    MIB_REQUIRE(!capture.isRunning(), "the camera is off at launch");

    // ---- 1. Overview while a probe holds the device: the auto-start waits for the probe ----------
    {
        const auto job = startProbe("connect-tab");
        tabs->setCurrentIndex(kOverviewTab);
        pump(std::chrono::milliseconds(500));
        MIB_EXPECT(device.failedOpens.load() == 0,
                   "the Overview auto-start does not open the camera while a discovery probe holds it");
        MIB_EXPECT(!cameraReady(), "the camera is not started while the probe runs");

        finishProbe(job);
        MIB_EXPECT(waitFor(cameraReady), "once the probe ends, the Overview auto-start opens the camera");
        MIB_EXPECT(device.failedOpens.load() == 0, "no failed open: the operator need not press Start Live View");
        MIB_EXPECT(device.opens.load() == 1, "the camera was opened once");
    }

    // ---- 2. leaving the Overview tab before the probe ends drops the deferred auto-start ----------
    {
        showConnect();
        const int opensBefore = device.opens.load();
        const int failedBefore = device.failedOpens.load();
        const auto job = startProbe("connect-tab-2");
        tabs->setCurrentIndex(kOverviewTab);
        pump(std::chrono::milliseconds(200));
        tabs->setCurrentIndex(kConnectTab);
        finishProbe(job);
        pump(std::chrono::milliseconds(800));
        MIB_EXPECT(!capture.isRunning(), "a deferred auto-start does not fire after the Overview tab was left");
        MIB_EXPECT(device.opens.load() == opensBefore, "no open after leaving the Overview tab");
        MIB_EXPECT(device.failedOpens.load() == failedBefore, "no failed open");
    }

    // ---- 3. no discovery in flight: the auto-start is immediate ---------------------------------
    {
        showConnect();
        const int failedBefore = device.failedOpens.load();
        MIB_REQUIRE(discovery.activeWorkerCount() == 0, "no discovery job in flight");
        tabs->setCurrentIndex(kOverviewTab);
        MIB_EXPECT(capture.isRunning(), "with no probe in flight, showing the Overview tab starts the camera at once");
        MIB_EXPECT(waitFor(cameraReady), "the camera opens");
        MIB_EXPECT(device.failedOpens.load() == failedBefore, "no failed open");
    }

    dialogCloser.stop();
    capture.stop();
    backend.shutdown();
    return mib::test::exitCode();
}
