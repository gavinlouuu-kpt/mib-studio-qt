#pragma once

#include <QWidget>

#include <string>

namespace backend { class AppBackend; }
class QTimer;
namespace Ui { class SyringePumpTab; }

namespace frontend {

class SyringePumpTab : public QWidget {
    Q_OBJECT
public:
    explicit SyringePumpTab(backend::AppBackend& backend, QWidget* parent = nullptr);
    ~SyringePumpTab();

private slots:
    void onConnectSample();
    void onDisconnectSample();
    void onConnectSheath();
    void onDisconnectSheath();
    void onStartSample();
    void onStopSample();
    void onStartSheath();
    void onStopSheath();
    void onApplySample();
    void onApplySheath();
    void onUpdateStatus();

private:
    void updatePumpUI(int pumpIndex);
    // Connect pump 0 (Sample) or 1 (Sheath) from config.json, following a
    // renumbered adapter (SerialAdapterIdentity.h).
    void connectPump(int pumpIndex);
    // After a connect: remember the adapter on `port`, and the port if it moved.
    void rememberPumpAdapter(int pumpIndex, const std::string& port);
    void loadConfig();
    void saveConfig();
    QString configPath() const;

    // Map flow unit combo index to Modbus register value
    uint16_t flowUnitFromCombo(int comboIndex) const;
    int comboIndexFromFlowUnit(uint16_t unit) const;

    Ui::SyringePumpTab* ui;
    backend::AppBackend& backend_;
    QTimer* statusUpdateTimer_{nullptr};
    QTimer* sampleApplyTimer_{nullptr};
    QTimer* sheathApplyTimer_{nullptr};
};

} // namespace frontend
