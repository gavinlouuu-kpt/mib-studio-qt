// dot_grid_config_test
//
// AppConfigWatcher applying the dot_grid block of a real temp config.json
// (offscreen, mock backend):
//  - a 64-bit codebook seed above 2^53 is applied exactly (JSON integer), and
//    one above 2^63 is accepted as a decimal string; a fractional seed is
//    rejected and the previous seed kept;
//  - the Overview's runtime Wafer Grid toggle survives a config.json reload
//    that does not change dot_grid.enabled; a reload that does change it is
//    applied.

#include "backend/app/AppBackend.h"
#include "backend/services/DotGridService.h"
#include "frontend/system/AppConfigWatcher.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
#include <QThread>

#include <functional>

namespace {

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

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(bytes);
}

QByteArray config(const char* enabled, const char* seed, int bufferThreshold)
{
    return QByteArray(R"({
  "buffer_threshold": )") + QByteArray::number(bufferThreshold) + R"(,
  "dot_grid": {
    "enabled": )" + enabled + R"(,
    "interval_ms": 250,
    "codebook_path": "",
    "registry_path": "",
    "codebook": {"seed": )" + seed + R"(, "columns": 200, "rows": 200,
                 "pitch_um": 30.0, "dot_diameter_um": 12.0, "displacement_um": 5.0,
                 "origin_x_um": 0.0, "origin_y_um": 0.0}
  }
})";
}

} // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,yolo,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    mib::test::Watchdog wd(120);
    QApplication app(argc, argv);
    mib::test::TempDir td("dot_grid_config");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    const QString cfgPath = QString::fromStdString((td.path() / "config.json").string());
    writeFile(cfgPath, config("false", "9007199254740993", 1000)); // 2^53 + 1
    {
        QSettings s;
        s.setValue(QStringLiteral("Config/ExternalAppConfigPath"), cfgPath);
        s.sync();
    }
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    auto& dotGrid = backend.dotGrid();

    wd.mark("initial load");
    frontend::AppConfigWatcher watcher(backend, nullptr);
    watcher.start();
    settleMs(100);
    MIB_EXPECT(dotGrid.getConfig().codebook.seed == 9007199254740993ULL,
               "seed above 2^53 applied exactly: " + std::to_string(dotGrid.getConfig().codebook.seed));
    MIB_EXPECT(!dotGrid.isEnabled(), "initially off, as in the file");

    // The Overview toggle turns it on at runtime (OverviewTab::onToggleDotGrid).
    wd.mark("toggle");
    {
        auto c = dotGrid.getConfig();
        c.enabled = true;
        std::string e;
        MIB_REQUIRE(dotGrid.setConfig(c, &e), e);
    }

    // Each write waits until the watcher has loaded it (its document
    // fingerprint changes), so no check races a pending reload.
    auto writeAndReload = [&](const QByteArray& bytes) {
        const QByteArray before = watcher.documentFingerprint();
        writeFile(cfgPath, bytes);
        MIB_REQUIRE(waitFor([&] { return watcher.documentFingerprint() != before; }, 5000),
                    "watcher reloaded the edit");
        settleMs(50);
    };
    auto setRuntime = [&](bool on) { // the Overview toggle (runtime only)
        auto c = dotGrid.getConfig();
        c.enabled = on;
        std::string e;
        MIB_REQUIRE(dotGrid.setConfig(c, &e), e);
    };

    // An unrelated config.json write reloads the file; the toggle must survive.
    wd.mark("unrelated reload");
    writeAndReload(config("false", "9007199254740993", 1234));
    MIB_EXPECT(dotGrid.isEnabled(), "Wafer Grid On survives a reload that did not change dot_grid.enabled");

    // A reload that changes dot_grid.enabled is applied, both ways.
    wd.mark("enabled changes");
    setRuntime(false);
    writeAndReload(config("true", "9007199254740993", 1234)); // file false -> true
    MIB_EXPECT(dotGrid.isEnabled(), "file enabled false -> true applied");
    setRuntime(false);
    writeAndReload(config("true", "9007199254740993", 999)); // enabled unchanged in the file
    MIB_EXPECT(!dotGrid.isEnabled(), "Wafer Grid Off survives a reload that did not change dot_grid.enabled");
    setRuntime(true);
    writeAndReload(config("false", "9007199254740993", 999)); // file true -> false
    MIB_EXPECT(!dotGrid.isEnabled(), "file enabled true -> false applied");

    // Seeds above 2^63 as a decimal string; a fractional seed is rejected.
    wd.mark("seeds");
    writeAndReload(config("false", "\"18446744073709551557\"", 999));
    MIB_EXPECT(dotGrid.getConfig().codebook.seed == 18446744073709551557ULL,
               "seed above 2^63 as a string applied exactly");
    writeAndReload(config("false", "7.5", 999));
    MIB_EXPECT(dotGrid.getConfig().codebook.seed == 18446744073709551557ULL,
               "fractional seed rejected, previous seed kept");

    wd.mark("teardown");
    return mib::test::exitCode();
}
