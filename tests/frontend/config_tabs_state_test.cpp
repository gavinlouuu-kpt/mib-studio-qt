#include <QPlainTextEdit>
// config_tabs_state_test (issue #361)
//
// ConfigTabs with explicit editor state and a bounded header (offscreen,
// isolated settings + temp config.json, mock backend):
//  - edit -> Dirty; hiding the widget / compact mode never clears it;
//  - external change while dirty -> Conflict, local bytes retained, notice
//    visible; Reset loads the external bytes and is clean;
//  - Save with a stale baseline (non-interactive) is refused and stays dirty;
//    Save with a fresh baseline writes through QSaveFile and is Saved;
//  - Save into an unwritable path keeps Dirty and reports the error;
//  - long paths / profile state text never change the required width;
//  - grouped JSON tables reflow 3 -> 1 -> 3 columns on resize without any
//    file write or model reload;
//  - secondary actions live in a keyboard-accessible More menu.

#include "backend/app/AppBackend.h"
#include "frontend/tabs/ConfigTabs.h"
#include "frontend/utils/ApplicationSettings.h"
#include "frontend/utils/ElidingLabel.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QStyleFactory>
#include <QToolButton>
#include <QElapsedTimer>
#include <QThread>

namespace {
void settle(int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}
// Real-time settle so the debounced relayout timer (100 ms) fires.
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
} // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("all"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    mib::test::Watchdog wd(180);
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    mib::test::TempDir td("config_tabs_state");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");

    const QString cfgPath = QString::fromStdString((td.path() / "cfg" / "config.json").string());
    QDir().mkpath(QFileInfo(cfgPath).absolutePath());
    const QByteArray original = "{\n  \"processing\": {\"area_threshold_min\": 10, \"area_threshold_max\": 500},\n  \"camera\": {\"frame_delivery_mode\": \"every_frame\"},\n  \"custom_section\": {\"unknown_key\": 42}\n}\n";
    writeFile(cfgPath, original);
    {
        QSettings s;
        s.setValue(QStringLiteral("Config/ExternalAppConfigPath"), cfgPath);
        s.setValue(QStringLiteral("Preview/ShowTable"), true);
    }

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");

    frontend::ConfigTabs tabs(backend);
    tabs.setNonInteractiveForTests(true);
    tabs.resize(1366, 600);
    tabs.show();
    settle(10);

    auto* defaultEditor = tabs.findChild<QPlainTextEdit*>("mvRawConfig");
    MIB_REQUIRE(defaultEditor, "default camera profile editor exists");
    const auto defaultRig = QJsonDocument::fromJson(defaultEditor->toPlainText().toUtf8()).object();
    MIB_EXPECT(defaultRig["live_view"].toObject()["port"].toString() == "auto" &&
                   defaultRig["live_view"].toObject()["enabled"].toBool(),
               "default rig requires no manual setup");
    MIB_EXPECT(defaultRig["exposure_time_us"].toDouble() == 100,
               "automatic default uses tested exposure setting");

    // ---- explicit state ----------------------------------------------------
    wd.mark("state");
    const auto& doc = tabs.appConfigDocument();
    MIB_EXPECT(!doc.dirty && !doc.conflict && doc.path == cfgPath, "loaded clean from the external path");
    MIB_EXPECT(tabs.profileStateText().startsWith(QStringLiteral("Loaded")), "state label: Loaded");
    const QString edited = QString::fromUtf8(original).replace(QStringLiteral("\"area_threshold_min\": 10"), QStringLiteral("\"area_threshold_min\": 12"));
    tabs.setAppConfigEditorText(edited);
    settle(4);
    MIB_EXPECT(doc.dirty && !doc.conflict, "edit -> dirty");
    MIB_EXPECT(tabs.profileStateText().startsWith(QStringLiteral("Edited")), "state label: Edited");
    MIB_EXPECT(!tabs.noticesText().isEmpty() && tabs.noticesText().contains(QStringLiteral("not saved")), "dirty notice rendered");
    tabs.hide();
    settle(2);
    MIB_EXPECT(doc.dirty, "hidden widget keeps dirty");
    tabs.setCompactMode(true);
    settle(2);
    MIB_EXPECT(doc.dirty && tabs.appConfigEditorText() == edited, "compact mode keeps edits");
    // External change while hidden + dirty -> conflict, bytes retained.
    const QByteArray external = "{\n  \"processing\": {\"area_threshold_min\": 99},\n  \"custom_section\": {\"unknown_key\": 42}\n}\n";
    writeFile(cfgPath, external);
    tabs.onExternalConfigFileChanged(cfgPath);
    settle(2);
    MIB_EXPECT(doc.conflict && doc.dirty, "external change while dirty -> conflict");
    MIB_EXPECT(tabs.appConfigEditorText() == edited, "local bytes retained (not overwritten by the external file)");
    tabs.setCompactMode(false);
    tabs.show();
    settle(4);
    MIB_EXPECT(tabs.noticesText().contains(QStringLiteral("changed elsewhere")), "conflict notice visible after showing");
    MIB_EXPECT(tabs.profileStateText().startsWith(QStringLiteral("Conflict")), "state label: Conflict");
    // Stale-baseline save refused non-interactively.
    QMetaObject::invokeMethod(&tabs, "onSaveJson", Qt::DirectConnection);
    settle(2);
    MIB_EXPECT(doc.dirty && doc.conflict && doc.lastSave == frontend::ConfigDocumentState::SaveOutcome::Conflict,
               "stale baseline: save refused, still dirty");
    MIB_EXPECT(fileBytes(cfgPath) == external, "file untouched by the refused save");
    // Reset loads the external content and is clean.
    QMetaObject::invokeMethod(&tabs, "onReloadJson", Qt::DirectConnection);
    settle(2);
    MIB_EXPECT(!doc.dirty && !doc.conflict && tabs.appConfigEditorText().toUtf8() == external, "reset -> external bytes, clean");
    // Fresh edit + save -> written through QSaveFile, Saved.
    const QString edited2 = QString::fromUtf8(external).replace(QStringLiteral("99"), QStringLiteral("77"));
    tabs.setAppConfigEditorText(edited2);
    settle(2);
    QMetaObject::invokeMethod(&tabs, "onSaveJson", Qt::DirectConnection);
    settle(2);
    MIB_EXPECT(!doc.dirty && doc.lastSave == frontend::ConfigDocumentState::SaveOutcome::Saved, "save -> Saved");
    MIB_EXPECT(fileBytes(cfgPath) == edited2.toUtf8(), "file holds the editor bytes (unknown keys kept)");
    MIB_EXPECT(tabs.profileStateText().startsWith(QStringLiteral("Saved")), "state label: Saved");
    // Unwritable destination keeps Dirty.
    {
        const QString dirAsFile = QString::fromStdString((td.path() / "cfg" / "adir").string());
        QDir().mkpath(dirAsFile);
        QSettings s;
        s.setValue(QStringLiteral("Config/ExternalAppConfigPath"), dirAsFile);
        tabs.setAppConfigEditorText(edited2 + QStringLiteral("\n"));
        settle(2);
        QMetaObject::invokeMethod(&tabs, "onSaveJson", Qt::DirectConnection);
        settle(2);
        MIB_EXPECT(doc.dirty && doc.lastSave == frontend::ConfigDocumentState::SaveOutcome::Failed && !doc.lastError.isEmpty(),
                   "write failure keeps dirty with an error");
        MIB_EXPECT(tabs.noticesText().contains(QStringLiteral("Last save failed")), "failure notice");
        s.setValue(QStringLiteral("Config/ExternalAppConfigPath"), cfgPath);
        QMetaObject::invokeMethod(&tabs, "onReloadJson", Qt::DirectConnection);
        settle(2);
    }

    // ---- bounded header ------------------------------------------------------
    wd.mark("header");
    const int minBefore = tabs.minimumSizeHint().width();
    auto* pathLabel = tabs.findChild<frontend::ElidingLabel*>(QStringLiteral("appConfigPathLabel"));
    auto* stateLabel = tabs.findChild<frontend::ElidingLabel*>(QStringLiteral("profileStateLabel"));
    MIB_REQUIRE(pathLabel && stateLabel, "header labels");
    pathLabel->setText(QStringLiteral("/an/extremely/long/") + QString(1200, QLatin1Char('p')) + QStringLiteral("/config.json"));
    stateLabel->setText(QString(600, QLatin1Char('s')));
    settle(4);
    MIB_EXPECT(tabs.minimumSizeHint().width() == minBefore, "long path/state do not widen the inspector");
    MIB_EXPECT(tabs.minimumSizeHint().width() <= 640, "header fits the narrow inspector budget (" + std::to_string(tabs.minimumSizeHint().width()) + ")");
    auto* more = tabs.secondaryActionsButton();
    MIB_REQUIRE(more && more->menu(), "More menu present");
    const auto actions = more->menu()->actions();
    int named = 0;
    for (QAction* a : actions) if (!a->isSeparator() && !a->text().isEmpty()) ++named;
    MIB_EXPECT(named >= 9, "secondary actions moved into the menu (" + std::to_string(named) + ")");
    MIB_EXPECT(more->focusPolicy() != Qt::NoFocus && more->isVisible(), "More button focusable");
    MIB_EXPECT(tabs.findChild<QLabel*>(QStringLiteral("appConfigNotices"))->wordWrap(), "notices wrap");
    MIB_EXPECT(!tabs.findChild<QToolButton*>(QStringLiteral("configMoreBtn"))->text().isEmpty(), "More has accessible text");

    // ---- reflow without data change --------------------------------------------
    wd.mark("reflow");
    const QByteArray beforeReflow = fileBytes(cfgPath);
    const QString editorBefore = tabs.appConfigEditorText();
    tabs.resize(1366, 700);
    settleMs(300);
    const int colsWide = tabs.jsonSectionColumns();
    tabs.resize(400, 700);
    settleMs(300);
    const int colsNarrow = tabs.jsonSectionColumns();
    std::fprintf(stderr, "reflow: wide=%d narrow=%d width=%d min=%d\n", colsWide, colsNarrow, tabs.width(), tabs.minimumSizeHint().width());
    tabs.resize(1366, 700);
    settleMs(300);
    MIB_EXPECT(colsWide >= 2 && colsNarrow == 1 && tabs.jsonSectionColumns() == colsWide,
               "columns follow the viewport width (" + std::to_string(colsWide) + " -> " + std::to_string(colsNarrow) + ")");
    MIB_EXPECT(fileBytes(cfgPath) == beforeReflow && tabs.appConfigEditorText() == editorBefore && !doc.dirty,
               "reflow wrote nothing and changed no document");

    wd.mark("one-click rig setup persistence");
    const QString rigPath = QString::fromStdString((td.path() / "camera.json").string());
    writeFile(rigPath, QByteArrayLiteral("{\"custom_key\":42}"));
    {
        QSettings settings;
        settings.setValue("Config/ExternalMindVisionConfigPath", rigPath);
    }
    {
        frontend::ConfigTabs rigTabs(backend);
        rigTabs.setNonInteractiveForTests(true);
        rigTabs.resize(1000, 650);
        rigTabs.show();
        auto* pages = rigTabs.findChild<QTabWidget*>();
        MIB_REQUIRE(pages, "config pages");
        for (int i = 0; i < pages->count(); ++i)
            if (pages->tabText(i).contains("MindVision")) pages->setCurrentIndex(i);
        settle();
        auto* advanced = rigTabs.findChild<QCheckBox*>("mvAdvancedSetup");
        auto* exposure = rigTabs.findChild<QDoubleSpinBox*>("mvExposure");
        auto* preset = rigTabs.findChild<QPushButton*>("mvSaveLiveRig");
        auto* port = rigTabs.findChild<QComboBox*>("mvGeneratorPort");
        MIB_REQUIRE(advanced && exposure && preset && port, "rig setup controls exist");
        MIB_EXPECT(!advanced->isChecked() && !preset->isVisible() && exposure->isVisible(),
                   "advanced hidden, exposure visible");
        advanced->setChecked(true);
        settle();
        auto* rawToggle = rigTabs.findChild<QCheckBox*>("mvRawConfigToggle");
        auto* rawEditor = rigTabs.findChild<QPlainTextEdit*>("mvRawConfig");
        MIB_REQUIRE(rawToggle && rawEditor, "raw configuration has separate disclosure");
        MIB_EXPECT(rawToggle->isVisible() && !rawEditor->isVisible(),
                   "hardware setup does not also expose raw editor");
        rawToggle->setChecked(true);
        MIB_EXPECT(rawEditor->isVisible(), "raw configuration explicitly accessible");
        port->addItem("Test adapter", "COM1");
        port->setCurrentIndex(port->count() - 1);
        preset->click();
        settle();
        const auto saved = QJsonDocument::fromJson(fileBytes(rigPath)).object();
        MIB_EXPECT(saved["live_view"].toObject()["enabled"].toBool(),
                   "preset persists coordinated ownership");
        MIB_EXPECT(saved["live_view"].toObject()["port"].toString() == "COM1",
                   "chosen adapter persisted");
        MIB_EXPECT(saved["exposure_time_us"].toDouble() == 100 && saved["custom_key"].toInt() == 42,
                   "preset preserves unrelated config");
        MIB_EXPECT(backend.cameraSelection().mindVisionConfigPath == rigPath.toStdString(),
                   "Save stages next capture without Apply");
        auto* fps = rigTabs.findChild<QDoubleSpinBox*>("mvFps");
        auto* save = rigTabs.findChild<QPushButton*>("mvSaveSetup");
        MIB_REQUIRE(fps && save && fps->isEnabled(), "saved rig enables everyday FPS");
        fps->setValue(2500);
        save->click();
        settle();
        const auto adjusted = QJsonDocument::fromJson(fileBytes(rigPath)).object();
        MIB_EXPECT(adjusted["live_view"].toObject()["frequency_hz"].toDouble() == 2500 &&
                       adjusted["live_view"].toObject()["duty_percent"].toDouble() == 5,
                   "FPS change preserves 20 us trigger pulse via adjusted duty");
        MIB_EXPECT(adjusted["exposure_time_us"].toDouble() == 100 &&
                       adjusted["strobe_pulse_width_us"].toInt() == 100,
                   "FPS change does not silently change exposure or strobe");
        advanced->setChecked(false);
        settle();
        MIB_EXPECT(fps->isVisible(), "FPS visible with advanced collapsed");
        MIB_EXPECT(!rawEditor->isVisible() && !rawToggle->isChecked(),
                   "closing hardware setup also closes raw configuration");
        rigTabs.grab().save("/tmp/mib-one-click-config.png");
    }
    {
        frontend::ConfigTabs restored(backend);
        auto* exposure = restored.findChild<QDoubleSpinBox*>("mvExposure");
        MIB_EXPECT(exposure && exposure->value() == 100, "reopening restores saved exposure");
        auto* fps = restored.findChild<QDoubleSpinBox*>("mvFps");
        MIB_EXPECT(fps && fps->value() == 2500, "reopening restores saved FPS");
        MIB_EXPECT(backend.cameraSelection().mindVisionConfigPath == rigPath.toStdString(),
                   "reopening stages saved profile");
    }
    backend.shutdown();
    return mib::test::exitCode();
}
