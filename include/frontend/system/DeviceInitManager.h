#pragma once

#include "backend/discovery/StartupDiscoveryCoordinator.h"
#include "backend/nanopositioner/INanopositionerBackend.h"

#include <QObject>

#include <atomic>
#include <memory>

namespace backend {
class AppBackend;
}
namespace frontend {
class ConnectTab;
class NanopositionerTab;
}

namespace frontend {

/**
 * Qt adapter over the backend startup discovery policy (issue #419, ADR 0005).
 *
 * All scheduling, retries, cancellation and the select/connect decisions live
 * in backend::discovery::StartupDiscoveryCoordinator; this object only
 * installs a UI-thread executor, forwards the saved nanopositioner
 * preference, and maps the coordinator's outcomes onto ConnectTab /
 * NanopositionerTab widgets. It owns no worker thread and never enumerates
 * hardware. The public surface is unchanged from the pre-#419 manager.
 */
class DeviceInitManager : public QObject {
    Q_OBJECT
public:
    explicit DeviceInitManager(backend::AppBackend& backend, QObject* parent = nullptr);
    ~DeviceInitManager();

    void setConnectTab(ConnectTab* connectTab) { connectTab_ = connectTab; }
    void setNanopositionerTab(NanopositionerTab* tab);

    /** Start initialisation: camera step after the coordinator's delay (400 ms),
     * then the nanopositioner step after the camera step completes. */
    void start();

    // Terminal: stops the coordinator (no further selection/connection) and
    // cancels its jobs. Draining the workers is AppBackend::shutdown()'s job.
    void stop();

    /** Run the camera step once (e.g. for "Try again"). Refused while a camera
     * step is running, capture is running, or a camera is configured. */
    void runCameraStep();

    /** Run the nanopositioner step (manual Refresh). */
    void runNanopositionerStep();

signals:
    void cameraInitFinished(bool success, const QString& message);
    void nanopositionerInitFinished(bool success);

private:
    void onCameraOutcome(const backend::discovery::StartupDiscoveryCoordinator::CameraOutcome& outcome);
    void onNanopositionerOutcome(
        const backend::discovery::StartupDiscoveryCoordinator::NanopositionerOutcome& outcome);

    backend::AppBackend& backend_;
    backend::discovery::StartupDiscoveryCoordinator& coordinator_;
    ConnectTab* connectTab_ = nullptr;
    NanopositionerTab* nanopositionerTab_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    bool stopped_ = false;
};

} // namespace frontend
