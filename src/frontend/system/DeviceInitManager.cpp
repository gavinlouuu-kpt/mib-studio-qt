#include "frontend/system/DeviceInitManager.h"

#include <QMetaObject>

#include <spdlog/spdlog.h>

#include "backend/app/AppBackend.h"
#include "backend/services/CameraControlService.h"
#include "frontend/tabs/ConnectTab.h"
#include "frontend/tabs/NanopositionerTab.h"

namespace frontend {

using Coordinator = backend::discovery::StartupDiscoveryCoordinator;

DeviceInitManager::DeviceInitManager(backend::AppBackend& backend, QObject* parent)
    : QObject(parent), backend_(backend), coordinator_(backend.startupDiscovery()) {
    // Decision actions (selection/connection hooks and outcome delivery) run
    // on the UI thread: AppBackend's selection setters are read by widgets.
    // `alive_` guards against a post that races this object's destruction.
    auto alive = alive_;
    coordinator_.setExecutor([this, alive](std::function<void()> fn) {
        if (!alive->load()) return;
        QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
    });
    coordinator_.setCameraListener([this](const Coordinator::CameraOutcome& outcome) {
        onCameraOutcome(outcome);
    });
    coordinator_.setNanopositionerListener(
        [this](const Coordinator::NanopositionerOutcome& outcome) { onNanopositionerOutcome(outcome); });
    coordinator_.setPreferredNanopositionerHook([this]() -> std::optional<backend::nanopositioner::Endpoint> {
        if (!nanopositionerTab_) return std::nullopt;
        return nanopositionerTab_->getConfiguredEndpoint();
    });
}

DeviceInitManager::~DeviceInitManager() {
    stop();
    // The coordinator outlives this adapter (AppBackend owns it): detach every
    // callback that references `this` before the QObject goes away.
    alive_->store(false);
    coordinator_.setExecutor({});
    coordinator_.setCameraListener({});
    coordinator_.setNanopositionerListener({});
    coordinator_.setPreferredNanopositionerHook({});
}

void DeviceInitManager::stop() {
    if (stopped_) return;
    stopped_ = true;
    SPDLOG_INFO("DeviceInitManager: stopping startup discovery");
    coordinator_.stop();
    if (nanopositionerTab_) nanopositionerTab_->setDiscoveryRunning(false);
}

void DeviceInitManager::setNanopositionerTab(NanopositionerTab* tab) {
    if (nanopositionerTab_) disconnect(nanopositionerTab_, nullptr, this, nullptr);
    nanopositionerTab_ = tab;
    if (tab) connect(tab, &NanopositionerTab::discoveryRequested, this,
                     &DeviceInitManager::runNanopositionerStep);
}

void DeviceInitManager::start() {
    if (stopped_) return;
    coordinator_.start();
}

void DeviceInitManager::runCameraStep() {
    if (stopped_) return;
    if (!coordinator_.runCameraStep()) {
        SPDLOG_INFO("DeviceInitManager: camera step refused (running, capturing or configured)");
    }
}

void DeviceInitManager::runNanopositionerStep() {
    if (stopped_) return;
    if (!coordinator_.runNanopositionerStep()) {
        SPDLOG_INFO("DeviceInitManager: nanopositioner step refused (running or connected)");
    }
}

void DeviceInitManager::onCameraOutcome(const Coordinator::CameraOutcome& outcome) {
    using Kind = Coordinator::CameraOutcome::Kind;
    if (stopped_) return;
    if (outcome.kind == Kind::Started) {
        if (connectTab_) connectTab_->showDiscoveryStarted();
        return;
    }
    if (outcome.kind != Kind::Skipped && connectTab_) {
        connectTab_->showDiscoveryResults(outcome.snapshot);
    }
    switch (outcome.kind) {
    case Kind::Started:
    case Kind::Skipped:
        break;
    case Kind::Selected: {
        const QString label = QString::fromStdString(outcome.device ? outcome.device->displayName : "");
        SPDLOG_INFO("DeviceInitManager: camera discovery selected '{}'", label.toStdString());
        if (connectTab_ && outcome.device && outcome.device->camera) {
            const auto& cam = *outcome.device->camera;
            if (cam.cameraType == backend::services::CameraType::MindVision) {
                connectTab_->applyMindVisionSelection(cam.cameraIndex, QString::fromStdString(cam.label));
            } else {
                connectTab_->applyCameraSelection(cam.interfaceIndex, cam.deviceIndex,
                                                  QString::fromStdString(cam.label));
            }
        }
        emit cameraInitFinished(true, label);
        break;
    }
    case Kind::NoneFound:
        SPDLOG_INFO("DeviceInitManager: camera discovery found no cameras");
        if (connectTab_) connectTab_->reportNoCameras();
        emit cameraInitFinished(false, tr("No cameras found."));
        break;
    case Kind::RequireSelection:
        if (connectTab_) connectTab_->reportMultipleCameras();
        emit cameraInitFinished(false, tr("Multiple cameras found; select one and click Connect."));
        break;
    case Kind::Incomplete:
    case Kind::Refused: {
        const QString message = tr("Camera discovery incomplete: %1")
                                    .arg(QString::fromStdString(outcome.message));
        SPDLOG_WARN("DeviceInitManager: {}", message.toStdString());
        if (connectTab_) connectTab_->reportDiscoveryProblem(message);
        emit cameraInitFinished(false, message);
        break;
    }
    }
}

void DeviceInitManager::onNanopositionerOutcome(const Coordinator::NanopositionerOutcome& outcome) {
    using Kind = Coordinator::NanopositionerOutcome::Kind;
    if (stopped_ || !nanopositionerTab_) return;
    switch (outcome.kind) {
    case Kind::Started:
        nanopositionerTab_->setDiscoveryRunning(true);
        nanopositionerTab_->setNanopositionerStatus(
            tr("Identifying nanopositioners across available ports..."));
        return;
    case Kind::Searching:
        // attempt N of M means retry N-1 of M-1 in the pre-#419 wording.
        nanopositionerTab_->setNanopositionerStatus(tr("Searching for nanopositioner... (retry %1/%2)")
                                                        .arg(outcome.attempt - 1)
                                                        .arg(outcome.maxAttempts - 1));
        return;
    case Kind::Skipped:
        return;
    default:
        break;
    }

    nanopositionerTab_->showDiscoveryCandidates(outcome.snapshot);
    nanopositionerTab_->setDiscoveryRunning(false);
    switch (outcome.kind) {
    case Kind::Connected:
        if (outcome.endpoint) {
            nanopositionerTab_->applyAutoConnectResult(*outcome.endpoint);
            SPDLOG_INFO("DeviceInitManager: auto-connected to nanopositioner on {}",
                        outcome.endpoint->systemPath);
        }
        emit nanopositionerInitFinished(true);
        break;
    case Kind::ConnectFailed:
        nanopositionerTab_->setNanopositionerStatus(
            tr("Auto-connect failed on %1")
                .arg(QString::fromStdString(outcome.endpoint ? outcome.endpoint->systemPath : "?")));
        emit nanopositionerInitFinished(false);
        break;
    case Kind::NotFound:
        nanopositionerTab_->setNanopositionerStatus(
            tr("Nanopositioner not found. Click Refresh to search again."));
        emit nanopositionerInitFinished(false);
        break;
    case Kind::RequireSelection:
        nanopositionerTab_->setNanopositionerStatus(
            tr("Multiple devices found; select one and click Connect."));
        emit nanopositionerInitFinished(false);
        break;
    case Kind::Incomplete:
        nanopositionerTab_->setNanopositionerStatus(
            tr("Nanopositioner discovery incomplete: %1. Click Refresh to search again.")
                .arg(QString::fromStdString(outcome.message)));
        emit nanopositionerInitFinished(false);
        break;
    case Kind::Started:
    case Kind::Searching:
    case Kind::Skipped:
        break;
    }
}

} // namespace frontend
