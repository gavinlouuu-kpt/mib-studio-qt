#include "frontend/tabs/ConnectTab.h"
#include "ui_ConnectTab.h"

#include <QMessageBox>
#include <QSignalBlocker>
#include <QVariant>

#include <spdlog/spdlog.h>

#include "backend/app/AppBackend.h"
#include "backend/camera/common/ICamera.h"
#include "backend/services/CameraControlService.h"
#include "backend/services/CaptureService.h"
#include "frontend/dialogs/MockConfigDialog.h"
#include "frontend/system/DeviceInitManager.h"
#include "backend/camera/mock/MockCamera.h"

#include <filesystem>
#include <algorithm>
#include <chrono>

namespace {

struct CameraSelection {
    backend::services::CameraType type = backend::services::CameraType::EGrabber;
    int ifIndex = -1;
    int devIndex = -1;
    int cameraIndex = -1;
};

QVariant toVariant(const CameraSelection& d) {
    switch (d.type) {
    case backend::services::CameraType::MindVision:
        return QVariant::fromValue<QString>(QString("mindvision:%1").arg(d.cameraIndex));
    case backend::services::CameraType::EGrabber:
    default:
        return QVariant::fromValue<QString>(QString("egrabber:%1:%2").arg(d.ifIndex).arg(d.devIndex));
    }
}

bool fromVariant(const QVariant& v, CameraSelection& out) {
    const auto s = v.toString();
    const auto parts = s.split(':');
    if (parts.isEmpty()) return false;
    if (parts[0] == "mindvision") {
        if (parts.size() != 2) return false;
        bool ok = false;
        out.type = backend::services::CameraType::MindVision;
        out.cameraIndex = parts[1].toInt(&ok);
        return ok;
    }
    if (parts[0] == "egrabber") {
        if (parts.size() != 3) return false;
        bool ok1 = false, ok2 = false;
        out.type = backend::services::CameraType::EGrabber;
        out.ifIndex = parts[1].toInt(&ok1);
        out.devIndex = parts[2].toInt(&ok2);
        return ok1 && ok2;
    }
    return false;
}

} // namespace

namespace frontend {

ConnectTab::ConnectTab(backend::AppBackend& backend, QWidget* parent)
    : QWidget(parent), ui(new Ui::ConnectTab), backend_(backend) {
    ui->setupUi(this);

    connect(ui->refreshBtn, &QPushButton::clicked, this, &ConnectTab::onRefresh);
    connect(ui->connectBtn, &QPushButton::clicked, this, &ConnectTab::onConnect);
    connect(ui->mockBtn, &QPushButton::clicked, this, &ConnectTab::onConfigureMock);

    // Combo index 0 = EveryFrame, 1 = LatestFrame (matches the .ui item order).
    {
        const QSignalBlocker blocker(ui->deliveryModeCombo);
        ui->deliveryModeCombo->setCurrentIndex(
            backend_.capture().activeDeliveryMode() == camera::common::FrameDeliveryMode::LatestFrame ? 1 : 0);
    }
    connect(ui->deliveryModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ConnectTab::onDeliveryModeComboChanged);

    onRefresh();
}

ConnectTab::~ConnectTab() {
    delete ui;
}

void ConnectTab::onRefresh() {
    populateDevices();
}

void ConnectTab::tryAutoConnect()
{
    if (initManager_) {
        initManager_->runCameraStep();
        return;
    }

    // Fallback when no DeviceInitManager: run discovery on UI thread (may block)
    if (backend_.capture().isRunning())
    {
        SPDLOG_INFO("ConnectTab: auto-connect skipped (capture running)");
        return;
    }
    if (backend_.isCameraConfigured())
    {
        SPDLOG_INFO("ConnectTab: auto-connect skipped (camera already configured)");
        return;
    }

    auto &cc = backend_.cameraControl();
    const auto cameras = cc.discoverCameras();
    const auto mindVisionCameras = cc.discoverMindVisionCameras();

    SPDLOG_INFO("ConnectTab: auto-connect discovery found {} eGrabber camera(s) and {} MindVision camera(s)",
                cameras.size(), mindVisionCameras.size());

    if (cameras.empty() && mindVisionCameras.empty())
    {
        ui->statusLabel->setText(tr("No cameras found."));
        emit noCamerasFound();
        return;
    }

    const std::size_t totalCameras = cameras.size() + mindVisionCameras.size();
    if (totalCameras == 1)
    {
        if (!cameras.empty())
        {
            const auto &cam = cameras[0];
            backend_.setHardwareCameraSelection(cam.interfaceIndex, cam.deviceIndex, cam.label);
            applyCameraSelection(cam.interfaceIndex, cam.deviceIndex, QString::fromStdString(cam.label));
        }
        else
        {
            const auto &cam = mindVisionCameras[0];
            backend_.setMindVisionCameraSelection(cam.cameraIndex, cam.label);
            applyMindVisionSelection(cam.cameraIndex, QString::fromStdString(cam.label));
        }
        return;
    }

    ui->statusLabel->setText(tr("Multiple cameras found; select one and click Connect."));
}

void ConnectTab::applyCameraSelection(int interfaceIndex, int deviceIndex, const QString& label)
{
    (void)interfaceIndex;
    (void)deviceIndex;
    ui->statusLabel->setText(tr("Connected to %1 (not capturing)").arg(label));
    ui->tabWidget->setCurrentIndex(0);
    for (int i = 0; i < ui->cameraList->count(); ++i)
    {
        auto *item = ui->cameraList->item(i);
        if (item && item->text() == label)
        {
            ui->cameraList->setCurrentRow(i);
            break;
        }
    }
    emit connected();
}

void ConnectTab::applyMindVisionSelection(int cameraIndex, const QString& label)
{
    (void)cameraIndex;
    ui->statusLabel->setText(tr("Connected to %1 (not capturing)").arg(label));
    ui->tabWidget->setCurrentIndex(1);
    for (int i = 0; i < ui->mindVisionList->count(); ++i)
    {
        auto *item = ui->mindVisionList->item(i);
        if (item && item->text() == label)
        {
            ui->mindVisionList->setCurrentRow(i);
            break;
        }
    }
    emit connected();
}

void ConnectTab::reportNoCameras()
{
    ui->statusLabel->setText(tr("No cameras found."));
    emit noCamerasFound();
}

void ConnectTab::reportMultipleCameras()
{
    ui->statusLabel->setText(tr("Multiple cameras found; select one and click Connect."));
}

void ConnectTab::populateDevices() {
    ui->framegrabberList->clear();
    ui->cameraList->clear();
    ui->mindVisionList->clear();

    auto& cc = backend_.cameraControl();
    
    // Populate framegrabbers tab
    const auto framegrabbers = cc.discoverFramegrabbers();
    for (const auto& fg : framegrabbers) {
        auto* item = new QListWidgetItem(QString::fromStdString(fg.label));
        item->setData(Qt::UserRole, toVariant(CameraSelection{backend::services::CameraType::EGrabber, fg.interfaceIndex, fg.deviceIndex, -1}));
        ui->framegrabberList->addItem(item);
    }
    
    if (ui->framegrabberList->count() > 0) {
        ui->framegrabberList->setCurrentRow(0);
    }
    
    // Populate eGrabber cameras tab
    const auto cameras = cc.discoverCameras();
    for (const auto& cam : cameras) {
        auto* item = new QListWidgetItem(QString::fromStdString(cam.label));
        item->setData(Qt::UserRole, toVariant(CameraSelection{cam.cameraType, cam.interfaceIndex, cam.deviceIndex, cam.cameraIndex}));
        ui->cameraList->addItem(item);
    }
    
    if (ui->cameraList->count() > 0) {
        ui->cameraList->setCurrentRow(0);
    }

    // Populate MindVision cameras tab
    const auto mindVisionCameras = cc.discoverMindVisionCameras();
    for (const auto& cam : mindVisionCameras) {
        auto* item = new QListWidgetItem(QString::fromStdString(cam.label));
        item->setData(Qt::UserRole, toVariant(CameraSelection{cam.cameraType, cam.interfaceIndex, cam.deviceIndex, cam.cameraIndex}));
        ui->mindVisionList->addItem(item);
    }

    if (ui->mindVisionList->count() > 0) {
        ui->mindVisionList->setCurrentRow(0);
    }

    ui->statusLabel->setText(QString("Found %1 framegrabber(s), %2 eGrabber camera(s), %3 MindVision camera(s)")
                         .arg(ui->framegrabberList->count())
                         .arg(ui->cameraList->count())
                         .arg(ui->mindVisionList->count()));
    SPDLOG_INFO("ConnectTab: refreshed, {} framegrabber(s), {} eGrabber camera(s), {} MindVision camera(s) listed",
                ui->framegrabberList->count(), ui->cameraList->count(), ui->mindVisionList->count());
}

void ConnectTab::onConnect() {
    // Guard: Cannot change camera connection while camera is running
    if (backend_.capture().isRunning()) {
        QMessageBox::warning(this, tr("Connect Device"),
                             tr("Cannot change camera connection while camera is running. Please stop the camera first."));
        return;
    }

    // Get the current tab and its list widget
    QListWidget* currentList = nullptr;
    QString deviceType;
    
    int currentTab = ui->tabWidget->currentIndex();
    if (currentTab == 0) {
        currentList = ui->cameraList;
        deviceType = tr("eGrabber camera");
    } else if (currentTab == 1) {
        currentList = ui->mindVisionList;
        deviceType = tr("MindVision camera");
    } else if (currentTab == 2) {
        currentList = ui->framegrabberList;
        deviceType = tr("framegrabber");
    } else {
        QMessageBox::information(this, tr("Connect Device"), tr("Please select a device from the list."));
        return;
    }
    
    const auto* item = currentList->currentItem();
    if (!item) {
        QMessageBox::information(this, tr("Connect Device"), 
                                 tr("Please select a %1 from the list.").arg(deviceType));
        return;
    }

    CameraSelection idx{};
    if (!fromVariant(item->data(Qt::UserRole), idx)) {
        QMessageBox::warning(this, tr("Connect Device"), tr("Internal error: invalid selection."));
        return;
    }

    const QString label = item->text();
    if (idx.type == backend::services::CameraType::MindVision) {
        SPDLOG_INFO("ConnectTab: selecting MindVision camera {} ({})",
                    label.toStdString(), idx.cameraIndex);
        backend_.setMindVisionCameraSelection(idx.cameraIndex, label.toStdString());
    } else {
        SPDLOG_INFO("ConnectTab: selecting hardware device {} ({}:{})",
                    label.toStdString(), idx.ifIndex, idx.devIndex);
        backend_.setHardwareCameraSelection(idx.ifIndex, idx.devIndex, label.toStdString());
    }
    ui->statusLabel->setText(tr("Connected to %1 (not capturing)").arg(label));
    emit connected();
}

void ConnectTab::onDeliveryModeComboChanged(int index) {
    const auto mode = index == 1 ? camera::common::FrameDeliveryMode::LatestFrame
                                 : camera::common::FrameDeliveryMode::EveryFrame;

    // First (and only) frontend caller of CaptureService::setConfig: buffer
    // sizing stays at the service defaults, only the delivery mode is
    // user-selectable here.
    backend::services::CaptureService::Config cfg{};
    cfg.deliveryMode = mode;
    backend_.capture().setConfig(cfg);

    const QString modeLabel = mode == camera::common::FrameDeliveryMode::LatestFrame
                                  ? tr("Latest Frame")
                                  : tr("Every Frame");
    if (backend_.capture().isRunning()) {
        // Real backends only honor the mode at start(); never restart silently.
        ui->statusLabel->setText(tr("Delivery mode set to %1 — applies at the next capture start.").arg(modeLabel));
    } else {
        ui->statusLabel->setText(tr("Delivery mode set to %1.").arg(modeLabel));
    }
    SPDLOG_INFO("ConnectTab: delivery mode set to {} (capture {})",
                camera::common::toString(mode),
                backend_.capture().isRunning() ? "running, applies at next start" : "stopped");
    emit deliveryModeChanged(mode);
}

void ConnectTab::setDeliveryMode(camera::common::FrameDeliveryMode mode) {
    const int index = mode == camera::common::FrameDeliveryMode::LatestFrame ? 1 : 0;
    if (ui->deliveryModeCombo->currentIndex() == index) {
        return;
    }
    // Triggers onDeliveryModeComboChanged, i.e. the same apply + persist path
    // as a user selection.
    ui->deliveryModeCombo->setCurrentIndex(index);
}

void ConnectTab::syncDeliveryMode(camera::common::FrameDeliveryMode mode) {
    const QSignalBlocker blocker(ui->deliveryModeCombo);
    ui->deliveryModeCombo->setCurrentIndex(
        mode == camera::common::FrameDeliveryMode::LatestFrame ? 1 : 0);
}

void ConnectTab::onConfigureMock() {
    // Guard: Cannot change camera connection while camera is running
    if (backend_.capture().isRunning()) {
        QMessageBox::warning(this, tr("Configure Mock Camera"),
                             tr("Cannot change camera connection while camera is running. Please stop the camera first."));
        return;
    }

    frontend::MockConfigDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    camera::mock::MockCameraOptions options{};
#ifdef _WIN32
    options.folder = std::filesystem::path(dlg.folderPath().toStdWString());
#else
    options.folder = std::filesystem::path(dlg.folderPath().toStdString());
#endif
    // Clamp FPS to >= 1 without relying on std::max (avoid Windows macros)
    double fps = dlg.framesPerSecond();
    if (fps < 1.0) fps = 1.0;
    const auto micros = static_cast<long long>(1'000'000.0 / fps);
    options.frameInterval = std::chrono::microseconds(micros);
    options.loopFiles = true;

    backend_.configureMockCamera(options);
    ui->statusLabel->setText(tr("Mock camera configured (not capturing)"));
    SPDLOG_INFO("ConnectTab: mock camera configured ({}, ~{} fps)",
                options.folder.string(),
                options.frameInterval.count() > 0 ? 1'000'000.0 / options.frameInterval.count() : 0.0);
    emit connected();
}

} // namespace frontend
