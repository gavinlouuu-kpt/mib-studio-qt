#include "backend/app/AppBackend.h"
#include "frontend/core/MainWindow.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <QApplication>
#include <QSettings>
#include <QTimer>
#include <QWidget>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("MIB_CAMERA_MODE", "mock");
    qputenv("MIB_DISABLED_SERVICES",
            "auto_update,autofocus,trigger,syringe_pump,pulse_generator");
    mib::test::Watchdog watchdog(45);
    QApplication app(argc, argv);
    mib::test::TempDir dir("mainwindow_shutdown");
    QCoreApplication::setOrganizationName("mib_shutdown_test");
    QCoreApplication::setApplicationName("mib_shutdown_test");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(dir.path().string()));
    QCoreApplication::setApplicationVersion(QStringLiteral("9.8.7-beta.test"));
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((dir / "data").string()), "initialize");
    // Rig check F1: a data dir outside Program Files keeps its log beside it.
    MIB_EXPECT(backend.logDir() == dir / "data" / "logs", "the log dir is <data dir>/logs");
    MainWindow window(backend);
    // Rig check F4: the title names the version, not "Scaffold".
    MIB_EXPECT(window.windowTitle() == QStringLiteral("MIB Studio Qt 9.8.7-beta.test"),
               "the main window title is MIB Studio Qt <version>");
    QWidget auxiliary; // A detached utility window must not keep hardware alive.
    auxiliary.show();
    window.show();
    bool timedOut = false;
    QTimer::singleShot(0, &window, [&] { window.close(); });
    QTimer::singleShot(1000, &app, [&] {
        timedOut = true;
        app.quit();
    });
    app.exec();
    MIB_EXPECT(!timedOut, "closing main window exits even with another top-level widget");
    return mib::test::exitCode();
}
