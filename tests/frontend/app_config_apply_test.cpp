// app_config_apply_test (issue #364)
//
// AppConfigWatcher::applyProcessingDraft on a real temp config.json (mock
// backend, offscreen):
//  - success persists only the patched keys (unknown keys and untouched
//    high-precision values preserved), applies the same patch to the
//    ProcessingService, reports persisted && applied, and broadcasts exactly
//    one configFileChanged (the watcher echo of the self-write is not
//    reloaded or re-broadcast);
//  - a stale baseline fingerprint or an external edit the watcher has not
//    processed yet is a conflict: nothing written, runtime unchanged; after
//    the watcher catches up the apply succeeds;
//  - malformed JSON on disk, a directory path, a missing document, an empty
//    patch and an inverted range all fail closed with nothing written;
//  - a genuine external edit still reloads and is broadcast.

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "frontend/system/AppConfigWatcher.h"
#include "frontend/system/ConfigDocumentStore.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QThread>

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
QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}
void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(bytes);
}
QJsonObject imageProcessing(const QString& path)
{
    return QJsonDocument::fromJson(fileBytes(path)).object().value(QStringLiteral("image_processing")).toObject();
}
const char* kConfig = R"({
  "buffer_threshold": 1000,
  "custom_section": {"keep_me": [1, 2, 3]},
  "image_processing": {
    "gaussian_blur_size": 5,
    "bg_subtract_threshold": 8,
    "area_threshold_min": 60,
    "area_threshold_max": 290,
    "deformability_threshold_min": 0.123456,
    "deformability_threshold_max": 1.0,
    "ring_ratio_min": 15.0,
    "ring_ratio_max": 25.0,
    "filters": {"enable_border_check": true, "enable_area_range_check": true, "experimental_flag": "yes"},
    "target_group": {"enabled": false, "area_min": 72, "area_max": 191, "emodulus_min": 0.5},
    "unknown_block": {"x": 1}
  }
})";
} // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,yolo,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR")) qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    mib::test::Watchdog wd(180);
    QApplication app(argc, argv);
    mib::test::TempDir td("config_apply");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    const QString cfgPath = QString::fromStdString((td.path() / "config.json").string());
    writeFile(cfgPath, QByteArray(kConfig));
    {
        QSettings s;
        s.setValue(QStringLiteral("Config/ExternalAppConfigPath"), cfgPath);
        s.sync();
    }
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    auto& processing = backend.processing();

    frontend::AppConfigWatcher watcher(backend, nullptr);
    int fileChangedCount = 0;
    QObject::connect(&watcher, &frontend::AppConfigWatcher::configFileChanged, [&](const QString&) { ++fileChangedCount; });
    watcher.start();
    settleMs(50);
    MIB_REQUIRE(watcher.watchedPath() == cfgPath, "watching the temp config");
    MIB_EXPECT(processing.getProcessingConfig().area_threshold_max == 290 && processing.getProcessingConfig().deformability_threshold_min == 0.123456,
               "initial load applied the file");
    MIB_EXPECT(!watcher.documentFingerprint().isEmpty(), "document fingerprint recorded");

    // ---- 1. success -------------------------------------------------------
    wd.mark("success");
    {
        frontend::ApplyProcessingDraftRequest req;
        req.requestId = 11;
        req.baselineFingerprint = watcher.documentFingerprint();
        req.patch.values[frontend::TuneField::AreaMax] = 333;
        req.patch.values[frontend::TuneField::DeformabilityMax] = 0.55;
        req.patch.values[frontend::TuneField::TargetGroupEnabled] = true;
        req.patch.values[frontend::TuneField::MultiImageCount] = 3; // multi_image object does not exist yet
        fileChangedCount = 0;
        const auto r = watcher.applyProcessingDraft(req);
        MIB_EXPECT(r.ok() && r.requestId == 11 && !r.conflict && r.error.isEmpty(), "persisted && applied");
        const QJsonObject ip = imageProcessing(cfgPath);
        MIB_EXPECT(ip.value(QStringLiteral("area_threshold_max")).toInt() == 333, "patched int on disk");
        MIB_EXPECT(ip.value(QStringLiteral("deformability_threshold_max")).toDouble() == 0.55, "patched double on disk");
        MIB_EXPECT(ip.value(QStringLiteral("target_group")).toObject().value(QStringLiteral("enabled")).toBool(), "nested bool on disk");
        MIB_EXPECT(ip.value(QStringLiteral("target_group")).toObject().value(QStringLiteral("emodulus_min")).toDouble() == 0.5, "sibling nested key kept");
        MIB_EXPECT(ip.value(QStringLiteral("multi_image")).toObject().value(QStringLiteral("count")).toInt() == 3, "missing object created");
        MIB_EXPECT(ip.value(QStringLiteral("deformability_threshold_min")).toDouble() == 0.123456, "untouched high-precision value not rewritten");
        MIB_EXPECT(ip.value(QStringLiteral("filters")).toObject().value(QStringLiteral("experimental_flag")).toString() == QStringLiteral("yes")
                       && ip.contains(QStringLiteral("unknown_block")),
                   "unknown keys preserved");
        const QJsonObject root = QJsonDocument::fromJson(fileBytes(cfgPath)).object();
        MIB_EXPECT(root.contains(QStringLiteral("custom_section")) && root.value(QStringLiteral("buffer_threshold")).toInt() == 1000, "other sections preserved");
        const auto rt = processing.getProcessingConfig();
        MIB_EXPECT(rt.area_threshold_max == 333 && rt.deformability_threshold_max == 0.55 && rt.enable_target_group && rt.multi_image_count == 3,
                   "runtime carries the patch");
        MIB_EXPECT(rt.deformability_threshold_min == 0.123456 && rt.gaussian_blur_size == 5, "runtime untouched fields intact");
        MIB_EXPECT(r.effectiveConfig.area_threshold_max == 333, "result carries the effective config");
        MIB_EXPECT(watcher.documentFingerprint() == r.fingerprint
                       && *frontend::ConfigDocumentStore::currentFingerprint(cfgPath) == r.fingerprint,
                   "document fingerprint follows the self-write");
        settleMs(600); // let the QFileSystemWatcher echo arrive
        MIB_EXPECT(fileChangedCount == 1, "exactly one configFileChanged for a self-write (no echo re-broadcast)");
        MIB_EXPECT(processing.getProcessingConfig().area_threshold_max == 333, "echo did not re-apply anything else");
    }

    // ---- 2. stale baseline / unprocessed external edit -------------------
    wd.mark("conflict");
    {
        const QByteArray before = fileBytes(cfgPath);
        frontend::ApplyProcessingDraftRequest req;
        req.requestId = 12;
        req.baselineFingerprint = frontend::ConfigDocumentStore::fingerprintOf(QByteArrayLiteral("not the document"));
        req.patch.values[frontend::TuneField::AreaMin] = 100;
        const auto r = watcher.applyProcessingDraft(req);
        MIB_EXPECT(!r.ok() && r.conflict && !r.persisted && !r.applied, "stale baseline -> conflict");
        MIB_EXPECT(fileBytes(cfgPath) == before && processing.getProcessingConfig().area_threshold_min == 60, "nothing written/applied on conflict");

        // External edit written directly, apply attempted before the watcher processed it.
        QJsonObject root = QJsonDocument::fromJson(before).object();
        QJsonObject ip = root.value(QStringLiteral("image_processing")).toObject();
        ip.insert(QStringLiteral("ring_ratio_max"), 40.0);
        root.insert(QStringLiteral("image_processing"), ip);
        const QByteArray external = QJsonDocument(root).toJson(QJsonDocument::Indented);
        writeFile(cfgPath, external);
        frontend::ApplyProcessingDraftRequest req2;
        req2.requestId = 13;
        req2.patch.values[frontend::TuneField::AreaMin] = 100;
        const auto r2 = watcher.applyProcessingDraft(req2);
        MIB_EXPECT(r2.conflict && !r2.persisted && fileBytes(cfgPath) == external, "unprocessed external edit retained (conflict)");
        fileChangedCount = 0;
        settleMs(600);
        MIB_EXPECT(fileChangedCount >= 1 && processing.getProcessingConfig().ring_ratio_max == 40.0, "genuine external edit reloaded + broadcast");
        MIB_EXPECT(watcher.documentFingerprint() == frontend::ConfigDocumentStore::fingerprintOf(external), "fingerprint follows the external edit");
        const auto r3 = watcher.applyProcessingDraft(req2);
        MIB_EXPECT(r3.ok() && processing.getProcessingConfig().area_threshold_min == 100 && processing.getProcessingConfig().ring_ratio_max == 40.0,
                   "after the reload the same request succeeds and keeps the external value");
        settleMs(400);
    }

    // ---- 3. fail-closed cases --------------------------------------------------
    wd.mark("fail closed");
    {
        const auto rtBefore = processing.getProcessingConfig();
        // Empty patch.
        frontend::ApplyProcessingDraftRequest empty;
        empty.requestId = 20;
        MIB_EXPECT(!watcher.applyProcessingDraft(empty).ok(), "empty patch refused");
        // Inverted range relative to the runtime config.
        frontend::ApplyProcessingDraftRequest inverted;
        inverted.requestId = 21;
        inverted.patch.values[frontend::TuneField::AreaMin] = 100000;
        const auto ri = watcher.applyProcessingDraft(inverted);
        MIB_EXPECT(!ri.persisted && !ri.applied && ri.error.contains(QStringLiteral("Area")), "inverted range refused field-locally");
        // Directory as the document.
        frontend::ApplyProcessingDraftRequest dir;
        dir.requestId = 22;
        dir.path = QString::fromStdString(td.path().string());
        dir.patch.values[frontend::TuneField::AreaMin] = 100;
        const auto rd = watcher.applyProcessingDraft(dir);
        MIB_EXPECT(!rd.persisted && !rd.applied && !rd.error.isEmpty(), "directory path refused");
        // Missing document.
        frontend::ApplyProcessingDraftRequest missing = dir;
        missing.requestId = 23;
        missing.path = QString::fromStdString((td.path() / "nope" / "config.json").string());
        MIB_EXPECT(!watcher.applyProcessingDraft(missing).persisted, "missing document refused");
        // Malformed JSON on disk (external corruption).
        const QByteArray good = fileBytes(cfgPath);
        writeFile(cfgPath, QByteArrayLiteral("{ this is not json"));
        settleMs(500); // watcher processes it (parse fails, nothing applied)
        frontend::ApplyProcessingDraftRequest bad;
        bad.requestId = 24;
        bad.patch.values[frontend::TuneField::AreaMin] = 100;
        const auto rb = watcher.applyProcessingDraft(bad);
        MIB_EXPECT(!rb.persisted && !rb.applied && rb.error.contains(QStringLiteral("JSON")), "malformed document refused with a JSON error");
        MIB_EXPECT(fileBytes(cfgPath) == QByteArrayLiteral("{ this is not json"), "malformed document left untouched");
        const auto rtAfter = processing.getProcessingConfig();
        MIB_EXPECT(rtAfter.area_threshold_min == rtBefore.area_threshold_min && rtAfter.area_threshold_max == rtBefore.area_threshold_max,
                   "runtime unchanged by every refused request");
        writeFile(cfgPath, good);
        settleMs(500);
        // No active document at all.
        frontend::AppConfigWatcher idle(backend, nullptr);
        frontend::ApplyProcessingDraftRequest none;
        none.requestId = 25;
        none.patch.values[frontend::TuneField::AreaMin] = 100;
        const auto rn = idle.applyProcessingDraft(none);
        MIB_EXPECT(!rn.persisted && !rn.applied && rn.error.contains(QStringLiteral("No active")), "no watched path -> explicit error");
    }

    // ---- 4. signal wrapper ------------------------------------------------------
    wd.mark("signal");
    {
        frontend::ConfigApplyResult got;
        QObject::connect(&watcher, &frontend::AppConfigWatcher::processingDraftApplied, [&](const frontend::ConfigApplyResult& r) { got = r; });
        frontend::ApplyProcessingDraftRequest req;
        req.requestId = 30;
        req.patch.values[frontend::TuneField::RingRatioMin] = 12.5;
        watcher.onApplyProcessingDraft(req);
        MIB_EXPECT(got.requestId == 30 && got.ok() && processing.getProcessingConfig().ring_ratio_min == 12.5, "slot wrapper delivers the result");
        settleMs(400);
    }

    backend.shutdown();
    return mib::test::exitCode();
}
