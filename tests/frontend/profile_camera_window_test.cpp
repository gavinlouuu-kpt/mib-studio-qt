// profile_camera_window_test (#833): the camera window in the Qt profile.
//  - camera.window.*, roi.* and experiment_buffer_max_mb are high risk in the
//    profile diff (a camera-window change shows as a high-risk Config row);
//  - Save Profile records the window the profile applies, else the camera
//    script's, with the connected camera (else the one already named);
//    other camera keys are kept;
//  - the watcher applies camera.window and target_fps to the backend's
//    acquisition profile, experiment_buffer_max_mb as the override (absent =
//    automatic), and refuses a malformed window without applying it;
//  - an Overview window edit is written back into camera.window only when the
//    document has the section (the profile then keeps following the operator).

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "frontend/system/AppConfigWatcher.h"
#include "frontend/system/ProfileManager.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace {
void settleMs(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    MIB_REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "write config");
    f.write(bytes);
}

QJsonObject readObject(const QString& path)
{
    QFile f(path);
    MIB_REQUIRE(f.open(QIODevice::ReadOnly), "read config");
    return QJsonDocument::fromJson(f.readAll()).object();
}

QJsonObject windowOf(const QJsonObject& root)
{
    return root.value("camera").toObject().value("window").toObject();
}

constexpr const char* kScript = R"(var g = grabbers[0];
g.RemotePort.set("Width", 512);
g.RemotePort.set("Height", 96);
g.RemotePort.set("OffsetY", 500);
g.RemotePort.set("OffsetX", 704);
)";
} // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    mib::test::Watchdog wd(120);
    QApplication app(argc, argv);
    mib::test::TempDir td("profile_camera_window");

    // ---- risk classification ------------------------------------------------------------------------
    using frontend::ProfileManager;
    for (const char* path : {"camera.window.width", "camera.window.offset_x", "camera.window.camera_model",
                             "camera.window", "roi.x", "roi.w", "roi", "experiment_buffer_max_mb"})
        MIB_EXPECT(ProfileManager::riskForPath(QString::fromLatin1(path)) == QStringLiteral("High"),
                   std::string(path) + " is high risk");
    MIB_EXPECT(ProfileManager::riskForPath(QStringLiteral("camera.frame_delivery_mode")) == QStringLiteral("Medium"),
               "delivery mode stays medium");
    MIB_EXPECT(ProfileManager::riskForPath(QStringLiteral("roi_overlay")) == QStringLiteral("Low"),
               "only roi and roi.* are the processing ROI");
    {
        ProfileManager pm;
        const auto local = QJsonDocument::fromJson(
            R"({"config_schema_version":1,"camera":{"window":{"width":512,"height":96,"offset_x":704,"offset_y":500}}})");
        const auto remote = QJsonDocument::fromJson(
            R"({"config_schema_version":1,"camera":{"window":{"width":1216,"height":208,"offset_x":368,"offset_y":616}}})");
        const auto rows = pm.diffConfigDocuments(local, remote);
        bool sawWidth = false;
        for (const auto& row : rows)
            if (row.path == QStringLiteral("camera.window.width")) {
                sawWidth = true;
                MIB_EXPECT(row.risk == QStringLiteral("High") && row.source == QStringLiteral("Config"),
                           "a camera-window change is a high-risk Config row in the diff");
            }
        MIB_EXPECT(sawWidth, "the diff lists camera.window.width");
    }

    // ---- what Save Profile records -------------------------------------------------------------------
    {
        // A document that names its camera but has no usable window (width 0).
        QJsonObject doc = QJsonDocument::fromJson(
            R"({"camera":{"frame_delivery_mode":"latestFrame","window":{"width":0,"height":0,
                "camera_vendor":"SVS-Vistek GmbH","camera_model":"MC2066"}}})").object();
        const auto fromScript = ProfileManager::cameraWindowForSave(std::nullopt, QString::fromLatin1(kScript), {}, {}, doc);
        MIB_REQUIRE(fromScript.has_value(), "the script's window when neither the document nor the profile has one");
        MIB_EXPECT(fromScript->window.width == 512 && fromScript->window.offsetX == 704, "512x96 at (704, 500)");
        MIB_EXPECT(fromScript->cameraModel == "MC2066", "no connected camera: keeps the camera the document names");

        backend::app::ProfileCameraWindow applied;
        applied.window = {1216, 208, 368, 616};
        const auto fromProfile = ProfileManager::cameraWindowForSave(applied, QString::fromLatin1(kScript),
                                                                     QStringLiteral("SVS-Vistek GmbH"),
                                                                     QStringLiteral("MC2066"), doc);
        MIB_EXPECT(fromProfile && fromProfile->window == applied.window,
                   "the applied profile window wins over the script (it is what the camera runs)");
        MIB_EXPECT(fromProfile->cameraVendor == "SVS-Vistek GmbH", "the connected camera is recorded");

        // The operator edited camera.window in the editor: that edit is what is saved.
        const QJsonObject edited = QJsonDocument::fromJson(
            R"({"camera":{"window":{"width":1024,"height":200,"offset_x":448,"offset_y":620,
                "camera_vendor":"SVS-Vistek GmbH","camera_model":"MC2066"}}})").object();
        const auto fromEdit = ProfileManager::cameraWindowForSave(applied, QString::fromLatin1(kScript), {}, {}, edited);
        MIB_EXPECT(fromEdit && fromEdit->window == (camera::common::CameraWindow{1024, 200, 448, 620}) &&
                       fromEdit->cameraModel == "MC2066",
                   "the document's own window wins over the applied one and the script");
        MIB_EXPECT(!ProfileManager::cameraWindowForSave(std::nullopt, QStringLiteral("// no window"), {}, {}, {}),
                   "no source: nothing recorded");

        const auto saved = ProfileManager::withCameraWindow(doc, *fromProfile);
        const auto w = windowOf(saved);
        MIB_EXPECT(w.value("width").toInt() == 1216 && w.value("offset_y").toInt() == 616 &&
                       w.value("camera_model").toString() == QStringLiteral("MC2066"),
                   "camera.window written");
        MIB_EXPECT(saved.value("camera").toObject().value("frame_delivery_mode").toString() == QStringLiteral("latestFrame"),
                   "other camera keys kept");
        // What the save writes is what the backend applier reads.
        const auto parsed = backend::app::parseAcquisitionProfile(QJsonDocument(saved).toJson().toStdString());
        MIB_EXPECT(parsed.window && parsed.window->window == applied.window && parsed.window->cameraModel == "MC2066",
                   "round trip through the backend parser");
    }

    // ---- the watcher applies the section ------------------------------------------------------------------
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    const QString cfgPath = QString::fromStdString((td.path() / "config.json").string());
    writeFile(cfgPath, R"({"config_schema_version":1,"buffer_threshold":1000,"target_fps":5000,
        "experiment_buffer_max_mb":512,
        "camera":{"frame_delivery_mode":"everyFrame",
                  "window":{"width":1216,"height":208,"offset_x":368,"offset_y":616,
                            "camera_vendor":"SVS-Vistek GmbH","camera_model":"MC2066"}}})");
    {
        QSettings s;
        s.setValue(QStringLiteral("Config/ExternalAppConfigPath"), cfgPath);
        s.sync();
    }
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    frontend::AppConfigWatcher watcher(backend, nullptr);
    int loaded = 0;
    QObject::connect(&watcher, &frontend::AppConfigWatcher::acquisitionProfileLoaded, [&] { ++loaded; });
    watcher.start();
    settleMs(50);
    {
        const auto p = backend.acquisitionProfile();
        MIB_EXPECT(p.window && p.window->window == (camera::common::CameraWindow{1216, 208, 368, 616}) &&
                       p.window->cameraModel == "MC2066",
                   "camera.window reaches the backend");
        MIB_EXPECT(p.targetFps == 5000.0, "target_fps reaches the backend");
        MIB_EXPECT(backend.processing().experimentBufferOverrideBytes() == 512ull * 1024 * 1024,
                   "experiment_buffer_max_mb is the override");
        MIB_EXPECT(loaded == 1, "acquisitionProfileLoaded emitted once for the new window");
    }

    // An Overview edit follows into the section, preserving the camera it names.
    MIB_EXPECT(watcher.writeBackCameraWindow(camera::common::CameraWindow{1024, 200, 448, 620}), "written back");
    {
        const auto w = windowOf(readObject(cfgPath));
        MIB_EXPECT(w.value("width").toInt() == 1024 && w.value("offset_x").toInt() == 448 &&
                       w.value("camera_model").toString() == QStringLiteral("MC2066"),
                   "section updated, camera kept");
        MIB_EXPECT(backend.acquisitionProfile().window->window.width == 1024, "and applied to the backend");
        MIB_EXPECT(!watcher.writeBackCameraWindow(camera::common::CameraWindow{1024, 200, 448, 620}),
                   "the same window is not written again");
    }

    // A document without the section: the script keeps the window; override absent = automatic.
    writeFile(cfgPath, R"({"config_schema_version":1,"buffer_threshold":1000,"camera":{"frame_delivery_mode":"everyFrame"}})");
    watcher.setWatchedPath(cfgPath);
    settleMs(50);
    MIB_EXPECT(!backend.acquisitionProfile().window && backend.acquisitionProfile().targetFps == 0.0,
               "no section: no profile window");
    MIB_EXPECT(backend.processing().experimentBufferOverrideBytes() == 0, "absent override: automatic");
    MIB_EXPECT(!watcher.writeBackCameraWindow(camera::common::CameraWindow{512, 96, 704, 500}),
               "no section: an Overview edit does not create one");
    MIB_EXPECT(!windowOf(readObject(cfgPath)).contains("width"), "document unchanged");

    // A malformed section is refused, not applied.
    writeFile(cfgPath, R"({"config_schema_version":1,"camera":{"window":{"width":512}}})");
    watcher.setWatchedPath(cfgPath);
    settleMs(50);
    MIB_EXPECT(!backend.acquisitionProfile().window, "a window without a height is not applied");

    return mib::test::exitCode();
}
