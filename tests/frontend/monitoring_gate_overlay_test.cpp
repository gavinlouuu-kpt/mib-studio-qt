// monitoring_gate_overlay_test
//
// The target-group sorting gate drawn on, and edited from, the Monitoring
// "Deformability vs Area" scatter (offscreen, mock backend with the bundled
// E-modulus table, frames injected into the rolling buffer):
//  0. ZoomableChartView press interceptor: a claimed press never pans; without
//     travel it is still a click; past the threshold it is an intercepted drag
//     that moves no axis; a lost gesture is reported; a view without an
//     interceptor (or one that declines) pans exactly as before;
//  1. the overlay follows the *draft*: hidden while TargetGroupEnabled is off,
//     drawn while on, "(unapplied)" only while a gate field differs from the
//     applied baseline; the live count equals a hand count (science expression
//     over the plotted cells' own E);
//  2. the box sits on the data coordinates of the draft values and the tinted
//     share of the plot equals the box's share;
//  3. hover: handle cursor + tooltip naming the field and value; a cell keeps
//     its tooltip; empty plot shows "Area X um2 / deformability Y / E Z kPa";
//  4. dragging a box edge writes the tune panel's own spin box, so the draft,
//     the `*` marker and the footer count follow; Apply is still required (the
//     backend is untouched); the axes do not move; min <= max and the control's
//     range clamp the value;
//  5. a drag that starts away from every handle still pans and edits nothing; a
//     click on a handle (with a cell under it) still selects the cell;
//  6. the E gate: contours + value labels, a smaller tinted region, E handles
//     that edit TargetEmodulusMin/Max along the table, no E handle with the E
//     gate off;
//  7. while an apply is in flight no handle can be grabbed;
//  8. stored snapshots never carry the overlay;
//  9. with no table loaded an enabled E gate shows the notice, tints nothing,
//     counts no cell and has no E handle; the table coming back restores them.

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingTypes.h"
#include "frontend/models/ProcessingConfigDraft.h"
#include "frontend/tabs/ExperimentMonitoringTab.h"
#include "frontend/utils/ApplicationSettings.h"
#include "frontend/widgets/ZoomableChartView.h"

#include "support/assert.h"
#include "support/qt_mouse.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QChart>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScatterSeries>
#include <QSettings>
#include <QSpinBox>
#include <QStyleFactory>
#include <QThread>
#include <QToolTip>
#include <QValueAxis>

#include <opencv2/core.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace {

using Tab = frontend::ExperimentMonitoringTab;
using frontend::TuneField;
namespace mouse = mib::test::mouse;
namespace gate = frontend::gate;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

void settle(int rounds = 4)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

// QToolTip hides a tip after a short delay; wait for the condition instead of sleeping a fixed time.
template <class F>
bool waitUntil(F cond, int timeoutMs = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return true;
}

// MIB_GATE_OVERLAY_OUT=<dir>: write the scatter as the operator sees it at a few steps (review aid, not asserted).
void shot(Tab& tab, const char* name)
{
    const QByteArray dir = qgetenv("MIB_GATE_OVERLAY_OUT");
    if (dir.isEmpty()) return;
    QDir().mkpath(QString::fromLocal8Bit(dir));
    tab.grab().save(QDir(QString::fromLocal8Bit(dir)).filePath(QString::fromLatin1(name) + QStringLiteral(".png")));
}

struct Cell {
    uint64_t index;
    double areaUm;
    double deform;
    double e; // the E the pipeline stored for the cell (table lookup; NaN outside it)
};

backend::services::ProcessedFrame frameOf(const Cell& c, double factor)
{
    backend::services::ProcessedFrame f;
    f.index = c.index;
    f.timestampNs = (c.index + 1) * 1000ULL;
    f.originalImage = cv::Mat(48, 64, CV_8UC1, cv::Scalar(static_cast<double>(40 + c.index % 200)));
    f.processedImage = cv::Mat(48, 64, CV_8UC1, cv::Scalar(0));
    f.validation.isValid = true;
    f.validation.area = c.areaUm / (factor * factor);
    f.validation.deformability = c.deform;
    f.validation.analysisPixelToMicronFactor = factor;
    f.validation.youngsModulus = c.e;
    return f;
}

// The target-group expression of ProcessingScience.cpp for a valid cell.
bool scienceTarget(const backend::services::ProcessingConfig& c, const Cell& cell)
{
    if (!c.enable_target_group) return false;
    const bool tgArea = cell.areaUm >= c.target_group_area_min && cell.areaUm <= c.target_group_area_max;
    const bool tgDeform = cell.deform >= c.target_group_deformability_min && cell.deform <= c.target_group_deformability_max;
    const bool tgEmod = !c.enable_target_group_emodulus ||
                        (!std::isnan(cell.e) && cell.e >= c.target_group_emodulus_min && cell.e <= c.target_group_emodulus_max);
    return tgArea && tgDeform && tgEmod;
}

QString expectedPreview(const backend::services::ProcessingConfig& c, const std::vector<Cell>& cells)
{
    int n = 0;
    for (const auto& cell : cells) n += scienceTarget(c, cell) ? 1 : 0;
    const double pct = 100.0 * n / static_cast<double>(cells.size());
    return QStringLiteral("Target group preview: %1 of %2 plotted valid cells (%3%)").arg(n).arg(cells.size()).arg(pct, 0, 'f', 1);
}

struct Ranges {
    double x0, x1, y0, y1;
    bool operator==(const Ranges& o) const { return x0 == o.x0 && x1 == o.x1 && y0 == o.y0 && y1 == o.y1; }
};

Ranges rangesOf(QChart* chart)
{
    auto* x = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).value(0));
    auto* y = qobject_cast<QValueAxis*>(chart->axes(Qt::Vertical).value(0));
    return {x->min(), x->max(), y->min(), y->max()};
}

bool finite(const QPointF& p) { return std::isfinite(p.x()) && std::isfinite(p.y()); }

// Data (area, deformability) under a scatter-view position.
QPointF dataAt(Tab& tab, const QPointF& viewPos)
{
    auto* view = tab.scatterViewForTests();
    auto* chart = view->chart();
    const QPointF chartPos = chart->mapFromScene(view->mapToScene(viewPos.toPoint()));
    return chart->mapToValue(chartPos, tab.scatterSeriesForTests());
}

QLabel* rowLabelOf(Tab& tab, QWidget* control)
{
    for (auto* label : tab.findChildren<QLabel*>())
        if (label->buddy() == control) return label;
    return nullptr;
}

double numberAfter(const QString& text, const QString& marker)
{
    const int at = text.indexOf(marker);
    if (at < 0) return kNaN;
    static const QRegularExpression re(QStringLiteral("(-?\\d+(?:\\.\\d+)?)"));
    const auto m = re.match(text, at + marker.size());
    return m.hasMatch() ? m.captured(1).toDouble() : kNaN;
}

} // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR")) qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    mib::test::Watchdog wd(180);
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    mib::test::TempDir td("monitoring_gate_overlay");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");

    // ---- 0. the view's press interceptor, on its own --------------------------------
    wd.mark("interceptor");
    {
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
        const QPointF centre = chart->plotArea().center();
        int clicks = 0, started = 0, moved = 0, finishedOk = 0, finishedLost = 0;
        QObject::connect(&view, &frontend::ZoomableChartView::plotClicked, [&](QPointF, Qt::MouseButton) { ++clicks; });
        QObject::connect(&view, &frontend::ZoomableChartView::interceptDragStarted, [&](QPointF) { ++started; });
        QObject::connect(&view, &frontend::ZoomableChartView::interceptDragMoved, [&](QPointF, QPointF) { ++moved; });
        QObject::connect(&view, &frontend::ZoomableChartView::interceptDragFinished, [&](QPointF, QPointF, bool ok) { ok ? ++finishedOk : ++finishedLost; });
        const Ranges home = rangesOf(chart);

        // No interceptor: an ordinary pan.
        mouse::drag(&view, centre, centre + QPointF(60, 20), 6);
        MIB_EXPECT(!(rangesOf(chart) == home) && started == 0 && clicks == 0, "no interceptor: the drag pans");
        view.resetZoom();

        bool claim = false;
        int asked = 0;
        QPointF askedAt;
        view.setPressInterceptor([&](const QPointF& p) {
            ++asked;
            askedAt = p;
            return claim;
        });
        mouse::drag(&view, centre, centre + QPointF(60, 20), 6);
        MIB_EXPECT(asked == 1 && askedAt == centre, "the interceptor is asked once per left press");
        MIB_EXPECT(!(rangesOf(chart) == home) && started == 0, "an interceptor that declines changes nothing: it pans");
        view.resetZoom();
        mouse::click(&view, centre, Qt::MiddleButton);
        MIB_EXPECT(asked == 1, "only the left button is offered");
        clicks = started = moved = finishedOk = finishedLost = 0;

        claim = true;
        mouse::click(&view, centre);
        MIB_EXPECT(clicks == 1 && started == 0 && finishedOk == 0, "a claimed press without travel is still a click");
        mouse::drag(&view, centre, centre + QPointF(60, 20), 6);
        MIB_EXPECT(rangesOf(chart) == home, "a claimed drag moves no axis");
        MIB_EXPECT(started == 1 && moved >= 2 && finishedOk == 1 && finishedLost == 0 && clicks == 1, "claimed drag: started, moved, finished(true), no click");
        // The wheel is ignored while dragging; a lost gesture is reported.
        mouse::press(&view, centre);
        mouse::move(&view, centre + QPointF(30, 0), Qt::LeftButton);
        MIB_EXPECT(view.isInterceptDragging(), "dragging");
        mouse::wheel(&view, centre, 120);
        MIB_EXPECT(rangesOf(chart) == home, "no wheel zoom during an intercepted drag");
        view.cancelGesture();
        MIB_EXPECT(!view.isInterceptDragging() && finishedLost == 1, "a cancelled gesture is reported as not completed");
        mouse::release(&view, centre + QPointF(30, 0));
        MIB_EXPECT(finishedOk == 1 && clicks == 1, "the late release is not a second finish or a click");
        view.setPressInterceptor({});
        mouse::drag(&view, centre, centre + QPointF(60, 20), 6);
        MIB_EXPECT(!(rangesOf(chart) == home), "cleared interceptor: pans again");
    }

    // ---- setup -------------------------------------------------------------------------------
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    auto& processing = backend.processing();
    const backend::EModulusLut& lut = processing.eModulusLut();
    MIB_REQUIRE(lut.isLoaded(), "the bundled E-modulus table is loaded by the test backend");
    const double areaLo = lut.gridAreaMin(), areaHi = lut.gridAreaMin() + lut.gridAreaStep() * (lut.gridAreaBins() - 1);
    const double defLo = lut.gridDeformMin(), defHi = lut.gridDeformMin() + lut.gridDeformStep() * (lut.gridDeformBins() - 1);
    std::fprintf(stderr, "table: %s, area %.1f..%.1f um2, deformability %.4f..%.4f\n", processing.eModulusLutIdentity().path.c_str(),
                 areaLo, areaHi, defLo, defHi);
    const double factor = processing.getPixelToMicronFactor();
    MIB_REQUIRE(factor > 0, "pixel-to-micron factor");
    {
        auto cfg = processing.getProcessingConfig();
        cfg.enable_target_group = true;
        cfg.target_group_area_min = 150;
        cfg.target_group_area_max = 400;
        cfg.target_group_deformability_min = 0.02;
        cfg.target_group_deformability_max = 0.06;
        cfg.enable_target_group_emodulus = false;
        cfg.target_group_emodulus_min = 1.0; // the table has E ~ 0.7..2.4 kPa across this box
        cfg.target_group_emodulus_max = 1.6;
        processing.setProcessingConfig(cfg);
    }

    // Plotted cells: a grid across the table plus one exactly on the area-max edge, one
    // outside the table's deformability range (no E) and one past its area range.
    std::vector<Cell> cells;
    for (int i = 0; i < 28; ++i) {
        const double a = 80.0 + 20.0 * i;
        const double d = 0.01 + 0.002 * ((i * 7) % 35);
        cells.push_back({1000u + static_cast<uint64_t>(i), a, d, lut.lookup(a, d)});
    }
    cells.push_back({2000, 400.0, 0.04, lut.lookup(400.0, 0.04)}); // on the area-max edge
    cells.push_back({2001, 300.0, 0.2, lut.lookup(300.0, 0.2)});   // above the table
    cells.push_back({2002, 690.0, 0.03, lut.lookup(690.0, 0.03)}); // right of the table
    MIB_EXPECT(std::isnan(cells[29].e) && std::isnan(cells[30].e) && std::isfinite(cells[28].e), "stored E: NaN outside the table only");
    std::vector<backend::services::ProcessedFrame> frames;
    for (const auto& c : cells) frames.push_back(frameOf(c, factor));

    Tab tab(backend);
    tab.resize(1200, 800);
    tab.show();
    tab.setScatterXRange(0, 700);
    tab.setScatterYRange(0, 0.1);
    tab.loadCurrentConfig();
    settle(6);
    auto* view = tab.scatterViewForTests();
    auto* chart = view->chart();
    auto& draft = tab.tuneDraft();
    QLabel* preview = tab.gatePreviewLabelForTests();
    MIB_REQUIRE(view && chart && preview, "surfaces");
    MIB_REQUIRE(tab.gateLutForTests() != nullptr && tab.gateLutForTests()->isLoaded(), "the tab holds a copy of the table");
    MIB_EXPECT(tab.gateLutForTests() != &lut, "... a private copy, not the service's object");

    // ---- 1. follows the draft ---------------------------------------------------------------------
    wd.mark("draft");
    auto info = tab.gateOverlayInfoForTests();
    MIB_EXPECT(info.shown && info.label == QStringLiteral("Target group") && info.notice.isEmpty(), "baseline gate on: overlay drawn, not unapplied");
    MIB_EXPECT(preview->isVisibleTo(&tab), "preview label shown while the gate is on");
    tab.injectMonitoringFramesForTests(frames);
    settle(2);
    MIB_REQUIRE(tab.scatterPointFramesForTests().size() == cells.size(), "every cell is plotted");
    MIB_EXPECT(preview->text() == expectedPreview(draft.draft(), cells), "preview == hand count: " + preview->text().toStdString());
    MIB_EXPECT(preview->text().startsWith(QStringLiteral("Target group preview: ")) && preview->text().contains(QStringLiteral(" plotted valid cells (")),
               "preview wording");

    tab.setTuneFieldForTests(TuneField::TargetGroupEnabled, false);
    settle(2);
    info = tab.gateOverlayInfoForTests();
    MIB_EXPECT(!info.shown && !preview->isVisibleTo(&tab), "gate off in the draft: overlay and count hidden");
    MIB_EXPECT(!finite(tab.gateHandleViewPosForTests(gate::HandleKind::AreaMax)), "no handles while the gate is off");
    tab.setTuneFieldForTests(TuneField::TargetGroupEnabled, true);
    settle(2);
    info = tab.gateOverlayInfoForTests();
    MIB_EXPECT(info.shown && preview->isVisibleTo(&tab) && !draft.dirty() && info.label == QStringLiteral("Target group"),
               "back to the baseline: drawn again, no unapplied marker");

    shot(tab, "1-baseline");
    // ---- 2. geometry -------------------------------------------------------------------------------
    wd.mark("geometry");
    const QPointF topLeft = tab.scatterDataViewPosForTests(150, 0.06);
    const QPointF bottomRight = tab.scatterDataViewPosForTests(400, 0.02);
    MIB_EXPECT(std::abs(info.boxPx.left() - topLeft.x()) < 1.5 && std::abs(info.boxPx.top() - topLeft.y()) < 1.5 &&
                   std::abs(info.boxPx.right() - bottomRight.x()) < 1.5 && std::abs(info.boxPx.bottom() - bottomRight.y()) < 1.5,
               "the box sits on the draft values in plot coordinates");
    {
        const double expected = (250.0 / 700.0) * (0.04 / 0.1);
        const double got = static_cast<double>(info.maskOn) / static_cast<double>(info.maskCells);
        MIB_EXPECT(info.maskCells > 5000 && std::abs(got - expected) < 0.012, "tinted share == box share: " + std::to_string(got) + " vs " + std::to_string(expected));
    }

    // ---- 3. hover -----------------------------------------------------------------------------------
    wd.mark("hover");
    const QPointF areaMaxAt = tab.gateHandleViewPosForTests(gate::HandleKind::AreaMax);
    const QPointF deformMaxAt = tab.gateHandleViewPosForTests(gate::HandleKind::DeformMax);
    MIB_REQUIRE(finite(areaMaxAt) && finite(deformMaxAt), "visible handles");
    MIB_EXPECT(tab.gateHandleAtForTests(areaMaxAt) == gate::HandleKind::AreaMax && tab.gateHandleAtForTests(deformMaxAt) == gate::HandleKind::DeformMax,
               "handle lookup");
    mouse::move(view, areaMaxAt);
    settle(1);
    MIB_EXPECT(view->cursor().shape() == Qt::SizeHorCursor, "area edge: horizontal resize cursor");
    MIB_EXPECT(QToolTip::text().contains(QStringLiteral("Target group area maximum")) && QToolTip::text().contains(QStringLiteral("400")),
               "tooltip names the field and its value: " + QToolTip::text().toStdString());
    MIB_EXPECT(tab.gateOverlayInfoForTests().hovered == gate::HandleKind::AreaMax, "the hovered edge is highlighted");
    mouse::move(view, deformMaxAt);
    settle(1);
    MIB_EXPECT(view->cursor().shape() == Qt::SizeVerCursor && QToolTip::text().contains(QStringLiteral("Target group deformability maximum")),
               "deformability edge: vertical resize cursor + tooltip");
    // Empty plot area, inside the table: area / deformability / E.
    const QPointF probe = tab.scatterDataViewPosForTests(310, 0.045);
    mouse::move(view, probe);
    settle(1);
    {
        const QString tip = QToolTip::text();
        const QPointF d = dataAt(tab, probe);
        MIB_EXPECT(tip.startsWith(QStringLiteral("Area ")) && tip.contains(QStringLiteral("µm² · deformability ")) && tip.contains(QStringLiteral(" · E ")) &&
                       tip.endsWith(QStringLiteral(" kPa")),
                   "readout: " + tip.toStdString());
        MIB_EXPECT(std::abs(numberAfter(tip, QStringLiteral("Area ")) - d.x()) < 0.06 && std::abs(numberAfter(tip, QStringLiteral("deformability ")) - d.y()) < 0.0006,
                   "readout shows the position under the pointer");
        MIB_EXPECT(std::abs(numberAfter(tip, QStringLiteral(" · E ")) - lut.lookup(d.x(), d.y())) < 0.02, "readout E == table lookup there");
        MIB_EXPECT(view->cursor().shape() == Qt::ArrowCursor, "no special cursor on empty plot");
    }
    // Outside the table: no E.
    mouse::move(view, tab.scatterDataViewPosForTests(250, 0.095));
    settle(1);
    MIB_EXPECT(QToolTip::text().startsWith(QStringLiteral("Area ")) && !QToolTip::text().contains(QStringLiteral(" · E ")), "no E where the table has none");
    // On a cell (away from every edge): the per-point tooltip.
    mouse::move(view, tab.scatterDataViewPosForTests(cells[12].areaUm, cells[12].deform));
    settle(1);
    MIB_EXPECT(QToolTip::text().startsWith(QStringLiteral("Frame 1012")), "a cell keeps its tooltip: " + QToolTip::text().toStdString());
    // Outside the plot: nothing.
    mouse::move(view, QPointF(chart->plotArea().left() - 20, chart->plotArea().center().y()));
    settle(1);
    MIB_EXPECT(waitUntil([] { return QToolTip::text().isEmpty(); }), "no readout outside the plot");

    // ---- 4. dragging an edge edits the draft -------------------------------------------------------------
    wd.mark("drag-area");
    const Ranges home = rangesOf(chart);
    const QRectF plot = chart->plotArea();
    const double pxPerUm2 = plot.width() / (home.x1 - home.x0);
    const double pxPerDef = plot.height() / (home.y1 - home.y0);
    auto* areaMaxSpin = qobject_cast<QSpinBox*>(tab.tuneFieldWidget(TuneField::TargetAreaMax));
    auto* deformMaxSpin = qobject_cast<QDoubleSpinBox*>(tab.tuneFieldWidget(TuneField::TargetDeformabilityMax));
    MIB_REQUIRE(areaMaxSpin && deformMaxSpin, "spin boxes");
    const auto baseline = processing.getProcessingConfig();
    mouse::drag(view, areaMaxAt + QPointF(2, 0), areaMaxAt + QPointF(2 + 60, 7), 8);
    settle(2);
    {
        const int expected = static_cast<int>(std::lround(400.0 + 60.0 / pxPerUm2));
        MIB_EXPECT(draft.field(TuneField::TargetAreaMax).toInt() == expected, "draft area max moved with the pointer: " + std::to_string(draft.field(TuneField::TargetAreaMax).toInt()) + " vs " + std::to_string(expected));
        MIB_EXPECT(areaMaxSpin->value() == expected, "the spin box shows it");
        MIB_EXPECT(draft.changeCount() == 1 && draft.isChanged(TuneField::TargetAreaMax) && tab.tuneStateText() == QStringLiteral("1 unapplied change"),
                   "footer: 1 unapplied change");
        auto* rowLabel = rowLabelOf(tab, areaMaxSpin);
        MIB_EXPECT(rowLabel && rowLabel->text().endsWith(QStringLiteral(" *")), "the row carries the * change marker");
        MIB_EXPECT(tab.tuneApplyButton()->isEnabled(), "Apply is offered (and still required)");
        MIB_EXPECT(processing.getProcessingConfig().target_group_area_max == baseline.target_group_area_max, "the backend is untouched until Apply");
        MIB_EXPECT(rangesOf(chart) == home, "an edge drag does not pan");
        info = tab.gateOverlayInfoForTests();
        MIB_EXPECT(info.label == QStringLiteral("Target group (unapplied)"), "unapplied label while the draft differs");
        MIB_EXPECT(std::abs(info.boxPx.right() - tab.scatterDataViewPosForTests(expected, 0.04).x()) < 1.5, "the box follows the draft");
        MIB_EXPECT(info.dragging == gate::HandleKind::None && QToolTip::text().contains(QStringLiteral("Target group area maximum")),
                   "drag ended; the handle is hovered again");
        MIB_EXPECT(preview->text() == expectedPreview(draft.draft(), cells), "preview count follows the drag: " + preview->text().toStdString());
        MIB_EXPECT(view->cursor().shape() == Qt::SizeHorCursor, "still over the handle after the drag");
        shot(tab, "2-area-max-dragged");
    }

    wd.mark("drag-deform");
    {
        const QPointF at = tab.gateHandleViewPosForTests(gate::HandleKind::DeformMax);
        const double dy = 28.0;
        mouse::drag(view, at, at + QPointF(0, -dy), 8);
        settle(2);
        const double expected = std::round((0.06 + dy / pxPerDef) * 100.0) / 100.0;
        MIB_EXPECT(std::abs(draft.field(TuneField::TargetDeformabilityMax).toDouble() - expected) < 1e-9, "deformability max follows (2 decimals): " + std::to_string(draft.field(TuneField::TargetDeformabilityMax).toDouble()));
        MIB_EXPECT(std::abs(draft.field(TuneField::TargetDeformabilityMax).toDouble() - (0.06 + dy / pxPerDef)) <= 0.005 + 1e-9, "... within rounding of the exact travel");
        MIB_EXPECT(std::abs(deformMaxSpin->value() - expected) < 1e-9 && draft.changeCount() == 2 && tab.tuneStateText() == QStringLiteral("2 unapplied changes"),
                   "spin box + footer: 2 unapplied changes");
        MIB_EXPECT(rangesOf(chart) == home, "axes unchanged");
        // The lower edge cannot pass the upper one.
        const QPointF low = tab.gateHandleViewPosForTests(gate::HandleKind::DeformMin);
        mouse::drag(view, low, low + QPointF(0, -300), 8);
        settle(2);
        MIB_EXPECT(std::abs(draft.field(TuneField::TargetDeformabilityMin).toDouble() - draft.field(TuneField::TargetDeformabilityMax).toDouble()) < 1e-9,
                   "min stops at max");
        MIB_EXPECT(draft.valid(), "the draft stays valid (min <= max)");
        tab.tuneRevertButton()->click();
        settle(2);
        MIB_REQUIRE(!draft.dirty(), "reverted");
        // The area maximum cannot pass the area minimum.
        const QPointF right = tab.gateHandleViewPosForTests(gate::HandleKind::AreaMax);
        mouse::drag(view, right, QPointF(plot.left() + 4, right.y()), 8);
        settle(2);
        MIB_EXPECT(draft.field(TuneField::TargetAreaMax).toInt() == draft.field(TuneField::TargetAreaMin).toInt() && draft.valid(), "area max stops at area min");
        tab.tuneRevertButton()->click();
        settle(2);
        MIB_EXPECT(!draft.dirty() && areaMaxSpin->value() == baseline.target_group_area_max, "Revert restores the applied gate");
        info = tab.gateOverlayInfoForTests();
        MIB_EXPECT(info.label == QStringLiteral("Target group") && info.shown, "... and the overlay follows");
    }

    // The control's range clamps: deformability maximum is at most 1.
    wd.mark("clamp");
    {
        tab.setScatterYRange(0, 1.2);
        settle(2);
        const Ranges tall = rangesOf(chart);
        const QRectF p2 = chart->plotArea();
        const QPointF at = tab.gateHandleViewPosForTests(gate::HandleKind::DeformMax);
        MIB_REQUIRE(finite(at), "upper edge visible on the tall plot");
        mouse::drag(view, at, QPointF(at.x(), p2.top() + 1), 8);
        settle(2);
        MIB_EXPECT(draft.field(TuneField::TargetDeformabilityMax).toDouble() == 1.0 && deformMaxSpin->value() == 1.0, "dragged past the control's maximum: clamped at 1.00");
        (void)tall;
        tab.tuneRevertButton()->click();
        tab.setScatterYRange(0, 0.1);
        settle(2);
        MIB_EXPECT(!draft.dirty() && rangesOf(chart) == home, "back to the working view");
    }

    // ---- 5. pan and click are untouched away from handles -----------------------------------------------------
    wd.mark("pan-click");
    {
        const QPointF inside = tab.scatterDataViewPosForTests(275, 0.04); // inside the box, far from its edges and from cells
        MIB_EXPECT(tab.gateHandleAtForTests(inside) == gate::HandleKind::None, "no handle in the middle of the box");
        mouse::drag(view, inside, inside + QPointF(-70, 25), 8);
        settle(2);
        MIB_EXPECT(!(rangesOf(chart) == home), "a drag starting away from the handles pans");
        MIB_EXPECT(!draft.dirty(), "... and edits nothing");
        view->resetZoom();
        settle(2);
        MIB_EXPECT(rangesOf(chart) == home, "zoom reset");

        // Cell 2000 sits on the area-max edge: a plain click selects it, nothing is edited.
        const std::size_t k = [&] {
            const auto& fr = tab.scatterPointFramesForTests();
            for (std::size_t i = 0; i < fr.size(); ++i) if (fr[i] == 2000) return i;
            return fr.size();
        }();
        MIB_REQUIRE(k < cells.size(), "edge cell plotted");
        const QPointF onEdge = tab.scatterPointViewPosForTests(k);
        MIB_EXPECT(tab.gateHandleAtForTests(onEdge) == gate::HandleKind::AreaMax, "the cell lies on the handle");
        mouse::move(view, onEdge);
        settle(1);
        MIB_EXPECT(view->cursor().shape() == Qt::SizeHorCursor, "the handle wins the cursor");
        mouse::click(view, onEdge);
        settle(2);
        MIB_EXPECT(tab.selectedFrameIndex() == 2000u, "a click without travel on a handle still selects the cell");
        MIB_EXPECT(!draft.dirty() && rangesOf(chart) == home, "... and edits nothing");
    }

    // ---- 6. the E gate ---------------------------------------------------------------------------------------------
    wd.mark("emodulus");
    {
        MIB_EXPECT(!finite(tab.gateHandleViewPosForTests(gate::HandleKind::EmodMin)) && tab.gateOverlayInfoForTests().contourLabels.isEmpty(),
                   "E gate off: no contours, no E handles");
        const auto plain = tab.gateOverlayInfoForTests();
        tab.setTuneFieldForTests(TuneField::TargetEmodulusEnabled, true);
        settle(2);
        info = tab.gateOverlayInfoForTests();
        MIB_EXPECT(info.contourLabels.contains(QStringLiteral("E ≥ 1.00 kPa")) && info.contourLabels.contains(QStringLiteral("E ≤ 1.60 kPa")),
                   "E contours labelled with their values: " + info.contourLabels.join(QStringLiteral(" | ")).toStdString());
        MIB_EXPECT(info.contourLines >= 2 && info.notice.isEmpty(), "both contours drawn, no notice");
        MIB_EXPECT(info.maskOn > 0 && info.maskOn < plain.maskOn, "the E range removes cells from the tinted region (but not all of it)");
        MIB_EXPECT(info.label == QStringLiteral("Target group (unapplied)"), "enabling the E gate is an unapplied change");
        MIB_EXPECT(preview->text() == expectedPreview(draft.draft(), cells), "preview with the E gate: " + preview->text().toStdString());
        {
            // The E gate really changes the count in this fixture.
            auto noE = draft.draft();
            noE.enable_target_group_emodulus = false;
            MIB_EXPECT(expectedPreview(noE, cells) != expectedPreview(draft.draft(), cells) && !expectedPreview(draft.draft(), cells).contains(QStringLiteral(": 0 of ")),
                       "fixture: the E range excludes some cells and keeps some");
        }

        const QPointF eMinAt = tab.gateHandleViewPosForTests(gate::HandleKind::EmodMin);
        MIB_REQUIRE(finite(eMinAt), "E-min contour visible");
        MIB_EXPECT(tab.gateHandleAtForTests(eMinAt) == gate::HandleKind::EmodMin, "E handle under the anchor");
        mouse::move(view, eMinAt);
        settle(1);
        MIB_EXPECT(QToolTip::text().contains(QStringLiteral("Target group Young's modulus minimum")) && QToolTip::text().contains(QStringLiteral("1.00")),
                   "E tooltip names the field: " + QToolTip::text().toStdString());
        MIB_EXPECT(view->cursor().shape() == Qt::SizeAllCursor, "E contour cursor");

        const QPointF dataBefore = dataAt(tab, eMinAt);
        const QPointF shift(40, 0);
        const QPointF dataAfter = dataAt(tab, eMinAt + shift);
        const double expectedE = 1.0 + lut.lookup(dataAfter.x(), dataAfter.y()) - lut.lookup(dataBefore.x(), dataBefore.y());
        MIB_REQUIRE(std::isfinite(expectedE), "inside the table");
        const int changesBefore = draft.changeCount();
        mouse::drag(view, eMinAt, eMinAt + shift, 8);
        settle(2);
        auto* eMinSpin = qobject_cast<QDoubleSpinBox*>(tab.tuneFieldWidget(TuneField::TargetEmodulusMin));
        MIB_REQUIRE(eMinSpin, "E min spin box");
        const double gotE = draft.field(TuneField::TargetEmodulusMin).toDouble();
        MIB_EXPECT(std::abs(gotE - expectedE) < 0.03 && gotE != 1.0, "E min moved along the table: " + std::to_string(gotE) + " vs " + std::to_string(expectedE));
        MIB_EXPECT(std::abs(eMinSpin->value() - gotE) < 1e-9 && draft.field(TuneField::TargetEmodulusMax).toDouble() == 1.6, "spin box follows; E max untouched");
        MIB_EXPECT(draft.changeCount() == changesBefore + 1, "one more unapplied change");
        auto* eRow = rowLabelOf(tab, eMinSpin);
        MIB_EXPECT(eRow && eRow->text().endsWith(QStringLiteral(" *")), "E row marked");
        MIB_EXPECT(tab.gateOverlayInfoForTests().contourLabels.contains(QStringLiteral("E ≥ %1 kPa").arg(gotE, 0, 'f', 2)), "the contour label shows the new value");
        MIB_EXPECT(rangesOf(chart) == home, "no pan");
        MIB_EXPECT(preview->text() == expectedPreview(draft.draft(), cells), "preview follows the E edit");
        shot(tab, "3-e-gate");

        // Unticking the E box removes the E handles again.
        tab.setTuneFieldForTests(TuneField::TargetEmodulusEnabled, false);
        settle(2);
        MIB_EXPECT(!finite(tab.gateHandleViewPosForTests(gate::HandleKind::EmodMin)) && tab.gateOverlayInfoForTests().contourLabels.isEmpty(), "E gate unticked: contours and handles gone");
        tab.tuneRevertButton()->click();
        settle(2);
        MIB_EXPECT(!draft.dirty(), "reverted");
    }

    // ---- 7. nothing can be grabbed while an apply is in flight --------------------------------------------------------
    wd.mark("applying");
    {
        frontend::ApplyProcessingDraftRequest last;
        QObject::connect(&tab, &Tab::applyRequested, &tab, [&](const frontend::ApplyProcessingDraftRequest& r) { last = r; });
        tab.setTuneFieldForTests(TuneField::TargetAreaMax, 420);
        settle(1);
        const QPointF at = tab.gateHandleViewPosForTests(gate::HandleKind::AreaMax);
        MIB_REQUIRE(finite(at) && tab.gateHandleAtForTests(at) == gate::HandleKind::AreaMax, "handle before the apply");
        tab.tuneApplyButton()->click();
        settle(2);
        MIB_REQUIRE(draft.applying(), "apply in flight");
        MIB_EXPECT(tab.gateHandleAtForTests(at) == gate::HandleKind::None, "no handle while applying");
        mouse::move(view, at);
        settle(1);
        MIB_EXPECT(view->cursor().shape() != Qt::SizeHorCursor, "no resize cursor while applying");
        mouse::drag(view, at, at + QPointF(50, 0), 8);
        settle(2);
        MIB_EXPECT(draft.field(TuneField::TargetAreaMax).toInt() == 420, "a drag during the apply edits nothing");
        view->resetZoom();
        frontend::ConfigApplyResult ok;
        ok.requestId = last.requestId;
        processing.setProcessingConfig(frontend::applyTunePatch(processing.getProcessingConfig(), last.patch));
        ok.effectiveConfig = processing.getProcessingConfig();
        ok.persisted = ok.applied = true;
        tab.onApplyResult(ok);
        settle(2);
        MIB_EXPECT(!draft.applying() && !draft.dirty() && tab.gateOverlayInfoForTests().label == QStringLiteral("Target group"), "applied: clean again");
        const QPointF again = tab.gateHandleViewPosForTests(gate::HandleKind::AreaMax);
        MIB_EXPECT(tab.gateHandleAtForTests(again) == gate::HandleKind::AreaMax, "handles work again after the result");
        mouse::drag(view, again, again + QPointF(30, 0), 8);
        settle(2);
        MIB_EXPECT(draft.field(TuneField::TargetAreaMax).toInt() > 420 && draft.isChanged(TuneField::TargetAreaMax), "... and edit again");
        tab.tuneRevertButton()->click();
        settle(2);
        MIB_EXPECT(!draft.dirty() && areaMaxSpin->value() == 420, "reverted to the newly applied gate");
    }

    // ---- 8. snapshots never carry the overlay --------------------------------------------------------------------------------
    wd.mark("snapshot");
    {
        MIB_REQUIRE(tab.gateOverlayInfoForTests().shown, "overlay shown before the snapshot");
        const QImage shown = view->grab().toImage().convertToFormat(QImage::Format_RGB32);
        cv::Mat hist, scatter;
        MIB_REQUIRE(tab.captureChartSnapshots(hist, scatter) && !scatter.empty(), "snapshot taken");
        MIB_EXPECT(tab.gateOverlayInfoForTests().shown, "overlay restored after the snapshot");
        MIB_REQUIRE(scatter.cols == shown.width() && scatter.rows == shown.height(), "same size as the view");
        const auto info2 = tab.gateOverlayInfoForTests();
        const QRect box = info2.boxPx.toAlignedRect().adjusted(3, 3, -3, -3).intersected(QRect(0, 0, shown.width(), shown.height()));
        int tinted = 0, plainInSnapshot = 0;
        for (int y = box.top(); y <= box.bottom(); ++y)
            for (int x = box.left(); x <= box.right(); ++x) {
                const QRgb live = shown.pixel(x, y);
                const cv::Vec3b snap = scatter.at<cv::Vec3b>(y, x); // BGR
                const bool snapWhite = snap[0] == 255 && snap[1] == 255 && snap[2] == 255;
                if (snapWhite) {
                    ++plainInSnapshot;
                    if (qRed(live) != 255 || qGreen(live) != 255 || qBlue(live) != 255) ++tinted;
                }
            }
        MIB_EXPECT(plainInSnapshot > 1000 && tinted > plainInSnapshot / 2,
                   "the live view is tinted inside the box where the stored snapshot is plain (" + std::to_string(tinted) + "/" + std::to_string(plainInSnapshot) + ")");
    }

    // ---- 9. no table -------------------------------------------------------------------------------------------------------------
    wd.mark("no-table");
    {
        tab.setTuneFieldForTests(TuneField::TargetEmodulusEnabled, true);
        settle(2);
        MIB_REQUIRE(tab.gateOverlayInfoForTests().notice.isEmpty() && !tab.gateOverlayInfoForTests().contourLabels.isEmpty(), "E gate drawn with the table");
        // The service's own "no table" state: a load that finds no file reports it unloaded.
        const QString here = QDir::currentPath();
        const QString away = QString::fromStdString((td.path() / "elsewhere" / "deep").string());
        MIB_REQUIRE(QDir().mkpath(away) && QDir::setCurrent(away), "empty working directory");
        const bool loaded = processing.loadEModulusLut("/nonexistent/mib-lut.txt");
        MIB_REQUIRE(QDir::setCurrent(here), "back to the working directory");
        MIB_REQUIRE(!loaded && !processing.eModulusLutIdentity().loaded, "the service reports no table");
        tab.loadCurrentConfig(); // what showing the tab does
        settle(2);
        info = tab.gateOverlayInfoForTests();
        MIB_EXPECT(tab.gateLutForTests() == nullptr, "the tab dropped its copy");
        MIB_EXPECT(info.shown && info.notice.contains(QStringLiteral("No E-modulus table")) && info.notice.contains(QStringLiteral("admits no cell")),
                   "clear notice: " + info.notice.toStdString());
        MIB_EXPECT(info.maskOn == 0 && info.contourLabels.isEmpty() && info.contourLines == 0, "nothing tinted, no contours");
        MIB_EXPECT(preview->text().contains(QStringLiteral(": 0 of ")), "no cell counted: " + preview->text().toStdString());
        MIB_EXPECT(!finite(tab.gateHandleViewPosForTests(gate::HandleKind::EmodMin)), "no E handle without a table");
        shot(tab, "4-no-table");
        mouse::move(view, tab.scatterDataViewPosForTests(310, 0.045));
        settle(1);
        MIB_EXPECT(QToolTip::text().startsWith(QStringLiteral("Area ")) && !QToolTip::text().contains(QStringLiteral(" · E ")), "no E in the readout without a table");
        // The box edges still work.
        const QPointF at = tab.gateHandleViewPosForTests(gate::HandleKind::AreaMax);
        MIB_EXPECT(tab.gateHandleAtForTests(at) == gate::HandleKind::AreaMax, "box handles do not need a table");

        MIB_REQUIRE(processing.loadEModulusLut((QDir(here).absoluteFilePath(QStringLiteral("resources/isoelastic_curve/scaled_isoelastic_data_LUT_6.16-4.24.txt"))).toStdString()),
                    "the table comes back");
        tab.loadCurrentConfig();
        settle(2);
        info = tab.gateOverlayInfoForTests();
        MIB_EXPECT(tab.gateLutForTests() != nullptr && info.notice.isEmpty() && info.contourLabels.size() == 2 && info.maskOn > 0,
                   "table back: contours and tint are back, no notice");
        MIB_EXPECT(preview->text() == expectedPreview(draft.draft(), cells), "count back: " + preview->text().toStdString());
    }

    tab.close();
    settle(2);
    backend.shutdown();
    return mib::test::exitCode();
}
