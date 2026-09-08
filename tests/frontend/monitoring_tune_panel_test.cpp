// monitoring_tune_panel_test (issue #364)
//
// ExperimentMonitoringTab tune panel (offscreen, mock backend, no file):
//  - every exposed field has a bound control grouped with its enable state
//    and mapped to ProcessingConfig; target-group controls carry unmistakable
//    accessible names;
//  - editing marks Dirty (count in the footer) without any backend mutation;
//    Apply without a coordinator fails explicitly and stays dirty;
//  - Apply through a fake coordinator sends only the changed fields; a
//    confirmed result clears Dirty; a failed result keeps it; a stale result
//    is ignored; edits and external reloads are parked while applying;
//  - Revert restores the authoritative values;
//  - an external change with no edits refreshes; with edits it is a visible
//    Conflict that retains the draft until Revert;
//  - disabled criteria keep their configured values visible (controls
//    disabled, not hidden);
//  - the panel is usable at 220 and 280 px without horizontal overflow and
//    the Apply/Revert footer stays visible at every scroll position;
//  - Apply/Revert are keyboard reachable.

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "frontend/models/ProcessingConfigDraft.h"
#include "frontend/tabs/ExperimentMonitoringTab.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStyleFactory>

using frontend::TuneField;

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
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,yolo,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR")) qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    mib::test::Watchdog wd(180);
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    mib::test::TempDir td("monitoring_tune");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    auto& processing = backend.processing();
    {
        auto cfg = processing.getProcessingConfig();
        cfg.deformability_threshold_min = 0.123456; // displayed as 0.12
        processing.setProcessingConfig(cfg);
    }

    frontend::ExperimentMonitoringTab tab(backend);
    tab.resize(1100, 760);
    tab.show();
    tab.loadCurrentConfig();
    settle(8);
    auto& draft = tab.tuneDraft();
    const auto baseline = processing.getProcessingConfig();

    // ---- 1. bindings + grouping -----------------------------------------------
    wd.mark("bindings");
    for (TuneField f : frontend::allTuneFields()) {
        QWidget* w = tab.tuneFieldWidget(f);
        MIB_REQUIRE(w != nullptr, "every field has a control");
        MIB_EXPECT(w->accessibleName() == frontend::tuneFieldLabel(f), "accessible name is the full label");
        const bool target = frontend::tuneFieldGroup(f) == frontend::TuneGroup::TargetGroup;
        MIB_EXPECT(w->accessibleName().startsWith(QStringLiteral("Target group")) == target, "target-group controls are unmistakable");
        MIB_EXPECT(frontend::tuneValuesEqual(f, draft.field(f), frontend::readTuneField(baseline, f)), "control reflects the runtime config");
    }
    // Enable state and values live in the same group box.
    auto* areaBox = tab.findChild<QGroupBox*>(QStringLiteral("criterionArea"));
    MIB_REQUIRE(areaBox && areaBox->isCheckable(), "area criterion is a checkable group");
    MIB_EXPECT(tab.tuneFieldWidget(TuneField::AreaEnabled) == areaBox && tab.tuneFieldWidget(TuneField::AreaMin)->parentWidget() == areaBox
                   && tab.tuneFieldWidget(TuneField::AreaMax)->parentWidget() == areaBox,
               "area enable + min + max grouped");
    auto* targetBox = tab.findChild<QGroupBox*>(QStringLiteral("targetGroupBox"));
    MIB_REQUIRE(targetBox, "target group box");
    MIB_EXPECT(tab.tuneFieldWidget(TuneField::TargetAreaMin)->parentWidget() == targetBox && targetBox != areaBox, "target group is a separate group");
    auto* areaSpin = qobject_cast<QAbstractSpinBox*>(tab.tuneFieldWidget(TuneField::AreaMin));
    MIB_EXPECT(areaSpin && areaSpin->text().contains(QStringLiteral("µm²")), "unit shown on the control");
    MIB_EXPECT(tab.tuneStateText() == QStringLiteral("Applied") && !tab.tuneApplyButton()->isEnabled(), "clean panel: Applied, Apply disabled");

    // ---- 2. edit -> dirty, no mutation; apply without coordinator -----------
    wd.mark("dirty");
    tab.setTuneFieldForTests(TuneField::AreaMin, baseline.area_threshold_min + 50);
    settle(2);
    MIB_EXPECT(draft.dirty() && draft.changeCount() == 1 && tab.tuneStateText() == QStringLiteral("1 unapplied change"), "one edit -> dirty with count");
    MIB_EXPECT(processing.getProcessingConfig().area_threshold_min == baseline.area_threshold_min, "editing does not touch the backend");
    MIB_EXPECT(tab.tuneApplyButton()->isEnabled() && tab.tuneRevertButton()->isEnabled(), "Apply/Revert enabled when dirty");
    tab.setTuneFieldForTests(TuneField::DeformabilityMin, 0.12); // the displayed baseline
    settle(1);
    MIB_EXPECT(draft.changeCount() == 1 && draft.draft().deformability_threshold_min == 0.123456, "re-entering the displayed value is not an edit");
    tab.tuneApplyButton()->click();
    settle(2);
    MIB_EXPECT(draft.dirty() && !draft.applying() && tab.tuneStateText().startsWith(QStringLiteral("Not applied")), "no coordinator -> explicit failure, still dirty");
    MIB_EXPECT(processing.getProcessingConfig().area_threshold_min == baseline.area_threshold_min, "still no backend mutation");

    // ---- 3. fake coordinator: patch content, confirmed result -------------
    wd.mark("apply");
    frontend::ApplyProcessingDraftRequest lastRequest;
    int requests = 0;
    bool respond = true;
    QObject::connect(&tab, &frontend::ExperimentMonitoringTab::applyRequested, &tab, [&](const frontend::ApplyProcessingDraftRequest& req) {
        lastRequest = req;
        ++requests;
        if (!respond) return; // asynchronous coordinator: answered later
        frontend::ConfigApplyResult r;
        r.requestId = req.requestId;
        auto cfg = frontend::applyTunePatch(processing.getProcessingConfig(), req.patch);
        processing.setProcessingConfig(cfg);
        r.effectiveConfig = processing.getProcessingConfig();
        r.persisted = true;
        r.applied = frontend::tunePatchSatisfiedBy(r.effectiveConfig, req.patch);
        tab.onApplyResult(r);
    });
    tab.tuneApplyButton()->click();
    settle(2);
    MIB_EXPECT(requests == 1 && lastRequest.patch.values.size() == 1 && lastRequest.patch.values.count(TuneField::AreaMin) == 1,
               "request carries only the changed field");
    MIB_EXPECT(!draft.dirty() && tab.tuneStateText() == QStringLiteral("Applied"), "confirmed result clears Dirty");
    MIB_EXPECT(processing.getProcessingConfig().area_threshold_min == baseline.area_threshold_min + 50, "coordinator applied it");
    MIB_EXPECT(processing.getProcessingConfig().deformability_threshold_min == 0.123456, "untouched precision preserved end to end");

    // Failed result keeps Dirty.
    QObject::disconnect(&tab, &frontend::ExperimentMonitoringTab::applyRequested, nullptr, nullptr);
    QObject::connect(&tab, &frontend::ExperimentMonitoringTab::applyRequested, &tab, [&](const frontend::ApplyProcessingDraftRequest& req) {
        frontend::ConfigApplyResult r;
        r.requestId = req.requestId;
        r.error = QStringLiteral("disk full");
        r.effectiveConfig = processing.getProcessingConfig();
        tab.onApplyResult(r);
    });
    tab.setTuneFieldForTests(TuneField::RingRatioMax, 33.5);
    tab.tuneApplyButton()->click();
    settle(2);
    MIB_EXPECT(draft.dirty() && tab.tuneStateText() == QStringLiteral("Not applied: disk full"), "failed result keeps the draft");
    MIB_EXPECT(processing.getProcessingConfig().ring_ratio_max == baseline.ring_ratio_max, "backend untouched on failure");

    // ---- 4. revert ------------------------------------------------------------
    wd.mark("revert");
    tab.setTuneFieldForTests(TuneField::TargetAreaMax, 4444);
    settle(1);
    MIB_EXPECT(draft.changeCount() == 2 && tab.tuneStateText() == QStringLiteral("2 unapplied changes"), "two edits");
    tab.tuneRevertButton()->click();
    settle(2);
    MIB_EXPECT(!draft.dirty() && tab.tuneStateText() == QStringLiteral("Applied"), "revert -> clean");
    MIB_EXPECT(frontend::tuneValuesEqual(TuneField::RingRatioMax, draft.field(TuneField::RingRatioMax), baseline.ring_ratio_max)
                   && draft.field(TuneField::TargetAreaMax).toInt() == baseline.target_group_area_max,
               "controls restored to the authoritative values");

    // ---- 5. external changes ----------------------------------------------
    wd.mark("external");
    {
        auto cfg = processing.getProcessingConfig();
        cfg.ring_ratio_max = 30.0;
        processing.setProcessingConfig(cfg);
        tab.loadCurrentConfig();
        settle(2);
        MIB_EXPECT(!draft.dirty() && draft.field(TuneField::RingRatioMax).toDouble() == 30.0 && tab.tuneStateText() == QStringLiteral("Applied"),
                   "external change with no edits refreshes cleanly");
        tab.setTuneFieldForTests(TuneField::DeformabilityMax, 0.9);
        settle(1);
        cfg = processing.getProcessingConfig();
        cfg.area_threshold_max = 777;
        processing.setProcessingConfig(cfg);
        tab.loadCurrentConfig();
        settle(2);
        MIB_EXPECT(draft.conflict() && tab.tuneStateText().startsWith(QStringLiteral("Conflict")), "external change with edits -> visible Conflict");
        MIB_EXPECT(!tab.tuneApplyButton()->isEnabled() && tab.tuneRevertButton()->isEnabled(), "conflict: Apply blocked, Revert offered");
        MIB_EXPECT(draft.field(TuneField::DeformabilityMax).toDouble() == 0.9 && draft.field(TuneField::AreaMax).toInt() != 777, "draft retained");
        // Hidden panel keeps the conflict.
        tab.hide();
        settle(2);
        MIB_EXPECT(draft.conflict(), "conflict survives hiding");
        tab.show();
        settle(2);
        tab.tuneRevertButton()->click();
        settle(2);
        MIB_EXPECT(!draft.conflict() && !draft.dirty() && draft.field(TuneField::AreaMax).toInt() == 777, "revert loads the external values");
        MIB_EXPECT(frontend::tuneValuesEqual(TuneField::DeformabilityMax, draft.field(TuneField::DeformabilityMax), cfg.deformability_threshold_max),
                   "local edit discarded on revert");
    }

    // ---- 6. asynchronous coordinator: parked edits / reloads, stale result ---
    wd.mark("async");
    QObject::disconnect(&tab, &frontend::ExperimentMonitoringTab::applyRequested, nullptr, nullptr);
    respond = false;
    QObject::connect(&tab, &frontend::ExperimentMonitoringTab::applyRequested, &tab, [&](const frontend::ApplyProcessingDraftRequest& req) {
        lastRequest = req;
        ++requests;
    });
    tab.setTuneFieldForTests(TuneField::AreaRatioMax, 2.25);
    tab.tuneApplyButton()->click();
    settle(2);
    MIB_EXPECT(draft.applying() && tab.tuneStateText() == QStringLiteral("Applying…"), "applying state shown");
    MIB_EXPECT(!tab.tuneApplyButton()->isEnabled() && !tab.tuneRevertButton()->isEnabled(), "no second submission while applying");
    tab.setTuneFieldForTests(TuneField::AreaMin, 5);
    settle(1);
    MIB_EXPECT(draft.changeCount() == 1 && !draft.isChanged(TuneField::AreaMin), "edits ignored while applying");
    {
        auto cfg = processing.getProcessingConfig();
        cfg.ring_ratio_min = 11.0;
        processing.setProcessingConfig(cfg);
        tab.loadCurrentConfig(); // Deferred
        settle(1);
        MIB_EXPECT(draft.applying() && !draft.conflict(), "reload parked during apply");
    }
    frontend::ConfigApplyResult stale;
    stale.requestId = lastRequest.requestId + 99;
    stale.persisted = stale.applied = true;
    tab.onApplyResult(stale);
    settle(1);
    MIB_EXPECT(draft.applying(), "stale result ignored");
    frontend::ConfigApplyResult ok;
    ok.requestId = lastRequest.requestId;
    {
        auto cfg = frontend::applyTunePatch(processing.getProcessingConfig(), lastRequest.patch);
        processing.setProcessingConfig(cfg);
        ok.effectiveConfig = processing.getProcessingConfig();
    }
    ok.persisted = ok.applied = true;
    tab.onApplyResult(ok);
    settle(2);
    MIB_EXPECT(!draft.applying() && !draft.dirty() && !draft.conflict() && draft.baseline().ring_ratio_min == 11.0
                   && draft.field(TuneField::AreaRatioMax).toDouble() == 2.25,
               "confirmed result reconciles the parked reload");
    // Saved but not applied.
    tab.setTuneFieldForTests(TuneField::MultiImageCount, 4);
    tab.tuneApplyButton()->click();
    settle(1);
    frontend::ConfigApplyResult partial;
    partial.requestId = lastRequest.requestId;
    partial.persisted = true;
    partial.applied = false;
    partial.effectiveConfig = processing.getProcessingConfig();
    tab.onApplyResult(partial);
    settle(2);
    MIB_EXPECT(draft.dirty() && draft.savedNotApplied() && tab.tuneStateText().startsWith(QStringLiteral("Saved, not applied")), "saved-but-not-applied is explicit");
    tab.tuneRevertButton()->click();
    settle(2);

    // ---- 7. disabled criteria keep their values -------------------------------
    wd.mark("disabled");
    {
        const int shown = qobject_cast<QAbstractSpinBox*>(tab.tuneFieldWidget(TuneField::AreaMin))->text().remove(QStringLiteral(" µm²")).toInt();
        tab.setTuneFieldForTests(TuneField::AreaEnabled, false);
        settle(2);
        auto* minSpin = tab.tuneFieldWidget(TuneField::AreaMin);
        MIB_EXPECT(!minSpin->isEnabled() && minSpin->isVisibleTo(&tab), "disabled criterion: value control disabled, not hidden");
        MIB_EXPECT(qobject_cast<QAbstractSpinBox*>(minSpin)->text().remove(QStringLiteral(" µm²")).toInt() == shown, "configured value still shown");
        MIB_EXPECT(!areaBox->isChecked() && draft.isChanged(TuneField::AreaEnabled), "enable toggle is an edit like any other");
        tab.tuneRevertButton()->click();
        settle(2);
        MIB_EXPECT(areaBox->isChecked() && minSpin->isEnabled(), "revert re-enables");
    }

    // ---- 8. compact widths + fixed footer -----------------------------------
    wd.mark("widths");
    for (int width : {frontend::ExperimentMonitoringTab::kTunePanelMinWidth, frontend::ExperimentMonitoringTab::kTunePanelMaxWidth}) {
        tab.tunePanel()->setMinimumWidth(width);
        tab.tunePanel()->setMaximumWidth(width);
        tab.resize(1100, 620);
        settle(6);
        QScrollArea* scroll = tab.tuneScrollArea();
        MIB_REQUIRE(scroll && scroll->widget(), "scroll area");
        MIB_EXPECT(tab.tunePanel()->width() == width, "panel width applied");
        const int viewport = scroll->viewport()->width();
        if (scroll->widget()->minimumSizeHint().width() > viewport) {
            fprintf(stderr, "tune panel overflow at %d: content min %d > viewport %d\n", width, scroll->widget()->minimumSizeHint().width(), viewport);
            for (QWidget* w : scroll->widget()->findChildren<QWidget*>())
                if (w->minimumSizeHint().width() > viewport - 40)
                    fprintf(stderr, "  %s (%s) min %d\n", w->metaObject()->className(), qPrintable(w->objectName()), w->minimumSizeHint().width());
        }
        MIB_EXPECT(scroll->widget()->minimumSizeHint().width() <= viewport, "content fits the compact width without horizontal overflow");
        MIB_EXPECT(scroll->horizontalScrollBar()->maximum() == 0, "no horizontal scrolling required");
        for (TuneField f : frontend::allTuneFields()) {
            QWidget* w = tab.tuneFieldWidget(f);
            const QRect r = QRect(w->mapTo(scroll->widget(), QPoint(0, 0)), w->size());
            MIB_EXPECT(r.right() <= scroll->widget()->width(), "no control is clipped horizontally");
        }
        QWidget* footer = tab.tuneFooter();
        MIB_REQUIRE(footer, "footer");
        const QRect footerInPanel(footer->mapTo(tab.tunePanel(), QPoint(0, 0)), footer->size());
        auto* vbar = scroll->verticalScrollBar();
        MIB_EXPECT(vbar->maximum() > 0, "content scrolls vertically at this height");
        for (int pos : {0, vbar->maximum() / 2, vbar->maximum()}) {
            vbar->setValue(pos);
            settle(2);
            const QRect now(footer->mapTo(tab.tunePanel(), QPoint(0, 0)), footer->size());
            MIB_EXPECT(now == footerInPanel && footer->isVisibleTo(&tab) && now.bottom() <= tab.tunePanel()->height(),
                       "Apply/Revert footer stays put and visible at every scroll position");
            MIB_EXPECT(tab.tuneApplyButton()->isVisibleTo(&tab) && tab.tuneRevertButton()->isVisibleTo(&tab), "buttons visible");
        }
    }
    MIB_EXPECT(tab.tuneApplyButton()->focusPolicy() != Qt::NoFocus && tab.tuneRevertButton()->focusPolicy() != Qt::NoFocus, "footer buttons keyboard reachable");
    MIB_EXPECT(!draft.dirty(), "layout work caused no edits");
    MIB_EXPECT(processing.getProcessingConfig().area_threshold_max == 777, "layout work caused no backend mutation");

    tab.close();
    settle(2);
    backend.shutdown();
    return mib::test::exitCode();
}
