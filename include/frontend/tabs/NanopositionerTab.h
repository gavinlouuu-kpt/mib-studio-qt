#pragma once

#include "backend/nanopositioner/INanopositionerBackend.h"

#include <QWidget>

#include <string>
#include <vector>

namespace backend {
class AppBackend;
}
class QTimer;
namespace Ui {
class NanopositionerTab;
}

namespace frontend {

class NanopositionerTab : public QWidget {
    Q_OBJECT
public:
    explicit NanopositionerTab(backend::AppBackend& backend, QWidget* parent = nullptr);
    ~NanopositionerTab();

    /** Used by DeviceInitManager before starting probe worker. */
    int getBaudRate() const;
    int getConfiguredComPort() const;
    unsigned char getDeviceAddress() const;
    backend::nanopositioner::Endpoint getConfiguredEndpoint() const;
    /** Called by DeviceInitManager on main thread to set status text (e.g. "Searching..."). */
    void setNanopositionerStatus(const QString& message);
    /** Called by DeviceInitManager on main thread after successful connect. Updates combo, saves
     * config, refreshes UI. */
    void applyAutoConnectResult(const backend::nanopositioner::Endpoint& endpoint);

    void setDiscoveryRunning(bool running);
signals:
    void discoveryRequested();

private slots:
    void onConnectNanopositioner();
    void onDisconnectNanopositioner();
    void onAutofocusEnabledChanged(int state);
    void onIncreaseVoltage();
    void onDecreaseVoltage();
    void onUpdateAutofocusStatus();
    void onTargetRingWidthChanged(double value);

private:
    void updateNanopositionerUI();
    void loadConfig();
    void saveConfig();
    QString configPath() const;
    void populateComPortList();
    backend::nanopositioner::Endpoint selectedEndpoint() const;

    Ui::NanopositionerTab* ui;
    backend::AppBackend& backend_;
    QTimer* statusUpdateTimer_ = nullptr;
    int configuredComPort_ = -1;
    bool discoveryRunning_ = false;
    std::string configuredEndpointId_;
    backend::nanopositioner::BackendKind configuredBackend_ =
        backend::nanopositioner::BackendKind::Auto;
    std::vector<backend::nanopositioner::Endpoint> endpoints_;
};

} // namespace frontend
