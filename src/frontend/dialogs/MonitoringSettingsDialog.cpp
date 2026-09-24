#include "frontend/dialogs/MonitoringSettingsDialog.h"
#include "ui_MonitoringSettingsDialog.h"
#include "frontend/tabs/ExperimentMonitoringTab.h"

#include <QPushButton>

#include <cmath>

#include <spdlog/spdlog.h>

MonitoringSettingsDialog::MonitoringSettingsDialog(frontend::ExperimentMonitoringTab* monitoringTab, QWidget* parent)
    : QDialog(parent), ui(new Ui::MonitoringSettingsDialog), monitoringTab_(monitoringTab) {
    ui->setupUi(this);

    // Load current values
    if (monitoringTab_) {
        ui->kdeBandwidthSpin->setValue(monitoringTab_->kdeBandwidthFactor());
        ui->kdeIntervalSpin->setValue(monitoringTab_->kdeIntervalMs());
        ui->kdeCoreFractionSpin->setValue(static_cast<int>(std::lround(monitoringTab_->kdeCoreFraction() * 100.0)));
        ui->scatterXMinSpin->setValue(monitoringTab_->getScatterXMin());
        ui->scatterXMaxSpin->setValue(monitoringTab_->getScatterXMax());
        ui->scatterYMinSpin->setValue(monitoringTab_->getScatterYMin());
        ui->scatterYMaxSpin->setValue(monitoringTab_->getScatterYMax());
        ui->histogramXMinSpin->setValue(monitoringTab_->getHistogramXMin());
        ui->histogramXMaxSpin->setValue(monitoringTab_->getHistogramXMax());
        ui->histogramYMaxSpin->setValue(monitoringTab_->getHistogramYMax());
        ui->histogramBinWidthSpin->setValue(monitoringTab_->getHistogramBinWidth());
    } else {
        ui->kdeBandwidthSpin->setValue(frontend::ExperimentMonitoringTab::kKdeBandwidthFactorDefault);
        ui->kdeIntervalSpin->setValue(frontend::ExperimentMonitoringTab::kKdeIntervalMsDefault);
        ui->kdeCoreFractionSpin->setValue(static_cast<int>(std::lround(frontend::ExperimentMonitoringTab::kKdeCoreFractionDefault * 100.0)));
        ui->scatterXMinSpin->setValue(0.0);
        ui->scatterXMaxSpin->setValue(1000.0);
        ui->scatterYMinSpin->setValue(0.0);
        ui->scatterYMaxSpin->setValue(1.0);
        ui->histogramXMinSpin->setValue(15.0);
        ui->histogramXMaxSpin->setValue(25.0);
        ui->histogramYMaxSpin->setValue(100.0);
        ui->histogramBinWidthSpin->setValue(0.5);
    }

    connect(ui->buttons, &QDialogButtonBox::accepted, this, &MonitoringSettingsDialog::onOk);
    connect(ui->buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(ui->buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &MonitoringSettingsDialog::onApply);
    // Reference contour actions take effect immediately (they are not values to apply).
    const bool kdeOn = monitoringTab_ && monitoringTab_->kdeEnabled();
    ui->kdePinReferenceBtn->setEnabled(kdeOn && !monitoringTab_->lastKdeContours().empty());
    ui->kdeClearReferenceBtn->setEnabled(monitoringTab_ && monitoringTab_->hasKdeReference());
    connect(ui->kdePinReferenceBtn, &QPushButton::clicked, this, [this]() {
        if (!monitoringTab_) return;
        monitoringTab_->pinKdeReference();
        ui->kdeClearReferenceBtn->setEnabled(monitoringTab_->hasKdeReference());
    });
    connect(ui->kdeClearReferenceBtn, &QPushButton::clicked, this, [this]() {
        if (!monitoringTab_) return;
        monitoringTab_->clearKdeReference();
        ui->kdeClearReferenceBtn->setEnabled(false);
    });
}

MonitoringSettingsDialog::~MonitoringSettingsDialog() {
    delete ui;
}

void MonitoringSettingsDialog::onApply() {
    applySettings();
}

void MonitoringSettingsDialog::onOk() {
    applySettings();
    accept();
}

void MonitoringSettingsDialog::applySettings() {
    if (monitoringTab_) {
        monitoringTab_->setKdeBandwidthFactor(ui->kdeBandwidthSpin->value());
        monitoringTab_->setKdeIntervalMs(ui->kdeIntervalSpin->value());
        monitoringTab_->setKdeCoreFraction(ui->kdeCoreFractionSpin->value() / 100.0);
        monitoringTab_->setScatterXRange(ui->scatterXMinSpin->value(), ui->scatterXMaxSpin->value());
        monitoringTab_->setScatterYRange(ui->scatterYMinSpin->value(), ui->scatterYMaxSpin->value());
        monitoringTab_->setHistogramXRange(ui->histogramXMinSpin->value(), ui->histogramXMaxSpin->value());
        monitoringTab_->setHistogramYMax(ui->histogramYMaxSpin->value());
        monitoringTab_->setHistogramBinWidth(ui->histogramBinWidthSpin->value());
        monitoringTab_->refreshCharts();
        SPDLOG_INFO("Monitoring settings applied: KDE bandwidth factor={}, interval={} ms, scatter X=[{},{}] Y=[{},{}], histogram X=[{},{}] Y max={} binWidth={}",
                    ui->kdeBandwidthSpin->value(), ui->kdeIntervalSpin->value(),
                    ui->scatterXMinSpin->value(), ui->scatterXMaxSpin->value(),
                    ui->scatterYMinSpin->value(), ui->scatterYMaxSpin->value(),
                    ui->histogramXMinSpin->value(), ui->histogramXMaxSpin->value(),
                    ui->histogramYMaxSpin->value(), ui->histogramBinWidthSpin->value());
    }
}

