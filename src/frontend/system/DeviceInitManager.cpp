#include "frontend/system/DeviceInitManager.h"

#include <QTimer>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>

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

std::vector<backend::services::nanopositioner::Candidate> probeNanopositionerPortsInWorker(int baudRate, unsigned char deviceAddress, int preferredPort) {
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
    std::vector<backend::services::SerialPortInfo> inventory;
    const auto details = backend::services::enumerateSerialPorts();
    for (int port : ports) {
        backend::services::SerialPortInfo info;
        info.systemName = "COM" + std::to_string(port);
        for (const auto& detail : details) {
            if (detail.systemName == info.systemName) info = detail;
        }
        inventory.push_back(std::move(info));
    }
    return backend::services::nanopositioner::discover(inventory,
        [baudRate, deviceAddress](backend::services::nanopositioner::Vendor vendor,
                                  const backend::services::SerialPortInfo& port) {
            if (vendor != backend::services::nanopositioner::Vendor::Coremorrow) return false;
            return backend::services::AutofocusService::probeComPort(
                std::stoi(port.systemName.substr(3)), baudRate, deviceAddress);
        });
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

void DeviceInitManager::setNanopositionerTab(NanopositionerTab* tab) {
    if (nanopositionerTab_) disconnect(nanopositionerTab_, nullptr, this, nullptr);
    nanopositionerTab_ = tab;
    if (tab) connect(tab, &NanopositionerTab::discoveryRequested,
                     this, &DeviceInitManager::scheduleNanopositionerStep);
}

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
        emit cameraInitFinished(false, tr("No cameras found."));
        scheduleNanopositionerStep();
        return;
    }

    if (cameras.size() == 1) {
        const auto& cam = cameras[0];
        if (cam.cameraType == backend::services::CameraType::MindVision) {
            backend_.setMindVisionCameraSelection(cam.cameraIndex, cam.label);
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

    if (nanopositionerWatcher_ && nanopositionerWatcher_->isRunning()) return;
    nanopositionerTab_->setNanopositionerStatus(tr("Identifying nanopositioners across available ports..."));
    nanopositionerTab_->setDiscoveryRunning(true);

    if (!nanopositionerWatcher_) {
        nanopositionerWatcher_ = std::make_unique<QFutureWatcher<std::vector<backend::services::nanopositioner::Candidate>>>(this);
        connect(nanopositionerWatcher_.get(), &QFutureWatcher<std::vector<backend::services::nanopositioner::Candidate>>::finished,
                this, &DeviceInitManager::onNanopositionerProbeFinished);
    }
    QFuture<std::vector<backend::services::nanopositioner::Candidate>> future = QtConcurrent::run(probeNanopositionerPortsInWorker, baudRate, deviceAddress, preferredPort);
    nanopositionerWatcher_->setFuture(future);
}

void DeviceInitManager::onNanopositionerProbeFinished() {
    if (!nanopositionerWatcher_ || !nanopositionerWatcher_->isFinished() || !nanopositionerTab_) {
        return;
    }
    const auto candidates = nanopositionerWatcher_->result();
    nanopositionerTab_->setDiscoveryRunning(false);
    const bool anyIdentified = std::any_of(candidates.begin(), candidates.end(), [](const auto& candidate) {
        return !candidate.identifiedVendors.empty();
    });

    if (!anyIdentified) {
        if (nanopositionerRetryCount_ < NANOPOSITIONER_MAX_RETRIES) {
            ++nanopositionerRetryCount_;
            nanopositionerTab_->setNanopositionerStatus(
                tr("Searching for nanopositioner... (retry %1/%2)").arg(nanopositionerRetryCount_).arg(NANOPOSITIONER_MAX_RETRIES));
            nanopositionerStepTimer_->start(NANOPOSITIONER_RETRY_DELAY_MS);
        } else {
            nanopositionerTab_->setNanopositionerStatus(tr("No supported controller identified. OEABT protocol support is pending. Click Refresh to search again."));
            emit nanopositionerInitFinished(false);
        }
        return;
    }

    const auto* selected = backend::services::nanopositioner::uniqueMatch(candidates);
    if (!selected) {
        nanopositionerTab_->setNanopositionerStatus(tr("Multiple devices found; select one and click Connect."));
        emit nanopositionerInitFinished(false);
        return;
    }

    int port = std::stoi(selected->port.systemName.substr(3));
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
