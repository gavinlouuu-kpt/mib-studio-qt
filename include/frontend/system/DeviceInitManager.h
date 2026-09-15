#pragma once

#include "backend/nanopositioner/INanopositionerBackend.h"

#include <QObject>

#include <vector>
#include <memory>

class QTimer;
template <typename T> class QFutureWatcher;

namespace backend {
class AppBackend;
}
namespace backend::services {
struct DiscoveredCamera;
}
namespace frontend {
class ConnectTab;
}
namespace frontend {
class NanopositionerTab;
}

namespace frontend {

/**
 * Manages auto-connect for camera, nanopositioner, and future devices.
 * Runs blocking discovery/probe in worker threads and applies results on the main thread
 * so the UI never blocks during retries.
 */
class DeviceInitManager : public QObject {
    Q_OBJECT
public:
    explicit DeviceInitManager(backend::AppBackend& backend, QObject* parent = nullptr);
    ~DeviceInitManager();

    void setConnectTab(ConnectTab* connectTab) { connectTab_ = connectTab; }
    void setNanopositionerTab(NanopositionerTab* tab);

    /** Start initialisation: schedule camera step (400 ms), then nanopositioner after camera
     * completes. */
    void start();

    /** Run camera discovery step once (e.g. for "Try again"). Does nothing if camera step is
     * already running. */
    void runCameraStep();

signals:
    void cameraInitFinished(bool success, const QString& message);
    void nanopositionerInitFinished(bool success);

private slots:
    void onCameraStepTimer();
    void onCameraDiscoveryFinished();
    void onNanopositionerStepTimer();
    void onNanopositionerProbeFinished();

private:
    void runCameraDiscoveryInWorker();
    void runNanopositionerProbeInWorker();
    void scheduleNanopositionerStep();

    backend::AppBackend& backend_;
    ConnectTab* connectTab_ = nullptr;
    NanopositionerTab* nanopositionerTab_ = nullptr;

    QTimer* cameraStepTimer_ = nullptr;
    QTimer* nanopositionerStepTimer_ = nullptr;
    std::unique_ptr<QFutureWatcher<std::vector<backend::services::DiscoveredCamera>>>
        cameraWatcher_;
    std::unique_ptr<QFutureWatcher<std::vector<backend::nanopositioner::Endpoint>>>
        nanopositionerWatcher_;

    bool cameraStepScheduled_ = false;
    bool cameraStepRunning_ = false;
    int nanopositionerRetryCount_ = 0;
    static constexpr int NANOPOSITIONER_MAX_RETRIES = 3;
    static constexpr int NANOPOSITIONER_RETRY_DELAY_MS = 4000;
};

} // namespace frontend
