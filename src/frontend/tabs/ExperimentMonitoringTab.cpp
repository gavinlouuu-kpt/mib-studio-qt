#include "frontend/tabs/ExperimentMonitoringTab.h"
#include "ui_ExperimentMonitoringTab.h"

#include <QTimer>
#include <QLabel>
#include <QPixmap>
#include <QPainter>
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
#include <QFormLayout>
#include <QFrame>
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

#include "frontend/widgets/ZoomableChartView.h"
#include "backend/app/AppBackend.h"
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
        }
    }

    return reasons;
}

} // anonymous namespace

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
                                      tr("When enabled, a valid cell inside both ranges is reported as target group and fires the sort trigger."));
            auto* form = makeForm(box);
            addIntField(form, tr("Area minimum"), TuneField::TargetAreaMin, QStringLiteral("targetAreaMinSpin"), 0, 100000, 10);
            addIntField(form, tr("Area maximum"), TuneField::TargetAreaMax, QStringLiteral("targetAreaMaxSpin"), 0, 100000, 10);
            addDoubleField(form, tr("Deformability minimum"), TuneField::TargetDeformabilityMin, QStringLiteral("targetDeformMinSpin"), 0.0, 1.0, 0.01);
            addDoubleField(form, tr("Deformability maximum"), TuneField::TargetDeformabilityMax, QStringLiteral("targetDeformMaxSpin"), 0.0, 1.0, 0.01);
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
        scatterXAxis_->setRange(scatterXMin_, scatterXMax_);
        scatterYAxis_->setRange(scatterYMin_, scatterYMax_);

        scatterplotView_ = new ZoomableChartView(scatterplotChart_);
        scatterplotView_->setRenderHint(QPainter::Antialiasing);
        scatterplotView_->setDefaultRange(scatterXAxis_, scatterXMin_, scatterXMax_);
        scatterplotView_->setDefaultRange(scatterYAxis_, scatterYMin_, scatterYMax_);
        // Replace placeholder with actual chart view
        ui->gridLayout->removeWidget(ui->scatterplotViewPlaceholder);
        delete ui->scatterplotViewPlaceholder;
        ui->gridLayout->addWidget(scatterplotView_, 1, 0);

        // Load isoelastic curves overlay
        loadIsoelasticCurves();

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
        // Replace placeholder with actual chart view
        ui->gridLayout->removeWidget(ui->histogramViewPlaceholder);
        delete ui->histogramViewPlaceholder;
        ui->gridLayout->addWidget(histogramView_, 1, 1);
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

        const double conversionFactor = backend_.processing().getPixelToMicronFactor();
        const double areaConversionFactor = conversionFactor * conversionFactor;

        for (const auto &frame : validFrames)
        {
            if (frame.validation.isValid)
            {
                double areaMicrons = frame.validation.area * areaConversionFactor;
                double deform = frame.validation.deformability;
                if (frame.validation.isTargetGroup) {
                    targetGroupSeries_->append(areaMicrons, deform);
                } else {
                    scatterSeries_->append(areaMicrons, deform);
                }
            }
        }

        if (!scatterplotView_->isUserZoomed()) {
            scatterXAxis_->setRange(scatterXMin_, scatterXMax_);
            scatterYAxis_->setRange(scatterYMin_, scatterYMax_);
        }
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
                bool alreadyRoi = (frame.originalImage.cols == roi.w && frame.originalImage.rows == roi.h);
                cv::Mat roiOriginal = alreadyRoi ? frame.originalImage : frame.originalImage(cv::Rect(roi.x, roi.y, roi.w, roi.h));
                cv::Mat roiMask = alreadyRoi ? frame.processedImage : frame.processedImage(cv::Rect(roi.x, roi.y, roi.w, roi.h));
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
                bool alreadyRoi = (frame.originalImage.cols == roi.w && frame.originalImage.rows == roi.h);
                cv::Mat roiOriginal = alreadyRoi ? frame.originalImage : frame.originalImage(cv::Rect(roi.x, roi.y, roi.w, roi.h));
                cv::Mat roiMask = alreadyRoi ? frame.processedImage : frame.processedImage(cv::Rect(roi.x, roi.y, roi.w, roi.h));
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

    std::vector<std::vector<double>> ExperimentMonitoringTab::computeKDE(const std::vector<std::pair<double, double>> &points,
                                                                         int gridX, int gridY, double bandwidth) const
    {
        if (points.empty() || bandwidth <= 0.0)
        {
            return std::vector<std::vector<double>>(gridY, std::vector<double>(gridX, 0.0));
        }

        // Find data range
        double minX = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();

        for (const auto &p : points)
        {
            minX = std::min(minX, p.first);
            maxX = std::max(maxX, p.first);
            minY = std::min(minY, p.second);
            maxY = std::max(maxY, p.second);
        }

        if (minX >= maxX || minY >= maxY)
        {
            return std::vector<std::vector<double>>(gridY, std::vector<double>(gridX, 0.0));
        }

        // Add padding
        double rangeX = maxX - minX;
        double rangeY = maxY - minY;
        minX -= rangeX * 0.1;
        maxX += rangeX * 0.1;
        minY -= rangeY * 0.1;
        maxY += rangeY * 0.1;

        // Initialize density grid
        std::vector<std::vector<double>> density(gridY, std::vector<double>(gridX, 0.0));

        double stepX = (maxX - minX) / gridX;
        double stepY = (maxY - minY) / gridY;

        // Compute KDE using Gaussian kernel
        const double sqrt2pi = std::sqrt(2.0 * M_PI);
        const double bandwidth2 = bandwidth * bandwidth;

        for (int gy = 0; gy < gridY; ++gy)
        {
            double gridYVal = minY + (gy + 0.5) * stepY;
            for (int gx = 0; gx < gridX; ++gx)
            {
                double gridXVal = minX + (gx + 0.5) * stepX;

                double sum = 0.0;
                for (const auto &p : points)
                {
                    double dx = p.first - gridXVal;
                    double dy = p.second - gridYVal;
                    double dist2 = dx * dx + dy * dy;
                    // Gaussian kernel
                    sum += std::exp(-dist2 / (2.0 * bandwidth2)) / (sqrt2pi * bandwidth);
                }
                density[gy][gx] = sum / points.size();
            }
        }

        return density;
    }

    void ExperimentMonitoringTab::setKdeBandwidth(double bandwidth)
    {
        kdeBandwidth_ = bandwidth;
        // Trigger update to refresh scatterplot
        updateScatterplot(recentValidFrames_);
    }

    void ExperimentMonitoringTab::setKdeGridResolution(int resolution)
    {
        kdeGridResolution_ = resolution;
        // Trigger update to refresh scatterplot
        updateScatterplot(recentValidFrames_);
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
        QPixmap scatterPlotPixmap = scatterplotView_->grab();
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

        // Convert from RGBA to BGR
        cv::cvtColor(histogramImage, histogramImage, cv::COLOR_RGBA2BGR);
        cv::cvtColor(scatterPlotImage, scatterPlotImage, cv::COLOR_RGBA2BGR);

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
        QPixmap pixmap = chartView->grab();
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
        
        // Convert from RGBA to BGR
        cv::Mat bgrMat;
        cv::cvtColor(mat, bgrMat, cv::COLOR_RGBA2BGR);

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

} // namespace frontend
