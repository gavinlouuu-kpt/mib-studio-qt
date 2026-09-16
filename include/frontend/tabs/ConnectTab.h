#pragma once

#include "backend/discovery/DeviceDiscoveryTypes.h"
#include "frontend/system/DiscoverySubscription.h"

#include <QWidget>

#include <cstdint>
#include <vector>
#include <optional>

namespace backend { class AppBackend; }
namespace camera::common { enum class FrameDeliveryMode; }
namespace Ui { class ConnectTab; }
namespace frontend { class DeviceInitManager; }

namespace frontend {

class ConnectTab : public QWidget {
    Q_OBJECT
public:
    explicit ConnectTab(backend::AppBackend& backend, QWidget* parent = nullptr);
    ~ConnectTab();

    void setDeviceInitManager(DeviceInitManager* manager) { initManager_ = manager; }

    /** Called by DeviceInitManager on main thread after setting backend selection. Updates UI and emits connected(). */
    void applyCameraSelection(int interfaceIndex, int deviceIndex, const QString& label);
    /** Called by DeviceInitManager on main thread after setting MindVision selection. Updates UI and emits connected(). */
    void applyMindVisionSelection(int cameraIndex, const QString& label);
    /** Called by DeviceInitManager on main thread when discovery finds 0 cameras. Updates UI and emits noCamerasFound(). */
    void reportNoCameras();
    /** Called by DeviceInitManager on main thread when discovery finds 2+ cameras. Updates status only. */
    void reportMultipleCameras();
    /** Called on the main thread when a discovery job ended without a decidable result. */
    void reportDiscoveryProblem(const QString& message);
    /** Discovery job accepted (startup or manual): show the scanning state. */
    void showDiscoveryStarted();
    /** Fill the framegrabber / eGrabber / MindVision lists from a discovery snapshot
     * (issue #419). The only way the lists are populated; never enumerates hardware. */
    void showDiscoveryResults(const backend::discovery::DiscoverySnapshot& snapshot);

    /** Programmatically select a delivery mode; runs the same setConfig + persist path as a user change. */
    void setDeliveryMode(camera::common::FrameDeliveryMode mode);
    /** Reflect an externally applied mode in the combo without re-applying or persisting it. */
    void syncDeliveryMode(camera::common::FrameDeliveryMode mode);

signals:
    void connected();
    void noCamerasFound();
    /** Emitted after the user (or setDeliveryMode) changed the delivery mode and it was applied to CaptureService. */
    void deliveryModeChanged(camera::common::FrameDeliveryMode mode);

public slots:
    // Delegates to DeviceInitManager (non-blocking). Without a manager there is
    // no automatic selection: discovery never runs on the UI thread (#419).
    void tryAutoConnect();

private slots:
    void onRefresh();
    void onConnect();
    void onConfigureMock();
    void onDeliveryModeComboChanged(int index);

private:
    void onDiscoverySnapshot(const backend::discovery::DiscoverySnapshot& snapshot);

    Ui::ConnectTab* ui;
    backend::AppBackend& backend_;
    DeviceInitManager* initManager_ = nullptr;
    // Manual Refresh runs a camera+framegrabber job on the backend service;
    // results arrive on the UI thread through this subscription. Declared
    // after `ui` so it is torn down (blocking on an in-flight callback)
    // before the widgets it fills.
    std::uint64_t refreshJob_ = 0;
    // A snapshot reaches the tab twice when DeviceInitManager also renders it
    // (startup / Try again); rendering a job once keeps the selection status
    // the manager wrote from being overwritten by the plain device count.
    std::uint64_t lastRenderedJob_ = 0;
    DiscoverySubscription discoverySubscription_;
};

} // namespace frontend


