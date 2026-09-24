// monitoring_kde_density_test
//
// ExperimentMonitoringTab scatter density (KDE) colouring (offscreen, mock
// backend, frames injected directly into the rolling buffer):
//  - off by default: the plain series carry the points, the density-level
//    series are empty and hidden, timer idle;
//  - enabling re-routes every point into a density-level series at once
//    (sparsest level until the first estimate), starts the periodic timer
//    and launches the estimate on a worker thread; when it lands, crowded
//    points sit in higher levels than isolated ones and the densest point
//    reaches the top level; target-group points use the rectangle family;
//  - points that arrive after an estimate sit in the sparsest level until
//    the next estimate, which then places them;
//  - an unchanged buffer does not relaunch the job; a large buffer is
//    computed asynchronously (the call returns with the job in flight);
//  - hide stops the timer, show restarts it; disabling empties and hides the
//    level series and restores the plain ones;
//  - the three settings persist through QSettings; a fresh tab and the
//    Monitoring Settings dialog read them back.

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingTypes.h"
#include "frontend/dialogs/MonitoringSettingsDialog.h"
#include "frontend/tabs/ExperimentMonitoringTab.h"
#include "frontend/tabs/MonitoringDensity.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineSeries>
#include <QPen>
#include <QPushButton>
#include <QScatterSeries>
#include <QSettings>
#include <QSpinBox>
#include <QStyleFactory>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

namespace {

using Tab = frontend::ExperimentMonitoringTab;

void settle(int rounds = 4) {
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!pred() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
    return pred();
}

backend::services::ProcessedFrame frame(uint64_t index, double areaPx, double deform,
                                        bool target = false) {
    backend::services::ProcessedFrame f;
    f.index = index;
    f.validation.isValid = true;
    f.validation.isTargetGroup = target;
    f.validation.area = areaPx;
    f.validation.deformability = deform;
    return f;
}

int levelPoints(const std::vector<QScatterSeries*>& series) {
    int n = 0;
    for (const auto* s : series)
        n += s->count();
    return n;
}

bool allHidden(const std::vector<QScatterSeries*>& series) {
    return std::all_of(series.begin(), series.end(),
                       [](const QScatterSeries* s) { return !s->isVisible() && s->count() == 0; });
}

double densityOf(const Tab& tab, uint64_t index) {
    double d = -1.0;
    return tab.kdeDensityForFrame(index, d) ? d : -1.0;
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES",
            QByteArrayLiteral("auto_update,autofocus,trigger,yolo,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL",
            QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL",
            QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR"))
        qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    mib::test::Watchdog wd(120);
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    mib::test::TempDir td("monitoring_kde");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");

    std::mt19937 rng(20260923);
    std::normal_distribution<double> clusterArea(300.0, 12.0), clusterDeform(0.05, 0.008);
    std::uniform_real_distribution<double> farArea(700.0, 1000.0), farDeform(0.5, 0.95);

    // Frames 0..299: a tight cluster (frame 0 at its centre); 300..329: isolated
    // outliers; 330..334: target-group members inside the cluster.
    std::vector<backend::services::ProcessedFrame> frames;
    frames.push_back(frame(0, 300.0, 0.05));
    for (uint64_t i = 1; i < 300; ++i)
        frames.push_back(frame(i, clusterArea(rng), clusterDeform(rng)));
    for (uint64_t i = 300; i < 330; ++i)
        frames.push_back(frame(i, farArea(rng), farDeform(rng)));
    for (uint64_t i = 330; i < 335; ++i)
        frames.push_back(frame(i, clusterArea(rng), clusterDeform(rng), true));

    {
        Tab tab(backend);
        tab.resize(1100, 760);
        tab.show();
        settle(4);
        QScatterSeries* scatter = tab.scatterSeriesForTests();
        QScatterSeries* target = tab.targetGroupSeriesForTests();
        const auto& levels = tab.kdeLevelSeriesForTests();
        const auto& targetLevels = tab.kdeTargetLevelSeriesForTests();
        MIB_REQUIRE(scatter && target, "series exposed");
        MIB_REQUIRE(levels.size() == Tab::kKdeLevels && targetLevels.size() == Tab::kKdeLevels,
                    "one series per density level, per family");

        // ---- 1. defaults --------------------------------------------------------
        wd.mark("defaults");
        MIB_EXPECT(!tab.kdeEnabled() && !tab.kdeToggle()->isChecked(), "KDE off by default");
        MIB_EXPECT(!tab.kdeTimerActive() && !tab.kdeJobInFlight(), "no timer, no job while off");
        MIB_EXPECT(tab.kdeBandwidthFactor() == Tab::kKdeBandwidthFactorDefault &&
                       tab.kdeIntervalMs() == Tab::kKdeIntervalMsDefault,
                   "default factor and interval");
        tab.injectMonitoringFramesForTests(frames);
        settle(2);
        MIB_EXPECT(scatter->count() == 330 && target->count() == 5,
                   "frames plotted into the plain series");
        MIB_EXPECT(scatter->isVisible() && target->isVisible(), "off: plain series visible");
        MIB_EXPECT(allHidden(levels) && allHidden(targetLevels),
                   "off: level series empty and hidden");
        MIB_EXPECT(Tab::kdeLevelForDensity(0.0) == 0 &&
                       Tab::kdeLevelForDensity(1.0) == Tab::kKdeLevels - 1 &&
                       Tab::kdeLevelForDensity(std::nan("")) == 0,
                   "density -> level mapping covers both ends and non-finite input");

        // ---- 2. enable: immediate re-routing, asynchronous estimate --------------
        wd.mark("enable");
        tab.kdeToggle()->setChecked(true);
        settle(1);
        MIB_EXPECT(tab.kdeEnabled() && tab.kdeTimerActive(),
                   "toggle enables and starts the periodic timer");
        MIB_EXPECT(!scatter->isVisible() && !target->isVisible() && scatter->count() == 0,
                   "on: plain series hidden and empty");
        MIB_EXPECT(levelPoints(levels) == 330 && levelPoints(targetLevels) == 5,
                   "every point routed into a level series as soon as KDE is on");
        // A 335-point estimate can finish within one settle round, so only
        // claim "everything sparse" while no estimate has landed yet.
        MIB_EXPECT(tab.kdeGeneration() >= 1 ||
                       (levels[0]->count() == 330 && targetLevels[0]->count() == 5),
                   "before the first estimate everything sits in the sparsest level");
        MIB_EXPECT(targetLevels[0]->markerShape() == QScatterSeries::MarkerShapeRectangle &&
                       levels[0]->markerShape() == QScatterSeries::MarkerShapeCircle,
                   "target family keeps its identity by shape");
        MIB_REQUIRE(waitFor([&] { return tab.kdeGeneration() >= 1; }, 15000),
                    "first estimate lands");
        MIB_EXPECT(!tab.kdeJobInFlight(), "job released after completion");
        settle(1);
        {
            std::vector<double> cluster, outliers;
            for (uint64_t i = 0; i < 300; ++i)
                cluster.push_back(densityOf(tab, i));
            for (uint64_t i = 300; i < 330; ++i)
                outliers.push_back(densityOf(tab, i));
            std::sort(cluster.begin(), cluster.end());
            const double clusterMedian = cluster[cluster.size() / 2];
            const double outlierMax = *std::max_element(outliers.begin(), outliers.end());
            MIB_EXPECT(cluster.front() >= 0.0 && outlierMax >= 0.0,
                       "every injected frame has a density");
            MIB_EXPECT(clusterMedian > outlierMax, "crowded points are denser than isolated ones");
            MIB_EXPECT(cluster.back() == 1.0, "the densest point reaches the top of the range");
            MIB_EXPECT(densityOf(tab, 330) > outlierMax,
                       "target-group members inside the cluster are dense too");
            MIB_EXPECT(levels[Tab::kKdeLevels - 1]->count() >= 1,
                       "the top level series holds the densest points");
            MIB_EXPECT(levelPoints(levels) == 330 && levelPoints(targetLevels) == 5,
                       "re-routing after the estimate conserves every point");
            MIB_EXPECT(levels[0]->count() < 330, "points moved out of the sparsest level");
        }

        // ---- 3. late points: sparsest level until the next estimate ---------------
        wd.mark("late");
        const int sparseBefore = levels[0]->count();
        std::vector<backend::services::ProcessedFrame> late;
        for (uint64_t i = 335; i < 340; ++i)
            late.push_back(frame(i, clusterArea(rng), clusterDeform(rng)));
        tab.injectMonitoringFramesForTests(late);
        settle(1);
        MIB_EXPECT(levelPoints(levels) == 335, "late points are plotted");
        MIB_EXPECT(levels[0]->count() == sparseBefore + 5,
                   "late points wait in the sparsest level");
        MIB_EXPECT(densityOf(tab, 337) < 0.0, "late points have no density yet");
        const uint64_t before = tab.kdeGeneration();
        tab.requestKdeUpdate();
        MIB_REQUIRE(waitFor([&] { return tab.kdeGeneration() > before; }, 15000),
                    "second estimate lands");
        settle(1);
        bool lateDense = true;
        for (uint64_t i = 335; i < 340; ++i)
            lateDense = lateDense && densityOf(tab, i) > 0.0;
        MIB_EXPECT(lateDense, "late cluster points are placed by the next estimate");

        // ---- 3b. core contour on the scatter, pinned reference ------------------
        wd.mark("contour");
        {
            const auto& contours = tab.kdeContourSeriesForTests();
            MIB_EXPECT(!tab.lastKdeContours().empty() && contours.size() == tab.lastKdeContours().size(),
                       "the live core contour is drawn as one line series per loop");
            MIB_EXPECT(tab.lastKdeCoreCount() >= 250 && tab.lastKdeCoreCount() <= 320,
                       "90% core of 335 samples holds ~302 cells");
            MIB_EXPECT(tab.kdeReferenceSeriesForTests().empty() && !tab.hasKdeReference(), "no reference yet");
            for (auto* s : contours)
                MIB_EXPECT(s->pen().style() == Qt::SolidLine && s->count() >= 4, "live contour: solid, closed polyline");
            tab.pinKdeReference();
            settle(1);
            MIB_EXPECT(tab.hasKdeReference() && tab.kdeReferenceSeriesForTests().size() == contours.size(),
                       "pinning copies the live loops into dashed reference series");
            for (auto* s : tab.kdeReferenceSeriesForTests())
                MIB_EXPECT(s->pen().style() == Qt::DashLine, "reference contour is dashed");
            const std::size_t coreBefore = tab.lastKdeCoreCount();
            const uint64_t genBefore = tab.kdeGeneration();
            tab.setKdeCoreFraction(0.5);
            MIB_REQUIRE(waitFor([&] { return tab.kdeGeneration() > genBefore; }, 15000), "fraction change re-estimates");
            settle(1);
            MIB_EXPECT(tab.lastKdeCoreCount() < coreBefore && tab.lastKdeCoreCount() >= 160,
                       "a 50% core holds fewer cells than the 90% core");
            MIB_EXPECT(tab.kdeReferenceSeriesForTests().size() == contours.size() || tab.hasKdeReference(),
                       "reference survives a re-estimate");
            tab.clearKdeReference();
            MIB_EXPECT(!tab.hasKdeReference() && tab.kdeReferenceSeriesForTests().empty(), "clear removes the reference");
            tab.setKdeCoreFraction(0.9);
            const uint64_t genRestore = tab.kdeGeneration();
            MIB_REQUIRE(waitFor([&] { return tab.kdeGeneration() > genRestore; }, 15000), "fraction restored");
        }

        // ---- 4. unchanged buffer: no relaunch; large buffer: asynchronous -------
        wd.mark("fingerprint");
        const uint64_t gen = tab.kdeGeneration();
        tab.requestKdeUpdate();
        MIB_EXPECT(!tab.kdeJobInFlight(), "unchanged buffer does not relaunch the estimate");
        settle(2);
        MIB_EXPECT(tab.kdeGeneration() == gen, "no new generation for an unchanged buffer");
        std::vector<backend::services::ProcessedFrame> many;
        for (uint64_t i = 1000; i < 2500; ++i)
            many.push_back(
                frame(i, clusterArea(rng) + (i % 7) * 40.0, clusterDeform(rng) + (i % 5) * 0.05));
        tab.injectMonitoringFramesForTests(many);
        settle(1);
        MIB_EXPECT(levelPoints(levels) + levelPoints(targetLevels) == 1000,
                   "rolling buffer capped at 1000 points");
        tab.requestKdeUpdate();
        MIB_EXPECT(tab.kdeJobInFlight(),
                   "a 1000-point estimate returns immediately with the job in flight");
        MIB_REQUIRE(waitFor([&] { return tab.kdeGeneration() > gen; }, 15000),
                    "large estimate lands");
        MIB_EXPECT(levelPoints(levels) + levelPoints(targetLevels) == 1000,
                   "all 1000 points still plotted");

        // ---- 5. hide / show, disable --------------------------------------------
        wd.mark("visibility");
        tab.hide();
        settle(2);
        MIB_EXPECT(!tab.kdeTimerActive(), "hidden tab stops the KDE timer");
        tab.show();
        settle(2);
        MIB_EXPECT(tab.kdeTimerActive(), "shown tab restarts the KDE timer");
        tab.kdeToggle()->setChecked(false);
        settle(2);
        MIB_EXPECT(!tab.kdeEnabled() && !tab.kdeTimerActive(), "toggle off stops the timer");
        MIB_EXPECT(allHidden(levels) && allHidden(targetLevels),
                   "off: level series emptied and hidden");
        MIB_EXPECT(scatter->isVisible() && target->isVisible() &&
                       scatter->count() + target->count() == 1000,
                   "off: plain series back with every point");
        MIB_EXPECT(tab.kdeContourSeriesForTests().empty() && tab.kdeReferenceSeriesForTests().empty(),
                   "off: no contour series remain on the chart");

        // ---- 6. settings persist ------------------------------------------------
        wd.mark("persist");
        tab.setKdeBandwidthFactor(1.5);
        tab.setKdeIntervalMs(3000);
        tab.setKdeIntervalMs(10); // below the floor: clamped, not accepted verbatim
        MIB_EXPECT(tab.kdeIntervalMs() == Tab::kKdeIntervalMsMin, "interval clamped to its floor");
        tab.setKdeIntervalMs(3000);
        tab.setKdeBandwidthFactor(99.0);
        MIB_EXPECT(tab.kdeBandwidthFactor() == Tab::kKdeBandwidthFactorMax,
                   "factor clamped to its ceiling");
        tab.setKdeBandwidthFactor(1.5);
        tab.setKdeEnabled(true);
        settle(1);
        {
            MonitoringSettingsDialog dlg(&tab);
            auto* factorSpin = dlg.findChild<QDoubleSpinBox*>(QStringLiteral("kdeBandwidthSpin"));
            auto* intervalSpin = dlg.findChild<QSpinBox*>(QStringLiteral("kdeIntervalSpin"));
            MIB_REQUIRE(factorSpin && intervalSpin, "dialog exposes the KDE controls");
            MIB_EXPECT(factorSpin->value() == 1.5 && intervalSpin->value() == 3000,
                       "dialog reads the tab's KDE settings");
            MIB_EXPECT(dlg.findChild<QSpinBox*>(QStringLiteral("kdeGridResolutionSpin")) == nullptr,
                       "grid resolution control retired");
            auto* coreSpin = dlg.findChild<QSpinBox*>(QStringLiteral("kdeCoreFractionSpin"));
            MIB_REQUIRE(coreSpin, "dialog exposes the core contour percentage");
            MIB_EXPECT(coreSpin->value() == 90, "dialog reads the core fraction");
            MIB_EXPECT(dlg.findChild<QPushButton*>(QStringLiteral("kdePinReferenceBtn")) != nullptr
                           && dlg.findChild<QPushButton*>(QStringLiteral("kdeClearReferenceBtn")) != nullptr,
                       "dialog carries the reference actions");
        }
        tab.close();
        settle(2);
    }
    {
        Tab again(backend);
        MIB_EXPECT(again.kdeEnabled() && again.kdeToggle()->isChecked(),
                   "enabled state restored from settings");
        MIB_EXPECT(again.kdeBandwidthFactor() == 1.5 && again.kdeIntervalMs() == 3000,
                   "factor and interval restored from settings");
        MIB_EXPECT(!again.kdeTimerActive(), "restored but hidden: timer idle");
        again.show();
        settle(2);
        MIB_EXPECT(again.kdeTimerActive(), "restored and shown: timer runs");
        MIB_EXPECT(!again.scatterSeriesForTests()->isVisible() &&
                       again.kdeLevelSeriesForTests()[0]->isVisible(),
                   "restored: density-level series are the visible ones");
        again.close();
        settle(2);
    }

    backend.shutdown();
    return mib::test::exitCode();
}
