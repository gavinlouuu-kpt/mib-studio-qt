// ZoomableChartView click/drag disambiguation (issue #466): a press that
// travels less than startDragDistance() is a click (plotClicked, axes
// untouched); past it, a pan that can no longer click. Wheel zoom,
// double-click reset (optional), a lost release after leaving the view.

#include <QApplication>
#include <QChart>
#include <QScatterSeries>
#include <QValueAxis>

#include "frontend/widgets/ZoomableChartView.h"

#include "support/assert.h"
#include "support/qt_mouse.h"
#include "support/watchdog.h"

namespace mouse = mib::test::mouse;

namespace {

void settle(int rounds = 4)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}

struct Range {
    double x0, x1, y0, y1;
};

Range rangeOf(const QValueAxis* x, const QValueAxis* y)
{
    return {x->min(), x->max(), y->min(), y->max()};
}

bool same(const Range& a, const Range& b)
{
    return qFuzzyCompare(a.x0 + 1, b.x0 + 1) && qFuzzyCompare(a.x1 + 1, b.x1 + 1) &&
           qFuzzyCompare(a.y0 + 1, b.y0 + 1) && qFuzzyCompare(a.y1 + 1, b.y1 + 1);
}

} // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    mib::test::Watchdog wd(60);
    QApplication app(argc, argv);

    auto* chart = new QChart();
    auto* series = new QScatterSeries();
    for (int i = 0; i < 50; ++i) series->append(i * 20.0, (i % 10) / 10.0);
    chart->addSeries(series);
    auto* x = new QValueAxis();
    auto* y = new QValueAxis();
    chart->addAxis(x, Qt::AlignBottom);
    chart->addAxis(y, Qt::AlignLeft);
    series->attachAxis(x);
    series->attachAxis(y);
    x->setRange(0, 1000);
    y->setRange(0, 1);

    frontend::ZoomableChartView view(chart);
    view.setDefaultRange(x, 0, 1000);
    view.setDefaultRange(y, 0, 1);
    view.resize(640, 480);
    view.show();
    settle(6);

    const QRectF plot = chart->plotArea();
    MIB_REQUIRE(plot.width() > 100 && plot.height() > 100, "plot area laid out");
    const QPointF centre = plot.center();
    const int threshold = QApplication::startDragDistance();
    MIB_REQUIRE(threshold > 2, "platform drag distance");

    // Signal counters (no QtTest in this tree).
    struct Counter {
        int n = 0;
        int count() const { return n; }
        bool isEmpty() const { return n == 0; }
        void clear() { n = 0; }
    };
    Counter clicked, doubled, hovered;
    Qt::MouseButton lastButton = Qt::NoButton;
    QObject::connect(&view, &frontend::ZoomableChartView::plotClicked, [&](QPointF, Qt::MouseButton b) {
        ++clicked.n;
        lastButton = b;
    });
    QObject::connect(&view, &frontend::ZoomableChartView::plotDoubleClicked, [&](QPointF) { ++doubled.n; });
    QObject::connect(&view, &frontend::ZoomableChartView::hoverMoved, [&](QPointF) { ++hovered.n; });

    // ---- click: below the threshold, axes untouched --------------------------
    wd.mark("click");
    const Range home = rangeOf(x, y);
    mouse::press(&view, centre);
    mouse::move(&view, centre + QPointF(threshold / 2.0 - 1, 0), Qt::LeftButton);
    mouse::release(&view, centre + QPointF(threshold / 2.0 - 1, 0));
    MIB_EXPECT(clicked.count() == 1, "sub-threshold press/release is one click");
    MIB_EXPECT(same(rangeOf(x, y), home), "a click does not pan");
    MIB_EXPECT(!view.isPanning(), "no pan left running after a click");
    MIB_EXPECT(lastButton == Qt::LeftButton, "button reported");

    // A click outside the plot area (axis labels) is not a plot click.
    clicked.clear();
    mouse::click(&view, QPointF(plot.left() / 2.0, centre.y()));
    MIB_EXPECT(clicked.isEmpty(), "click on the axis region is not a plot click");

    // ---- drag: past the threshold, a pan that never clicks -------------------
    wd.mark("drag");
    clicked.clear();
    mouse::drag(&view, centre, centre + QPointF(threshold * 6.0, threshold * 3.0));
    MIB_EXPECT(clicked.isEmpty(), "a drag never clicks");
    const Range panned = rangeOf(x, y);
    MIB_EXPECT(panned.x0 < home.x0 && panned.y0 > home.y0, "dragging right/down pans left/up in data");
    MIB_EXPECT(qFuzzyCompare(panned.x1 - panned.x0, home.x1 - home.x0), "pan keeps the span");
    MIB_EXPECT(view.isUserZoomed(), "pan marks the view as user-zoomed");
    MIB_EXPECT(!view.isPanning(), "release ends the pan");

    // Middle-drag pans too.
    const Range beforeMiddle = rangeOf(x, y);
    mouse::drag(&view, centre, centre - QPointF(threshold * 4.0, 0), 4, Qt::MiddleButton);
    MIB_EXPECT(rangeOf(x, y).x0 > beforeMiddle.x0 && clicked.isEmpty(), "middle-drag pans, never clicks");

    // ---- double-click: reset by default, signal only when turned off --------
    wd.mark("double-click");
    mouse::doubleClick(&view, centre);
    MIB_EXPECT(same(rangeOf(x, y), home), "double-click resets to the default ranges");
    MIB_EXPECT(!view.isUserZoomed(), "reset clears the user-zoomed flag");
    MIB_EXPECT(doubled.count() == 1, "double-click is reported");

    view.setResetOnDoubleClick(false);
    mouse::drag(&view, centre, centre + QPointF(threshold * 5.0, 0));
    const Range movedAway = rangeOf(x, y);
    doubled.clear();
    mouse::doubleClick(&view, centre);
    MIB_EXPECT(doubled.count() == 1, "double-click still reported with reset off");
    MIB_EXPECT(same(rangeOf(x, y), movedAway), "no reset when the owner handles double-click");

    view.resetZoomAction()->trigger();
    MIB_EXPECT(same(rangeOf(x, y), home), "Reset zoom action restores the default ranges");

    // ---- wheel: zoom in around the cursor ------------------------------------
    wd.mark("wheel");
    mouse::wheel(&view, centre, 120);
    const Range zoomed = rangeOf(x, y);
    MIB_EXPECT(zoomed.x1 - zoomed.x0 < home.x1 - home.x0 && zoomed.y1 - zoomed.y0 < home.y1 - home.y0,
               "wheel up zooms both axes in");
    mouse::wheel(&view, centre, 120, Qt::ControlModifier);
    const Range zoomedX = rangeOf(x, y);
    MIB_EXPECT(zoomedX.x1 - zoomedX.x0 < zoomed.x1 - zoomed.x0 &&
                   qFuzzyCompare(zoomedX.y1 - zoomedX.y0, zoomed.y1 - zoomed.y0),
               "Ctrl+wheel zooms X only");

    // ---- lost release: leaving the view drops an armed pan -------------------
    wd.mark("lost-release");
    view.resetZoom();
    mouse::press(&view, centre);
    mouse::move(&view, centre + QPointF(threshold * 3.0, 0), Qt::LeftButton);
    MIB_EXPECT(view.isPanning(), "pan armed past the threshold");
    mouse::leave(view.viewport());
    mouse::leave(&view);
    MIB_EXPECT(!view.isPanning(), "leaving with no button held (release lost) drops the pan");
    const Range afterLeave = rangeOf(x, y);
    mouse::move(&view, centre + QPointF(threshold * 9.0, threshold * 2.0));
    MIB_EXPECT(same(rangeOf(x, y), afterLeave), "a later move without a button does not pan");

    // cancelGesture() before a dialog takes input.
    clicked.clear();
    mouse::press(&view, centre);
    view.cancelGesture();
    mouse::release(&view, centre);
    MIB_EXPECT(clicked.isEmpty(), "a cancelled press does not click");

    // Hover is reported only without a press or pan.
    hovered.clear();
    mouse::move(&view, centre + QPointF(3, 3));
    MIB_EXPECT(hovered.count() == 1, "hover reported when idle");
    hovered.clear();
    mouse::press(&view, centre);
    mouse::move(&view, centre + QPointF(1, 1), Qt::LeftButton);
    mouse::release(&view, centre + QPointF(1, 1));
    MIB_EXPECT(hovered.isEmpty(), "no hover while a press is pending");

    return mib::test::exitCode();
}
