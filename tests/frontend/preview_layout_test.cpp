// preview_layout_test (issue #362)
//
// PreviewPage inspector modes (offscreen, isolated settings, mock backend):
//  - first launch at 1366x700 uses the image-biased default (no 50/50);
//  - Expanded / Compact / Hidden are explicit: Compact shows the header only
//    (editors hidden), Hidden hides the inspector, the mode bar stays;
//  - the expanded inspector never becomes a few-pixel sliver;
//  - user drag persists the ratio; a new page restores it; an oversized or
//    corrupt saved state is clamped/defaulted;
//  - 30 mode cycles leave the outer geometry unchanged;
//  - a temporary workflow mode does not overwrite the preference;
//  - edited JSON survives Compact -> external change -> Expanded with the
//    conflict visible (depends on #361's explicit state);
//  - a narrow page height falls back to Compact while keeping the preference.

#include "backend/app/AppBackend.h"
#include "frontend/tabs/ConfigTabs.h"
#include "frontend/tabs/PreviewPage.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
#include <QSplitter>
#include <QElapsedTimer>
#include <QThread>
#include <QStyleFactory>

#include <cmath>
#include <memory>

namespace {
void settle(int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}
// Real-time settle so debounced timers (relayout 80 ms, persist 300 ms) fire.
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
using Mode = frontend::PreviewPage::InspectorMode;
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
    mib::test::TempDir td("preview_layout");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    const QString cfgPath = QString::fromStdString((td.path() / "config.json").string());
    {
        QFile f(cfgPath);
        f.open(QIODevice::WriteOnly);
        f.write("{\n  \"processing\": {\"area_threshold_min\": 10}\n}\n");
        QSettings s;
        s.setValue(QStringLiteral("Config/ExternalAppConfigPath"), cfgPath);
    }
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");

    const QSize page(1366, 700);
    auto make = [&]() {
        auto p = std::make_unique<frontend::PreviewPage>(backend);
        p->getConfigTabs()->setNonInteractiveForTests(true);
        p->resize(page);
        p->show();
        settle(10);
        return p;
    };

    // ---- 1. first launch default ----------------------------------------------
    {
        wd.mark("default");
        auto p = make();
        auto* sp = p->splitter();
        const auto sizes = sp->sizes();
        MIB_REQUIRE(sizes.size() == 2, "two splitter children");
        const int usable = sizes[0] + sizes[1];
        const double frac = static_cast<double>(sizes[1]) / usable;
        std::fprintf(stderr, "default: image=%d inspector=%d frac=%.2f\n", sizes[0], sizes[1], frac);
        MIB_EXPECT(p->effectiveInspectorMode() == Mode::Expanded, "expanded by default");
        MIB_EXPECT(std::fabs(frac - 0.5) > 0.05, "not a 50/50 split");
        MIB_EXPECT(frac >= 0.25 && frac <= 0.45, "image-biased default allocation");
        MIB_EXPECT(sizes[1] >= frontend::PreviewPage::kInspectorMinExpandedHeight, "no sliver");
        MIB_EXPECT(p->inspectorBar() && p->inspectorBar()->isVisible(), "mode bar visible");
        MIB_EXPECT(p->inspectorModeAction(Mode::Expanded)->isChecked(), "Expanded action checked");

        // ---- 2. explicit modes ---------------------------------------------------
        wd.mark("modes");
        const QSize outer = p->size();
        p->setInspectorMode(Mode::Compact);
        settle(6);
        MIB_EXPECT(p->effectiveInspectorMode() == Mode::Compact && p->getConfigTabs()->isCompactMode(), "compact mode");
        MIB_EXPECT(p->getConfigTabs()->isVisible() && p->getConfigTabs()->headerWidget()->isVisible(), "compact keeps the header");
        MIB_EXPECT(p->getConfigTabs()->height() <= 80, "compact inspector is header-height (" + std::to_string(p->getConfigTabs()->height()) + ")");
        MIB_EXPECT(p->inspectorBar()->isVisible() && p->inspectorModeAction(Mode::Compact)->isChecked(), "bar + compact checked");
        p->setInspectorMode(Mode::Hidden);
        settle(6);
        MIB_EXPECT(p->effectiveInspectorMode() == Mode::Hidden && !p->getConfigTabs()->isVisible(), "hidden mode hides the inspector");
        MIB_EXPECT(p->inspectorBar()->isVisible() && p->inspectorModeAction(Mode::Expanded)->isEnabled(), "reopen affordance remains");
        MIB_EXPECT(sp->sizes()[0] >= usable - 2, "image takes the space");
        p->setInspectorMode(Mode::Expanded);
        settle(6);
        MIB_EXPECT(p->effectiveInspectorMode() == Mode::Expanded && !p->getConfigTabs()->isCompactMode() &&
                       sp->sizes()[1] >= frontend::PreviewPage::kInspectorMinExpandedHeight,
                   "expanded again with a usable height");
        MIB_EXPECT(p->size() == outer, "mode changes did not change the page size");

        // ---- 3. drag persists -----------------------------------------------------
        wd.mark("drag");
        const int wantedInspector = static_cast<int>(usable * 0.42);
        sp->setSizes({usable - wantedInspector, wantedInspector});
        emit sp->splitterMoved(usable - wantedInspector, 1);
        settle(8);
        MIB_EXPECT(std::fabs(p->preferredInspectorRatio() - 0.42) < 0.03, "drag recorded the ratio");
        for (int i = 0; i < 30; ++i) {
            p->setInspectorMode(i % 2 ? Mode::Compact : Mode::Expanded);
            settle(2);
        }
        p->setInspectorMode(Mode::Expanded);
        settle(6);
        MIB_EXPECT(p->size() == outer, "30 cycles: outer geometry unchanged");
        MIB_EXPECT(std::fabs(static_cast<double>(sp->sizes()[1]) / usable - 0.42) < 0.03, "expanded height restored after cycles");

        // ---- 4. temporary workflow mode ------------------------------------------
        wd.mark("temporary");
        p->setTemporaryInspectorMode(Mode::Compact);
        settle(4);
        MIB_EXPECT(p->effectiveInspectorMode() == Mode::Compact && p->preferredInspectorMode() == Mode::Expanded,
                   "temporary compact keeps the preference");
        p->setTemporaryInspectorMode(std::nullopt);
        settle(6);
        MIB_EXPECT(p->effectiveInspectorMode() == Mode::Expanded, "ending the override restores the preference");

        // ---- 5. edits survive compact + external change ---------------------------
        wd.mark("edits");
        auto* tabs = p->getConfigTabs();
        tabs->setAppConfigEditorText(QStringLiteral("{\n  \"processing\": {\"area_threshold_min\": 11}\n}\n"));
        settle(2);
        p->setInspectorMode(Mode::Compact);
        settle(2);
        {
            QFile f(cfgPath);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write("{\n  \"processing\": {\"area_threshold_min\": 55}\n}\n");
        }
        tabs->onExternalConfigFileChanged(cfgPath);
        p->setInspectorMode(Mode::Expanded);
        settle(6);
        MIB_EXPECT(tabs->appConfigDocument().dirty && tabs->appConfigDocument().conflict &&
                       tabs->appConfigEditorText().contains(QStringLiteral("11")),
                   "local edit retained with conflict after compact + external change");
        MIB_EXPECT(!tabs->noticesText().isEmpty(), "conflict visible");
        p->close();
        settleMs(400);
        QSettings s;
        MIB_EXPECT(s.value(QStringLiteral("Preview/LayoutVersion")).toInt() == frontend::PreviewPage::kLayoutVersion &&
                       std::fabs(s.value(QStringLiteral("Preview/InspectorRatio")).toDouble() - 0.42) < 0.03 &&
                       s.value(QStringLiteral("Preview/InspectorMode")).toString() == QStringLiteral("expanded"),
                   "preference persisted with version");
    }

    // ---- 6. restore + clamp -------------------------------------------------------
    {
        wd.mark("restore");
        auto p = make();
        const auto sizes = p->splitter()->sizes();
        const int usable = sizes[0] + sizes[1];
        MIB_EXPECT(std::fabs(static_cast<double>(sizes[1]) / usable - 0.42) < 0.03, "restart restores the dragged ratio");
        p->close();
        settle(2);
        QSettings s;
        s.setValue(QStringLiteral("Preview/InspectorRatio"), 0.93); // large-screen habit
        auto p2 = make();
        const auto s2 = p2->splitter()->sizes();
        MIB_EXPECT(s2[0] >= frontend::PreviewPage::kImageMinHeight, "oversized ratio clamped: image keeps its minimum");
        p2->close();
        settle(2);
        s.setValue(QStringLiteral("Preview/InspectorRatio"), QStringLiteral("garbage"));
        s.setValue(QStringLiteral("Preview/InspectorMode"), QStringLiteral("sideways"));
        auto p3 = make();
        MIB_EXPECT(std::fabs(p3->preferredInspectorRatio() - frontend::PreviewPage::kDefaultInspectorRatio) < 1e-9 &&
                       p3->preferredInspectorMode() == Mode::Expanded,
                   "corrupt state -> defaults");
        // Narrow height: expanded preference falls back to compact for space.
        p3->resize(1366, 300);
        settleMs(300);
        MIB_EXPECT(p3->effectiveInspectorMode() == Mode::Compact && p3->preferredInspectorMode() == Mode::Expanded,
                   "too little height -> compact shown, preference kept");
        p3->resize(page);
        settleMs(300);
        MIB_EXPECT(p3->effectiveInspectorMode() == Mode::Expanded, "space returns -> expanded again");
        p3->close();
        settle(2);
    }

    backend.shutdown();
    return mib::test::exitCode();
}
