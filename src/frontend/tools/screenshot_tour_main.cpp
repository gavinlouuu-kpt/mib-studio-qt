// screenshot_tour — drives the MIB Studio Qt UI in mock-camera mode and
// captures the user-manual screenshots (docs/manual/). Runs headless with
// QT_QPA_PLATFORM=offscreen; see docs/manual/README.md for the workflow and
// scripts/check_screenshots.py for the doc<->registry sync check.
//
// Usage:
//   screenshot_tour [--out <dir>] [--frames <dir>] [--width N] [--height N]
//
// Exit code 0 = every registered screenshot was captured; 1 otherwise.

#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QSet>
#include <QSettings>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <spdlog/spdlog.h>

#include <functional>
#include <iterator>
#include <vector>

#include "backend/app/AppBackend.h"
#include "frontend/core/MainWindow.h"
#include "frontend/tabs/OverviewTab.h"
#include "frontend/utils/ApplicationSettings.h"

namespace {

struct Shot {
    const char* id;     // file name stem under docs/manual/images/
    const char* title;  // human-readable label for the manifest
};

// Every screenshot the tour produces. scripts/check_screenshots.py parses the
// ids between the REGISTRY markers and cross-checks them against the image
// references in docs/manual/*.md — keep the two in sync.
// SCREENSHOT_REGISTRY_BEGIN
constexpr Shot kShots[] = {
    {"connect-tab", "Connect tab on startup (mock camera configured)"},
    {"overview-live", "Overview tab with the live camera view and ROI overlay"},
    {"experiment-preview", "Experiment > Preview page with playback panel and config editor"},
    {"experiment-monitoring", "Experiment > Monitoring page with realtime metric charts"},
    {"review-tab", "Review tab for browsing recorded HDF5 experiments"},
    {"sidebar-collapsed", "Main window with the hardware panel hidden (reopen control in the tab-bar corner)"},
    {"dialog-processing-settings", "Settings > Processing Settings dialog"},
    {"dialog-monitoring-settings", "Settings > Monitoring Settings dialog"},
    {"dialog-pixel-to-micron", "Settings > Pixel to Micron Conversion dialog"},
};
// SCREENSHOT_REGISTRY_END

const Shot* findShot(const QString& id)
{
    for (const Shot& shot : kShots) {
        if (id == QLatin1String(shot.id)) {
            return &shot;
        }
    }
    return nullptr;
}

// Runs a list of delayed steps on the event loop, then a completion callback.
// Plain QTimer chaining — no Q_OBJECT machinery needed.
class TourDriver {
public:
    void addStep(int delayMs, std::function<void()> fn)
    {
        steps_.push_back({delayMs, std::move(fn)});
    }

    void start(std::function<void()> onDone)
    {
        onDone_ = std::move(onDone);
        runNext(0);
    }

private:
    void runNext(size_t index)
    {
        if (index >= steps_.size()) {
            if (onDone_) onDone_();
            return;
        }
        QTimer::singleShot(steps_[index].delayMs, [this, index]() {
            steps_[index].fn();
            runNext(index + 1);
        });
    }

    struct Step {
        int delayMs;
        std::function<void()> fn;
    };
    std::vector<Step> steps_;
    std::function<void()> onDone_;
};

class ShotSink {
public:
    ShotSink(QDir outDir) : outDir_(std::move(outDir)) {}

    void save(const QString& id, const QPixmap& pixmap)
    {
        const Shot* shot = findShot(id);
        if (!shot) {
            SPDLOG_ERROR("screenshot_tour: shot id '{}' is not in the registry", id.toStdString());
            ++failures_;
            return;
        }
        if (pixmap.isNull()) {
            SPDLOG_ERROR("screenshot_tour: grab for '{}' produced a null pixmap", id.toStdString());
            ++failures_;
            return;
        }
        const QString file = outDir_.filePath(id + QStringLiteral(".png"));
        if (!pixmap.save(file, "PNG")) {
            SPDLOG_ERROR("screenshot_tour: failed to write {}", file.toStdString());
            ++failures_;
            return;
        }
        captured_.insert(id);
        SPDLOG_INFO("screenshot_tour: captured {} ({}x{})", file.toStdString(),
                    pixmap.width(), pixmap.height());
    }

    // Grab the currently active modal dialog, save it, and close it. Must be
    // scheduled (QTimer::singleShot) *before* triggering the action that
    // exec()s the dialog, because exec() blocks until the dialog closes.
    void scheduleModalShot(const QString& id, int settleMs)
    {
        QTimer::singleShot(settleMs, [this, id]() {
            QWidget* modal = QApplication::activeModalWidget();
            if (!modal) {
                SPDLOG_ERROR("screenshot_tour: no active modal widget for '{}'", id.toStdString());
                ++failures_;
                return;
            }
            save(id, modal->grab());
            modal->close();
        });
    }

    bool writeManifest() const
    {
        QJsonArray shots;
        for (const Shot& shot : kShots) {
            QJsonObject entry;
            entry.insert(QStringLiteral("id"), QLatin1String(shot.id));
            entry.insert(QStringLiteral("file"), QLatin1String(shot.id) + QStringLiteral(".png"));
            entry.insert(QStringLiteral("title"), QLatin1String(shot.title));
            entry.insert(QStringLiteral("captured"), captured_.contains(QLatin1String(shot.id)));
            shots.append(entry);
        }
        QJsonObject root;
        root.insert(QStringLiteral("generator"), QStringLiteral("screenshot_tour"));
        root.insert(QStringLiteral("appVersion"), QStringLiteral(MIB_STUDIO_QT_VERSION_FULL));
        root.insert(QStringLiteral("shots"), shots);

        QFile file(outDir_.filePath(QStringLiteral("manifest.json")));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            SPDLOG_ERROR("screenshot_tour: cannot write manifest.json in {}",
                         outDir_.absolutePath().toStdString());
            return false;
        }
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return true;
    }

    int missingCount() const
    {
        int missing = 0;
        for (const Shot& shot : kShots) {
            if (!captured_.contains(QLatin1String(shot.id))) {
                SPDLOG_ERROR("screenshot_tour: registered shot '{}' was never captured", shot.id);
                ++missing;
            }
        }
        return missing + failures_;
    }

private:
    QDir outDir_;
    QSet<QString> captured_;
    int failures_ = 0;
};

QString resolveFramesDir(const QString& cliValue)
{
    if (!cliValue.isEmpty()) {
        return cliValue;
    }
    if (!qEnvironmentVariableIsEmpty("MIB_MOCK_CAMERA_DIR")) {
        return qEnvironmentVariable("MIB_MOCK_CAMERA_DIR");
    }
    // Walk up from the working directory looking for the repo's sample frames.
    QDir dir = QDir::current();
    for (int depth = 0; depth < 6; ++depth) {
        const QString candidate = dir.filePath(QStringLiteral("data/mock_frames"));
        if (QDir(candidate).exists()) {
            return candidate;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QCoreApplication::setApplicationName(QStringLiteral("MIB Studio Qt"));
    QCoreApplication::setApplicationVersion(QStringLiteral(MIB_STUDIO_QT_VERSION_FULL));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Captures the docs/manual user-manual screenshots by driving the "
                       "app in mock-camera mode (use QT_QPA_PLATFORM=offscreen headless)."));
    parser.addHelpOption();
    QCommandLineOption outOpt({QStringLiteral("o"), QStringLiteral("out")},
                              QStringLiteral("Output directory for PNGs + manifest.json."),
                              QStringLiteral("dir"), QStringLiteral("docs/manual/images"));
    QCommandLineOption framesOpt(QStringLiteral("frames"),
                                 QStringLiteral("Mock camera frame folder (default: MIB_MOCK_CAMERA_DIR "
                                                "or data/mock_frames found from the working directory)."),
                                 QStringLiteral("dir"));
    QCommandLineOption widthOpt(QStringLiteral("width"), QStringLiteral("Window width."),
                                QStringLiteral("px"), QStringLiteral("1280"));
    QCommandLineOption heightOpt(QStringLiteral("height"), QStringLiteral("Window height."),
                                 QStringLiteral("px"), QStringLiteral("800"));
    parser.addOption(outOpt);
    parser.addOption(framesOpt);
    parser.addOption(widthOpt);
    parser.addOption(heightOpt);
    parser.process(app);

    const QString framesDir = resolveFramesDir(parser.value(framesOpt));
    if (framesDir.isEmpty() || !QDir(framesDir).exists()) {
        SPDLOG_ERROR("screenshot_tour: mock frame folder not found (pass --frames or set "
                     "MIB_MOCK_CAMERA_DIR; sample frames live in data/mock_frames)");
        return 1;
    }

    QDir outDir(parser.value(outOpt));
    if (!outDir.exists() && !QDir().mkpath(outDir.absolutePath())) {
        SPDLOG_ERROR("screenshot_tour: cannot create output directory {}",
                     outDir.absolutePath().toStdString());
        return 1;
    }

    // Force the mock camera and mute services that reach for hardware or the
    // network. Explicit env vars from the caller win.
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_MOCK_CAMERA_DIR", framesDir.toUtf8());
    if (qEnvironmentVariableIsEmpty("MIB_DISABLED_SERVICES")) {
        qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,yolo"));
    }

    // Isolate backend runtime data (logs, sqlite, HDF5) and QSettings so each
    // run starts from a clean, deterministic state.
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        SPDLOG_ERROR("screenshot_tour: cannot create temporary data directory");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       scratch.filePath(QStringLiteral("settings")));
    QString settingsError;
    if (!frontend::applicationsettings::initialize(&settingsError)) {
        SPDLOG_ERROR("screenshot_tour: settings initialization failed: {}",
                     settingsError.toStdString());
        return 1;
    }

    backend::AppBackend backend;
    if (!backend.initialize(scratch.filePath(QStringLiteral("data")).toStdString())) {
        SPDLOG_ERROR("screenshot_tour: backend initialization failed");
        return 1;
    }

    // Issue #358: the tour's requested size is the viewport contract. The
    // window is told the available desktop explicitly (the offscreen platform
    // screen is only 800x600), and after settling we assert the *actual*
    // geometry: a window silently enlarged by minimum-size constraints must
    // fail the tour rather than produce a misleadingly successful PNG.
    const QSize requested(parser.value(widthOpt).toInt(), parser.value(heightOpt).toInt());
    MainWindow window(backend);
    window.setAvailableGeometryOverrideForTests(QRect(QPoint(0, 0), requested + QSize(200, 200)));
    window.resize(requested);
    window.show();
    int geometryFailures = 0;
    auto assertViewport = [&](const QString& where) {
        const QSize need = window.minimumSizeHint().expandedTo(window.minimumSize());
        if (window.size() != requested) {
            SPDLOG_ERROR("screenshot_tour: {}: window is {}x{} but {}x{} was requested",
                         where.toStdString(), window.width(), window.height(), requested.width(), requested.height());
            ++geometryFailures;
        }
        if (need.width() > requested.width() || need.height() > requested.height()) {
            SPDLOG_ERROR("screenshot_tour: {}: minimum size {}x{} exceeds the requested viewport {}x{}",
                         where.toStdString(), need.width(), need.height(), requested.width(), requested.height());
            ++geometryFailures;
        }
    };

    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("tabs"));
    if (!tabs || tabs->count() < 4) {
        SPDLOG_ERROR("screenshot_tour: main tab widget not found — UI layout changed?");
        return 1;
    }
    // The Experiment entry is itself a QTabWidget (Preview + Monitoring).
    auto* experimentTabs = qobject_cast<QTabWidget*>(tabs->widget(2));

    ShotSink sink(outDir);
    TourDriver tour;

    // Timings are generous so slow CI runners still render each view before
    // the grab; the mock camera streams frames the whole time.
    constexpr int kSettleMs = 1000;
    constexpr int kModalSettleMs = 800;

    tour.addStep(kSettleMs, [&]() {
        tabs->setCurrentIndex(0);
        assertViewport(QStringLiteral("connect-tab"));
        sink.save(QStringLiteral("connect-tab"), window.grab());
    });
    tour.addStep(200, [&]() {
        // Start the mock camera stream (same slot the Start Camera button uses).
        QMetaObject::invokeMethod(&window, "onStartCapture", Qt::DirectConnection);
    });
    tour.addStep(2000, [&]() {
        tabs->setCurrentIndex(1);
        // Show the ROI rectangle the manual describes (normally enabled by
        // the ConnectTab connected signal, which the mock path skips), and
        // move it to the origin — the configured default position targets a
        // full-size sensor and would fall outside the small mock frames.
        if (auto* overview = window.findChild<frontend::OverviewTab*>()) {
            overview->setRoiOverlayVisible(true);
            QMetaObject::invokeMethod(overview, "onRoiPositionChanged", Qt::DirectConnection,
                                      Q_ARG(QPointF, QPointF(0, 0)));
        }
    });
    tour.addStep(kSettleMs, [&]() {
        assertViewport(QStringLiteral("overview-live"));
        sink.save(QStringLiteral("overview-live"), window.grab());
    });
    tour.addStep(200, [&]() {
        tabs->setCurrentIndex(2);
        if (experimentTabs) experimentTabs->setCurrentIndex(0);
    });
    tour.addStep(kSettleMs, [&]() {
        assertViewport(QStringLiteral("experiment-preview"));
        sink.save(QStringLiteral("experiment-preview"), window.grab());
        if (experimentTabs) experimentTabs->setCurrentIndex(1);
    });
    tour.addStep(kSettleMs, [&]() {
        assertViewport(QStringLiteral("experiment-monitoring"));
        sink.save(QStringLiteral("experiment-monitoring"), window.grab());
        tabs->setCurrentIndex(3);
    });
    tour.addStep(kSettleMs, [&]() {
        assertViewport(QStringLiteral("review-tab"));
        sink.save(QStringLiteral("review-tab"), window.grab());
        // Issue #359: hide the hardware panel through the single owner path.
        window.setHardwarePanelVisible(false);
        tabs->setCurrentIndex(1);
    });
    tour.addStep(kSettleMs, [&]() {
        assertViewport(QStringLiteral("sidebar-collapsed"));
        if (window.isHardwarePanelVisible()) {
            SPDLOG_ERROR("screenshot_tour: hardware panel still visible after hiding it");
            ++geometryFailures;
        }
        sink.save(QStringLiteral("sidebar-collapsed"), window.grab());
        window.setHardwarePanelVisible(true);
    });

    // Modal dialogs: schedule the grab-and-close first, then trigger the menu
    // action — QDialog::exec() blocks this step until the dialog closes.
    const struct {
        const char* shotId;
        const char* actionName;
    } modalShots[] = {
        {"dialog-processing-settings", "processingSettingsAct"},
        {"dialog-monitoring-settings", "monitoringSettingsAct"},
        {"dialog-pixel-to-micron", "conversionFactorAct"},
    };
    for (const auto& modal : modalShots) {
        tour.addStep(400, [&, modal]() {
            auto* action = window.findChild<QAction*>(QLatin1String(modal.actionName));
            if (!action) {
                SPDLOG_ERROR("screenshot_tour: menu action '{}' not found", modal.actionName);
                return;
            }
            sink.scheduleModalShot(QLatin1String(modal.shotId), kModalSettleMs);
            action->trigger();
        });
    }

    tour.addStep(300, [&]() {
        QMetaObject::invokeMethod(&window, "onStopCapture", Qt::DirectConnection);
    });

    int exitCode = 1;
    tour.start([&]() {
        const bool manifestOk = sink.writeManifest();
        const int missing = sink.missingCount();
        exitCode = (manifestOk && missing == 0 && geometryFailures == 0) ? 0 : 1;
        if (geometryFailures > 0) {
            SPDLOG_ERROR("screenshot_tour: {} viewport/geometry assertion(s) failed", geometryFailures);
        }
        if (missing > 0) {
            SPDLOG_ERROR("screenshot_tour: {} screenshot(s) missing or failed", missing);
        } else {
            SPDLOG_INFO("screenshot_tour: all {} screenshots captured to {}",
                        static_cast<int>(std::size(kShots)), outDir.absolutePath().toStdString());
        }
        window.close();
        QCoreApplication::quit();
    });

    app.exec();
    return exitCode;
}
