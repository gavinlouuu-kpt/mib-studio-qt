#include "frontend/tabs/ExperimentMonitoringTab.h"
#include "backend/processing/MonitoringDensity.h"
#include "backend/processing/KdeCoreRecord.h"
#include "frontend/tabs/MonitoringRoiCrop.h"
#include "ui_ExperimentMonitoringTab.h"

#include <QElapsedTimer>
#include <QDateTime>
#include <QFileInfo>
#include <QLegendMarker>
#include <QPen>
#include <QSettings>
#include <QLegend>
#include <QLegendMarker>
#include <QTimer>
#include <QLabel>
#include <QPixmap>
#include <QPainter>
#include <QPolygonF>
#include <QChartView>
#include <QScatterSeries>
#include <QLineSeries>
#include <QBarSeries>
#include <QBarSet>
#include <QChart>
#include <QValueAxis>
#ifndef MIB_HAS_QHISTOGRAMSERIES
#if __has_include(<QHistogramSeries>)
#define MIB_HAS_QHISTOGRAMSERIES 1
#else
#define MIB_HAS_QHISTOGRAMSERIES 0
#endif
#endif
#if MIB_HAS_QHISTOGRAMSERIES
#include <QHistogramSeries>
#else
#include <QBarCategoryAxis>
#endif
#include <QFontMetricsF>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsItem>
#include <QGroupBox>
#include <QMetaMethod>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVariant>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QShowEvent>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QSplitter>
#include <QToolButton>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <numeric>
#include <map>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QDir>
#include <QPixmap>
#include <QFileDialog>

#include "frontend/models/TargetGatePreview.h"
#include "frontend/widgets/ZoomableChartView.h"
#include "frontend/dialogs/FrameViewerDialog.h"
#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingScience.h"
#include "backend/services/TriggerService.h"

#include <spdlog/spdlog.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace {

struct InvalidReason {
    QString shortText;
    QString longText;
};

std::vector<InvalidReason> getInvalidReasons(
    const backend::services::FilterResult& result,
    const backend::services::ProcessingConfig& config,
    double pixelToMicronFactor = 1.0)
{
    // Which reasons apply comes from the shared classifier so this tooltip and
    // the live invalid-reason histogram never disagree. The detailed value text
    // is built here (the classifier returns codes only).
    namespace science = backend::processing::science;
    const double areaUm = result.area * pixelToMicronFactor * pixelToMicronFactor;

    std::vector<InvalidReason> reasons;
    for (auto code : science::classifyInvalidReasons(result, config, pixelToMicronFactor)) {
        switch (code) {
        case science::InvalidReasonCode::NoContour:
            reasons.push_back({"No contour", "No inner contour found"});
            break;
        case science::InvalidReasonCode::Border:
            reasons.push_back({"Border", "Contour touches ROI border"});
            break;
        case science::InvalidReasonCode::Area:
            reasons.push_back({"Area",
                               QString("Area: %1 μm² (range: %2-%3)")
                                   .arg(areaUm, 0, 'f', 0)
                                   .arg(config.area_threshold_min)
                                   .arg(config.area_threshold_max)});
            break;
        case science::InvalidReasonCode::Ring:
            reasons.push_back({"Ring",
                               QString("Ring ratio: %1 (range: %2-%3)")
                                   .arg(result.ringRatio, 0, 'f', 1)
                                   .arg(config.ring_ratio_min, 0, 'f', 1)
                                   .arg(config.ring_ratio_max, 0, 'f', 1)});
            break;
        case science::InvalidReasonCode::Deform:
            reasons.push_back({"Deform",
                               QString("Deformability: %1 (range: %2-%3)")
                                   .arg(result.deformability, 0, 'f', 3)
                                   .arg(config.deformability_threshold_min, 0, 'f', 3)
                                   .arg(config.deformability_threshold_max, 0, 'f', 3)});
            break;
        case science::InvalidReasonCode::AreaRatio:
            reasons.push_back({"Ratio",
                               QString("Area ratio: %1 (max: %2)")
                                   .arg(result.areaRatio, 0, 'f', 2)
                                   .arg(config.area_ratio_threshold_max, 0, 'f', 2)});
            break;
        case science::InvalidReasonCode::Laplacian:
            reasons.push_back({"Focus",
                               QString("Laplacian variance: %1 (range: %2-%3)")
                                   .arg(result.laplacianVariance, 0, 'f', 1)
                                   .arg(config.laplacian_variance_min, 0, 'f', 1)
                                   .arg(config.laplacian_variance_max, 0, 'f', 1)});
            break;
        case science::InvalidReasonCode::Channel:
            reasons.push_back({"Wall",
                               QString("Centroid row %1 outside the channel band")
                                   .arg(result.centroidY, 0, 'f', 1)});
            break;
        }
    }

    return reasons;
}

// QSettings keys for the scatter density (KDE) preferences. The version guard
// lets a future change of semantics fall back to defaults instead of reading
// stale values (same pattern as PreviewPage's layout preference).
constexpr const char* kKdeKeyVersion = "Monitoring/KdeVersion";
constexpr const char* kKdeKeyEnabled = "Monitoring/KdeEnabled";
constexpr const char* kKdeKeyBandwidthFactor = "Monitoring/KdeBandwidthFactor";
constexpr const char* kKdeKeyIntervalMs = "Monitoring/KdeIntervalMs";
constexpr const char* kKdeKeyCoreFraction = "Monitoring/KdeCoreFraction";
constexpr const char* kKdeKeyReferencePath = "Monitoring/KdeReferencePath";
constexpr int kKdePreferenceVersion = 1;

// Colour of a density level (0 = sparsest) on the sequential ramp.
QColor kdeLevelColor(int level, int levels)
{
    const double t = levels > 1 ? static_cast<double>(level) / (levels - 1) : 1.0;
    const auto rgb = backend::monitoring::densityRampColor(t);
    return QColor(rgb.r, rgb.g, rgb.b);
}

// Hides the selected-cell marker while a chart is grabbed: stored snapshots
// and exports show the data, not the operator's current selection.
class HiddenWhileGrabbing {
public:
    explicit HiddenWhileGrabbing(QScatterSeries* series) : series_(series), wasVisible_(series && series->isVisible())
    {
        if (wasVisible_) series_->setVisible(false);
    }
    ~HiddenWhileGrabbing()
    {
        if (wasVisible_) series_->setVisible(true);
    }
    HiddenWhileGrabbing(const HiddenWhileGrabbing&) = delete;
    HiddenWhileGrabbing& operator=(const HiddenWhileGrabbing&) = delete;

private:
    QScatterSeries* series_;
    bool wasVisible_;
};

// Same for an overlay item (the operator's gate preview is editing UI, not data).
class HiddenItemWhileGrabbing {
public:
    explicit HiddenItemWhileGrabbing(QGraphicsItem* item) : item_(item), wasVisible_(item && item->isVisible())
    {
        if (wasVisible_) item_->setVisible(false);
    }
    ~HiddenItemWhileGrabbing()
    {
        if (wasVisible_) item_->setVisible(true);
    }
    HiddenItemWhileGrabbing(const HiddenItemWhileGrabbing&) = delete;
    HiddenItemWhileGrabbing& operator=(const HiddenItemWhileGrabbing&) = delete;

private:
    QGraphicsItem* item_;
    bool wasVisible_;
};

} // anonymous namespace
namespace frontend
{

// The draft target-group gate drawn over the Deformability-vs-Area scatter: a
// tinted "would be target" region, the dashed area/deformability box and, with
// the E gate on and a table loaded, the dashed-dotted iso-E contours with value
// labels. A child item of the chart (chart coordinates = plot-area coordinates),
// transparent to the mouse. All geometry comes from the live axes and plot area
// at paint time; the tab only hands it the gate, the table and the contours.
// Lines carry dash patterns and text, never colour alone.
class GateOverlayItem : public QGraphicsItem {
public:
    struct Params {
        gate::GateSpec spec;
        const backend::EModulusLut* lut = nullptr;
        std::uint64_t lutGeneration = 0;
        std::shared_ptr<const gate::Contour> eMin, eMax;
        QString label;
        QString notice;
    };

    static constexpr double kMaskStepPx = 3.0;

    GateOverlayItem(QChart* chart, QValueAxis* x, QValueAxis* y) : QGraphicsItem(chart), chart_(chart), x_(x), y_(y)
    {
        setZValue(4.5); // above the series (4), below the legend (5)
        setAcceptedMouseButtons(Qt::NoButton);
    }

    void setParams(Params p)
    {
        params_ = std::move(p);
        update();
    }
    void setHover(gate::HandleKind k)
    {
        if (hover_ == k) return;
        hover_ = k;
        update();
    }
    void setDragging(gate::HandleKind k)
    {
        if (dragging_ == k) return;
        dragging_ = k;
        update();
    }
    gate::HandleKind hover() const { return hover_; }
    gate::HandleKind dragging() const { return dragging_; }
    void plotAreaChanged()
    {
        prepareGeometryChange();
        update();
    }

    gate::Viewport viewport() const
    {
        gate::Viewport v;
        const QRectF plot = chart_->plotArea();
        v.x0 = x_->min();
        v.x1 = x_->max();
        v.y0 = y_->min();
        v.y1 = y_->max();
        v.left = plot.left();
        v.top = plot.top();
        v.width = plot.width();
        v.height = plot.height();
        return v;
    }

    QRectF boundingRect() const override { return chart_->plotArea(); }

    ExperimentMonitoringTab::GateOverlayInfo info()
    {
        ExperimentMonitoringTab::GateOverlayInfo i;
        i.shown = isVisible();
        i.label = params_.label;
        i.notice = params_.notice;
        i.hovered = hover_;
        i.dragging = dragging_;
        const gate::Viewport vp = viewport();
        i.plotPx = QRectF(vp.left, vp.top, vp.width, vp.height);
        if (!gate::viewportValid(vp)) return i;
        i.boxPx = boxRect(vp);
        const gate::Mask& m = mask(vp);
        for (int r = 0; r < m.rows; ++r) {
            for (int c = 0; c < m.cols; ++c) {
                const double cx = vp.left + (c + 0.5) * m.stepPx;
                const double cy = vp.top + (r + 0.5) * m.stepPx;
                if (cx > vp.left + vp.width || cy > vp.top + vp.height) continue;
                ++i.maskCells;
                if (m.at(c, r)) ++i.maskOn;
            }
        }
        for (const auto& drawn : drawnContours(vp)) {
            i.contourLabels << drawn.text;
            i.contourLines += static_cast<int>(drawn.contour->size());
        }
        return i;
    }

    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        const gate::GateSpec& spec = params_.spec;
        const gate::Viewport vp = viewport();
        if (!spec.enabled || !gate::viewportValid(vp)) return;
        const QRectF plot(vp.left, vp.top, vp.width, vp.height);
        const QRectF box = boxRect(vp);
        const QRectF boxVisible = box.intersected(plot);

        p->save();
        p->setClipRect(plot);
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setFont(chart_->font());

        // 1. Tinted region: exact rectangle when the E layer does not filter (no E
        //    gate, or no table loaded: the group is the box alone); otherwise the
        //    sampled predicate (box ^ E range), clipped to the exact box.
        if (!boxVisible.isEmpty()) {
            if (!gate::eLayerFilters(spec, gate::lutLoaded(params_.lut))) {
                p->fillRect(boxVisible, tintColor());
            } else {
                const gate::Mask& m = mask(vp);
                if (m.onCount() > 0) {
                    if (maskImage_.isNull()) {
                        maskImage_ = QImage(m.cols, m.rows, QImage::Format_ARGB32_Premultiplied);
                        maskImage_.fill(0);
                        const QRgb px = qPremultiply(tintColor().rgba());
                        for (int r = 0; r < m.rows; ++r)
                            for (int c = 0; c < m.cols; ++c)
                                if (m.at(c, r)) maskImage_.setPixel(c, r, px);
                    }
                    p->save();
                    p->setClipRect(boxVisible);
                    p->setRenderHint(QPainter::SmoothPixmapTransform, true);
                    p->drawImage(QRectF(vp.left, vp.top, m.cols * m.stepPx, m.rows * m.stepPx), maskImage_);
                    p->restore();
                }
            }
        }

        // 2. Dashed box. A hovered or dragged edge is drawn solid and heavier.
        auto drawEdge = [&](gate::HandleKind k, const QLineF& line) {
            const bool active = hover_ == k || dragging_ == k;
            QPen pen(active ? activeColor() : edgeColor(), active ? 3.5 : 2.0, active ? Qt::SolidLine : Qt::DashLine);
            pen.setCosmetic(true);
            p->setPen(pen);
            p->drawLine(line);
        };
        drawEdge(gate::HandleKind::AreaMin, QLineF(box.left(), box.top(), box.left(), box.bottom()));
        drawEdge(gate::HandleKind::AreaMax, QLineF(box.right(), box.top(), box.right(), box.bottom()));
        drawEdge(gate::HandleKind::DeformMax, QLineF(box.left(), box.top(), box.right(), box.top()));
        drawEdge(gate::HandleKind::DeformMin, QLineF(box.left(), box.bottom(), box.right(), box.bottom()));

        // 3. Name (and unapplied state) of the gate and the E-gate notice sit in the
        //    plot's top-left corner; their rectangles are reserved first so the
        //    contour labels can move out of their way.
        const qreal maxPill = std::max(60.0, plot.width() - 12.0);
        const QFontMetricsF fm(p->font());
        std::vector<QRectF> reserved;
        QRectF labelRect, noticeRect;
        QPointF cursor(plot.left() + 6.0, plot.top() + 6.0);
        if (!params_.label.isEmpty()) {
            labelRect = QRectF(cursor, pillSize(fm, params_.label, maxPill));
            reserved.push_back(labelRect);
            cursor.setY(labelRect.bottom() + 4.0);
        }
        if (!params_.notice.isEmpty()) {
            noticeRect = QRectF(cursor, pillSize(fm, params_.notice, maxPill));
            reserved.push_back(noticeRect);
        }

        // 4. Iso-E contours and their value labels.
        for (const auto& drawn : drawnContours(vp)) {
            const bool active = hover_ == drawn.kind || dragging_ == drawn.kind;
            QPen pen(active ? activeColor() : emodColor(), active ? 3.5 : 2.0, active ? Qt::SolidLine : Qt::DashDotLine);
            pen.setCosmetic(true);
            p->setPen(pen);
            for (const gate::Polyline& line : *drawn.contour) {
                QPolygonF poly;
                poly.reserve(static_cast<qsizetype>(line.size()));
                for (const QPointF& d : line) poly << gate::toPixel(vp, d);
                p->drawPolyline(poly);
            }
        }
        for (const auto& drawn : drawnContours(vp)) {
            const bool active = hover_ == drawn.kind || dragging_ == drawn.kind;
            const QSizeF sz = pillSize(fm, drawn.text, maxPill);
            QRectF r(QPointF(drawn.labelAt.x() - sz.width() - 4.0, drawn.labelAt.y() - sz.height() - 2.0), sz);
            auto collides = [&](const QRectF& c) {
                for (const QRectF& other : reserved)
                    if (other.intersects(c)) return true;
                return false;
            };
            for (int tries = 0; tries < 6 && collides(r); ++tries) r.translate(0.0, sz.height() + 2.0);
            r.moveLeft(std::clamp(r.left(), plot.left() + 2.0, std::max(plot.left() + 2.0, plot.right() - sz.width() - 2.0)));
            r.moveTop(std::clamp(r.top(), plot.top() + 2.0, std::max(plot.top() + 2.0, plot.bottom() - sz.height() - 2.0)));
            reserved.push_back(r);
            drawPill(p, r, drawn.text, active ? activeColor() : emodColor());
        }
        if (!labelRect.isEmpty()) drawPill(p, labelRect, params_.label, edgeColor());
        if (!noticeRect.isEmpty()) drawPill(p, noticeRect, params_.notice, warnColor());
        p->restore();
    }

private:
    struct DrawnContour {
        gate::HandleKind kind;
        const gate::Contour* contour;
        QString text;
        QPointF labelAt;
    };

    static QColor tintColor() { return QColor(0, 120, 255, 44); }
    static QColor edgeColor() { return QColor(0, 70, 180); }
    static QColor emodColor() { return QColor(150, 40, 170); }
    static QColor activeColor() { return QColor(220, 110, 0); }
    static QColor warnColor() { return QColor(170, 30, 30); }

    // A text pill, word-wrapped to maxWidth.
    static QSizeF pillSize(const QFontMetricsF& fm, const QString& text, qreal maxWidth)
    {
        const QRectF text2 = fm.boundingRect(QRectF(0, 0, maxWidth - 10.0, 10000.0), Qt::AlignLeft | Qt::TextWordWrap, text);
        return QSizeF(text2.width() + 10.0, text2.height() + 4.0);
    }
    static void drawPill(QPainter* p, const QRectF& r, const QString& text, const QColor& color)
    {
        p->save();
        p->setPen(QPen(color, 1.0));
        p->setBrush(QColor(255, 255, 255, 225));
        p->drawRoundedRect(r, 3.0, 3.0);
        p->setPen(color);
        p->drawText(r.adjusted(5.0, 2.0, -5.0, -2.0), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, text);
        p->restore();
    }

    QRectF boxRect(const gate::Viewport& vp) const
    {
        const QPointF a = gate::toPixel(vp, QPointF(params_.spec.areaMin, params_.spec.deformMax));
        const QPointF b = gate::toPixel(vp, QPointF(params_.spec.areaMax, params_.spec.deformMin));
        return QRectF(a, b).normalized();
    }

    std::vector<DrawnContour> drawnContours(const gate::Viewport& vp) const
    {
        std::vector<DrawnContour> out;
        const gate::GateSpec& s = params_.spec;
        if (!s.enabled || !s.emodEnabled || !params_.lut) return out;
        auto add = [&](gate::HandleKind kind, const std::shared_ptr<const gate::Contour>& c, const QString& text) {
            if (!c || c->empty()) return;
            const auto at = gate::contourLabelAnchor(*c, vp);
            if (!at) return;
            out.push_back({kind, c.get(), text, *at});
        };
        add(gate::HandleKind::EmodMin, params_.eMin, QObject::tr("E ≥ %1 kPa").arg(s.emodMin, 0, 'f', 2));
        add(gate::HandleKind::EmodMax, params_.eMax, QObject::tr("E ≤ %1 kPa").arg(s.emodMax, 0, 'f', 2));
        return out;
    }

    // The sampled predicate, recomputed only when the gate, the table or the
    // geometry changed (a pan or zoom recomputes it: ~10-20k lookups).
    const gate::Mask& mask(const gate::Viewport& vp)
    {
        const MaskKey key{params_.spec, params_.lutGeneration, params_.lut != nullptr, vp.x0, vp.x1, vp.y0, vp.y1,
                          vp.left, vp.top, vp.width, vp.height};
        if (!maskKey_ || !(*maskKey_ == key)) {
            mask_ = gate::computeMask(params_.spec, params_.lut, vp, kMaskStepPx);
            maskImage_ = QImage(); // rebuilt on the next paint
            maskKey_ = key;
        }
        return mask_;
    }

    struct MaskKey {
        gate::GateSpec spec;
        std::uint64_t lutGeneration;
        bool hasLut;
        double x0, x1, y0, y1, left, top, width, height;
        bool operator==(const MaskKey& o) const
        {
            return spec == o.spec && lutGeneration == o.lutGeneration && hasLut == o.hasLut && x0 == o.x0 &&
                   x1 == o.x1 && y0 == o.y0 && y1 == o.y1 && left == o.left && top == o.top && width == o.width &&
                   height == o.height;
        }
    };

    QChart* chart_;
    QValueAxis* x_;
    QValueAxis* y_;
    Params params_;
    gate::HandleKind hover_ = gate::HandleKind::None;
    gate::HandleKind dragging_ = gate::HandleKind::None;
    std::optional<MaskKey> maskKey_;
    gate::Mask mask_;
    QImage maskImage_; // the mask as a tint image (E gate on), cached with it
};

} // namespace frontend


namespace frontend
{

    ExperimentMonitoringTab::ExperimentMonitoringTab(backend::AppBackend &backend, QWidget *parent)
        : QWidget(parent), ui(new Ui::ExperimentMonitoringTab), backend_(backend)
    {
        ui->setupUi(this);

        roiLabel_ = new QLabel(tr("ROI: --"), this);
        roiLabel_->setStyleSheet("font-weight: bold; padding: 0 8px;");
        ui->topRowLayout->addWidget(roiLabel_);

        setupCharts();
        setupTuneParamsPanel();

        // Connect signals
        connect(ui->clearBufferBtn, &QPushButton::clicked, this, &ExperimentMonitoringTab::onClearBuffer);
        connect(ui->sortTriggerBtn, &QPushButton::clicked, this, &ExperimentMonitoringTab::onSortTrigger);
        connect(ui->triggerDurationSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int us) {
            backend_.trigger().setPulseDurationUs(us);
        });

        // Setup periodic test trigger timer
        periodicTriggerTimer_ = new QTimer(this);
        periodicTriggerTimer_->setInterval(ui->periodicTriggerIntervalSpin->value());
        connect(periodicTriggerTimer_, &QTimer::timeout, this, [this]() {
            backend_.trigger().onTargetGroupResult([] {
                backend::services::TargetGroupSignal signal;
                signal.isTargetGroup = true;
                return signal;
            }());
            ++periodicTriggerPulseCount_;
        });
        connect(ui->periodicTriggerBtn, &QPushButton::toggled, this, &ExperimentMonitoringTab::onPeriodicTriggerToggled);
        connect(ui->periodicTriggerIntervalSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int ms) {
            if (periodicTriggerTimer_) periodicTriggerTimer_->setInterval(ms);
        });
        connect(ui->validOverlayCheck, &QCheckBox::toggled, this, &ExperimentMonitoringTab::onToggleOverlay);
        connect(ui->invalidOverlayCheck, &QCheckBox::toggled, this, &ExperimentMonitoringTab::onToggleOverlay);

        // Set column stretch to make panels equal size
        ui->gridLayout->setColumnStretch(0, 1);
        ui->gridLayout->setColumnStretch(1, 1);
        ui->gridLayout->setColumnStretch(2, 0); // Tune panel: minimum width
        ui->gridLayout->setRowStretch(1, 1); // Charts row
        ui->gridLayout->setRowStretch(2, 1); // Frame grids row

        // Setup update timer
        updateTimer_ = new QTimer(this);
        updateTimer_->setInterval(UPDATE_INTERVAL_MS);
        connect(updateTimer_, &QTimer::timeout, this, &ExperimentMonitoringTab::onUpdate);

        // Scatter density (KDE): the backend service computes on its own
        // lowest-priority worker; this timer only polls for a new result.
        loadKdePreferences();
        kdeTimer_ = new QTimer(this);
        kdeTimer_->setInterval(kKdePollMs);
        connect(kdeTimer_, &QTimer::timeout, this, &ExperimentMonitoringTab::pollKdeResult);
        connect(ui->kdeToggleCheck, &QCheckBox::toggled, this, &ExperimentMonitoringTab::setKdeEnabled);
        {
            QSignalBlocker blocker(ui->kdeToggleCheck);
            ui->kdeToggleCheck->setChecked(kdeEnabled_);
        }
        setKdeModeVisuals(kdeEnabled_);
        {
            // Reload the reference contour chosen in a previous session; a file
            // that went away is dropped quietly (one info line).
            const QString referencePath = QSettings().value(QLatin1String(kKdeKeyReferencePath)).toString();
            if (!referencePath.isEmpty())
            {
                QString why;
                if (!loadKdeReferenceFromFile(referencePath, &why))
                {
                    SPDLOG_INFO("Monitoring KDE: previous reference contour {} not restored: {}", referencePath.toStdString(),
                                why.toStdString());
                    setKdeReferencePath(QString());
                }
            }
        }
    }

    void ExperimentMonitoringTab::updateRoiDisplay(int offsetX, int offsetY, int width, int height) {
        if (roiLabel_)
            roiLabel_->setText(tr("ROI: %1 x %2 @ (%3, %4)").arg(width).arg(height).arg(offsetX).arg(offsetY));
    }

    ExperimentMonitoringTab::~ExperimentMonitoringTab() {
        // Cleanup isoelastic curve line series
        for (QLineSeries* series : isoelasticCurves_)
        {
            if (series)
            {
                delete series;
            }
        }
        isoelasticCurves_.clear();
        delete ui;
    }

    // ------------------------------------------------------------------
    // Tune panel (issue #364): criteria grouped with their enable state,
    // full names + units, fixed Apply/Revert footer outside the scroll
    // area, explicit draft state. The panel never mutates the backend or
    // the config file itself; Apply is a request to the config coordinator.
    // ------------------------------------------------------------------

    void ExperimentMonitoringTab::bindTuneField(TuneField field, QWidget* widget, QWidget* rowLabel,
                                                std::function<QVariant()> read, std::function<void(const QVariant&)> write)
    {
        TuneBinding b;
        b.field = field;
        b.widget = widget;
        b.rowLabel = rowLabel;
        b.read = std::move(read);
        b.write = std::move(write);
        widget->setAccessibleName(tuneFieldLabel(field));
        if (widget->toolTip().isEmpty()) widget->setToolTip(tuneFieldLabel(field));
        tuneBindings_.push_back(std::move(b));
    }

    void ExperimentMonitoringTab::setupTuneParamsPanel()
    {
        auto* placeholder = ui->tuneParamsPlaceholder;
        placeholder->setObjectName(QStringLiteral("tunePanel"));
        placeholder->setMinimumWidth(kTunePanelMinWidth);
        placeholder->setMaximumWidth(kTunePanelMaxWidth);

        auto* outerLayout = new QVBoxLayout(placeholder);
        outerLayout->setContentsMargins(0, 0, 0, 0);
        outerLayout->setSpacing(0);

        tuneScrollArea_ = new QScrollArea(placeholder);
        tuneScrollArea_->setObjectName(QStringLiteral("tuneScrollArea"));
        tuneScrollArea_->setWidgetResizable(true);
        tuneScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tuneScrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        tuneScrollArea_->setFrameShape(QFrame::NoFrame);

        tunePanelContent_ = new QWidget();
        tunePanelContent_->setObjectName(QStringLiteral("tunePanelContent"));
        auto* contentLayout = new QVBoxLayout(tunePanelContent_);
        contentLayout->setContentsMargins(4, 4, 4, 4);
        contentLayout->setSpacing(6);

        auto addHeading = [&](const QString& objectName, const QString& title, const QString& hint) {
            auto* heading = new QLabel(QStringLiteral("<b>%1</b>").arg(title), tunePanelContent_);
            heading->setObjectName(objectName);
            heading->setWordWrap(true);
            heading->setAccessibleName(title);
            contentLayout->addWidget(heading);
            if (!hint.isEmpty()) {
                auto* hintLabel = new QLabel(hint, tunePanelContent_);
                hintLabel->setObjectName(objectName + QStringLiteral("Hint"));
                hintLabel->setWordWrap(true);
                hintLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
                contentLayout->addWidget(hintLabel);
            }
        };
        auto makeForm = [](QWidget* host) {
            auto* form = new QFormLayout(host);
            form->setRowWrapPolicy(QFormLayout::WrapLongRows);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
            form->setContentsMargins(6, 2, 6, 6);
            form->setHorizontalSpacing(6);
            form->setVerticalSpacing(3);
            return form;
        };
        // Checkable group = the criterion's enable state; Qt disables the
        // value controls while unchecked, their configured values stay
        // visible and the checkbox text carries the state (not color).
        auto makeCriterion = [&](const QString& title, const QString& objectName, TuneField enableField, const QString& tooltip) {
            auto* box = new QGroupBox(title, tunePanelContent_);
            box->setObjectName(objectName);
            box->setCheckable(true);
            box->setToolTip(tooltip);
            bindTuneField(enableField, box, nullptr,
                          [box]() { return QVariant(box->isChecked()); },
                          [box](const QVariant& v) { box->setChecked(v.toBool()); });
            connect(box, &QGroupBox::toggled, this, [this, enableField](bool on) { onTuneFieldEdited(enableField, on); });
            contentLayout->addWidget(box);
            return box;
        };
        auto addIntField = [&](QFormLayout* form, const QString& rowLabel, TuneField field, const QString& objectName,
                               int min, int max, int step) {
            auto* spin = new QSpinBox();
            spin->setObjectName(objectName);
            spin->setRange(min, max);
            spin->setSingleStep(step);
            spin->setSuffix(tuneFieldUnit(field));
            spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            auto* label = new QLabel(rowLabel);
            label->setBuddy(spin);
            form->addRow(label, spin);
            bindTuneField(field, spin, label,
                          [spin]() { return QVariant(spin->value()); },
                          [spin](const QVariant& v) { spin->setValue(v.toInt()); });
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, field](int v) { onTuneFieldEdited(field, v); });
            return spin;
        };
        auto addDoubleField = [&](QFormLayout* form, const QString& rowLabel, TuneField field, const QString& objectName,
                                  double min, double max, double step) {
            auto* spin = new QDoubleSpinBox();
            spin->setObjectName(objectName);
            spin->setRange(min, max);
            spin->setSingleStep(step);
            spin->setDecimals(tuneFieldDecimals(field));
            spin->setSuffix(tuneFieldUnit(field));
            spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            auto* label = new QLabel(rowLabel);
            label->setBuddy(spin);
            form->addRow(label, spin);
            bindTuneField(field, spin, label,
                          [spin]() { return QVariant(spin->value()); },
                          [spin](const QVariant& v) { spin->setValue(v.toDouble()); });
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, field](double v) { onTuneFieldEdited(field, v); });
            return spin;
        };
        // --- Cell acceptance filters ---------------------------------------
        addHeading(QStringLiteral("acceptanceHeading"), tr("Cell acceptance filters"),
                   tr("An object must pass every enabled criterion to count as a valid cell."));
        {
            auto* box = makeCriterion(tr("Area (µm²)"), QStringLiteral("criterionArea"), TuneField::AreaEnabled,
                                      tr("Accept objects whose projected area lies within this range (µm², after pixel-to-micron conversion)."));
            auto* form = makeForm(box);
            addIntField(form, tr("Minimum"), TuneField::AreaMin, QStringLiteral("areaMinSpin"), 0, 100000, 10);
            addIntField(form, tr("Maximum"), TuneField::AreaMax, QStringLiteral("areaMaxSpin"), 0, 100000, 10);
        }
        {
            auto* box = makeCriterion(tr("Deformability"), QStringLiteral("criterionDeformability"), TuneField::DeformabilityEnabled,
                                      tr("Accept objects whose deformability (1 − circularity, unitless) lies within this range."));
            auto* form = makeForm(box);
            addDoubleField(form, tr("Minimum"), TuneField::DeformabilityMin, QStringLiteral("deformMinSpin"), 0.0, 1.0, 0.01);
            addDoubleField(form, tr("Maximum"), TuneField::DeformabilityMax, QStringLiteral("deformMaxSpin"), 0.0, 1.0, 0.01);
        }
        {
            auto* box = makeCriterion(tr("Ring ratio"), QStringLiteral("criterionRingRatio"), TuneField::RingRatioEnabled,
                                      tr("Accept objects whose ring ratio (focus indicator reported by the processing core, unitless) lies within this range."));
            auto* form = makeForm(box);
            addDoubleField(form, tr("Minimum"), TuneField::RingRatioMin, QStringLiteral("ringMinSpin"), 0.0, 100.0, 0.5);
            addDoubleField(form, tr("Maximum"), TuneField::RingRatioMax, QStringLiteral("ringMaxSpin"), 0.0, 100.0, 0.5);
        }
        {
            auto* box = makeCriterion(tr("Area ratio"), QStringLiteral("criterionAreaRatio"), TuneField::AreaRatioEnabled,
                                      tr("Reject objects whose convex-hull to contour area ratio (unitless) exceeds the maximum."));
            auto* form = makeForm(box);
            addDoubleField(form, tr("Maximum"), TuneField::AreaRatioMax, QStringLiteral("areaRatioMaxSpin"), 0.0, 10.0, 0.1);
        }
        // Enable-only criteria: the checkable title is the whole setting; a
        // wrapping description explains it (a QCheckBox caption cannot wrap
        // and would force the compact width to overflow).
        auto addDescription = [&](QGroupBox* box, const QString& text) {
            auto* layout = new QVBoxLayout(box);
            layout->setContentsMargins(6, 2, 6, 6);
            auto* label = new QLabel(text, box);
            label->setObjectName(box->objectName() + QStringLiteral("Description"));
            label->setWordWrap(true);
            label->setStyleSheet(QStringLiteral("color: palette(mid);"));
            layout->addWidget(label);
        };
        addDescription(makeCriterion(tr("Border exclusion"), QStringLiteral("criterionBorder"), TuneField::BorderEnabled,
                                     tr("Border exclusion: an object whose contour touches the ROI edge is rejected.")),
                       tr("Reject objects touching the ROI edge."));
        addDescription(makeCriterion(tr("Single inner contour"), QStringLiteral("criterionSingleInner"), TuneField::SingleInnerContourEnabled,
                                     tr("Single inner contour: objects with zero or several inner contours are rejected.")),
                       tr("Reject objects with zero or several inner contours."));

        // --- Target group / sorting gate --------------------------------
        addHeading(QStringLiteral("targetHeading"), tr("Target group / sorting gate"),
                   tr("Applied to valid cells only: selects which of them fire the sort trigger. Never changes validity."));
        {
            auto* box = makeCriterion(tr("Target group gate"), QStringLiteral("targetGroupBox"), TuneField::TargetGroupEnabled,
                                      tr("When enabled, a valid cell inside every enabled range is reported as target group and fires the sort trigger."));
            auto* form = makeForm(box);
            addIntField(form, tr("Area minimum"), TuneField::TargetAreaMin, QStringLiteral("targetAreaMinSpin"), 0, 100000, 10);
            addIntField(form, tr("Area maximum"), TuneField::TargetAreaMax, QStringLiteral("targetAreaMaxSpin"), 0, 100000, 10);
            addDoubleField(form, tr("Deformability minimum"), TuneField::TargetDeformabilityMin, QStringLiteral("targetDeformMinSpin"), 0.0, 1.0, 0.01);
            addDoubleField(form, tr("Deformability maximum"), TuneField::TargetDeformabilityMax, QStringLiteral("targetDeformMaxSpin"), 0.0, 1.0, 0.01);
            // Optional third range inside the gate: Young's modulus from the
            // E-modulus LUT lookup on (area, deformability). Nested checkable
            // box so its enable state sits with its values, like the others.
            auto* emodBox = new QGroupBox(tr("Young's modulus"), box);
            emodBox->setObjectName(QStringLiteral("targetEmodulusBox"));
            emodBox->setCheckable(true);
            emodBox->setToolTip(tr("When enabled, a target cell must also have a Young's modulus (kPa, looked up in the loaded E-modulus table) inside this range. "
                                   "A cell whose lookup falls outside the table is never target group. "
                                   "When no table is loaded this range is skipped and the target group is the area and deformability box alone."));
            bindTuneField(TuneField::TargetEmodulusEnabled, emodBox, nullptr,
                          [emodBox]() { return QVariant(emodBox->isChecked()); },
                          [emodBox](const QVariant& v) { emodBox->setChecked(v.toBool()); });
            connect(emodBox, &QGroupBox::toggled, this, [this](bool on) { onTuneFieldEdited(TuneField::TargetEmodulusEnabled, on); });
            form->addRow(emodBox);
            auto* emodForm = makeForm(emodBox);
            // Nested inside the target box: no extra side margins, so the
            // compact (220 px) panel width still fits without scrolling.
            emodForm->setContentsMargins(0, 2, 0, 4);
            addDoubleField(emodForm, tr("Minimum"), TuneField::TargetEmodulusMin, QStringLiteral("targetEmodMinSpin"), 0.0, 1000.0, 0.1);
            addDoubleField(emodForm, tr("Maximum"), TuneField::TargetEmodulusMax, QStringLiteral("targetEmodMaxSpin"), 0.0, 1000.0, 0.1);
        }

        // --- Multi-image acquisition -------------------------------------
        addHeading(QStringLiteral("multiImageHeading"), tr("Multi-image acquisition"), QString());
        {
            auto* box = makeCriterion(tr("Record image series"), QStringLiteral("multiImageBox"), TuneField::MultiImageEnabled,
                                      tr("Capture N consecutive frames per valid detection; metrics come from the first frame."));
            auto* form = makeForm(box);
            addIntField(form, tr("Images per trigger"), TuneField::MultiImageCount, QStringLiteral("multiImageCountSpin"), 1, 32, 1);
        }

        contentLayout->addStretch(1);
        tuneScrollArea_->setWidget(tunePanelContent_);
        outerLayout->addWidget(tuneScrollArea_, 1);

        // --- Fixed footer (outside the scroll area) ------------------------
        auto* line = new QFrame(placeholder);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        outerLayout->addWidget(line);
        tuneFooter_ = new QWidget(placeholder);
        tuneFooter_->setObjectName(QStringLiteral("tuneFooter"));
        auto* footerLayout = new QVBoxLayout(tuneFooter_);
        footerLayout->setContentsMargins(4, 4, 4, 4);
        footerLayout->setSpacing(3);
        tuneStateLabel_ = new QLabel(tuneFooter_);
        tuneStateLabel_->setObjectName(QStringLiteral("tuneStateLabel"));
        tuneStateLabel_->setWordWrap(true);
        tuneStateLabel_->setTextFormat(Qt::PlainText);
        tuneValidationLabel_ = new QLabel(tuneFooter_);
        tuneValidationLabel_->setObjectName(QStringLiteral("tuneValidationLabel"));
        tuneValidationLabel_->setWordWrap(true);
        tuneValidationLabel_->setTextFormat(Qt::PlainText);
        tuneValidationLabel_->setVisible(false);
        auto* buttons = new QHBoxLayout();
        buttons->setSpacing(4);
        tuneApplyBtn_ = new QPushButton(tr("Apply changes"), tuneFooter_);
        tuneApplyBtn_->setObjectName(QStringLiteral("tuneApplyBtn"));
        tuneApplyBtn_->setToolTip(tr("Persist the changed criteria to the active configuration file and apply them to processing."));
        tuneRevertBtn_ = new QPushButton(tr("Revert"), tuneFooter_);
        tuneRevertBtn_->setObjectName(QStringLiteral("tuneRevertBtn"));
        tuneRevertBtn_->setToolTip(tr("Discard unapplied edits and reload the current configuration."));
        buttons->addWidget(tuneApplyBtn_, 1);
        buttons->addWidget(tuneRevertBtn_);
        footerLayout->addWidget(tuneStateLabel_);
        footerLayout->addWidget(tuneValidationLabel_);
        footerLayout->addLayout(buttons);
        outerLayout->addWidget(tuneFooter_, 0);
        connect(tuneApplyBtn_, &QPushButton::clicked, this, &ExperimentMonitoringTab::onApplyParams);
        connect(tuneRevertBtn_, &QPushButton::clicked, this, &ExperimentMonitoringTab::onRevertParams);

        // Load current config values into widgets
        loadCurrentConfig();
    }

    QWidget* ExperimentMonitoringTab::tunePanel() const { return ui ? ui->tuneParamsPlaceholder : nullptr; }

    QString ExperimentMonitoringTab::tuneStateText() const { return tuneStateLabel_ ? tuneStateLabel_->text() : QString(); }

    QWidget* ExperimentMonitoringTab::tuneFieldWidget(TuneField field) const
    {
        for (const auto& b : tuneBindings_)
            if (b.field == field) return b.widget;
        return nullptr;
    }

    bool ExperimentMonitoringTab::setTuneFieldForTests(TuneField field, const QVariant& value)
    {
        for (const auto& b : tuneBindings_) {
            if (b.field != field) continue;
            b.write(value);
            return true;
        }
        return false;
    }

    void ExperimentMonitoringTab::populateTuneWidgetsFromDraft()
    {
        tuneLoading_ = true;
        for (const auto& b : tuneBindings_) {
            QSignalBlocker blocker(b.widget);
            b.write(draft_.field(b.field));
        }
        tuneLoading_ = false;
        refreshTuneChangeMarkers();
    }

    void ExperimentMonitoringTab::refreshTuneChangeMarkers()
    {
        for (const auto& b : tuneBindings_) {
            const bool changed = draft_.isChanged(b.field);
            b.widget->setProperty("changed", changed);
            if (changed) {
                const QVariant was = draft_.baselineField(b.field);
                b.widget->setAccessibleDescription(tr("Unapplied change (applied value: %1)").arg(was.toString()));
                b.widget->setToolTip(tr("%1 — unapplied change (applied value: %2)").arg(tuneFieldLabel(b.field), was.toString()));
            } else {
                b.widget->setAccessibleDescription(QString());
                b.widget->setToolTip(tuneFieldLabel(b.field));
            }
            if (b.rowLabel) {
                auto* label = qobject_cast<QLabel*>(b.rowLabel);
                if (label) {
                    QString text = label->text();
                    const bool marked = text.endsWith(QStringLiteral(" *"));
                    if (changed && !marked) label->setText(text + QStringLiteral(" *"));
                    else if (!changed && marked) label->setText(text.chopped(2));
                }
            }
        }
    }

    void ExperimentMonitoringTab::refreshTuneFooter()
    {
        if (!tuneStateLabel_) return;
        tuneStateLabel_->setText(draft_.stateText());
        tuneStateLabel_->setAccessibleName(tr("Tune panel state: %1").arg(draft_.stateText()));
        const QStringList issues = draft_.dirty() ? draft_.validationIssues() : QStringList();
        tuneValidationLabel_->setText(issues.join(QLatin1Char('\n')));
        tuneValidationLabel_->setVisible(!issues.isEmpty());
        const bool canApply = draft_.dirty() && draft_.valid() && !draft_.conflict() && !draft_.applying();
        tuneApplyBtn_->setEnabled(canApply);
        tuneRevertBtn_->setEnabled((draft_.dirty() || draft_.conflict()) && !draft_.applying());
        // Enabled/disabled criteria must not be inferred from editability
        // while applying: controls are read-only for the duration.
        if (tunePanelContent_) tunePanelContent_->setEnabled(!draft_.applying());
        // Every draft change ends here (edit, load, revert, apply result): the
        // scatter's gate overlay and preview count follow the draft.
        refreshGateOverlay();
    }

    void ExperimentMonitoringTab::onTuneFieldEdited(TuneField field, const QVariant& value)
    {
        if (tuneLoading_) return;
        draft_.setField(field, value);
        refreshTuneChangeMarkers();
        refreshTuneFooter();
        emit tuneStateChanged();
    }

    void ExperimentMonitoringTab::loadCurrentConfig() { loadCurrentConfig(QByteArray()); }

    void ExperimentMonitoringTab::loadCurrentConfig(const QByteArray& documentFingerprint)
    {
        const auto cfg = backend_.processing().getProcessingConfig();
        if (!documentFingerprint.isEmpty()) documentFingerprint_ = documentFingerprint;
        const auto outcome = draft_.noteExternalBaseline(cfg);
        switch (outcome) {
        case ProcessingConfigDraft::ExternalOutcome::Refreshed:
        case ProcessingConfigDraft::ExternalOutcome::Unchanged:
            populateTuneWidgetsFromDraft();
            // Sync histogram axis defaults from ring ratio config and refresh the chart defaults.
            setHistogramXRange(cfg.ring_ratio_min, cfg.ring_ratio_max);
            break;
        case ProcessingConfigDraft::ExternalOutcome::Conflict:
            // Local edits retained; the footer says so. The chart range still
            // follows the authoritative config.
            setHistogramXRange(cfg.ring_ratio_min, cfg.ring_ratio_max);
            SPDLOG_WARN("Tune panel: external configuration change while {} edit(s) are unapplied; keeping the draft (conflict)", draft_.changeCount());
            break;
        case ProcessingConfigDraft::ExternalOutcome::Deferred:
            break;
        }
        refreshTuneFooter();
        emit tuneStateChanged();
    }

    void ExperimentMonitoringTab::onRevertParams()
    {
        draft_.revert();
        populateTuneWidgetsFromDraft();
        const auto& cfg = draft_.baseline();
        setHistogramXRange(cfg.ring_ratio_min, cfg.ring_ratio_max);
        refreshTuneFooter();
        emit tuneStateChanged();
    }

    void ExperimentMonitoringTab::onApplyParams()
    {
        const uint64_t requestId = draft_.beginApply();
        if (requestId == 0) {
            refreshTuneFooter();
            return;
        }
        ApplyProcessingDraftRequest request;
        request.requestId = requestId;
        request.baselineFingerprint = documentFingerprint_;
        request.patch = draft_.patch();
        refreshTuneFooter();
        static const QMetaMethod applySignal = QMetaMethod::fromSignal(&ExperimentMonitoringTab::applyRequested);
        if (!isSignalConnected(applySignal)) {
            draft_.failApply(tr("no configuration coordinator is connected"));
            refreshTuneFooter();
            emit tuneStateChanged();
            return;
        }
        QStringList fields;
        for (const auto& [field, value] : request.patch.values)
            fields << QStringLiteral("%1=%2").arg(QLatin1String(toString(field)), value.toString());
        SPDLOG_INFO("Tune panel: apply request {} ({})", requestId, fields.join(QStringLiteral(", ")).toStdString());
        emit tuneStateChanged();
        emit applyRequested(request);
    }

    void ExperimentMonitoringTab::onApplyResult(const frontend::ConfigApplyResult& result)
    {
        if (!draft_.completeApply(result)) {
            SPDLOG_WARN("Tune panel: ignoring apply result for request {} (active {})", result.requestId, draft_.activeRequest());
            return;
        }
        if (result.ok()) {
            populateTuneWidgetsFromDraft();
            setHistogramXRange(result.effectiveConfig.ring_ratio_min, result.effectiveConfig.ring_ratio_max);
            SPDLOG_INFO("Tune panel: request {} persisted and applied", result.requestId);
        } else {
            refreshTuneChangeMarkers();
            SPDLOG_WARN("Tune panel: request {} not applied (persisted={}, applied={}, conflict={}): {}",
                        result.requestId, result.persisted, result.applied, result.conflict, result.error.toStdString());
        }
        refreshTuneFooter();
        emit tuneStateChanged();
    }

    void ExperimentMonitoringTab::setupCharts() {
        // Panel 1: Scatterplot (top-left)
        scatterplotChart_ = new QChart();
        scatterSeries_ = new QScatterSeries();
        scatterSeries_->setMarkerSize(6.0);
        scatterSeries_->setName("Valid Frames");
        scatterSeries_->setColor(QColor(0, 200, 0));
        scatterplotChart_->addSeries(scatterSeries_);

        targetGroupSeries_ = new QScatterSeries();
        targetGroupSeries_->setMarkerSize(6.0);
        targetGroupSeries_->setName("Target Group");
        targetGroupSeries_->setColor(QColor(0, 120, 255));
        scatterplotChart_->addSeries(targetGroupSeries_);
        scatterplotChart_->setTitle("Deformability vs Area (μm²)");
        scatterplotChart_->legend()->setVisible(false);

        scatterXAxis_ = new QValueAxis();
        scatterXAxis_->setTitleText("Area (μm²)");
        scatterYAxis_ = new QValueAxis();
        scatterYAxis_->setTitleText("Deformability");
        scatterplotChart_->addAxis(scatterXAxis_, Qt::AlignBottom);
        scatterplotChart_->addAxis(scatterYAxis_, Qt::AlignLeft);
        scatterSeries_->attachAxis(scatterXAxis_);
        scatterSeries_->attachAxis(scatterYAxis_);
        targetGroupSeries_->attachAxis(scatterXAxis_);
        targetGroupSeries_->attachAxis(scatterYAxis_);
        setupKdeLevelSeries();
        scatterXAxis_->setRange(scatterXMin_, scatterXMax_);
        scatterYAxis_->setRange(scatterYMin_, scatterYMax_);

        scatterplotView_ = new ZoomableChartView(scatterplotChart_);
        scatterplotView_->setRenderHint(QPainter::Antialiasing);
        scatterplotView_->setDefaultRange(scatterXAxis_, scatterXMin_, scatterXMax_);
        scatterplotView_->setDefaultRange(scatterYAxis_, scatterYMin_, scatterYMax_);
        // Replace placeholder with the chart view and, under it, the gate preview
        // count (hidden while the draft gate is off).
        ui->gridLayout->removeWidget(ui->scatterplotViewPlaceholder);
        delete ui->scatterplotViewPlaceholder;
        scatterContainer_ = new QWidget(this);
        scatterContainer_->setObjectName(QStringLiteral("monitoringScatterContainer"));
        auto* scatterLayout = new QVBoxLayout(scatterContainer_);
        scatterLayout->setContentsMargins(0, 0, 0, 0);
        scatterLayout->setSpacing(2);
        scatterLayout->addWidget(scatterplotView_, 1);
        gatePreviewLabel_ = new QLabel(scatterContainer_);
        gatePreviewLabel_->setObjectName(QStringLiteral("gatePreviewLabel"));
        gatePreviewLabel_->setTextFormat(Qt::PlainText);
        gatePreviewLabel_->setWordWrap(true);
        gatePreviewLabel_->setAccessibleName(tr("Target group preview count"));
        gatePreviewLabel_->setVisible(false);
        scatterLayout->addWidget(gatePreviewLabel_, 0);
        ui->gridLayout->addWidget(scatterContainer_, 1, 0);

        // Load isoelastic curves overlay
        loadIsoelasticCurves();

        // Selected cell: one point drawn after the data series (Review tab look).
        scatterHighlight_ = new QScatterSeries();
        scatterHighlight_->setObjectName(QStringLiteral("monitoringScatterHighlight"));
        scatterHighlight_->setName(tr("Selected cell"));
        scatterHighlight_->setMarkerSize(13.0);
        scatterHighlight_->setColor(QColor(0xf2, 0x8e, 0x2b));
        scatterHighlight_->setBorderColor(Qt::white);
        scatterplotChart_->addSeries(scatterHighlight_);
        scatterHighlight_->attachAxis(scatterXAxis_);
        scatterHighlight_->attachAxis(scatterYAxis_);
        scatterHighlight_->setVisible(false);
        for (auto* marker : scatterplotChart_->legend()->markers(scatterHighlight_)) marker->setVisible(false);

        // Click a point to see the cell; double-click resets the zoom only on
        // empty space, so a quick second click on a point never zooms out.
        scatterplotView_->setResetOnDoubleClick(false);
        connect(scatterplotView_, &ZoomableChartView::plotClicked, this, &ExperimentMonitoringTab::onScatterClicked);
        connect(scatterplotView_, &ZoomableChartView::plotDoubleClicked, this,
                &ExperimentMonitoringTab::onScatterDoubleClicked);
        connect(scatterplotView_, &ZoomableChartView::hoverMoved, this, &ExperimentMonitoringTab::onScatterHover);
        setupGateOverlay();

        // Panel 2: Histogram (top-right)
        histogramChart_ = new QChart();
        histogramChart_->setTitle("Ring Width Distribution");
        histogramChart_->legend()->setVisible(false);

        histogramYAxis_ = new QValueAxis();
        histogramYAxis_->setTitleText("Frequency");
        histogramChart_->addAxis(histogramYAxis_, Qt::AlignLeft);

        histogramXAxis_ = new QValueAxis();
        histogramXAxis_->setLabelsAngle(-90);
        histogramXAxis_->setLabelFormat("%.2f");
        histogramChart_->addAxis(histogramXAxis_, Qt::AlignBottom);

#if MIB_HAS_QHISTOGRAMSERIES
        histogramSeries_ = new QHistogramSeries();
        histogramSeries_->setName("Ring Width");
        histogramChart_->addSeries(histogramSeries_);
        histogramSeries_->attachAxis(histogramXAxis_);
        histogramSeries_->attachAxis(histogramYAxis_);
#else
        barSeries_ = new QBarSeries();
        histogramChart_->addSeries(barSeries_);
        // Fallback: use category axis for X when bar series is used
        histogramCategoryAxis_ = new QBarCategoryAxis();
        histogramCategoryAxis_->setLabelsAngle(-90);
        // Remove previously added numeric X axis to avoid overlap
        if (histogramXAxis_)
        {
            histogramChart_->removeAxis(histogramXAxis_);
            delete histogramXAxis_;
            histogramXAxis_ = nullptr;
        }
        histogramChart_->addAxis(histogramCategoryAxis_, Qt::AlignBottom);
        barSeries_->attachAxis(histogramCategoryAxis_);
        barSeries_->attachAxis(histogramYAxis_);
#endif

        if (histogramXAxis_)
            histogramXAxis_->setRange(histogramXMin_, histogramXMax_);
        histogramYAxis_->setRange(0, std::max(1.0, histogramYMax_));

        histogramView_ = new ZoomableChartView(histogramChart_);
        histogramView_->setRenderHint(QPainter::Antialiasing);
        if (histogramXAxis_)
            histogramView_->setDefaultRange(histogramXAxis_, histogramXMin_, histogramXMax_);
        histogramView_->setDefaultRange(histogramYAxis_, 0, std::max(1.0, histogramYMax_));
        // Replace placeholder with the frame pane over the histogram
        ui->gridLayout->removeWidget(ui->histogramViewPlaceholder);
        delete ui->histogramViewPlaceholder;
        setupFramePane();
    }

    void ExperimentMonitoringTab::onUpdate()
    {
        // Get frames from processing service (use monitoring frames which work without experiment)
        auto validFrames = backend_.processing().getMonitoringValidFrames();
        auto invalidFrames = backend_.processing().getMonitoringInvalidFrames();

        // Merge new frames into rolling buffers (maintain recent history)
        // Track seen frame indices to avoid duplicates
        std::set<uint64_t> seenValidIndices;
        std::set<uint64_t> seenInvalidIndices;
        for (const auto &frame : recentValidFrames_)
        {
            seenValidIndices.insert(frame.index);
        }
        for (const auto &frame : recentInvalidFrames_)
        {
            seenInvalidIndices.insert(frame.index);
        }

        // Add new frames that we haven't seen
        for (const auto &frame : validFrames)
        {
            if (seenValidIndices.find(frame.index) == seenValidIndices.end())
            {
                recentValidFrames_.push_back(frame);
                if (frame.index > lastValidFrameIndex_)
                {
                    lastValidFrameIndex_ = frame.index;
                }
            }
        }
        for (const auto &frame : invalidFrames)
        {
            if (seenInvalidIndices.find(frame.index) == seenInvalidIndices.end())
            {
                recentInvalidFrames_.push_back(frame);
                if (frame.index > lastInvalidFrameIndex_)
                {
                    lastInvalidFrameIndex_ = frame.index;
                }
            }
        }

        // Trim to max size, keeping most recent frames
        if (recentValidFrames_.size() > MAX_RECENT_FRAMES)
        {
            size_t excess = recentValidFrames_.size() - MAX_RECENT_FRAMES;
            recentValidFrames_.erase(recentValidFrames_.begin(), recentValidFrames_.begin() + excess);
        }
        if (recentInvalidFrames_.size() > MAX_RECENT_FRAMES)
        {
            size_t excess = recentInvalidFrames_.size() - MAX_RECENT_FRAMES;
            recentInvalidFrames_.erase(recentInvalidFrames_.begin(), recentInvalidFrames_.begin() + excess);
        }

        // Update all panels using rolling buffers
        updateScatterplot(recentValidFrames_);
        updateHistogram(recentValidFrames_);
        updateValidFramesGrid(recentValidFrames_);
        updateInvalidFramesGrid(recentInvalidFrames_);
    }

    void ExperimentMonitoringTab::showEvent(QShowEvent *event)
    {
        QWidget::showEvent(event);
        if (updateTimer_ && !updateTimer_->isActive())
        {
            updateTimer_->start();
        }
        // Enable monitoring accumulation only while this tab is visible —
        // ProcessingService skips the per-object frame copies when inactive.
        backend_.processing().setMonitoringActive(true);
        if (kdeEnabled_ && kdeTimer_ && !kdeTimer_->isActive())
        {
            kdeTimer_->start();
        }
        pushKdeSettings(true); // the backend estimates only while someone looks
        // Refresh tune panel with current config when tab becomes visible
        loadCurrentConfig();
    }

    void ExperimentMonitoringTab::hideEvent(QHideEvent *event)
    {
        QWidget::hideEvent(event);
        if (updateTimer_ && updateTimer_->isActive())
        {
            updateTimer_->stop();
        }
        if (kdeTimer_ && kdeTimer_->isActive())
        {
            kdeTimer_->stop();
        }
        pushKdeSettings(false);
        // Stop monitoring accumulation while hidden (no consumer polling)
        backend_.processing().setMonitoringActive(false);
        // Disarm periodic test trigger on hide to avoid background pulsing
        if (ui->periodicTriggerBtn->isChecked())
        {
            ui->periodicTriggerBtn->setChecked(false);
        }
    }


    void ExperimentMonitoringTab::updateScatterplot(const std::vector<backend::services::ProcessedFrame> &validFrames)
    {
        scatterSeries_->clear();
        targetGroupSeries_->clear();
        for (auto* s : kdeLevelSeries_) s->clear();
        for (auto* s : kdeTargetLevelSeries_) s->clear();

        // Batch the points: one append per series instead of one signal per
        // point. With KDE on, each point goes to the series of its density
        // level (unknown density = sparsest level until the next estimate).
        scatterPoints_.clear();
        scatterPointFrames_.clear();
        scatterPointE_.clear();
        scatterPoints_.reserve(validFrames.size());
        scatterPointFrames_.reserve(validFrames.size());
        scatterPointE_.reserve(validFrames.size());
        QList<QPointF> scatterPoints;
        QList<QPointF> targetPoints;
        std::vector<QList<QPointF>> levelPoints(kdeEnabled_ ? kKdeLevels : 0);
        std::vector<QList<QPointF>> targetLevelPoints(kdeEnabled_ ? kKdeLevels : 0);
        scatterPoints.reserve(static_cast<qsizetype>(validFrames.size()));
        for (const auto &frame : validFrames)
        {
            if (!frame.validation.isValid) continue;
            // Each result's own calibration (the factor it was analysed with);
            // results with an unknown historical calibration are not plotted.
            const double factor = frame.validation.analysisPixelToMicronFactor;
            if (!std::isfinite(factor) || factor <= 0) continue;
            const QPointF point(frame.validation.area * factor * factor, frame.validation.deformability);
            scatterPoints_.push_back({point.x(), point.y(), static_cast<int>(scatterPoints_.size())});
            scatterPointFrames_.push_back(frame.index);
            scatterPointE_.push_back(frame.validation.youngsModulus);
            if (kdeEnabled_)
            {
                double density = 0.0;
                kdeDensityForFrame(frame.index, density);
                const int level = kdeLevelForDensity(density);
                (frame.validation.isTargetGroup ? targetLevelPoints : levelPoints)[static_cast<std::size_t>(level)].append(point);
            }
            else if (frame.validation.isTargetGroup) {
                targetPoints.append(point);
            } else {
                scatterPoints.append(point);
            }
        }
        scatterSeries_->append(scatterPoints);
        targetGroupSeries_->append(targetPoints);
        for (std::size_t level = 0; level < levelPoints.size(); ++level)
        {
            kdeLevelSeries_[level]->append(levelPoints[level]);
            kdeTargetLevelSeries_[level]->append(targetLevelPoints[level]);
        }

        if (!scatterplotView_->isUserZoomed()) {
            scatterXAxis_->setRange(scatterXMin_, scatterXMax_);
            scatterYAxis_->setRange(scatterYMin_, scatterYMax_);
        }
        refreshScatterHighlight();
        refreshGatePreview();
    }

    void ExperimentMonitoringTab::updateHistogram(const std::vector<backend::services::ProcessedFrame> &validFrames)
    {
#if MIB_HAS_QHISTOGRAMSERIES
        if (histogramSeries_)
            histogramSeries_->clear();
#else
        if (barSeries_)
            barSeries_->clear();
#endif

        const double minVal = histogramXMin_;
        const double maxVal = histogramXMax_;
        const double binWidth = histogramBinWidth_;
        const int histogramBins = std::max(1, static_cast<int>(std::round((maxVal - minVal) / binWidth)));

        if (!histogramView_->isUserZoomed()) {
            if (histogramXAxis_)
            {
                histogramXAxis_->setRange(minVal, maxVal);
                histogramXAxis_->setTickCount(6);
            }
        }

        std::vector<double> ringRatios;
        if (!validFrames.empty())
        {
            for (const auto &frame : validFrames)
            {
                if (frame.validation.isValid && frame.validation.ringRatio > 0.0)
                {
                    ringRatios.push_back(frame.validation.ringRatio);
                }
            }
        }

        if (!histogramView_->isUserZoomed()) {
            const double yMax = std::max(1.0, histogramYMax_);
            histogramYAxis_->setRange(0, yMax);
        }

        if (ringRatios.empty())
        {
#if !MIB_HAS_QHISTOGRAMSERIES
            if (histogramCategoryAxis_)
            {
                histogramChart_->removeAxis(histogramCategoryAxis_);
                delete histogramCategoryAxis_;
                histogramCategoryAxis_ = nullptr;
            }
            histogramCategoryAxis_ = new QBarCategoryAxis();
            QStringList categories;
            categories.reserve(histogramBins);
            for (int i = 0; i < histogramBins; ++i)
            {
                const double start = minVal + i * binWidth;
                const double end = (i == histogramBins - 1) ? maxVal : (start + binWidth);
                categories << QString("%1-%2").arg(start, 0, 'f', 1).arg(end, 0, 'f', 1);
            }
            histogramCategoryAxis_->append(categories);
            histogramCategoryAxis_->setLabelsAngle(-90);
            histogramChart_->addAxis(histogramCategoryAxis_, Qt::AlignBottom);
            barSeries_->attachAxis(histogramCategoryAxis_);
#endif
            return;
        }

        std::vector<int> binCounts(static_cast<size_t>(histogramBins), 0);
        for (double val : ringRatios)
        {
            double clampedVal = std::clamp(val, minVal, maxVal);
            int binIndex = static_cast<int>((clampedVal - minVal) / binWidth);
            if (binIndex >= histogramBins)
                binIndex = histogramBins - 1;
            binIndex = std::clamp(binIndex, 0, histogramBins - 1);
            binCounts[static_cast<size_t>(binIndex)]++;
        }

#if MIB_HAS_QHISTOGRAMSERIES
        if (histogramSeries_)
        {
            QVector<qreal> samples;
            samples.reserve(static_cast<int>(ringRatios.size()));
            for (double v : ringRatios)
            {
                samples.append(static_cast<qreal>(v));
            }
            histogramSeries_->setBinsCount(histogramBins);
            histogramSeries_->setSamples(samples);
        }
#else
        auto *barSet = new QBarSet("");
        for (int count : binCounts)
        {
            *barSet << count;
        }
        barSeries_->append(barSet);
        if (histogramCategoryAxis_)
        {
            histogramChart_->removeAxis(histogramCategoryAxis_);
            delete histogramCategoryAxis_;
            histogramCategoryAxis_ = nullptr;
        }
        histogramCategoryAxis_ = new QBarCategoryAxis();
        {
            QStringList categories;
            categories.reserve(histogramBins);
            for (int i = 0; i < histogramBins; ++i)
            {
                const double start = minVal + i * binWidth;
                const double end = (i == histogramBins - 1) ? maxVal : (start + binWidth);
                categories << QString("%1-%2").arg(start, 0, 'f', 1).arg(end, 0, 'f', 1);
            }
            histogramCategoryAxis_->append(categories);
        }
        histogramCategoryAxis_->setLabelsAngle(-90);
        histogramChart_->addAxis(histogramCategoryAxis_, Qt::AlignBottom);
        barSeries_->attachAxis(histogramCategoryAxis_);
#endif
    }

    void ExperimentMonitoringTab::updateValidFramesGrid(const std::vector<backend::services::ProcessedFrame> &validFrames)
    {
        clearGrid(ui->validFramesGrid);

        // Get ROI
        auto roi = backend_.processing().getRealtimeRoi();
        if (roi.w <= 0 || roi.h <= 0)
        {
            return;
        }

        // Get last MAX_FRAMES_TO_SHOW valid frames
        size_t startIdx = validFrames.size() > MAX_FRAMES_TO_SHOW
                              ? validFrames.size() - MAX_FRAMES_TO_SHOW
                              : 0;

        int row = 0;
        int col = 0;
        for (size_t i = startIdx; i < validFrames.size(); ++i)
        {
            const auto &frame = validFrames[i];
            QImage roiImage;

            if (showValidOverlay_ && !frame.processedImage.empty())
            {
                // If monitoring stores ROI-only images, use them directly. Otherwise, crop.
                // Clamped crop: the frame may predate the current ROI (crash review 2026-09-08).
                cv::Mat roiOriginal = frontend::monitoring::cropToRoi(frame.originalImage, roi.x, roi.y, roi.w, roi.h);
                cv::Mat roiMask = frontend::monitoring::cropToRoi(frame.processedImage, roi.x, roi.y, roi.w, roi.h);
                roiImage = createOverlayImage(roiOriginal, roiMask, &frame.validation);
            }
            else
            {
                // If already ROI, convert full; else extract
                if (frame.originalImage.cols == roi.w && frame.originalImage.rows == roi.h)
                {
                    roiImage = matToQImage(frame.originalImage);
                }
                else
                {
                    roiImage = extractRoiImage(frame.originalImage, roi.x, roi.y, roi.w, roi.h);
                }
            }

            if (!roiImage.isNull())
            {
                QPixmap pixmap = QPixmap::fromImage(roiImage.scaled(
                    THUMBNAIL_SIZE, THUMBNAIL_SIZE,
                    Qt::KeepAspectRatio, Qt::SmoothTransformation));

                QLabel *label = new QLabel(ui->validFramesWidget);
                label->setPixmap(pixmap);
                label->setAlignment(Qt::AlignCenter);
                label->setFrameStyle(QFrame::Box);
                label->setLineWidth(1);
                label->setStyleSheet("QLabel { border: 1px solid gray; }");
                label->setMinimumSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE);
                label->setMaximumSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE);
                label->setScaledContents(false);

                ui->validFramesGrid->addWidget(label, row, col);

                col++;
                if (col >= GRID_COLUMNS)
                {
                    col = 0;
                    row++;
                }
            }
        }
    }

    void ExperimentMonitoringTab::updateInvalidFramesGrid(const std::vector<backend::services::ProcessedFrame> &invalidFrames)
    {
        clearGrid(ui->invalidFramesGrid);

        // Get ROI
        auto roi = backend_.processing().getRealtimeRoi();
        if (roi.w <= 0 || roi.h <= 0)
        {
            return;
        }

        // Fetch config once for reason derivation
        auto config = backend_.processing().getProcessingConfig();

        // Get last MAX_FRAMES_TO_SHOW invalid frames
        size_t startIdx = invalidFrames.size() > MAX_FRAMES_TO_SHOW
                              ? invalidFrames.size() - MAX_FRAMES_TO_SHOW
                              : 0;

        int row = 0;
        int col = 0;
        for (size_t i = startIdx; i < invalidFrames.size(); ++i)
        {
            const auto &frame = invalidFrames[i];
            QImage roiImage;

            if (showInvalidOverlay_ && !frame.processedImage.empty())
            {
                // Clamped crop: the frame may predate the current ROI (crash review 2026-09-08).
                cv::Mat roiOriginal = frontend::monitoring::cropToRoi(frame.originalImage, roi.x, roi.y, roi.w, roi.h);
                cv::Mat roiMask = frontend::monitoring::cropToRoi(frame.processedImage, roi.x, roi.y, roi.w, roi.h);
                roiImage = createOverlayImage(roiOriginal, roiMask, &frame.validation);
            }
            else
            {
                if (frame.originalImage.cols == roi.w && frame.originalImage.rows == roi.h)
                {
                    roiImage = matToQImage(frame.originalImage);
                }
                else
                {
                    roiImage = extractRoiImage(frame.originalImage, roi.x, roi.y, roi.w, roi.h);
                }
            }

            if (!roiImage.isNull())
            {
                QPixmap pixmap = QPixmap::fromImage(roiImage.scaled(
                    THUMBNAIL_SIZE, THUMBNAIL_SIZE,
                    Qt::KeepAspectRatio, Qt::SmoothTransformation));

                // Derive rejection reasons
                auto reasons = getInvalidReasons(frame.validation, config,
                    backend_.processing().getPixelToMicronFactor());

                // Container widget: image on top, reason text below
                QWidget *container = new QWidget(ui->invalidFramesWidget);
                QVBoxLayout *vbox = new QVBoxLayout(container);
                vbox->setContentsMargins(0, 0, 0, 0);
                vbox->setSpacing(2);

                QLabel *imageLabel = new QLabel(container);
                imageLabel->setPixmap(pixmap);
                imageLabel->setAlignment(Qt::AlignCenter);
                imageLabel->setFrameStyle(QFrame::Box);
                imageLabel->setLineWidth(1);
                imageLabel->setStyleSheet("QLabel { border: 1px solid gray; }");
                imageLabel->setFixedSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE);
                imageLabel->setScaledContents(false);
                vbox->addWidget(imageLabel, 0, Qt::AlignCenter);

                if (!reasons.empty())
                {
                    QStringList shortReasons;
                    QStringList tooltipLines;
                    for (const auto &r : reasons)
                    {
                        shortReasons << r.shortText;
                        tooltipLines << r.longText;
                    }

                    QLabel *reasonLabel = new QLabel(shortReasons.join(" | "), container);
                    reasonLabel->setAlignment(Qt::AlignCenter);
                    reasonLabel->setWordWrap(true);
                    reasonLabel->setStyleSheet("QLabel { font-size: 9px; color: #cc0000; }");
                    reasonLabel->setFixedWidth(THUMBNAIL_SIZE);
                    vbox->addWidget(reasonLabel, 0, Qt::AlignCenter);

                    container->setToolTip(tooltipLines.join("\n"));
                }

                ui->invalidFramesGrid->addWidget(container, row, col);

                col++;
                if (col >= GRID_COLUMNS)
                {
                    col = 0;
                    row++;
                }
            }
        }
    }

    QImage ExperimentMonitoringTab::extractRoiImage(const cv::Mat &image, int x, int y, int w, int h) const
    {
        if (image.empty())
        {
            return QImage();
        }

        // Ensure ROI is within image bounds
        int imgWidth = image.cols;
        int imgHeight = image.rows;
        int roiX = std::max(0, std::min(x, imgWidth - 1));
        int roiY = std::max(0, std::min(y, imgHeight - 1));
        int roiW = std::min(w, imgWidth - roiX);
        int roiH = std::min(h, imgHeight - roiY);

        if (roiW <= 0 || roiH <= 0)
        {
            return QImage();
        }

        cv::Rect roiRect(roiX, roiY, roiW, roiH);
        cv::Mat roiMat = image(roiRect);

        return matToQImage(roiMat);
    }

    QImage ExperimentMonitoringTab::matToQImage(const cv::Mat &mat) const
    {
        if (mat.empty())
        {
            return QImage();
        }

        if (mat.type() == CV_8UC1)
        {
            // Grayscale
            QImage img(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step), QImage::Format_Grayscale8);
            return img.copy();
        }
        else if (mat.type() == CV_8UC3)
        {
            // BGR to RGB
            cv::Mat rgb;
            cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
            QImage img(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888);
            return img.copy();
        }
        else if (mat.type() == CV_8UC4)
        {
            // BGRA to RGBA
            cv::Mat rgba;
            cv::cvtColor(mat, rgba, cv::COLOR_BGRA2RGBA);
            QImage img(rgba.data, rgba.cols, rgba.rows, static_cast<int>(rgba.step), QImage::Format_RGBA8888);
            return img.copy();
        }

        // Fallback: convert to grayscale
        cv::Mat gray;
        cv::cvtColor(mat, gray, cv::COLOR_BGR2GRAY);
        QImage img(gray.data, gray.cols, gray.rows, static_cast<int>(gray.step), QImage::Format_Grayscale8);
        return img.copy();
    }

    void ExperimentMonitoringTab::clearGrid(QGridLayout *grid)
    {
        // Remove all widgets from the grid
        QLayoutItem *item;
        while ((item = grid->takeAt(0)) != nullptr)
        {
            if (item->widget())
            {
                item->widget()->deleteLater();
            }
            delete item;
        }
    }

    void ExperimentMonitoringTab::onToggleOverlay(bool enabled)
    {
        QCheckBox *sender = qobject_cast<QCheckBox *>(this->sender());
        if (sender == ui->validOverlayCheck)
        {
            showValidOverlay_ = enabled;
            // Trigger update to refresh grid with overlay
            updateValidFramesGrid(recentValidFrames_);
            if (framePane_) framePane_->setOverlayMode(paneOverlayMode());
        }
        else if (sender == ui->invalidOverlayCheck)
        {
            showInvalidOverlay_ = enabled;
            // Trigger update to refresh grid with overlay
            updateInvalidFramesGrid(recentInvalidFrames_);
        }
    }

    void ExperimentMonitoringTab::onClearBuffer()
    {
        int ret = QMessageBox::question(this, tr("Clear Buffer"),
                                        tr("Are you sure you want to clear the monitoring buffer? This will remove all accumulated frames."),
                                        QMessageBox::Yes | QMessageBox::No,
                                        QMessageBox::No);
        if (ret == QMessageBox::Yes)
        {
            backend_.processing().clearMonitoringFrames();
            recentValidFrames_.clear();
            recentInvalidFrames_.clear();
            lastValidFrameIndex_ = 0;
            lastInvalidFrameIndex_ = 0;
            kdeDensityByIndex_.clear();
            clearSelection();
            requestKdeUpdate(); // the service drops its result for the empty ring
            // Update displays
            updateScatterplot(recentValidFrames_);
            updateHistogram(recentValidFrames_);
            updateValidFramesGrid(recentValidFrames_);
            updateInvalidFramesGrid(recentInvalidFrames_);
            SPDLOG_INFO("Monitoring buffer cleared");
        }
    }

    void ExperimentMonitoringTab::onSortTrigger()
    {
        backend::services::TargetGroupSignal signal;
        signal.isTargetGroup = true;
        backend_.trigger().onTargetGroupResult(signal);
        SPDLOG_INFO("Manual sort trigger fired");
    }

    void ExperimentMonitoringTab::onPeriodicTriggerToggled(bool checked)
    {
        if (!periodicTriggerTimer_) return;
        if (checked)
        {
            const int intervalMs = ui->periodicTriggerIntervalSpin->value();
            periodicTriggerPulseCount_ = 0;
            periodicTriggerTimer_->setInterval(intervalMs);
            periodicTriggerTimer_->start();
            ui->periodicTriggerIntervalSpin->setEnabled(false);
            SPDLOG_INFO("Periodic sort trigger started (interval={} ms)", intervalMs);
        }
        else
        {
            periodicTriggerTimer_->stop();
            ui->periodicTriggerIntervalSpin->setEnabled(true);
            SPDLOG_INFO("Periodic sort trigger stopped (pulses fired={})", periodicTriggerPulseCount_);
        }
    }

    QImage ExperimentMonitoringTab::createOverlayImage(const cv::Mat &original, const cv::Mat &mask,
                                                       const backend::services::FilterResult *validation) const
    {
        if (original.empty() || mask.empty())
        {
            return QImage();
        }

        // Classification color: blue=target, green=valid, red=invalid
        cv::Vec3b tint(0, 255, 0); // default green
        if (validation) {
            if (validation->isTargetGroup)      tint = {0, 120, 255};  // Blue
            else if (validation->isValid)       tint = {0, 255, 0};    // Green
            else                                tint = {255, 0, 0};    // Red
        }

        // Convert original to RGB if needed
        cv::Mat rgb;
        if (original.channels() == 1)
        {
            cv::cvtColor(original, rgb, cv::COLOR_GRAY2RGB);
        }
        else
        {
            rgb = original.clone();
            if (rgb.channels() == 3)
            {
                cv::cvtColor(rgb, rgb, cv::COLOR_BGR2RGB);
            }
        }

        // Extract contours with hierarchy to isolate nested (inner) contours.
        // Inner contours (used for metrics) have a parent in the hierarchy.
        std::vector<std::vector<cv::Point>> contours;
        std::vector<cv::Vec4i> hierarchy;
        cv::findContours(mask.clone(), contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_SIMPLE);

        // Build a mask containing only the inner (nested) contour regions
        cv::Mat innerMask = cv::Mat::zeros(mask.rows, mask.cols, CV_8UC1);
        bool hasInner = false;
        for (int i = 0; i < static_cast<int>(contours.size()); ++i) {
            if (hierarchy[i][3] >= 0) { // has parent → inner contour
                cv::drawContours(innerMask, contours, i, cv::Scalar(255), -1);
                hasInner = true;
            }
        }
        // Fallback: if no nested contour found, use the full mask
        const cv::Mat &tintMask = hasInner ? innerMask : mask;

        // Create overlay: colored tint where tintMask is non-zero (inner contour only)
        cv::Mat overlay = rgb.clone();
        for (int y = 0; y < overlay.rows && y < tintMask.rows; ++y)
        {
            for (int x = 0; x < overlay.cols && x < tintMask.cols; ++x)
            {
                if (tintMask.at<uchar>(y, x) > 0)
                {
                    cv::Vec3b &pixel = overlay.at<cv::Vec3b>(y, x);
                    pixel[0] = static_cast<uchar>(std::min(255.0, pixel[0] * 0.7 + tint[0] * 0.3));
                    pixel[1] = static_cast<uchar>(std::min(255.0, pixel[1] * 0.7 + tint[1] * 0.3));
                    pixel[2] = static_cast<uchar>(std::min(255.0, pixel[2] * 0.7 + tint[2] * 0.3));
                }
            }
        }

        // Draw contour outlines for both outer and inner contours
        const cv::Scalar contourColor(tint[0], tint[1], tint[2]);
        cv::drawContours(overlay, contours, -1, contourColor, 1);

        QImage img(overlay.data, overlay.cols, overlay.rows, static_cast<int>(overlay.step), QImage::Format_RGB888);
        return img.copy();
    }

    // ------------------------------------------------------------------
    // Scatter density (KDE) colouring
    // ------------------------------------------------------------------

    bool ExperimentMonitoringTab::kdeTimerActive() const
    {
        return kdeTimer_ && kdeTimer_->isActive();
    }

    QCheckBox* ExperimentMonitoringTab::kdeToggle() const
    {
        return ui->kdeToggleCheck;
    }

    void ExperimentMonitoringTab::injectMonitoringFramesForTests(const std::vector<backend::services::ProcessedFrame>& frames)
    {
        // The backend density service reads the processing ring; feed both.
        for (const auto& frame : frames)
        {
            backend_.processing().appendMonitoringFrameForTests(frame);
        }
        recentValidFrames_.insert(recentValidFrames_.end(), frames.begin(), frames.end());
        if (recentValidFrames_.size() > MAX_RECENT_FRAMES)
        {
            const size_t excess = recentValidFrames_.size() - MAX_RECENT_FRAMES;
            recentValidFrames_.erase(recentValidFrames_.begin(), recentValidFrames_.begin() + excess);
        }
        for (const auto& frame : frames)
        {
            lastValidFrameIndex_ = std::max(lastValidFrameIndex_, frame.index);
        }
        refreshCharts();
    }

    void ExperimentMonitoringTab::loadKdePreferences()
    {
        QSettings s;
        kdeEnabled_ = false;
        kdeBandwidthFactor_ = kKdeBandwidthFactorDefault;
        kdeIntervalMs_ = kKdeIntervalMsDefault;
        if (s.value(QLatin1String(kKdeKeyVersion), 0).toInt() != kKdePreferenceVersion) return; // missing/obsolete: defaults
        kdeEnabled_ = s.value(QLatin1String(kKdeKeyEnabled), false).toBool();
        bool ok = false;
        const double factor = s.value(QLatin1String(kKdeKeyBandwidthFactor)).toDouble(&ok);
        if (ok && std::isfinite(factor))
        {
            kdeBandwidthFactor_ = std::clamp(factor, kKdeBandwidthFactorMin, kKdeBandwidthFactorMax);
        }
        const int interval = s.value(QLatin1String(kKdeKeyIntervalMs)).toInt(&ok);
        if (ok)
        {
            kdeIntervalMs_ = std::clamp(interval, kKdeIntervalMsMin, kKdeIntervalMsMax);
        }
        const double fraction = s.value(QLatin1String(kKdeKeyCoreFraction)).toDouble(&ok);
        if (ok && std::isfinite(fraction))
        {
            kdeCoreFraction_ = std::clamp(fraction, kKdeCoreFractionMin, kKdeCoreFractionMax);
        }
    }

    void ExperimentMonitoringTab::saveKdePreferences()
    {
        QSettings s;
        s.setValue(QLatin1String(kKdeKeyVersion), kKdePreferenceVersion);
        s.setValue(QLatin1String(kKdeKeyEnabled), kdeEnabled_);
        s.setValue(QLatin1String(kKdeKeyBandwidthFactor), kdeBandwidthFactor_);
        s.setValue(QLatin1String(kKdeKeyIntervalMs), kdeIntervalMs_);
        s.setValue(QLatin1String(kKdeKeyCoreFraction), kdeCoreFraction_);
    }

    void ExperimentMonitoringTab::setKdeEnabled(bool enabled)
    {
        {
            QSignalBlocker blocker(ui->kdeToggleCheck);
            ui->kdeToggleCheck->setChecked(enabled);
        }
        if (kdeEnabled_ == enabled) return;
        kdeEnabled_ = enabled;
        if (enabled)
        {
            setKdeModeVisuals(true);
            if (kdeTimer_ && isVisible() && !kdeTimer_->isActive()) kdeTimer_->start();
            updateScatterplot(recentValidFrames_); // sparse colouring until the first estimate lands
            pushKdeSettings(); // enabling wakes the backend worker at once
        }
        else
        {
            if (kdeTimer_) kdeTimer_->stop();
            kdeDensityByIndex_.clear();
            pushKdeSettings();
            setKdeModeVisuals(false);
            updateScatterplot(recentValidFrames_); // plain series take the points back immediately
            refreshKdeTooltip();
        }
        saveKdePreferences();
        SPDLOG_INFO("Monitoring scatter density (KDE) {}", enabled ? "enabled" : "disabled");
    }

    void ExperimentMonitoringTab::setKdeBandwidthFactor(double factor)
    {
        if (!std::isfinite(factor)) return;
        factor = std::clamp(factor, kKdeBandwidthFactorMin, kKdeBandwidthFactorMax);
        if (factor == kdeBandwidthFactor_) return;
        kdeBandwidthFactor_ = factor;
        saveKdePreferences();
        pushKdeSettings();
    }

    void ExperimentMonitoringTab::setKdeIntervalMs(int ms)
    {
        ms = std::clamp(ms, kKdeIntervalMsMin, kKdeIntervalMsMax);
        if (ms == kdeIntervalMs_) return;
        kdeIntervalMs_ = ms;
        saveKdePreferences();
        pushKdeSettings();
    }

    void ExperimentMonitoringTab::setKdeCoreFraction(double fraction)
    {
        if (!std::isfinite(fraction)) return;
        fraction = std::clamp(fraction, kKdeCoreFractionMin, kKdeCoreFractionMax);
        if (fraction == kdeCoreFraction_) return;
        kdeCoreFraction_ = fraction;
        saveKdePreferences();
        pushKdeSettings();
    }

    void ExperimentMonitoringTab::redrawKdeContours(std::vector<QLineSeries*>& family,
                                                    const std::vector<std::vector<backend::monitoring::DensityPoint>>& loops,
                                                    bool reference)
    {
        if (!scatterplotChart_) return;
        for (auto* series : family)
        {
            scatterplotChart_->removeSeries(series);
            delete series;
        }
        family.clear();
        if (!kdeEnabled_) return;
        // Categorical slots 1 (blue) and 2 (orange), never the density ramp.
        QPen pen(reference ? QColor(0xeb, 0x68, 0x34) : QColor(0x2a, 0x78, 0xd6));
        pen.setWidthF(2.0);
        pen.setCosmetic(true);
        if (reference) pen.setStyle(Qt::DashLine);
        int loopIndex = 0;
        for (const auto& loop : loops)
        {
            auto* series = new QLineSeries();
            series->setName(QStringLiteral("%1-contour-%2").arg(reference ? QStringLiteral("reference") : QStringLiteral("core")).arg(loopIndex++));
            series->setPen(pen);
            QList<QPointF> pts;
            pts.reserve(static_cast<qsizetype>(loop.size()));
            for (const auto& p : loop) pts.append(QPointF(p.x, p.y));
            series->append(pts);
            scatterplotChart_->addSeries(series);
            series->attachAxis(scatterXAxis_);
            series->attachAxis(scatterYAxis_);
            for (auto* marker : scatterplotChart_->legend()->markers(series)) marker->setVisible(false);
            family.push_back(series);
        }
    }

    void ExperimentMonitoringTab::pinKdeReference()
    {
        setKdeReferencePath(QString()); // a pinned contour lives for the session only
        setKdeReference(lastKdeContours_, tr("pinned %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
    }

    void ExperimentMonitoringTab::setKdeReferencePath(const QString& path)
    {
        kdeReferencePath_ = path;
        QSettings s;
        if (path.isEmpty()) s.remove(QLatin1String(kKdeKeyReferencePath));
        else s.setValue(QLatin1String(kKdeKeyReferencePath), path);
    }

    bool ExperimentMonitoringTab::loadKdeReferenceFromFile(const QString& path, QString* error)
    {
        auto fail = [&](const QString& why) {
            if (error) *error = why;
            SPDLOG_WARN("Monitoring KDE: reference contour from {} not loaded: {}", path.toStdString(), why.toStdString());
            return false;
        };
        if (path.isEmpty() || !QFileInfo::exists(path)) return fail(tr("file not found"));
        backend::services::Hdf5Service reader;
        if (!reader.loadFile(path.toStdString())) return fail(tr("not a readable HDF5 file"));
        // Prefer the authoritative full-run record, fall back to the live one.
        std::string json;
        std::optional<backend::monitoring::KdeCoreRecord> record;
        std::string whyAnalysis, whyLive;
        if (reader.readKdeAnalysisJson(json)) record = backend::monitoring::fromJson(json, &whyAnalysis);
        if (!record && reader.readKdeLiveJson(json)) record = backend::monitoring::fromJson(json, &whyLive);
        reader.closeFile();
        if (!record)
        {
            if (!whyAnalysis.empty() || !whyLive.empty())
                return fail(tr("stored core contour unreadable (%1)").arg(QString::fromStdString(!whyLive.empty() ? whyLive : whyAnalysis)));
            return fail(tr("the experiment has no stored core contour"));
        }
        if (record->contours.empty()) return fail(tr("the stored core contour is empty (too few cells)"));
        const QString label = tr("%1, %2 %3%")
                                  .arg(QFileInfo(path).fileName(),
                                       record->provisional ? tr("live estimate") : tr("full-run"))
                                  .arg(std::lround(record->coreFraction * 100.0));
        setKdeReferencePath(path);
        setKdeReference(std::move(record->contours), label);
        return true;
    }

    std::string ExperimentMonitoringTab::lastCoreRecordJson() const
    {
        if (!kdeEnabled_ || !kdeResult_) return {};
        return backend::services::MonitoringDensityService::liveRecordJson(*kdeResult_);
    }

    void ExperimentMonitoringTab::setKdeReference(std::vector<std::vector<backend::monitoring::DensityPoint>> loops, const QString& label)
    {
        kdeReference_ = std::move(loops);
        kdeReferenceLabel_ = kdeReference_.empty() ? QString() : label;
        redrawKdeContours(kdeReferenceSeries_, kdeReference_, true);
        refreshKdeTooltip();
        SPDLOG_INFO("Monitoring KDE reference contour: {} loop(s) ({})", kdeReference_.size(), kdeReferenceLabel_.toStdString());
    }

    void ExperimentMonitoringTab::clearKdeReference()
    {
        setKdeReferencePath(QString());
        setKdeReference({}, QString());
    }

    void ExperimentMonitoringTab::refreshKdeTooltip()
    {
        if (!kdeEnabled_)
        {
            ui->kdeToggleCheck->setToolTip(tr("Colour scatter points by local population density (Gaussian KDE, recomputed periodically)"));
            return;
        }
        // The service may space estimates beyond the setting (compute budget).
        const int refreshMs = std::max(kdeIntervalMs_, backend_.monitoringDensity().stats().nextIntervalMs);
        QString text = tr("Density (KDE) over %1 points; bandwidth %2 μm² × %3; computed in %4 ms; refreshed every %5 s")
                           .arg(lastKdePointCount_)
                           .arg(lastKdeBandwidthX_, 0, 'f', 1)
                           .arg(lastKdeBandwidthY_, 0, 'f', 4)
                           .arg(lastKdeComputeMs_)
                           .arg(refreshMs / 1000.0, 0, 'g', 3);
        if (std::isfinite(lastKdeCoreLevel_))
        {
            text += tr("\nCore %1%: %2 cells, %3 contour loop(s)")
                        .arg(std::lround(kdeCoreFraction_ * 100.0))
                        .arg(lastKdeCoreCount_)
                        .arg(lastKdeContours_.size());
        }
        else
        {
            text += tr("\nCore: too few cells for a contour");
        }
        if (!kdeReference_.empty()) text += tr("\nReference contour: %1").arg(kdeReferenceLabel_);
        ui->kdeToggleCheck->setToolTip(text);
    }

    void ExperimentMonitoringTab::setupKdeLevelSeries()
    {
        // Created once, hidden until KDE is switched on. Both families take
        // the density colour, so the target group keeps its identity through
        // the marker shape (rectangle) instead of its colour. None of them
        // appears in the legend.
        for (int level = 0; level < kKdeLevels; ++level)
        {
            for (bool target : {false, true})
            {
                auto* series = new QScatterSeries();
                series->setMarkerSize(6.0);
                series->setMarkerShape(target ? QScatterSeries::MarkerShapeRectangle : QScatterSeries::MarkerShapeCircle);
                series->setColor(kdeLevelColor(level, kKdeLevels));
                series->setName(QStringLiteral("density-%1%2").arg(level).arg(target ? QStringLiteral("-target") : QString()));
                scatterplotChart_->addSeries(series);
                series->attachAxis(scatterXAxis_);
                series->attachAxis(scatterYAxis_);
                series->setVisible(false);
                (target ? kdeTargetLevelSeries_ : kdeLevelSeries_).push_back(series);
            }
        }
        hideKdeLegendMarkers();
    }

    void ExperimentMonitoringTab::hideKdeLegendMarkers()
    {
        // The legend lists every series of the chart; the density levels are
        // an encoding of the plain series, not extra populations.
        if (!scatterplotChart_) return;
        for (const auto* family : {&kdeLevelSeries_, &kdeTargetLevelSeries_})
        {
            for (auto* series : *family)
            {
                for (auto* marker : scatterplotChart_->legend()->markers(series)) marker->setVisible(false);
            }
        }
    }

    int ExperimentMonitoringTab::kdeLevelForDensity(double density)
    {
        if (!std::isfinite(density)) return 0;
        const int level = static_cast<int>(std::clamp(density, 0.0, 1.0) * kKdeLevels);
        return std::min(level, kKdeLevels - 1);
    }

    bool ExperimentMonitoringTab::kdeDensityForFrame(uint64_t frameIndex, double& density) const
    {
        const auto it = kdeDensityByIndex_.find(frameIndex);
        if (it == kdeDensityByIndex_.end()) return false;
        density = it->second;
        return true;
    }

    void ExperimentMonitoringTab::setKdeModeVisuals(bool on)
    {
        if (!scatterSeries_ || !targetGroupSeries_) return;
        scatterSeries_->setVisible(!on);
        targetGroupSeries_->setVisible(!on);
        for (auto* s : kdeLevelSeries_)
        {
            if (!on) s->clear();
            s->setVisible(on);
        }
        for (auto* s : kdeTargetLevelSeries_)
        {
            if (!on) s->clear();
            s->setVisible(on);
        }
        if (!on)
        {
            // Contours belong to the KDE mode; the off state is the plain chart.
            lastKdeContours_.clear();
            lastKdeCoreLevel_ = std::nan("");
            lastKdeCoreCount_ = 0;
            kdeResult_.reset();
            redrawKdeContours(kdeContourSeries_, {}, false);
            redrawKdeContours(kdeReferenceSeries_, {}, true);
        }
        else
        {
            redrawKdeContours(kdeReferenceSeries_, kdeReference_, true);
        }
        hideKdeLegendMarkers();
    }

    void ExperimentMonitoringTab::pushKdeSettings()
    {
        pushKdeSettings(isVisible());
    }

    void ExperimentMonitoringTab::pushKdeSettings(bool visible)
    {
        backend::services::MonitoringDensitySettings settings;
        settings.enabled = kdeEnabled_ && visible;
        settings.intervalMs = kdeIntervalMs_;
        settings.bandwidthFactor = kdeBandwidthFactor_;
        settings.coreFraction = kdeCoreFraction_;
        // The contour grid spans the chart's fixed axes.
        settings.x0 = scatterXMin_;
        settings.x1 = scatterXMax_;
        settings.y0 = scatterYMin_;
        settings.y1 = scatterYMax_;
        backend_.monitoringDensity().setSettings(settings);
    }

    void ExperimentMonitoringTab::requestKdeUpdate()
    {
        if (!kdeEnabled_) return;
        pushKdeSettings();
        backend_.monitoringDensity().requestUpdate();
    }

    bool ExperimentMonitoringTab::kdeJobInFlight() const
    {
        return backend_.monitoringDensity().busy();
    }

    void ExperimentMonitoringTab::pollKdeResult()
    {
        auto& service = backend_.monitoringDensity();
        const uint64_t generation = service.generation();
        if (generation == kdeServiceGeneration_) return;
        kdeServiceGeneration_ = generation;
        if (!kdeEnabled_) return; // switched off while the estimate was running
        auto result = service.latest();
        kdeDensityByIndex_.clear();
        if (!result)
        {
            // The ring emptied (cleared buffer): back to the sparsest level.
            kdeResult_.reset();
            lastKdeContours_.clear();
            lastKdeCoreLevel_ = std::nan("");
            lastKdeCoreCount_ = 0;
            updateScatterplot(recentValidFrames_);
            redrawKdeContours(kdeContourSeries_, {}, false);
            refreshKdeTooltip();
            return;
        }
        kdeDensityByIndex_.reserve(result->frameIndices.size());
        for (std::size_t i = 0; i < result->frameIndices.size() && i < result->density.size(); ++i)
        {
            kdeDensityByIndex_[result->frameIndices[i]] = result->density[i];
        }
        ++kdeGeneration_;
        lastKdeComputeMs_ = result->computeMs;
        lastKdePointCount_ = result->frameIndices.size();
        lastKdeBandwidthX_ = result->bandwidth.x;
        lastKdeBandwidthY_ = result->bandwidth.y;
        lastKdeCoreLevel_ = result->coreLevel;
        lastKdeCoreCount_ = result->coreCount;
        lastKdeContours_ = result->contours;
        kdeResult_ = std::move(result);
        updateScatterplot(recentValidFrames_); // re-route points into their new density levels
        redrawKdeContours(kdeContourSeries_, lastKdeContours_, false);
        refreshKdeTooltip();
    }


    void ExperimentMonitoringTab::setScatterXRange(double minVal, double maxVal)
    {
        if (minVal >= maxVal)
            return;
        scatterXMin_ = minVal;
        scatterXMax_ = maxVal;
        if (scatterplotView_) {
            scatterplotView_->setDefaultRange(scatterXAxis_, minVal, maxVal);
            scatterplotView_->resetZoom();
        }
        pushKdeSettings();
    }

    void ExperimentMonitoringTab::setScatterYRange(double minVal, double maxVal)
    {
        if (minVal >= maxVal)
            return;
        scatterYMin_ = minVal;
        scatterYMax_ = maxVal;
        if (scatterplotView_) {
            scatterplotView_->setDefaultRange(scatterYAxis_, minVal, maxVal);
            scatterplotView_->resetZoom();
        }
        pushKdeSettings();
    }

    void ExperimentMonitoringTab::setHistogramXRange(double minVal, double maxVal)
    {
        if (minVal >= maxVal)
            return;
        histogramXMin_ = minVal;
        histogramXMax_ = maxVal;
        if (histogramView_ && histogramXAxis_) {
            histogramView_->setDefaultRange(histogramXAxis_, minVal, maxVal);
            histogramView_->resetZoom();
        }
    }

    void ExperimentMonitoringTab::setHistogramYMax(double maxVal)
    {
        if (maxVal <= 0)
            return;
        histogramYMax_ = maxVal;
        if (histogramView_) {
            histogramView_->setDefaultRange(histogramYAxis_, 0, std::max(1.0, maxVal));
            histogramView_->resetZoom();
        }
    }

    void ExperimentMonitoringTab::setHistogramBinWidth(double width)
    {
        if (width <= 0)
            return;
        histogramBinWidth_ = width;
    }

    void ExperimentMonitoringTab::refreshCharts()
    {
        updateScatterplot(recentValidFrames_);
        updateHistogram(recentValidFrames_);
    }

    void ExperimentMonitoringTab::loadIsoelasticCurves()
    {
        // Find the isoelastic curve data file
        QString appDir = QCoreApplication::applicationDirPath();
        QString filePath = QDir(appDir).absoluteFilePath("../resources/isoelastic_curve/scaled_isoelastic_data_6.16-4.24.txt");
        
        // Try alternative path if file doesn't exist
        if (!QFile::exists(filePath))
        {
            filePath = QDir(appDir).absoluteFilePath("resources/isoelastic_curve/scaled_isoelastic_data_6.16-4.24.txt");
        }
        
        // Try source directory path for development
        if (!QFile::exists(filePath))
        {
            filePath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../resources/isoelastic_curve/scaled_isoelastic_data_6.16-4.24.txt");
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            SPDLOG_WARN("Failed to open isoelastic curve file: {}", filePath.toStdString());
            return;
        }

        // Group data points by emodulus value
        std::map<double, std::vector<std::pair<double, double>>> curvesByModulus;

        QTextStream in(&file);
        while (!in.atEnd())
        {
            QString line = in.readLine().trimmed();
            
            // Skip empty lines and comments
            if (line.isEmpty() || line.startsWith('#'))
            {
                continue;
            }

            // Parse tab-separated values: area_um, deform, emodulus
            QStringList parts = line.split('\t', Qt::SkipEmptyParts);
            if (parts.size() < 3)
            {
                continue;
            }

            bool ok1, ok2, ok3;
            double areaUm = parts[0].toDouble(&ok1);
            double deform = parts[1].toDouble(&ok2);
            double emodulus = parts[2].toDouble(&ok3);

            if (ok1 && ok2 && ok3)
            {
                curvesByModulus[emodulus].push_back({areaUm, deform});
            }
        }

        file.close();

        if (curvesByModulus.empty())
        {
            SPDLOG_WARN("No isoelastic curve data found in file: {}", filePath.toStdString());
            return;
        }

        // Create QLineSeries for each modulus value (in reverse order for legend)
        for (auto it = curvesByModulus.rbegin(); it != curvesByModulus.rend(); ++it)
        {
            const auto& [emodulus, points] = *it;
            QLineSeries* series = new QLineSeries();
            series->setName(QString("%1 kPa").arg(emodulus, 0, 'f', 2));
            
            // Add points to series
            for (const auto& [area, deform] : points)
            {
                series->append(area, deform);
            }

            // Add series to chart
            scatterplotChart_->addSeries(series);
            series->attachAxis(scatterXAxis_);
            series->attachAxis(scatterYAxis_);
            
            // Store pointer for cleanup
            isoelasticCurves_.push_back(series);
        }

        // Enable legend to show all series and position it on the right
        scatterplotChart_->legend()->setVisible(true);
        scatterplotChart_->legend()->setAlignment(Qt::AlignRight);
        hideKdeLegendMarkers();

        SPDLOG_INFO("Loaded {} isoelastic curves from {}", curvesByModulus.size(), filePath.toStdString());
    }

    bool ExperimentMonitoringTab::captureChartSnapshots(cv::Mat& histogramImage, cv::Mat& scatterPlotImage) const
    {
        histogramImage.release();
        scatterPlotImage.release();

        if (!histogramView_ || !scatterplotView_)
        {
            SPDLOG_WARN("Chart views not available for snapshot capture");
            return false;
        }

        // Capture histogram chart
        QPixmap histogramPixmap = histogramView_->grab();
        if (histogramPixmap.isNull())
        {
            SPDLOG_WARN("Failed to grab histogram chart");
            return false;
        }

        // Capture scatter plot chart
        QPixmap scatterPlotPixmap;
        {
            HiddenWhileGrabbing hide(scatterHighlight_);
            HiddenItemWhileGrabbing hideGate(gateOverlay_);
            scatterPlotPixmap = scatterplotView_->grab();
        }
        if (scatterPlotPixmap.isNull())
        {
            SPDLOG_WARN("Failed to grab scatter plot chart");
            return false;
        }

        // Convert QPixmap to QImage
        QImage histogramQImage = histogramPixmap.toImage();
        QImage scatterPlotQImage = scatterPlotPixmap.toImage();

        // Convert QImage to cv::Mat
        // QImage uses ARGB32 format, need to convert to BGR for OpenCV
        histogramQImage = histogramQImage.convertToFormat(QImage::Format_RGB32);
        scatterPlotQImage = scatterPlotQImage.convertToFormat(QImage::Format_RGB32);

        // Create cv::Mat from QImage
        histogramImage = cv::Mat(histogramQImage.height(), histogramQImage.width(), CV_8UC4, 
                                  const_cast<uchar*>(histogramQImage.constBits()), 
                                  histogramQImage.bytesPerLine()).clone();
        scatterPlotImage = cv::Mat(scatterPlotQImage.height(), scatterPlotQImage.width(), CV_8UC4,
                                   const_cast<uchar*>(scatterPlotQImage.constBits()),
                                   scatterPlotQImage.bytesPerLine()).clone();

        // Format_RGB32 is B,G,R,A in memory (0xAARRGGBB): BGRA to OpenCV.
        // Read as RGBA this swapped red and blue in every stored chart
        // snapshot (same bug as the Review export, found by
        // integration.review_scatter_e2e).
        cv::cvtColor(histogramImage, histogramImage, cv::COLOR_BGRA2BGR);
        cv::cvtColor(scatterPlotImage, scatterPlotImage, cv::COLOR_BGRA2BGR);

        SPDLOG_DEBUG("Captured chart snapshots: histogram {}x{}, scatter plot {}x{}",
                     histogramImage.cols, histogramImage.rows,
                     scatterPlotImage.cols, scatterPlotImage.rows);
        return true;
    }

    bool ExperimentMonitoringTab::exportChartAsTiff(QChartView* chartView, const QString& filePath) const
    {
        if (!chartView)
        {
            SPDLOG_ERROR("Chart view is null");
            return false;
        }

        // Grab chart as pixmap
        QPixmap pixmap;
        {
            HiddenWhileGrabbing hide(chartView == scatterplotView_ ? scatterHighlight_ : nullptr);
            HiddenItemWhileGrabbing hideGate(chartView == scatterplotView_ ? gateOverlay_ : nullptr);
            pixmap = chartView->grab();
        }
        if (pixmap.isNull())
        {
            SPDLOG_ERROR("Failed to grab chart view");
            return false;
        }

        // Convert QPixmap to QImage
        QImage qImage = pixmap.toImage();
        qImage = qImage.convertToFormat(QImage::Format_RGB32);

        // Convert QImage to cv::Mat
        cv::Mat mat(qImage.height(), qImage.width(), CV_8UC4, 
                    const_cast<uchar*>(qImage.constBits()), 
                    qImage.bytesPerLine());
        
        // Format_RGB32 is B,G,R,A in memory: BGRA to OpenCV (see above).
        cv::Mat bgrMat;
        cv::cvtColor(mat, bgrMat, cv::COLOR_BGRA2BGR);

        // Save as TIFF with compression
        std::vector<int> compression_params;
        compression_params.push_back(cv::IMWRITE_TIFF_COMPRESSION);
        compression_params.push_back(1); // LZW compression

        if (!cv::imwrite(filePath.toStdString(), bgrMat, compression_params))
        {
            SPDLOG_ERROR("Failed to write TIFF file: {}", filePath.toStdString());
            return false;
        }

        SPDLOG_INFO("Exported chart to TIFF: {}", filePath.toStdString());
        return true;
    }

    bool ExperimentMonitoringTab::exportHistogramAsTiff(const QString& filePath) const
    {
        return exportChartAsTiff(histogramView_, filePath);
    }

    bool ExperimentMonitoringTab::exportScatterPlotAsTiff(const QString& filePath) const
    {
        return exportChartAsTiff(scatterplotView_, filePath);
    }

    // ---- Scatter click -> frame pane (the Review tab's interaction, live) ----

    void ExperimentMonitoringTab::setupFramePane()
    {
        framePaneContainer_ = new QWidget(this);
        framePaneContainer_->setObjectName(QStringLiteral("monitoringFramePane"));
        auto* paneLayout = new QVBoxLayout(framePaneContainer_);
        paneLayout->setContentsMargins(0, 0, 0, 0);
        paneLayout->setSpacing(2);

        auto* titleRow = new QHBoxLayout();
        titleRow->setContentsMargins(0, 0, 0, 0);
        framePaneTitle_ = new QLabel(framePaneContainer_);
        framePaneTitle_->setObjectName(QStringLiteral("monitoringFramePaneTitle"));
        framePaneTitle_->setWordWrap(true);
        titleRow->addWidget(framePaneTitle_, 1);
        auto* closeBtn = new QToolButton(framePaneContainer_);
        closeBtn->setObjectName(QStringLiteral("monitoringFramePaneClose"));
        closeBtn->setText(QStringLiteral("\u2715"));
        closeBtn->setToolTip(tr("Close the cell view"));
        closeBtn->setAccessibleName(tr("Close the cell view"));
        closeBtn->setAutoRaise(true);
        connect(closeBtn, &QToolButton::clicked, this, &ExperimentMonitoringTab::clearSelection);
        titleRow->addWidget(closeBtn, 0, Qt::AlignTop);
        paneLayout->addLayout(titleRow);

        framePane_ = new FrameViewerDialog(backend::services::ProcessedFrame{}, backend_.processing().getRealtimeRoi(),
                                           paneOverlayMode(), false, framePaneContainer_);
        framePane_->setEmbedded(true);
        framePane_->setObjectName(QStringLiteral("monitoringFramePaneViewer"));
        paneLayout->addWidget(framePane_, 1);
        connect(framePane_, &FrameViewerDialog::requestPreviousFrame, this, [this]() { stepScatterSelection(-1); });
        connect(framePane_, &FrameViewerDialog::requestNextFrame, this, [this]() { stepScatterSelection(+1); });
        connect(framePane_, &FrameViewerDialog::requestOpenInWindow, this,
                &ExperimentMonitoringTab::openSelectedFrameInWindow);

        // (frame pane over histogram); the pane appears with the first click,
        // so the histogram keeps the whole cell until someone asks for a cell.
        chartsRightSplitter_ = new QSplitter(Qt::Vertical, this);
        chartsRightSplitter_->setObjectName(QStringLiteral("monitoringChartsRightSplitter"));
        chartsRightSplitter_->addWidget(framePaneContainer_);
        chartsRightSplitter_->addWidget(histogramView_);
        chartsRightSplitter_->setStretchFactor(0, 3);
        chartsRightSplitter_->setStretchFactor(1, 2);
        chartsRightSplitter_->setChildrenCollapsible(false);
        framePaneContainer_->setMinimumHeight(160);
        framePaneContainer_->hide();
        ui->gridLayout->addWidget(chartsRightSplitter_, 1, 1);
    }

    OverlayMode ExperimentMonitoringTab::paneOverlayMode() const
    {
        // The valid-frames "overlay" checkbox drives the pane like the thumbnails.
        return showValidOverlay_ ? OverlayMode::OuterInnerColorCoded : OverlayMode::None;
    }

    scatterhit::Viewport ExperimentMonitoringTab::scatterViewport() const
    {
        scatterhit::Viewport v;
        const QRectF plot = scatterplotChart_->plotArea();
        v.x0 = scatterXAxis_->min();
        v.x1 = scatterXAxis_->max();
        v.y0 = scatterYAxis_->min();
        v.y1 = scatterYAxis_->max();
        v.left = plot.left();
        v.top = plot.top();
        v.width = plot.width();
        v.height = plot.height();
        return v;
    }

    std::optional<std::size_t> ExperimentMonitoringTab::scatterPointAt(QPointF viewPos) const
    {
        if (scatterPoints_.empty() || !scatterplotView_) return std::nullopt;
        // View (viewport) -> scene -> chart item coordinates, where plotArea() lives.
        const QPointF chartPos = scatterplotChart_->mapFromScene(scatterplotView_->mapToScene(viewPos.toPoint()));
        const double tolerance = std::max(scatterSeries_->markerSize(), 8.0);
        return scatterhit::nearest(scatterPoints_, scatterViewport(), chartPos.x(), chartPos.y(), tolerance);
    }

    void ExperimentMonitoringTab::onScatterClicked(QPointF viewPos, Qt::MouseButton button)
    {
        if (button != Qt::LeftButton) return;
        const auto hit = scatterPointAt(viewPos);
        if (!hit) return; // empty space: the selection only changes on a point
        const auto* frame = recentValidFrame(scatterPointFrames_[*hit]);
        if (!frame) return;
        SPDLOG_DEBUG("Monitoring: scatter point {} -> frame {}", *hit, frame->index);
        selectFrame(*frame);
    }

    void ExperimentMonitoringTab::onScatterDoubleClicked(QPointF viewPos)
    {
        // Its first click already selected; a double-click on a point never
        // zooms out from under the user.
        if (scatterPointAt(viewPos)) return;
        scatterplotView_->resetZoom();
    }

    void ExperimentMonitoringTab::onScatterHover(QPointF viewPos)
    {
        // A gate handle under the pointer wins: pressing there and dragging edits
        // the gate (a click without travel still selects a cell).
        if (showGateHover(viewPos)) return;
        const auto hit = scatterPointAt(viewPos);
        if (!hit) {
            scatterplotView_->unsetCursor();
            // Not on a cell or a handle: what the pointer is over.
            const gate::Viewport vp = scatterViewport();
            const QPointF chartPos = chartPosFromView(viewPos);
            if (!gate::viewportValid(vp) || !scatterhit::insidePlot(vp, chartPos.x(), chartPos.y())) {
                QToolTip::hideText();
                return;
            }
            const QPointF data = gate::toData(vp, chartPos);
            QString text = tr("Area %1 µm² · deformability %2").arg(data.x(), 0, 'f', 1).arg(data.y(), 0, 'f', 3);
            const double e = gate::lookupE(lutSnapshot_.get(), data.x(), data.y());
            if (std::isfinite(e)) text += tr(" · E %1 kPa").arg(e, 0, 'f', 2);
            QToolTip::showText(scatterplotView_->mapToGlobal(viewPos.toPoint()), text, scatterplotView_);
            return;
        }
        scatterplotView_->setCursor(Qt::PointingHandCursor);
        const auto& p = scatterPoints_[*hit];
        QToolTip::showText(scatterplotView_->mapToGlobal(viewPos.toPoint()),
                           tr("Frame %1 · %2 µm² · deformability %3 · click to view")
                               .arg(static_cast<qulonglong>(scatterPointFrames_[*hit]))
                               .arg(p.x, 0, 'f', 1)
                               .arg(p.y, 0, 'f', 4),
                           scatterplotView_);
    }

    const backend::services::ProcessedFrame* ExperimentMonitoringTab::recentValidFrame(uint64_t frameIndex) const
    {
        // Newest first: a click almost always lands on a recent cell.
        for (auto it = recentValidFrames_.rbegin(); it != recentValidFrames_.rend(); ++it)
            if (it->index == frameIndex) return &*it;
        return nullptr;
    }

    backend::services::ProcessedFrame ExperimentMonitoringTab::frameForDisplay(
        const backend::services::ProcessedFrame& frame) const
    {
        // Same crop as the thumbnails (and the ROI-sized images the Review tab
        // reads back from the file); clamped, since the frame may predate the
        // current ROI (crash review 2026-09-08). The crops share pixels.
        backend::services::ProcessedFrame shown = frame;
        const auto roi = backend_.processing().getRealtimeRoi();
        if (roi.w <= 0 || roi.h <= 0) return shown;
        shown.originalImage = frontend::monitoring::cropToRoi(frame.originalImage, roi.x, roi.y, roi.w, roi.h);
        shown.processedImage = frontend::monitoring::cropToRoi(frame.processedImage, roi.x, roi.y, roi.w, roi.h);
        for (auto& image : shown.seriesImages)
            image = frontend::monitoring::cropToRoi(image, roi.x, roi.y, roi.w, roi.h);
        return shown;
    }

    void ExperimentMonitoringTab::selectFrame(const backend::services::ProcessedFrame& frame)
    {
        // A copy: the rolling buffer drops the frame after MAX_RECENT_FRAMES
        // newer cells, the operator may still be looking at it.
        selectedFrame_ = std::make_unique<backend::services::ProcessedFrame>(frameForDisplay(frame));
        framePane_->setOverlayMode(paneOverlayMode());
        framePane_->setFrame(*selectedFrame_);
        if (frameWindow_) {
            frameWindow_->setFrame(*selectedFrame_);
        }
        framePaneContainer_->show();
        refreshScatterHighlight();
    }

    void ExperimentMonitoringTab::stepScatterSelection(int delta)
    {
        if (scatterPointFrames_.empty()) return;
        const int n = static_cast<int>(scatterPointFrames_.size());
        int from = delta > 0 ? -1 : 0; // nothing selected (or gone): start at an end
        if (selectedFrame_) {
            const auto it = std::find(scatterPointFrames_.begin(), scatterPointFrames_.end(), selectedFrame_->index);
            if (it != scatterPointFrames_.end()) from = static_cast<int>(it - scatterPointFrames_.begin());
        }
        const int to = ((from + delta) % n + n) % n;
        if (const auto* frame = recentValidFrame(scatterPointFrames_[static_cast<std::size_t>(to)])) selectFrame(*frame);
    }

    void ExperimentMonitoringTab::clearSelection()
    {
        selectedFrame_.reset();
        if (framePaneContainer_) framePaneContainer_->hide();
        if (framePane_) framePane_->setFrame(backend::services::ProcessedFrame{});
        // Hidden, not deleted: the window is reused by the next "Open in window…".
        if (frameWindow_) frameWindow_->close();
        refreshScatterHighlight();
    }

    void ExperimentMonitoringTab::refreshScatterHighlight()
    {
        if (!scatterHighlight_) return;
        scatterHighlight_->clear();
        std::optional<std::size_t> point;
        if (selectedFrame_) {
            for (std::size_t i = scatterPointFrames_.size(); i-- > 0;) {
                if (scatterPointFrames_[i] == selectedFrame_->index) {
                    point = i;
                    break;
                }
            }
        }
        if (point) {
            scatterHighlight_->append(scatterPoints_[*point].x, scatterPoints_[*point].y);
        }
        scatterHighlight_->setVisible(point.has_value());

        if (!framePaneTitle_ || !selectedFrame_) return;
        const double factor = selectedFrame_->validation.analysisPixelToMicronFactor;
        QString title = tr("Frame %1 · %2 µm² · deformability %3")
                            .arg(static_cast<qulonglong>(selectedFrame_->index))
                            .arg(selectedFrame_->validation.area * factor * factor, 0, 'f', 1)
                            .arg(selectedFrame_->validation.deformability, 0, 'f', 4);
        if (selectedFrame_->validation.isTargetGroup) title += tr(" · target group");
        if (!point) title += tr(" · no longer on the scatter (left the monitoring buffer)");
        framePaneTitle_->setText(title);
    }

    void ExperimentMonitoringTab::openSelectedFrameInWindow()
    {
        if (!selectedFrame_) return;
        if (!frameWindow_) {
            // Non-modal: the run and the charts keep updating behind it.
            frameWindow_ = new FrameViewerDialog(*selectedFrame_, backend_.processing().getRealtimeRoi(),
                                                 paneOverlayMode(), false, this);
            frameWindow_->setObjectName(QStringLiteral("monitoringFrameWindow"));
            frameWindow_->setModal(false);
            connect(frameWindow_, &FrameViewerDialog::requestPreviousFrame, this, [this]() { stepScatterSelection(-1); });
            connect(frameWindow_, &FrameViewerDialog::requestNextFrame, this, [this]() { stepScatterSelection(+1); });
        } else {
            frameWindow_->setFrame(*selectedFrame_);
        }
        frameWindow_->show();
        frameWindow_->raise();
        frameWindow_->activateWindow();
    }

    FrameViewerDialog* ExperimentMonitoringTab::frameWindowForTests() const
    {
        return frameWindow_.data();
    }

    QString ExperimentMonitoringTab::framePaneTitleForTests() const
    {
        return framePaneTitle_ ? framePaneTitle_->text() : QString();
    }

    std::optional<uint64_t> ExperimentMonitoringTab::selectedFrameIndex() const
    {
        if (!selectedFrame_) return std::nullopt;
        return selectedFrame_->index;
    }

    QPointF ExperimentMonitoringTab::scatterPointViewPosForTests(std::size_t point) const
    {
        const auto& p = scatterPoints_.at(point);
        const QPointF chartPos = scatterplotChart_->mapToPosition(QPointF(p.x, p.y), scatterSeries_);
        return QPointF(scatterplotView_->mapFromScene(scatterplotChart_->mapToScene(chartPos)));
    }

    // ---- Gate overlay: the draft target-group gate on the scatter -----------------
    //
    // Drawn from the tune draft (an unapplied edit previews live), shown only
    // while TargetGroupEnabled is on in the draft. The tint, box and E contours
    // follow ProcessingScience exactly (gate::isTarget); editing goes through the
    // tune panel's own spin boxes, so nothing here can bypass Apply.

    void ExperimentMonitoringTab::setupGateOverlay()
    {
        gateOverlay_ = new GateOverlayItem(scatterplotChart_, scatterXAxis_, scatterYAxis_);
        gateOverlay_->setVisible(false);
        auto repaint = [this]() {
            if (gateOverlay_) gateOverlay_->update();
        };
        connect(scatterXAxis_, &QValueAxis::rangeChanged, this, repaint);
        connect(scatterYAxis_, &QValueAxis::rangeChanged, this, repaint);
        connect(scatterplotChart_, &QChart::plotAreaChanged, this, [this](const QRectF&) {
            if (gateOverlay_) gateOverlay_->plotAreaChanged();
        });
        scatterplotView_->setPressInterceptor([this](const QPointF& viewPos) { return beginGateEdit(viewPos); });
        connect(scatterplotView_, &ZoomableChartView::interceptDragStarted, this,
                [this](QPointF) { onGateDragStarted(); });
        connect(scatterplotView_, &ZoomableChartView::interceptDragMoved, this,
                [this](QPointF, QPointF viewPos) { onGateDragMoved(viewPos); });
        connect(scatterplotView_, &ZoomableChartView::interceptDragFinished, this,
                [this](QPointF, QPointF viewPos, bool completed) { onGateDragFinished(viewPos, completed); });
        connect(scatterplotView_, &ZoomableChartView::hoverLeft, this, [this]() {
            setGateHover(gate::HandleKind::None);
            QToolTip::hideText();
        });
        refreshLutSnapshot();
    }

    gate::GateSpec ExperimentMonitoringTab::gateSpec() const { return gate::GateSpec::fromConfig(draft_.draft()); }

    void ExperimentMonitoringTab::refreshLutSnapshot()
    {
        // The table identity is read under the service's lock; the table itself is
        // loaded once at backend bootstrap and never mutated afterwards, so the
        // one-off copy below (GUI thread) cannot overlap a write.
        const auto identity = backend_.processing().eModulusLutIdentity();
        if (!identity.loaded) {
            if (lutSnapshot_) {
                lutSnapshot_.reset();
                lutSnapshotKey_.clear();
                contourCache_.clear();
                ++lutGeneration_;
            }
            return;
        }
        const std::string key = identity.sha256 + "|" + identity.path;
        if (lutSnapshot_ && key == lutSnapshotKey_) return;
        auto copy = std::make_shared<backend::EModulusLut>(backend_.processing().eModulusLut());
        if (!copy->isLoaded()) return;
        lutSnapshot_ = std::move(copy);
        lutSnapshotKey_ = key;
        contourCache_.clear();
        ++lutGeneration_;
    }

    std::shared_ptr<const gate::Contour> ExperimentMonitoringTab::contourFor(double level) const
    {
        if (!lutSnapshot_) return nullptr;
        const auto it = contourCache_.find(level);
        if (it != contourCache_.end()) return it->second;
        if (contourCache_.size() >= 8) contourCache_.clear(); // a drag walks through many levels
        auto contour = std::make_shared<const gate::Contour>(gate::isoContour(*lutSnapshot_, level));
        contourCache_[level] = contour;
        return contour;
    }

    ExperimentMonitoringTab::GateGeometry ExperimentMonitoringTab::gateGeometry() const
    {
        GateGeometry g;
        g.spec = gateSpec();
        g.viewport = scatterViewport();
        g.lut = lutSnapshot_.get();
        if (g.spec.enabled && g.spec.emodEnabled && lutSnapshot_) {
            g.eMin = contourFor(g.spec.emodMin);
            g.eMax = contourFor(g.spec.emodMax);
        }
        return g;
    }

    void ExperimentMonitoringTab::refreshGateOverlay()
    {
        if (!gateOverlay_) return;
        refreshLutSnapshot();
        const GateGeometry geo = gateGeometry();
        GateOverlayItem::Params params;
        params.spec = geo.spec;
        params.lut = geo.lut;
        params.lutGeneration = lutGeneration_;
        params.eMin = geo.eMin;
        params.eMax = geo.eMax;
        bool unapplied = false;
        for (TuneField f : {TuneField::TargetGroupEnabled, TuneField::TargetAreaMin, TuneField::TargetAreaMax,
                            TuneField::TargetDeformabilityMin, TuneField::TargetDeformabilityMax,
                            TuneField::TargetEmodulusEnabled, TuneField::TargetEmodulusMin, TuneField::TargetEmodulusMax})
            unapplied = unapplied || draft_.isChanged(f);
        params.label = unapplied ? tr("Target group (unapplied)") : tr("Target group");
        if (geo.spec.enabled && geo.spec.emodEnabled && !lutSnapshot_)
            params.notice = tr("No E-modulus table loaded: the target group uses the box only");
        gateOverlay_->setParams(std::move(params));
        gateOverlay_->setVisible(geo.spec.enabled);
        if (!geo.spec.enabled) setGateHover(gate::HandleKind::None);
        refreshGatePreview();
    }

    void ExperimentMonitoringTab::refreshGatePreview()
    {
        if (!gatePreviewLabel_) return;
        const gate::GateSpec spec = gateSpec();
        gatePreviewLabel_->setVisible(spec.enabled);
        if (!spec.enabled) return;
        // Without a table the E layer is skipped (the stored E of a cell is a placeholder then): the
        // count is the box alone, the same rule as the science.
        gatePreviewLabel_->setText(
            gate::previewText(gate::countTargets(spec, lutSnapshot_ != nullptr, scatterPoints_, scatterPointE_)));
    }

    QPointF ExperimentMonitoringTab::chartPosFromView(QPointF viewPos) const
    {
        return scatterplotChart_->mapFromScene(scatterplotView_->mapToScene(viewPos.toPoint()));
    }

    gate::Limits ExperimentMonitoringTab::gateLimits(TuneField field) const
    {
        gate::Limits limits = gate::defaultLimits(field);
        QWidget* w = tuneFieldWidget(field);
        if (auto* spin = qobject_cast<QSpinBox*>(w)) {
            limits = {static_cast<double>(spin->minimum()), static_cast<double>(spin->maximum())};
        } else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(w)) {
            limits = {dspin->minimum(), dspin->maximum()};
        }
        return limits;
    }

    bool ExperimentMonitoringTab::gateHandleEditable(gate::HandleKind kind) const
    {
        if (kind == gate::HandleKind::None || draft_.applying()) return false;
        // The same lock the panel has: a disabled control cannot be edited here either.
        QWidget* w = tuneFieldWidget(gate::handleField(kind));
        return w && w->isEnabled();
    }

    gate::HandleHit ExperimentMonitoringTab::gateHandleHit(QPointF viewPos) const
    {
        const GateGeometry geo = gateGeometry();
        if (!geo.spec.enabled) return {};
        const QPointF chartPos = chartPosFromView(viewPos);
        gate::HandleHit hit = gate::hitTest(geo.spec, geo.contours(), geo.viewport, chartPos.x(), chartPos.y());
        if (hit && !gateHandleEditable(hit.kind)) return {};
        return hit;
    }

    gate::HandleKind ExperimentMonitoringTab::gateHandleAtForTests(QPointF viewPos) const
    {
        return gateHandleHit(viewPos).kind;
    }

    ExperimentMonitoringTab::GateOverlayInfo ExperimentMonitoringTab::gateOverlayInfoForTests() const
    {
        return gateOverlay_ ? gateOverlay_->info() : GateOverlayInfo{};
    }

    QPointF ExperimentMonitoringTab::gateHandleViewPosForTests(gate::HandleKind kind) const
    {
        const GateGeometry geo = gateGeometry();
        const auto anchor = gate::handleAnchor(kind, geo.spec, geo.contours(), geo.viewport);
        if (!anchor) return QPointF(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());
        return QPointF(scatterplotView_->mapFromScene(scatterplotChart_->mapToScene(*anchor)));
    }

    QPointF ExperimentMonitoringTab::scatterDataViewPosForTests(double areaUm, double deformability) const
    {
        const QPointF chartPos = gate::toPixel(scatterViewport(), QPointF(areaUm, deformability));
        return QPointF(scatterplotView_->mapFromScene(scatterplotChart_->mapToScene(chartPos)));
    }

    QString ExperimentMonitoringTab::gateHandleToolTip(gate::HandleKind kind, bool dragging) const
    {
        const TuneField field = gate::handleField(kind);
        const QString value = QString::number(draft_.field(field).toDouble(), 'f', tuneFieldDecimals(field)) + tuneFieldUnit(field);
        return dragging ? tr("%1: %2").arg(tuneFieldLabel(field), value)
                        : tr("%1: %2 — drag to change").arg(tuneFieldLabel(field), value);
    }

    void ExperimentMonitoringTab::setGateHover(gate::HandleKind kind)
    {
        if (gateHover_ == kind) return;
        gateHover_ = kind;
        if (gateOverlay_) gateOverlay_->setHover(kind);
    }

    bool ExperimentMonitoringTab::showGateHover(QPointF viewPos)
    {
        const gate::HandleHit hit = gateHandleHit(viewPos);
        setGateHover(hit.kind);
        if (!hit) return false;
        scatterplotView_->setCursor(gate::isVerticalEdge(hit.kind)     ? Qt::SizeHorCursor
                                    : gate::isHorizontalEdge(hit.kind) ? Qt::SizeVerCursor
                                                                       : Qt::SizeAllCursor);
        QToolTip::showText(scatterplotView_->mapToGlobal(viewPos.toPoint()), gateHandleToolTip(hit.kind, false),
                           scatterplotView_);
        return true;
    }

    bool ExperimentMonitoringTab::beginGateEdit(const QPointF& viewPos)
    {
        gatePress_.reset();
        gateDragging_ = false;
        const GateGeometry geo = gateGeometry();
        if (!geo.spec.enabled) return false;
        const QPointF chartPos = chartPosFromView(viewPos);
        const gate::HandleHit hit = gate::hitTest(geo.spec, geo.contours(), geo.viewport, chartPos.x(), chartPos.y());
        if (!hit || !gateHandleEditable(hit.kind)) return false;
        gatePress_ = GatePress{hit, gate::beginDrag(hit, geo.spec, geo.lut, geo.viewport, chartPos)};
        return true;
    }

    void ExperimentMonitoringTab::onGateDragStarted()
    {
        if (!gatePress_) return;
        gateDragging_ = true;
        if (gateOverlay_) gateOverlay_->setDragging(gatePress_->hit.kind);
        QToolTip::hideText();
    }

    void ExperimentMonitoringTab::onGateDragMoved(const QPointF& viewPos)
    {
        if (!gateDragging_ || !gatePress_) return;
        const gate::HandleKind kind = gatePress_->hit.kind;
        if (!gateHandleEditable(kind)) return; // locked meanwhile (apply in flight)
        const TuneField field = gate::handleField(kind);
        const auto value = gate::dragValue(gatePress_->session, gateSpec(), lutSnapshot_.get(), scatterViewport(),
                                           chartPosFromView(viewPos), gateLimits(field));
        if (!value) return; // no E at this position: keep the last value
        writeGateField(field, *value);
        QToolTip::showText(scatterplotView_->mapToGlobal(viewPos.toPoint()), gateHandleToolTip(kind, true),
                           scatterplotView_);
    }

    void ExperimentMonitoringTab::onGateDragFinished(const QPointF& viewPos, bool completed)
    {
        gateDragging_ = false;
        gatePress_.reset();
        if (gateOverlay_) gateOverlay_->setDragging(gate::HandleKind::None);
        QToolTip::hideText();
        if (completed) showGateHover(viewPos);
        else setGateHover(gate::HandleKind::None);
    }

    bool ExperimentMonitoringTab::writeGateField(TuneField field, double value)
    {
        for (const auto& b : tuneBindings_) {
            if (b.field != field) continue;
            if (!b.widget->isEnabled() || draft_.applying()) return false;
            // The bound spin box: its valueChanged runs onTuneFieldEdited, the
            // very path of a typed edit (draft, `*` markers, footer).
            b.write(tuneFieldDecimals(field) == 0 ? QVariant(static_cast<int>(std::lround(value))) : QVariant(value));
            return true;
        }
        return false;
    }

} // namespace frontend
