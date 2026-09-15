#pragma once

#include <QWidget>

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
    // If DeviceInitManager is set, delegates to it (non-blocking). Otherwise runs discovery on UI thread.
    void tryAutoConnect();

private slots:
    void onRefresh();
    void onConnect();
    void onConfigureMock();
    void onDeliveryModeComboChanged(int index);

private:
    void populateDevices();

    Ui::ConnectTab* ui;
    backend::AppBackend& backend_;
    DeviceInitManager* initManager_ = nullptr;
};

} // namespace frontend


