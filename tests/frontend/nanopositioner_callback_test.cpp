#include "backend/app/AppBackend.h"
#include "backend/services/AutofocusService.h"
#include "frontend/tabs/NanopositionerTab.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <QApplication>
#include <QSettings>
#include <QLabel>
#include <QCoreApplication>
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    mib::test::Watchdog watchdog(30);
    mib::test::TempDir temp("mib_callback");
    QCoreApplication::setOrganizationName("MIB-tests");
    QCoreApplication::setApplicationName("callback-lifetime");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(temp.path().string()));
    QSettings().setValue("Config/ExternalAppConfigPath",
                         QString::fromStdString((temp / "config.json").string()));
    qputenv("MIB_CAMERA_MODE", "mock");
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL",
            QByteArray("file:") +
                QByteArray::fromStdString((temp / "missing-manifest.json").string()));
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize(temp.path().string()), "backend initializes without hardware");
    backend.shutdown(); // No processing workers are needed for UI callback tests.
    spdlog::set_level(spdlog::level::err);
    auto* tab = new frontend::NanopositionerTab(backend);
    auto* label = tab->findChild<QLabel*>("statusLabel");
    MIB_REQUIRE(label, "status label exists");
    backend.autofocus().connect(0, 0, 0);
    QCoreApplication::processEvents();
    MIB_REQUIRE(label->text().contains("unavailable on this platform"), "status reaches live tab");
    delete tab;
    watchdog.mark("callback after UI destruction");
    // Same registered callback as disconnect(), without connecting hardware.
    backend.autofocus().connect(0, 0, 0);
    QCoreApplication::processEvents();
    // A worker may emit while the UI destroys/replaces its subscription. A
    // queued delivery must not change widgets before the UI event loop runs.
    for (int cycle = 0; cycle < 50; ++cycle) {
        watchdog.mark("concurrent tab destruction and replacement");
        tab = new frontend::NanopositionerTab(backend);
        label = tab->findChild<QLabel*>("statusLabel");
        label->setText("sentinel");
        std::thread once([&] { backend.autofocus().connect(0, 0, 0); });
        once.join();
        MIB_REQUIRE(label->text() == "sentinel", "worker does not touch UI directly");
        QCoreApplication::processEvents();
        MIB_REQUIRE(label->text().contains("unavailable on this platform"),
                    "worker status delivered on event loop");
        std::atomic<bool> started{false};
        std::thread producer([&] {
            for (int i = 0; i < 200; ++i) {
                backend.autofocus().connect(0, 0, 0);
                started.store(true);
            }
        });
        while (!started.load())
            std::this_thread::yield();
        delete tab; // queued work is discarded; concurrent queue admission is gated
        tab = new frontend::NanopositionerTab(backend);
        producer.join();
        delete tab;
        QCoreApplication::processEvents();
    }
    // Registration lock must not be held while invoking callbacks.
    bool called = false;
    backend.autofocus().setStatusCallback([&](const std::string&) {
        called = true;
        backend.autofocus().setStatusCallback({});
    });
    backend.autofocus().connect(0, 0, 0);
    MIB_REQUIRE(called, "callback can unregister itself without deadlock");
    backend.shutdown();
    return mib::test::exitCode();
}
