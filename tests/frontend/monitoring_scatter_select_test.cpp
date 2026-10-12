// monitoring_scatter_select_test
//
// ExperimentMonitoringTab scatter click-to-view during a run (the Review
// tab's interaction, issue #467, on the live monitoring buffer). Offscreen,
// mock backend, frames injected into the rolling buffer:
//  - the pane is hidden and nothing is highlighted until a point is clicked;
//  - a click on point k shows the frame behind it (point k is not frame k:
//    a frame without analysis-time calibration has no point), draws the
//    highlight on it and leaves the axes alone; empty space changes nothing;
//  - prev/next walk the plotted points and wrap;
//  - the pane keeps its copy when the frame leaves the rolling buffer; the
//    highlight goes and the title says so;
//  - with KDE on (points in level series) a click still selects;
//  - chart snapshots never show the highlight and give it back;
//  - "Open in window…" opens a non-modal viewer that follows the selection;
//    the close button clears the selection, hides the pane and the window.

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingTypes.h"
#include "frontend/dialogs/FrameViewerDialog.h"
#include "frontend/tabs/ExperimentMonitoringTab.h"
#include "frontend/utils/ApplicationSettings.h"
#include "frontend/widgets/ZoomableChartView.h"

#include "support/assert.h"
#include "support/qt_mouse.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QCheckBox>
#include <QChart>
#include <QCoreApplication>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QScatterSeries>
#include <QSettings>
#include <QStyleFactory>
#include <QToolButton>
#include <QValueAxis>

#include <opencv2/core.hpp>

#include <optional>
#include <vector>

namespace {

using Tab = frontend::ExperimentMonitoringTab;
namespace mouse = mib::test::mouse;

void settle(int rounds = 4) {
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

double gAnalysisFactor = 0.0;

// Frame indices start at 1000 so a point position never equals its frame.
backend::services::ProcessedFrame frame(uint64_t index, double areaPx, double deform, bool target = false,
                                        bool calibrated = true) {
    backend::services::ProcessedFrame f;
    f.index = index;
    f.timestampNs = (index + 1) * 1000ULL;
    f.originalImage = cv::Mat(48, 64, CV_8UC1, cv::Scalar(static_cast<double>(40 + index % 200)));
    f.processedImage = cv::Mat(48, 64, CV_8UC1, cv::Scalar(0));
    f.validation.isValid = true;
    f.validation.isTargetGroup = target;
    f.validation.area = areaPx;
    f.validation.deformability = deform;
    f.validation.analysisPixelToMicronFactor = calibrated ? gAnalysisFactor : 0.0;
    return f;
}

struct Ranges {
    double x0, x1, y0, y1;
};

Ranges ranges(QChart* chart) {
    auto* x = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).value(0));
    auto* y = qobject_cast<QValueAxis*>(chart->axes(Qt::Vertical).value(0));
    return {x->min(), x->max(), y->min(), y->max()};
}

bool same(const Ranges& a, const Ranges& b) {
    return a.x0 == b.x0 && a.x1 == b.x1 && a.y0 == b.y0 && a.y1 == b.y1;
}

std::optional<std::size_t> pointOfFrame(const Tab& tab, uint64_t frameIndex) {
    const auto& frames = tab.scatterPointFramesForTests();
    for (std::size_t i = 0; i < frames.size(); ++i)
        if (frames[i] == frameIndex) return i;
    return std::nullopt;
}

bool paneShowsImage(const Tab& tab) {
    auto* image = tab.framePaneViewerForTests()->findChild<QLabel*>(QStringLiteral("imageLabel"));
    return image && !image->pixmap().isNull();
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR"))
        qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    mib::test::Watchdog wd(120);
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    mib::test::TempDir td("monitoring_scatter_select");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    gAnalysisFactor = backend.processing().getPixelToMicronFactor();
    MIB_REQUIRE(gAnalysisFactor > 0, "pixel-to-micron factor");

    // 40 frames spread over the default axes (area 0..1000 µm², deformability
    // 0..1); frame 1003 has no calibration and therefore no point.
    const double umToPx2 = 1.0 / (gAnalysisFactor * gAnalysisFactor);
    std::vector<backend::services::ProcessedFrame> frames;
    for (uint64_t i = 0; i < 40; ++i) {
        const double areaUm = 100.0 + 20.0 * static_cast<double>(i);
        const double deform = 0.1 + 0.02 * static_cast<double>(i % 30);
        frames.push_back(frame(1000 + i, areaUm * umToPx2, deform, i % 7 == 0, i != 3));
    }

    {
        Tab tab(backend);
        tab.resize(1200, 800);
        tab.show();
        settle(4);
        auto* view = tab.scatterViewForTests();
        auto* chart = view->chart();
        auto* highlight = tab.scatterHighlightForTests();
        MIB_REQUIRE(view && chart && highlight, "scatter surfaces exposed");

        // ---- 1. nothing selected before a click -------------------------------
        wd.mark("initial");
        tab.injectMonitoringFramesForTests(frames);
        settle(2);
        MIB_REQUIRE(tab.scatterPointFramesForTests().size() == 39, "39 plotted points (one uncalibrated frame)");
        MIB_EXPECT(!tab.selectedFrameIndex(), "nothing selected");
        MIB_EXPECT(!tab.framePaneForTests()->isVisible(), "pane hidden until a point is clicked");
        MIB_EXPECT(!highlight->isVisible(), "no highlight");

        // ---- 2. click selects the frame behind the point ----------------------
        wd.mark("click");
        const std::size_t k = 10; // after the uncalibrated frame: point 10 is frame 1011
        const uint64_t frameK = tab.scatterPointFramesForTests()[k];
        MIB_REQUIRE(frameK == 1011, "point k stands for frame 1011");
        const Ranges home = ranges(chart);
        mouse::click(view, tab.scatterPointViewPosForTests(k));
        settle(4);
        MIB_EXPECT(tab.selectedFrameIndex() == frameK, "click selects the frame behind the point");
        MIB_EXPECT(tab.framePaneForTests()->isVisible(), "pane shown");
        MIB_EXPECT(paneShowsImage(tab), "pane shows the cell image");
        MIB_EXPECT(tab.framePaneTitleForTests().contains(QStringLiteral("Frame 1011")), "title names the frame");
        MIB_EXPECT(highlight->isVisible() && highlight->count() == 1, "highlight drawn");
        MIB_EXPECT(same(ranges(chart), home), "a click does not move the axes");
        const QRect paneRect(tab.framePaneForTests()->mapToGlobal(QPoint(0, 0)), tab.framePaneForTests()->size());
        const QRect viewRect(view->mapToGlobal(QPoint(0, 0)), view->size());
        MIB_EXPECT(!paneRect.intersects(viewRect), "the pane never covers the scatter");

        // Empty space (top-left corner of the plot, far from every point).
        const QRectF plot = chart->plotArea();
        const QPointF empty = QPointF(view->mapFromScene(chart->mapToScene(plot.topLeft() + QPointF(4, 4))));
        mouse::click(view, empty);
        settle(2);
        MIB_EXPECT(tab.selectedFrameIndex() == frameK, "click on empty space keeps the selection");

        // The periodic refresh rebuilds the series; the selection stays.
        tab.refreshCharts();
        settle(2);
        MIB_EXPECT(tab.selectedFrameIndex() == frameK && highlight->isVisible() && highlight->count() == 1,
                   "selection and highlight survive a refresh");

        // ---- 3. prev/next walk the plotted points and wrap ---------------------
        wd.mark("prev-next");
        tab.stepScatterSelectionForTests(+1);
        MIB_EXPECT(tab.selectedFrameIndex() == 1012u, "next point");
        tab.stepScatterSelectionForTests(-9);
        MIB_EXPECT(tab.selectedFrameIndex() == 1002u, "previous points skip the frame without a point");
        tab.stepScatterSelectionForTests(-3);
        MIB_EXPECT(tab.selectedFrameIndex() == 1039u, "prev wraps to the newest point");
        tab.stepScatterSelectionForTests(+1);
        MIB_EXPECT(tab.selectedFrameIndex() == 1000u, "next wraps to the oldest point");

        // ---- 4. snapshots and exports never show the selection -----------------
        wd.mark("snapshot");
        MIB_REQUIRE(highlight->isVisible(), "highlight before the snapshot");
        cv::Mat hist, scatter;
        MIB_EXPECT(tab.captureChartSnapshots(hist, scatter) && !scatter.empty(), "snapshot taken");
        MIB_EXPECT(highlight->isVisible(), "highlight restored after the snapshot");

        // ---- 5. "Open in window…" follows the selection; close clears ----------
        wd.mark("window");
        auto* openBtn = tab.framePaneViewerForTests()->findChild<QPushButton*>(QStringLiteral("closeButton"));
        MIB_REQUIRE(openBtn != nullptr, "pane viewer button");
        MIB_EXPECT(openBtn->text().startsWith(QStringLiteral("Open in window")), "Close becomes Open in window");
        openBtn->click();
        settle(2);
        auto* window = tab.frameWindowForTests();
        MIB_REQUIRE(window != nullptr, "viewer window opened");
        MIB_EXPECT(window->isVisible() && !window->isModal(), "non-modal: the run keeps updating behind it");
        auto* nextBtn = window->findChild<QPushButton*>(QStringLiteral("nextButton"));
        MIB_REQUIRE(nextBtn != nullptr, "window next button");
        nextBtn->click();
        settle(2);
        MIB_EXPECT(tab.selectedFrameIndex() == 1001u, "window next steps the selection");
        auto* closeBtn = tab.framePaneForTests()->findChild<QToolButton*>(QStringLiteral("monitoringFramePaneClose"));
        MIB_REQUIRE(closeBtn != nullptr, "pane close button");
        closeBtn->click();
        settle(4);
        MIB_EXPECT(!tab.selectedFrameIndex(), "close clears the selection");
        MIB_EXPECT(!tab.framePaneForTests()->isVisible(), "close hides the pane");
        MIB_EXPECT(!highlight->isVisible(), "close removes the highlight");
        MIB_EXPECT(tab.frameWindowForTests() == nullptr, "close closes the viewer window");

        // ---- 6. KDE on: points sit in level series, a click still selects ------
        wd.mark("kde");
        tab.kdeToggle()->setChecked(true);
        settle(2);
        MIB_EXPECT(!tab.scatterSeriesForTests()->isVisible(), "KDE on: plain series hidden");
        const auto kde = pointOfFrame(tab, 1020);
        MIB_REQUIRE(kde.has_value(), "frame 1020 plotted");
        mouse::click(view, tab.scatterPointViewPosForTests(*kde));
        settle(2);
        MIB_EXPECT(tab.selectedFrameIndex() == 1020u, "KDE on: click selects");
        tab.kdeToggle()->setChecked(false);
        settle(2);

        // ---- 7. the frame leaves the buffer; the pane keeps its copy ----------
        wd.mark("evict");
        std::vector<backend::services::ProcessedFrame> newer;
        for (uint64_t i = 0; i < 1000; ++i)
            newer.push_back(frame(5000 + i, (100.0 + static_cast<double>(i % 800)) * umToPx2, 0.3));
        tab.injectMonitoringFramesForTests(newer);
        settle(2);
        MIB_EXPECT(!pointOfFrame(tab, 1020), "frame 1020 left the rolling buffer");
        MIB_EXPECT(tab.selectedFrameIndex() == 1020u, "selection kept");
        MIB_EXPECT(tab.framePaneForTests()->isVisible() && paneShowsImage(tab), "pane still shows the cell");
        MIB_EXPECT(!highlight->isVisible(), "no highlight for a frame no longer plotted");
        MIB_EXPECT(tab.framePaneTitleForTests().contains(QStringLiteral("no longer on the scatter")),
                   "title says the cell left the scatter");
        tab.stepScatterSelectionForTests(+1);
        MIB_EXPECT(tab.selectedFrameIndex() == 5000u, "next from a vanished frame starts at the oldest point");
        MIB_EXPECT(highlight->isVisible(), "highlight back on a plotted frame");
    }

    backend.shutdown();
    return mib::test::exitCode();
}
