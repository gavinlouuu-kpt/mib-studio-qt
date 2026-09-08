#include "frontend/system/DeviceInitManager.h"

#include <QTimer>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QStringList>

#include <algorithm>

#include <spdlog/spdlog.h>

#include "backend/app/AppBackend.h"
#include "backend/services/CameraControlService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/AutofocusService.h"
#include "backend/app/Tools.h"
#include "frontend/tabs/ConnectTab.h"
#include "frontend/tabs/NanopositionerTab.h"

namespace frontend {

namespace {

std::vector<backend::services::DiscoveredCamera> discoverCamerasInWorker() {
    backend::services::CameraControlService cc;
    return cc.discoverAllCameras();
}

std::vector<int> probeNanopositionerPortsInWorker(int baudRate, unsigned char deviceAddress, int preferredPort) {
    std::vector<int> validPorts;
    std::vector<int> ports = backend::Tools::availableComPortNumbers();
    if (preferredPort > 0 && std::find(ports.begin(), ports.end(), preferredPort) == ports.end()) {
        ports.insert(ports.begin(), preferredPort);
    }
    std::stable_sort(ports.begin(), ports.end(), [preferredPort](int lhs, int rhs) {
        const int lhsRank = (lhs == preferredPort) ? 0 : 1;
        const int rhsRank = (rhs == preferredPort) ? 0 : 1;
        return lhsRank == rhsRank ? lhs < rhs : lhsRank < rhsRank;
    });
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    for (int port : ports) {
        if (backend::services::AutofocusService::probeComPort(port, baudRate, deviceAddress)) {
            validPorts.push_back(port);
        }
    }
    return validPorts;
}

} // namespace

DeviceInitManager::DeviceInitManager(backend::AppBackend& backend, QObject* parent)
    : QObject(parent), backend_(backend) {
    cameraStepTimer_ = new QTimer(this);
    cameraStepTimer_->setSingleShot(true);
    connect(cameraStepTimer_, &QTimer::timeout, this, &DeviceInitManager::onCameraStepTimer);

    nanopositionerStepTimer_ = new QTimer(this);
    nanopositionerStepTimer_->setSingleShot(true);
    connect(nanopositionerStepTimer_, &QTimer::timeout, this, &DeviceInitManager::onNanopositionerStepTimer);
}

DeviceInitManager::~DeviceInitManager() = default;

void DeviceInitManager::start() {
    cameraStepScheduled_ = true;
    cameraStepTimer_->start(400);
}

void DeviceInitManager::runCameraStep() {
    if (cameraStepRunning_) {
        SPDLOG_INFO("DeviceInitManager: camera step already running, skipping");
        return;
    }
    if (backend_.capture().isRunning()) {
        SPDLOG_INFO("DeviceInitManager: camera step skipped (capture running)");
        return;
    }
    if (backend_.isCameraConfigured()) {
        SPDLOG_INFO("DeviceInitManager: camera step skipped (already configured)");
        return;
    }
    runCameraDiscoveryInWorker();
}

void DeviceInitManager::onCameraStepTimer() {
    if (!cameraStepScheduled_) {
        return;
    }
    if (backend_.capture().isRunning() || backend_.isCameraConfigured()) {
        scheduleNanopositionerStep();
        return;
    }
    runCameraDiscoveryInWorker();
}

void DeviceInitManager::runCameraDiscoveryInWorker() {
    cameraStepRunning_ = true;
    if (!cameraWatcher_) {
        cameraWatcher_ = std::make_unique<QFutureWatcher<std::vector<backend::services::DiscoveredCamera>>>(this);
        connect(cameraWatcher_.get(), &QFutureWatcher<std::vector<backend::services::DiscoveredCamera>>::finished,
                this, &DeviceInitManager::onCameraDiscoveryFinished);
    }
    QFuture<std::vector<backend::services::DiscoveredCamera>> future = QtConcurrent::run(discoverCamerasInWorker);
    cameraWatcher_->setFuture(future);
}

void DeviceInitManager::onCameraDiscoveryFinished() {
    cameraStepRunning_ = false;
    if (!cameraWatcher_ || !cameraWatcher_->isFinished()) {
        return;
    }
    std::vector<backend::services::DiscoveredCamera> cameras = cameraWatcher_->result();

    SPDLOG_INFO("DeviceInitManager: camera discovery found {} camera(s)", cameras.size());

    if (cameras.empty()) {
        if (connectTab_) {
            connectTab_->reportNoCameras();
        }
        // Distinguish "no hardware" from "support compiled out": official CI
        // builds ship without the vendor SDKs, so discovery finding nothing
        // is expected there and must not read as a hardware fault (#338).
        QString message = tr("No cameras found.");
        QStringList missing;
        if (!backend::services::CameraControlService::eGrabberSupported()) {
            missing << QStringLiteral("EGrabber");
        }
        if (!backend::services::CameraControlService::mindVisionSupported()) {
            missing << QStringLiteral("MindVision");
        }
        if (!missing.isEmpty()) {
            message += QLatin1Char(' ') +
                       tr("Note: this build does not include %1 support; such cameras cannot be detected.")
                           .arg(missing.join(QStringLiteral(" or ")));
        }
        emit cameraInitFinished(false, message);
        scheduleNanopositionerStep();
        return;
    }

    if (cameras.size() == 1) {
        const auto& cam = cameras[0];
        if (cam.cameraType == backend::services::CameraType::MindVision) {
            std::string error;
            if (!backend_.setMindVisionCameraSelection(cam.cameraIndex, cam.label, &error)) {
                emit cameraInitFinished(false, QString::fromStdString(error));
                scheduleNanopositionerStep();
                return;
            }
            if (connectTab_) {
                connectTab_->applyMindVisionSelection(cam.cameraIndex, QString::fromStdString(cam.label));
            }
        } else {
            backend_.setHardwareCameraSelection(cam.interfaceIndex, cam.deviceIndex, cam.label);
            if (connectTab_) {
                connectTab_->applyCameraSelection(cam.interfaceIndex, cam.deviceIndex, QString::fromStdString(cam.label));
            }
        }
        emit cameraInitFinished(true, QString::fromStdString(cam.label));
    } else {
        if (connectTab_) {
            connectTab_->reportMultipleCameras();
        }
        emit cameraInitFinished(false, tr("Multiple cameras found; select one and click Connect."));
    }
    scheduleNanopositionerStep();
}

void DeviceInitManager::scheduleNanopositionerStep() {
    cameraStepScheduled_ = false;
    if (!nanopositionerTab_) {
        return;
    }
    if (backend_.autofocus().isConnected()) {
        return;
    }
    nanopositionerRetryCount_ = 0;
    nanopositionerStepTimer_->start(0);
}

void DeviceInitManager::onNanopositionerStepTimer() {
    if (!nanopositionerTab_) {
        return;
    }
    if (backend_.autofocus().isConnected()) {
        return;
    }
    int baudRate = nanopositionerTab_->getBaudRate();
    unsigned char deviceAddress = nanopositionerTab_->getDeviceAddress();
    int preferredPort = nanopositionerTab_->getConfiguredComPort();

    if (preferredPort > 0 && nanopositionerRetryCount_ == 0) {
        nanopositionerTab_->setNanopositionerStatus(tr("Checking saved nanopositioner port COM%1...").arg(preferredPort));
        if (backend::services::AutofocusService::probeComPort(preferredPort, baudRate, deviceAddress) &&
            backend_.autofocus().connect(preferredPort, baudRate, deviceAddress)) {
            nanopositionerTab_->applyAutoConnectResult(preferredPort);
            SPDLOG_INFO("DeviceInitManager: auto-connected to nanopositioner on saved COM{}", preferredPort);
            emit nanopositionerInitFinished(true);
            return;
        }
        SPDLOG_WARN("DeviceInitManager: saved nanopositioner COM{} did not validate; scanning all ports", preferredPort);
    }

    if (!nanopositionerWatcher_) {
        nanopositionerWatcher_ = std::make_unique<QFutureWatcher<std::vector<int>>>(this);
        connect(nanopositionerWatcher_.get(), &QFutureWatcher<std::vector<int>>::finished,
                this, &DeviceInitManager::onNanopositionerProbeFinished);
    }
    QFuture<std::vector<int>> future = QtConcurrent::run(probeNanopositionerPortsInWorker, baudRate, deviceAddress, preferredPort);
    nanopositionerWatcher_->setFuture(future);
}

void DeviceInitManager::onNanopositionerProbeFinished() {
    if (!nanopositionerWatcher_ || !nanopositionerWatcher_->isFinished() || !nanopositionerTab_) {
        return;
    }
    std::vector<int> validPorts = nanopositionerWatcher_->result();

    if (validPorts.empty()) {
        if (nanopositionerRetryCount_ < NANOPOSITIONER_MAX_RETRIES) {
            ++nanopositionerRetryCount_;
            nanopositionerTab_->setNanopositionerStatus(
                tr("Searching for nanopositioner... (retry %1/%2)").arg(nanopositionerRetryCount_).arg(NANOPOSITIONER_MAX_RETRIES));
            nanopositionerStepTimer_->start(NANOPOSITIONER_RETRY_DELAY_MS);
        } else {
            nanopositionerTab_->setNanopositionerStatus(tr("Nanopositioner not found. Click Refresh to search again."));
            emit nanopositionerInitFinished(false);
        }
        return;
    }

    if (validPorts.size() != 1) {
        nanopositionerTab_->setNanopositionerStatus(tr("Multiple devices found; select one and click Connect."));
        emit nanopositionerInitFinished(false);
        return;
    }

    int port = validPorts[0];
    int baudRate = nanopositionerTab_->getBaudRate();
    unsigned char deviceAddress = nanopositionerTab_->getDeviceAddress();
    bool success = backend_.autofocus().connect(port, baudRate, deviceAddress);
    if (success) {
        nanopositionerTab_->applyAutoConnectResult(port);
        SPDLOG_INFO("DeviceInitManager: auto-connected to nanopositioner on COM{}", port);
        emit nanopositionerInitFinished(true);
    } else {
        nanopositionerTab_->setNanopositionerStatus(tr("Auto-connect failed on COM%1").arg(port));
        emit nanopositionerInitFinished(false);
    }
}

} // namespace frontend
