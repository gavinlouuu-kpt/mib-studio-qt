// dot_grid_overview_test
//
// Dot-grid wafer localization runs on the Overview tab only (offscreen, real
// widgets, mock backend with the DotGridService thread running):
//  - the Wafer Grid toggle exists on the Overview and not on the Experiment
//    Preview page;
//  - while another tab (the Experiment Preview page) is current, the service
//    is paused and decodes nothing, even with Wafer Grid on and new frames;
//  - showing the Overview resumes it: the frame is decoded and the overlay
//    carries the pose; switching away pauses it again;
//  - the toggle drives DotGridService::Config::enabled.

#include "backend/app/AppBackend.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/DotGridDecoder.h"
#include "backend/services/DotGridService.h"
#include "frontend/tabs/ConfigTabs.h"
#include "frontend/tabs/OverviewTab.h"
#include "frontend/tabs/PreviewPage.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/opencv_tsan.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSettings>
#include <QTabWidget>
#include <QThread>
#include <QToolButton>

#include <cmath>
#include <functional>

namespace {

constexpr uint64_t kMono8 = 0x01080001;

void settleMs(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QThread::msleep(5);
    }
}

bool waitFor(const std::function<bool()>& cond, int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (cond()) return true;
        settleMs(20);
    }
    return cond();
}

int waferGridButtons(const QWidget& root)
{
    int n = 0;
    for (const auto* b : root.findChildren<QToolButton*>())
        if (b->text().startsWith(QStringLiteral("Wafer Grid"))) ++n;
    return n;
}

} // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    // Keep playback + dot_grid; nothing that reaches for hardware or the network.
    qputenv("MIB_DISABLED_SERVICES",
            QByteArrayLiteral("sqlite,hdf5,processing,autofocus,trigger,capture,auto_update"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    mib::test::serializeOpenCvUnderTsan();
    mib::test::Watchdog wd(180);
    QApplication app(argc, argv);
    mib::test::TempDir td("dot_grid_overview");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((td.path() / "settings").string()));
    QString settingsErr;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&settingsErr), "settings init");

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    auto& service = backend.dotGrid();
    MIB_REQUIRE(service.isRunning(), "dot-grid service thread running");

    backend::services::DotGridService::Config cfg = service.getConfig();
    cfg.enabled = false;
    cfg.intervalMs = 20;
    cfg.umPerPxHint = 0.3;
    std::string err;
    MIB_REQUIRE(service.setConfig(cfg, &err), "config: " + err);

    // The Experiment tab's Preview page and the Overview, in one tab widget
    // as in MainWindow; the Preview page is current first.
    wd.mark("build tabs");
    QTabWidget tabs;
    auto* preview = new frontend::PreviewPage(backend);
    preview->getConfigTabs()->setNonInteractiveForTests(true);
    auto* overview = new frontend::OverviewTab(backend);
    const int previewIndex = tabs.addTab(preview, QStringLiteral("Experiment"));
    const int overviewIndex = tabs.addTab(overview, QStringLiteral("Overview"));
    tabs.setCurrentIndex(previewIndex);
    tabs.resize(1366, 800);
    tabs.show();
    settleMs(100);

    MIB_EXPECT(waferGridButtons(*preview) == 0, "no Wafer Grid toggle on the Experiment Preview page");
    MIB_REQUIRE(waferGridButtons(*overview) == 1, "Wafer Grid toggle on the Overview");
    auto* toggle = overview->findChild<QToolButton*>(QStringLiteral("overviewWaferGridBtn"));
    MIB_REQUIRE(toggle != nullptr, "toggle found by object name");
    MIB_EXPECT(service.isPaused(), "paused while the Overview is not on screen");

    // Wafer Grid on (from the Overview's toggle) while the Experiment page is current.
    wd.mark("toggle on");
    toggle->click();
    MIB_EXPECT(service.isEnabled(), "toggle enables localization");
    MIB_EXPECT(toggle->text() == QStringLiteral("Wafer Grid: On"), "toggle shows On");

    // Frames arriving while the Experiment page is current are not decoded.
    const backend::dotgrid::Codebook codebook = backend::dotgrid::Codebook::generate(cfg.codebook);
    auto pushFrameAt = [&](double x, double y, uint64_t ts) {
        backend::dotgrid::ViewPose pose;
        pose.centreXUm = x;
        pose.centreYUm = y;
        pose.thetaDeg = 14.0;
        pose.umPerPx = 0.293;
        pose.mirrored = true;
        const cv::Mat img = backend::dotgrid::renderView(codebook, pose, backend::dotgrid::RenderOptions{});
        backend.getFrameStore()->pushFrame(img.data, img.total(), static_cast<uint64_t>(img.cols),
                                           static_cast<uint64_t>(img.rows), static_cast<size_t>(img.step),
                                           kMono8, ts);
    };
    wd.mark("experiment tab");
    pushFrameAt(41000.0, 53000.0, 1000);
    settleMs(300);
    MIB_EXPECT(service.decodeAttempts() == 0, "nothing decoded while the Experiment tab is current");

    // Showing the Overview resumes decoding; the overlay shows the pose.
    wd.mark("overview tab");
    tabs.setCurrentIndex(overviewIndex);
    MIB_EXPECT(waitFor([&] { return !service.isPaused(); }, 1000), "resumed when the Overview is shown");
    const bool decoded = waitFor([&] {
        const auto& o = overview->dotGridOverlay();
        return o.active && o.valid && !o.dots.isEmpty();
    }, 5000);
    MIB_EXPECT(decoded, "Overview overlay shows a decoded pose");
    backend::services::DotGridService::Pose pose;
    MIB_EXPECT(service.getLatestPose(pose) && pose.valid && std::abs(pose.centreXUm - 41000.0) < 1.0 &&
                   std::abs(pose.centreYUm - 53000.0) < 1.0,
               "pose is the frame under the Overview");
    MIB_EXPECT(overview->dotGridOverlay().text.contains(QStringLiteral("Wafer X 41000")),
               "overlay text: " + overview->dotGridOverlay().text.toStdString());

    // Back to the Experiment tab: paused again, new frames ignored.
    wd.mark("back to experiment");
    tabs.setCurrentIndex(previewIndex);
    MIB_EXPECT(waitFor([&] { return service.isPaused(); }, 1000), "paused again when the Overview is hidden");
    settleMs(100); // let an in-flight decode finish
    const uint64_t attempts = service.decodeAttempts();
    pushFrameAt(42000.0, 54000.0, 2000);
    settleMs(300);
    MIB_EXPECT(service.decodeAttempts() == attempts, "no decode after leaving the Overview");

    // Toggle off from the Overview.
    wd.mark("toggle off");
    tabs.setCurrentIndex(overviewIndex);
    settleMs(50);
    toggle->click();
    MIB_EXPECT(!service.isEnabled(), "toggle disables localization");
    settleMs(50);
    MIB_EXPECT(toggle->text() == QStringLiteral("Wafer Grid: Off") && !overview->dotGridOverlay().active,
               "toggle and overlay show Off");

    wd.mark("teardown");
    tabs.hide();
    settleMs(20);
    return mib::test::exitCode();
}
