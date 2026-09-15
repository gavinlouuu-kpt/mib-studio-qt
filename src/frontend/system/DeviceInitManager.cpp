#include "frontend/system/DeviceInitManager.h"
#include "backend/services/NanopositionerDiscovery.h"

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

std::vector<backend::nanopositioner::Endpoint>
probeNanopositionerEndpointsInWorker(backend::nanopositioner::Endpoint preferred,
                                     std::shared_ptr<std::atomic<bool>> stopped) {
    std::vector<backend::nanopositioner::Endpoint> validEndpoints;
    auto endpoints = backend::services::AutofocusService::availableEndpoints();
    std::stable_sort(
        endpoints.begin(), endpoints.end(), [&preferred](const auto& lhs, const auto& rhs) {
            const int lhsRank =
                (!preferred.persistentId.empty() && lhs.persistentId == preferred.persistentId) ? 0
                                                                                                : 1;
            const int rhsRank =
                (!preferred.persistentId.empty() && rhs.persistentId == preferred.persistentId) ? 0
                                                                                                : 1;
            return lhsRank == rhsRank ? lhs.displayName < rhs.displayName : lhsRank < rhsRank;
        });
    endpoints.erase(std::remove_if(endpoints.begin(), endpoints.end(), [&preferred](const auto& endpoint) {
        return preferred.backend != backend::nanopositioner::BackendKind::Auto &&
               endpoint.backend != preferred.backend;
    }), endpoints.end());
    for (auto& endpoint : endpoints) {
        endpoint.coremorBaudRate = preferred.coremorBaudRate;
        endpoint.coremorAddress = preferred.coremorAddress;
    }
    const auto candidates =
        backend::services::nanopositioner::discover(endpoints, [stopped](const auto& endpoint) {
            return !stopped->load() && backend::services::AutofocusService::probeEndpoint(endpoint);
        });
    for (const auto& candidate : candidates) {
        if (!candidate.identifiedVendors.empty()) validEndpoints.push_back(candidate.port);
    }
    return validEndpoints;
}

} // namespace

DeviceInitManager::DeviceInitManager(backend::AppBackend& backend, QObject* parent)
    : QObject(parent), backend_(backend) {
    cameraStepTimer_ = new QTimer(this);
    cameraStepTimer_->setSingleShot(true);
    connect(cameraStepTimer_, &QTimer::timeout, this, &DeviceInitManager::onCameraStepTimer);

    nanopositionerStepTimer_ = new QTimer(this);
    nanopositionerStepTimer_->setSingleShot(true);
    connect(nanopositionerStepTimer_, &QTimer::timeout, this,
            &DeviceInitManager::onNanopositionerStepTimer);
}

DeviceInitManager::~DeviceInitManager() {
    stop();
}

void DeviceInitManager::stop() {
    stopped_->store(true);
    cameraStepTimer_->stop();
    nanopositionerStepTimer_->stop();
    cameraStepScheduled_ = false;
    SPDLOG_INFO("DeviceInitManager: shutdown waiting for active discovery");
    // Workers own temporary hardware objects and never call the UI/backend.
    if (cameraWatcher_) cameraWatcher_->waitForFinished();
    if (nanopositionerWatcher_) nanopositionerWatcher_->waitForFinished();
    SPDLOG_INFO("DeviceInitManager: discovery stopped");
}

void DeviceInitManager::setNanopositionerTab(NanopositionerTab* tab) {
    if (nanopositionerTab_) disconnect(nanopositionerTab_, nullptr, this, nullptr);
    nanopositionerTab_ = tab;
    if (tab) connect(tab, &NanopositionerTab::discoveryRequested,
                     this, &DeviceInitManager::scheduleNanopositionerStep);
}

void DeviceInitManager::start() {
    if (stopped_->load()) return;
    cameraStepScheduled_ = true;
    cameraStepTimer_->start(400);
}

void DeviceInitManager::runCameraStep() {
    if (stopped_->load()) return;
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
    if (stopped_->load()) return;
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
        cameraWatcher_ =
            std::make_unique<QFutureWatcher<std::vector<backend::services::DiscoveredCamera>>>(
                this);
        connect(cameraWatcher_.get(),
                &QFutureWatcher<std::vector<backend::services::DiscoveredCamera>>::finished, this,
                &DeviceInitManager::onCameraDiscoveryFinished);
    }
    QFuture<std::vector<backend::services::DiscoveredCamera>> future =
        QtConcurrent::run(discoverCamerasInWorker);
    cameraWatcher_->setFuture(future);
}

void DeviceInitManager::onCameraDiscoveryFinished() {
    if (stopped_->load()) return;
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
                connectTab_->applyMindVisionSelection(cam.cameraIndex,
                                                      QString::fromStdString(cam.label));
            }
        } else {
            backend_.setHardwareCameraSelection(cam.interfaceIndex, cam.deviceIndex, cam.label);
            if (connectTab_) {
                connectTab_->applyCameraSelection(cam.interfaceIndex, cam.deviceIndex,
                                                  QString::fromStdString(cam.label));
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
    if (stopped_->load()) return;
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
    if (stopped_->load()) return;
    if (!nanopositionerTab_) {
        return;
    }
    if (backend_.autofocus().isConnected()) {
        return;
    }
    const auto preferredEndpoint = nanopositionerTab_->getConfiguredEndpoint();

    if (nanopositionerWatcher_ && nanopositionerWatcher_->isRunning()) return;
    nanopositionerTab_->setDiscoveryRunning(true);
    nanopositionerTab_->setNanopositionerStatus(tr("Identifying nanopositioners across available ports..."));

    if (!nanopositionerWatcher_) {
        nanopositionerWatcher_ =
            std::make_unique<QFutureWatcher<std::vector<backend::nanopositioner::Endpoint>>>(this);
        connect(nanopositionerWatcher_.get(),
                &QFutureWatcher<std::vector<backend::nanopositioner::Endpoint>>::finished, this,
                &DeviceInitManager::onNanopositionerProbeFinished);
    }
    QFuture<std::vector<backend::nanopositioner::Endpoint>> future =
        QtConcurrent::run(probeNanopositionerEndpointsInWorker, preferredEndpoint, stopped_);
    nanopositionerWatcher_->setFuture(future);
}

void DeviceInitManager::onNanopositionerProbeFinished() {
    if (stopped_->load()) return;
    if (!nanopositionerWatcher_ || !nanopositionerWatcher_->isFinished() || !nanopositionerTab_) {
        return;
    }
    auto validEndpoints = nanopositionerWatcher_->result();
    nanopositionerTab_->setDiscoveryRunning(false);

    if (validEndpoints.empty()) {
        if (nanopositionerRetryCount_ < NANOPOSITIONER_MAX_RETRIES) {
            ++nanopositionerRetryCount_;
            nanopositionerTab_->setNanopositionerStatus(
                tr("Searching for nanopositioner... (retry %1/%2)")
                    .arg(nanopositionerRetryCount_)
                    .arg(NANOPOSITIONER_MAX_RETRIES));
            nanopositionerStepTimer_->start(NANOPOSITIONER_RETRY_DELAY_MS);
        } else {
            nanopositionerTab_->setNanopositionerStatus(
                tr("Nanopositioner not found. Click Refresh to search again."));
            emit nanopositionerInitFinished(false);
        }
        return;
    }

    if (validEndpoints.size() != 1) {
        nanopositionerTab_->setNanopositionerStatus(
            tr("Multiple devices found; select one and click Connect."));
        emit nanopositionerInitFinished(false);
        return;
    }

    const auto endpoint = validEndpoints.front();
    bool success = backend_.autofocus().connect(endpoint);
    if (success) {
        nanopositionerTab_->applyAutoConnectResult(endpoint);
        SPDLOG_INFO("DeviceInitManager: auto-connected to nanopositioner on {}",
                    endpoint.systemPath);
        emit nanopositionerInitFinished(true);
    } else {
        nanopositionerTab_->setNanopositionerStatus(
            tr("Auto-connect failed on %1").arg(QString::fromStdString(endpoint.systemPath)));
        emit nanopositionerInitFinished(false);
    }
}

} // namespace frontend
