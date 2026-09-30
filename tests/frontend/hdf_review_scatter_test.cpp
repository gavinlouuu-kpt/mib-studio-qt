// Review scatter interaction (issue #467): a single click on a point shows
// that cell in the frame pane docked beside the scatter; drag pans; wheel
// zooms; double-click resets only on empty space; prev/next walk the valid
// set; exports are full extent without the highlight and give the view
// back; the hit rule matches tests/fixtures/review_scatter_hits.json (shared
// with the React shell). Point k is not frame k: the fixture puts frames
// that failed validation into the valid set.

#include <QApplication>
#include <QChart>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QPushButton>
#include <QScatterSeries>
#include <QSettings>
#include <QTabWidget>
#include <QTableView>
#include <QValueAxis>

#include <algorithm>
#include <chrono>
#include <map>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <optional>
#include <random>
#include <vector>

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "frontend/utils/ApplicationSettings.h"
#include "frontend/tabs/HdfReviewTab.h"
#include "frontend/utils/ScatterHitTest.h"
#include "frontend/widgets/ZoomableChartView.h"

#include "support/assert.h"
#include "support/qt_mouse.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
namespace mouse = mib::test::mouse;
namespace hit = frontend::scatterhit;

namespace {

// Optional screenshots for review (like MIB_KDE_E2E_OUT): set
// MIB_REVIEW_SCATTER_OUT to a directory.
void saveShot(QWidget& w, const char* name) {
    const QByteArray dir = qgetenv("MIB_REVIEW_SCATTER_OUT");
    if (dir.isEmpty()) return;
    const QString path = QString::fromLocal8Bit(dir) + QLatin1Char('/') + QLatin1String(name) + QStringLiteral(".png");
    if (!w.grab().save(path)) std::fprintf(stderr, "could not save %s\n", qPrintable(path));
}

void settle(int rounds = 4) {
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}

ProcessedFrame frame(uint64_t idx, double area, double deform, bool valid) {
    ProcessedFrame f;
    f.index = idx;
    f.timestampNs = (idx + 1) * 1000ULL;
    f.originalImage = cv::Mat(8, 10, CV_8UC1, cv::Scalar(static_cast<double>(40 + idx % 200)));
    f.processedImage = cv::Mat(8, 10, CV_8UC1, cv::Scalar(255));
    f.validation.isValid = valid;
    f.validation.objectId = static_cast<int>(idx);
    f.validation.objectCount = 1;
    f.validation.area = area;
    f.validation.deformability = deform;
    return f;
}

// Frames into /valid_frames; every fifth (i % 5 == 2) failed validation.
void writeExperiment(const std::string& path, const std::vector<ProcessedFrame>& frames) {
    Hdf5Service hdf5;
    MIB_REQUIRE(hdf5.openFile(path), "fixture create");
    MIB_REQUIRE(hdf5.initializeDatasets(), "fixture datasets");
    MIB_REQUIRE(hdf5.appendFrames(frames, {}), "fixture frames");
    backend::services::ProcessingConfig cfg;
    backend::services::ProcessingService::Roi roi{0, 0, 10, 8};
    MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 4000, frames.size(), 0, cfg, roi, nullptr, nullptr), "fixture info");
    hdf5.closeFile();
}

std::vector<ProcessedFrame> gridFrames(int n) {
    std::vector<ProcessedFrame> frames;
    for (int i = 0; i < n; ++i)
        frames.push_back(frame(static_cast<uint64_t>(i), 400.0 + i * 37.0, 0.02 + (i % 7) * 0.013, i % 5 != 2));
    return frames;
}

struct Ranges {
    double x0, x1, y0, y1;
};

Ranges ranges(QChart* chart) {
    auto* x = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).first());
    auto* y = qobject_cast<QValueAxis*>(chart->axes(Qt::Vertical).first());
    return {x->min(), x->max(), y->min(), y->max()};
}

bool same(const Ranges& a, const Ranges& b) {
    auto eq = [](double u, double v) { return std::abs(u - v) <= 1e-9 * std::max(1.0, std::abs(u)); };
    return eq(a.x0, b.x0) && eq(a.x1, b.x1) && eq(a.y0, b.y0) && eq(a.y1, b.y1);
}

// A view position at least `clearance` px from every scatter point, inside the plot.
std::optional<QPointF> emptySpot(frontend::HdfReviewTab& tab, double clearance) {
    auto* view = tab.scatterViewForTests();
    const QRectF plot = view->chart()->plotArea();
    const int points = static_cast<int>(tab.scatterPointToFrameForTests().size());
    std::vector<QPointF> pos;
    for (int k = 0; k < points; ++k) pos.push_back(tab.scatterPointViewPosForTests(k));
    for (double fy = 0.1; fy < 0.95; fy += 0.05) {
        for (double fx = 0.1; fx < 0.95; fx += 0.05) {
            const QPointF c = view->mapFromScene(view->chart()->mapToScene(
                QPointF(plot.left() + fx * plot.width(), plot.top() + fy * plot.height())));
            bool clear = true;
            for (const auto& p : pos) {
                if (QLineF(p, c).length() < clearance) { clear = false; break; }
            }
            if (clear) return c;
        }
    }
    return std::nullopt;
}

int orangePixels(const cv::Mat& bgr) {
    // The highlight marker colour (0xf2, 0x8e, 0x2b); tight so the chart
    // palette's other oranges (0xf6a625, 0xeb6834) do not count.
    int n = 0;
    for (int r = 0; r < bgr.rows; ++r) {
        const auto* row = bgr.ptr<cv::Vec3b>(r);
        for (int c = 0; c < bgr.cols; ++c) {
            const auto& px = row[c];
            if (std::abs(px[2] - 0xf2) <= 6 && std::abs(px[1] - 0x8e) <= 6 && std::abs(px[0] - 0x2b) <= 6) ++n;
        }
    }
    return n;
}

int orangePixels(const QImage& image) {
    int n = 0;
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < rgb.height(); ++y) {
        for (int x = 0; x < rgb.width(); ++x) {
            const QColor c = rgb.pixelColor(x, y);
            if (std::abs(c.red() - 0xf2) <= 6 && std::abs(c.green() - 0x8e) <= 6 && std::abs(c.blue() - 0x2b) <= 6) ++n;
        }
    }
    return n;
}

void sharedFixture() {
    QFile f(QStringLiteral("tests/fixtures/review_scatter_hits.json"));
    MIB_REQUIRE(f.open(QIODevice::ReadOnly), "shared hit fixture readable (run from the source root)");
    const auto doc = nlohmann::json::parse(f.readAll().toStdString());
    std::vector<hit::Point> points;
    for (const auto& p : doc.at("points"))
        points.push_back({p.at("x").get<double>(), p.at("y").get<double>(), p.at("frame").get<int>()});
    const double tol = doc.at("tolerance_px").get<double>();
    int checked = 0;
    for (const auto& c : doc.at("cases")) {
        const auto& v = c.at("viewport");
        hit::Viewport vp{v.at("x0"), v.at("x1"), v.at("y0"), v.at("y1"),
                         v.at("left"), v.at("top"), v.at("width"), v.at("height")};
        for (const auto& k : c.at("clicks")) {
            const auto got = hit::nearest(points, vp, k.at("px"), k.at("py"), tol);
            const int gotFrame = got ? points[*got].frame : -1;
            const int want = k.at("expect").is_null() ? -1 : k.at("expect").get<int>();
            MIB_EXPECT(gotFrame == want, c.at("name").get<std::string>() + ": " + k.at("why").get<std::string>() +
                                             " (got " + std::to_string(gotFrame) + ")");
            ++checked;
        }
    }
    MIB_EXPECT(checked >= 10, "shared fixture has cases");
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,yolo,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR"))
        qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    mib::test::Watchdog wd(180);
    QApplication app(argc, argv);
    mib::test::TempDir td("hdf_review_scatter");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");

    wd.mark("shared-fixture");
    sharedFixture();

    const std::string grid = (td.path() / "grid.h5").string();
    writeExperiment(grid, gridFrames(60));

    wd.mark("load");
    frontend::HdfReviewTab tab(backend);
    tab.resize(1400, 900);
    tab.show();
    settle(4);
    int viewerCalls = 0;
    std::pair<int, bool> viewerLast{-1, false};
    tab.setFrameViewerSinkForTests([&](int f, bool valid) {
        ++viewerCalls;
        viewerLast = {f, valid};
    });
    tab.loadHdfFileForTests(QString::fromStdString(grid));
    auto* tabs = tab.findChild<QTabWidget*>(QStringLiteral("frameTypeTabs"));
    MIB_REQUIRE(tabs != nullptr, "frame type tabs");
    tabs->setCurrentIndex(2); // Charts: isShowingValid_ is false here
    settle(8);

    auto* view = tab.scatterViewForTests();
    auto* chart = view->chart();
    auto* validTable = tab.findChild<QTableView*>(QStringLiteral("validMetricsTable"));
    MIB_REQUIRE(view && chart && validTable, "scatter view and valid table");
    const auto& map = tab.scatterPointToFrameForTests();
    MIB_REQUIRE(map.size() == 48, "one point per frame that passed validation (60 - 12)");
    int k = -1;
    for (int i = 0; i < static_cast<int>(map.size()); ++i) {
        if (map[static_cast<size_t>(i)] != i && map[static_cast<size_t>(i)] % 5 == 3) { k = i; break; }
    }
    MIB_REQUIRE(k >= 0, "a point whose frame index differs from its point index");
    const int frameK = map[static_cast<size_t>(k)];
    MIB_EXPECT(!tab.framePaneFrameForTests(), "pane starts empty");

    // ---- click selects the valid frame behind the point ----------------------
    wd.mark("click");
    const Ranges home = ranges(chart);
    mouse::click(view, tab.scatterPointViewPosForTests(k));
    settle(4);
    const auto pane = tab.framePaneFrameForTests();
    MIB_EXPECT(pane && pane->first == frameK && pane->second, "pane shows frame_k of the valid set, not frame k");
    
    MIB_EXPECT(tab.selectedFrameForTests() == frameK, "selection is frame_k");
    MIB_EXPECT(validTable->selectionModel()->currentIndex().row() == frameK, "Valid Frames row selected");
    auto* highlight = tab.scatterHighlightForTests();
    MIB_EXPECT(highlight->isVisible() && highlight->count() == 1, "highlight drawn");
    MIB_EXPECT(chart->series().last() == highlight, "highlight drawn above every other series");
    MIB_EXPECT(same(ranges(chart), home), "a click does not move the axes");
    const QRect paneRect(tab.framePaneForTests()->mapToGlobal(QPoint(0, 0)), tab.framePaneForTests()->size());
    const QRect viewRect(view->mapToGlobal(QPoint(0, 0)), view->size());
    MIB_EXPECT(tab.framePaneForTests()->isVisible() && !paneRect.intersects(viewRect),
               "the pane sits beside the scatter and never covers it");
    MIB_EXPECT(orangePixels(view->grab().toImage()) > 20, "the highlight is visible on screen");
    saveShot(tab, "review-charts-selected");

    // ---- drag pans and never selects ----------------------------------------
    wd.mark("drag");
    const QPointF anchor = tab.scatterPointViewPosForTests(0);
    mouse::drag(view, anchor, anchor + QPointF(120, 40));
    settle(2);
    MIB_EXPECT(!same(ranges(chart), home), "drag pans");
    MIB_EXPECT(tab.selectedFrameForTests() == frameK && tab.framePaneFrameForTests()->first == frameK,
               "drag does not change the selection");
    view->resetZoomAction()->trigger();
    MIB_EXPECT(same(ranges(chart), home), "Reset zoom restores the data extent");

    // ---- wheel zoom, then a click hits the point at the new scale -----------
    wd.mark("zoom");
    const int k2 = k + 3 < static_cast<int>(map.size()) ? k + 3 : k - 3;
    for (int i = 0; i < 4; ++i) mouse::wheel(view, tab.scatterPointViewPosForTests(k2), 240);
    settle(2);
    const Ranges zoomed = ranges(chart);
    MIB_EXPECT(zoomed.x1 - zoomed.x0 < home.x1 - home.x0, "wheel zoomed in");
    mouse::click(view, tab.scatterPointViewPosForTests(k2));
    settle(2);
    saveShot(tab, "review-charts-zoomed");
    MIB_EXPECT(tab.selectedFrameForTests() == map[static_cast<size_t>(k2)], "click at the zoomed scale selects that point");

    // ---- double-click: never resets on a point, resets on empty space -------
    wd.mark("double-click");
    mouse::doubleClick(view, tab.scatterPointViewPosForTests(k2));
    settle(2);
    MIB_EXPECT(same(ranges(chart), zoomed), "double-click on a point keeps the zoom");
    view->resetZoomAction()->trigger();
    settle(2);
    const auto spot = emptySpot(tab, 30.0);
    MIB_REQUIRE(spot.has_value(), "an empty spot in the plot");
    const int before = tab.selectedFrameForTests();
    mouse::click(view, *spot);
    MIB_EXPECT(tab.selectedFrameForTests() == before, "click on empty space leaves the selection alone");
    mouse::drag(view, *spot, *spot + QPointF(60, 0));
    MIB_EXPECT(!same(ranges(chart), home), "panned away");
    mouse::doubleClick(view, *spot);
    settle(2);
    MIB_EXPECT(same(ranges(chart), home), "double-click on empty space resets to the data extent");

    // ---- prev/next walk the valid set; frames without a point hide the mark --
    wd.mark("prev-next");
    const int beforeInvalid = 1; // frame 2 failed validation
    mouse::click(view, tab.scatterPointViewPosForTests(1));
    settle(2);
    MIB_REQUIRE(tab.selectedFrameForTests() == beforeInvalid, "frame 1 selected");
    tab.stepScatterSelectionForTests(+1);
    settle(2);
    MIB_EXPECT(tab.framePaneFrameForTests() && tab.framePaneFrameForTests()->first == 2, "pane shows frame 2");
    MIB_EXPECT(!highlight->isVisible(), "no highlight for a frame that is not on the scatter");
    saveShot(tab, "review-charts-no-point");
    tab.stepScatterSelectionForTests(+1);
    settle(2);
    MIB_EXPECT(tab.selectedFrameForTests() == 3 && highlight->isVisible(), "highlight back on frame 3");
    tab.stepScatterSelectionForTests(-4);
    MIB_EXPECT(tab.selectedFrameForTests() == 59, "prev wraps to the last frame");
    tab.stepScatterSelectionForTests(+1);
    MIB_EXPECT(tab.selectedFrameForTests() == 0, "next wraps to the first frame");

    // ---- the embedded viewer never closes; "Open in window…" opens the modal --
    wd.mark("pane");
    auto* openBtn = tab.framePaneForTests()->findChild<QPushButton*>(QStringLiteral("closeButton"));
    MIB_REQUIRE(openBtn != nullptr, "pane viewer button");
    MIB_EXPECT(openBtn->text().startsWith(QStringLiteral("Open in window")), "Close becomes Open in window");
    openBtn->click();
    settle(2);
    MIB_EXPECT(viewerCalls == 1 && viewerLast.first == 0 && viewerLast.second, "Open in window opens the valid frame");
    auto* paneViewer = tab.framePaneForTests()->findChild<QWidget*>(QStringLiteral("reviewFramePaneViewer"));
    MIB_REQUIRE(paneViewer != nullptr, "embedded viewer");
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(paneViewer, &esc);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(paneViewer, &enter);
    settle(2);
    MIB_EXPECT(paneViewer->isVisible() && tab.framePaneFrameForTests(), "Esc / Enter do not close the pane");

    // ---- export: full extent, no highlight, the view comes back -------------
    wd.mark("export");
    mouse::click(view, tab.scatterPointViewPosForTests(k));
    for (int i = 0; i < 3; ++i) mouse::wheel(view, tab.scatterPointViewPosForTests(k), 240);
    settle(2);
    const Ranges userView = ranges(chart);
    MIB_EXPECT(highlight->isVisible() && !same(userView, home), "zoomed with a highlighted cell");
    auto snaps = tab.renderChartSnapshotsForTests();
    settle(2);
    MIB_EXPECT(snaps.count("scatter_plot.tiff") && !snaps["scatter_plot.tiff"].empty(), "scatter snapshot rendered");
    if (!snaps["scatter_plot.tiff"].empty())
        MIB_EXPECT(orangePixels(snaps["scatter_plot.tiff"]) == 0, "snapshot has no selection highlight");
    MIB_EXPECT(same(ranges(chart), userView), "the user's zoom comes back after the export");
    MIB_EXPECT(highlight->isVisible() && tab.selectedFrameForTests() == frameK, "the highlight comes back");

    // ---- tab switching keeps the pane in step with table selections ---------
    wd.mark("tables");
    tabs->setCurrentIndex(0);
    settle(2);
    validTable->selectionModel()->setCurrentIndex(validTable->model()->index(10, 0),
                                                  QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    tabs->setCurrentIndex(2);
    settle(4);
    MIB_EXPECT(tab.framePaneFrameForTests() && tab.framePaneFrameForTests()->first == 10,
               "a Valid Frames row picked elsewhere shows in the pane on return");

    // ---- close: nothing selected, nothing drawn ------------------------------
    wd.mark("close");
    auto* closeBtn = tab.findChild<QPushButton*>(QStringLiteral("closeFileBtn"));
    MIB_REQUIRE(closeBtn != nullptr, "close button");
    closeBtn->click();
    settle(4);
    MIB_EXPECT(!highlight->isVisible() && !tab.framePaneFrameForTests(), "close clears highlight and pane");
    MIB_EXPECT(tab.scatterPointToFrameForTests().empty(), "close clears the point map");
    const Ranges closed = ranges(chart);
    MIB_EXPECT(closed.x0 == 0 && closed.x1 == 1000 && closed.y0 == 0 && closed.y1 == 1, "axes back to defaults");
    mouse::click(view, view->rect().center());
    MIB_EXPECT(tab.selectedFrameForTests() < 0, "a click on an empty chart selects nothing");

    // ---- cost at 20 000 cells: hit test and pan step -------------------------
    wd.mark("perf");
    const std::string big = (td.path() / "big.h5").string();
    {
        std::mt19937 rng(7);
        std::normal_distribution<double> a(900, 250), d(0.06, 0.02);
        std::vector<ProcessedFrame> frames;
        frames.reserve(20000);
        for (int i = 0; i < 20000; ++i) frames.push_back(frame(static_cast<uint64_t>(i), std::max(50.0, a(rng)),
                                                               std::clamp(d(rng), 0.0, 0.3), true));
        writeExperiment(big, frames);
    }
    tab.loadHdfFileForTests(QString::fromStdString(big));
    tabs->setCurrentIndex(2);
    settle(8);
    MIB_REQUIRE(tab.scatterPointToFrameForTests().size() == 20000, "20 000 points");
    std::vector<double> hitMs;
    const QPointF centre = view->mapFromScene(chart->mapToScene(chart->plotArea().center()));
    for (int i = 0; i < 40; ++i) {
        QElapsedTimer t;
        t.start();
        mouse::move(view, centre + QPointF(i % 7, i % 5)); // hover runs the same hit test
        hitMs.push_back(t.nsecsElapsed() / 1e6);
    }
    std::sort(hitMs.begin(), hitMs.end());
    // The hit rule alone (no tooltip/cursor work), same 20 000-point scale.
    std::vector<hit::Point> pts;
    {
        std::mt19937 rng(9);
        std::uniform_real_distribution<double> ux(0, 2000), uy(0, 0.3);
        for (int i = 0; i < 20000; ++i) pts.push_back({ux(rng), uy(rng), i});
    }
    const hit::Viewport vp{0, 2000, 0, 0.3, 60, 30, 900, 600};
    QElapsedTimer pureTimer;
    pureTimer.start();
    int pureHits = 0;
    for (int i = 0; i < 100; ++i) pureHits += hit::nearest(pts, vp, 60 + i * 9.0, 30 + i * 6.0, 8.0).has_value();
    const double pureMs = pureTimer.nsecsElapsed() / 1e6 / 100.0;
    std::vector<double> panMs, moveOnlyMs;
    mouse::press(view, centre);
    for (int i = 1; i <= 30; ++i) {
        QElapsedTimer t;
        t.start();
        mouse::move(view, centre + QPointF(i * 6.0, i * 2.0), Qt::LeftButton);
        moveOnlyMs.push_back(t.nsecsElapsed() / 1e6);
        view->viewport()->repaint(); // charge the repaint to the step
        panMs.push_back(t.nsecsElapsed() / 1e6);
    }
    std::sort(moveOnlyMs.begin(), moveOnlyMs.end());
    mouse::release(view, centre + QPointF(180, 60));
    std::sort(panMs.begin(), panMs.end());
    const double hitMedian = hitMs[hitMs.size() / 2];
    const double panMedian = panMs[panMs.size() / 2];
    const double moveMedian = moveOnlyMs[moveOnlyMs.size() / 2];
    std::fprintf(stderr,
                 "perf @20k: hit rule %.3f ms/call (%d hits); hover (hit + tooltip) median %.3f ms (max %.3f); "
                 "pan step axis update median %.1f ms, with repaint median %.1f ms (max %.1f)\n",
                 pureMs, pureHits, hitMedian, hitMs.back(), moveMedian, panMedian, panMs.back());
    MIB_EXPECT(pureMs < 2.0, "hit rule at 20 000 points");
    // Generous absolute bounds (shared CI runners); the numbers above are the
    // record. The pan's own work (axis update + series geometry) is gated
    // tightly; the repaint is Qt Charts drawing one item per marker on the
    // CPU (~200-260 ms at 20 000 points here, tracked as TD-18), so its bound
    // only catches a regression of the O(n²) kind.
    MIB_EXPECT(hitMedian < 5.0, "hover/click hit test stays interactive at 20 000 points");
    MIB_EXPECT(moveMedian < 60.0, "pan axis/geometry update stays interactive at 20 000 points");
    MIB_EXPECT(panMedian < 1500.0, "pan repaint at 20 000 points stays bounded");

    return mib::test::exitCode();
}
