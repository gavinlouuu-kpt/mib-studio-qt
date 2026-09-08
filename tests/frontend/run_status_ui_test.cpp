// run_status_ui_test (issue #363)
//
// MainWindow surfaces (offscreen, mock backend):
//  - an alert raised before 25 statistics ticks is still shown after them
//    (metrics refresh cannot overwrite it) and the status text is bounded;
//  - a camera-failure alert survives ticks; acknowledging hides the banner
//    but leaves the condition unresolved;
//  - run lifecycle text + glyph follow the model (Idle / Running / Saving /
//    Failed), a latched failure stays Failed after Complete, no color-only
//    semantics;
//  - long diagnostic values never change the window's required width;
//  - the banner's buttons are keyboard reachable and the Diagnostics dialog
//    opens without touching alerts.

#include "backend/app/AppBackend.h"
#include "frontend/core/MainWindow.h"
#include "frontend/models/RunStatusModel.h"
#include "frontend/utils/ApplicationSettings.h"
#include "frontend/utils/ElidingLabel.h"
#include "frontend/widgets/AlertBanner.h"
#include "frontend/widgets/RunStatusWidget.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStyleFactory>
#include <QToolButton>

namespace {
void settle(int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}
} // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,yolo,syringe_pump,pulse_generator"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR")) qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    mib::test::Watchdog wd(180);
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    mib::test::TempDir td("run_status_ui");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");

    MainWindow window(backend);
    window.setAvailableGeometryOverrideForTests(QRect(0, 0, 1366, 728));
    window.resize(1366, 728);
    window.show();
    settle(10);
    auto* alerts = window.alertModel();
    auto* run = window.runStatusModel();
    auto* banner = window.alertBanner();
    auto* status = window.runStatusWidget();
    MIB_REQUIRE(alerts && run && banner && status, "surfaces exist");
    MIB_EXPECT(!banner->isVisible(), "no alert -> banner hidden");
    MIB_EXPECT(status->text() == frontend::runPhaseLabel(frontend::RunPhase::Idle), "idle text");
    const int minWidth = window.minimumSizeHint().width();

    // ---- 1. alerts survive metrics refreshes ---------------------------------
    wd.mark("alerts");
    alerts->raise(QStringLiteral("save.fatal"), frontend::AlertSeverity::Critical, QStringLiteral("disk full while writing run.h5"),
                  QStringLiteral("Free space on the destination"));
    settle(2);
    MIB_EXPECT(banner->isVisible() && banner->summaryText().contains(QStringLiteral("disk full")), "critical alert shown");
    const QString shown = banner->summaryText();
    for (int i = 0; i < 25; ++i) {
        QMetaObject::invokeMethod(&window, "onUpdateStats", Qt::DirectConnection);
        settle(1);
    }
    MIB_EXPECT(banner->isVisible() && banner->summaryText() == shown, "25 stats ticks did not touch the alert");
    MIB_EXPECT(!window.compactStatusText().isEmpty() && window.compactStatusText().contains(QStringLiteral("Valid")),
               "compact metrics rendered separately");
    MIB_EXPECT(!window.compactStatusText().contains(QStringLiteral("MB/s")), "verbose transport values are not in the compact status");
    MIB_EXPECT(window.minimumSizeHint().width() == minWidth, "status rendering does not change the required width");
    // Camera failure alert + acknowledgement semantics.
    for (int i = 0; i < 100; ++i)
        alerts->raise(QStringLiteral("camera.start"), frontend::AlertSeverity::Error, QStringLiteral("Camera start failed"), QStringLiteral("Check the cable"));
    settle(2);
    MIB_EXPECT(alerts->find(QStringLiteral("camera.start"))->count == 100 && alerts->unresolvedCount() == 2, "repeats grouped");
    MIB_EXPECT(banner->summaryText().contains(QStringLiteral("+1 more")), "banner counts further alerts");
    MIB_EXPECT(banner->acknowledgeButton()->focusPolicy() != Qt::NoFocus && banner->detailsButton()->focusPolicy() != Qt::NoFocus,
               "banner buttons keyboard reachable");
    banner->detailsButton()->setChecked(true);
    settle(2);
    MIB_EXPECT(banner->detailsVisible(), "details expand");
    banner->acknowledgeButton()->click();
    settle(2);
    MIB_EXPECT(!banner->isVisible(), "acknowledged -> banner hidden");
    MIB_EXPECT(alerts->unresolvedCount(frontend::AlertSeverity::Error) == 2, "acknowledging does not resolve the conditions");
    alerts->raise(QStringLiteral("camera.start"), frontend::AlertSeverity::Error, QStringLiteral("Camera start failed again"));
    settle(2);
    MIB_EXPECT(banner->isVisible(), "a repeat re-surfaces the alert");
    alerts->resolve(QStringLiteral("camera.start"));
    alerts->resolve(QStringLiteral("save.fatal"));
    settle(2);
    MIB_EXPECT(!banner->isVisible(), "resolved -> hidden");

    // ---- 2. run lifecycle text ---------------------------------------------------
    wd.mark("run state");
    const uint64_t op = run->beginOperation(frontend::RunPhase::Starting, QStringLiteral("run1.h5"));
    settle(1);
    MIB_EXPECT(status->text().startsWith(frontend::runPhaseLabel(frontend::RunPhase::Starting)), "Starting text");
    run->setPhase(frontend::RunPhase::Running, op, QStringLiteral("run1.h5"));
    run->setPhase(frontend::RunPhase::Stopping, op);
    run->setPhase(frontend::RunPhase::Saving, op);
    settle(1);
    MIB_EXPECT(status->text().startsWith(frontend::runPhaseLabel(frontend::RunPhase::Saving)), "Saving text");
    run->latchFailure(op, QStringLiteral("metadata write failed"));
    run->setPhase(frontend::RunPhase::Complete, op);
    settle(1);
    MIB_EXPECT(status->phase() == frontend::RunPhase::Failed && status->text().contains(QStringLiteral("metadata")),
               "latched failure keeps Failed after Complete");
    MIB_EXPECT(!run->setPhase(frontend::RunPhase::Complete, op + 7), "stale/unknown operation rejected");
    for (int i = 0; i < 10; ++i) QMetaObject::invokeMethod(&window, "onUpdateStats", Qt::DirectConnection);
    settle(2);
    MIB_EXPECT(status->phase() == frontend::RunPhase::Failed, "stats ticks never erase the failed state");
    MIB_EXPECT(!status->accessibleName().isEmpty() && status->accessibleName().contains(QStringLiteral("Failed")), "accessible state text");
    MIB_EXPECT(window.minimumSizeHint().width() == minWidth, "long failure detail does not widen the window");

    // ---- 3. diagnostics -------------------------------------------------------------
    wd.mark("diagnostics");
    QMetaObject::invokeMethod(&window, "showDiagnostics", Qt::DirectConnection);
    settle(3);
    auto* diag = window.findChild<QDialog*>(QStringLiteral("diagnosticsDialog"));
    auto* diagText = window.findChild<QPlainTextEdit*>(QStringLiteral("diagnosticsText"));
    MIB_EXPECT(diag && diag->isVisible() && diagText && diagText->toPlainText().contains(QStringLiteral("Processing core")),
               "diagnostics dialog shows detailed identities/telemetry");
    const int alertsBefore = alerts->all().size();
    for (int i = 0; i < 5; ++i) QMetaObject::invokeMethod(&window, "onUpdateStats", Qt::DirectConnection);
    settle(2);
    MIB_EXPECT(alerts->all().size() == alertsBefore, "diagnostics refresh adds no alerts");
    if (diag) diag->hide();
    window.close();
    settle(4);
    backend.shutdown();
    return mib::test::exitCode();
}
