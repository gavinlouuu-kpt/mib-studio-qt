#include "frontend/tabs/SyringePumpTab.h"
#include "ui_SyringePumpTab.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QTextStream>
#include <QTimer>
#include <QSettings>

#include <spdlog/spdlog.h>
#include <cstdlib>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

#include "backend/app/AppBackend.h"
#include "backend/services/ISerialPort.h"
#include "backend/services/SerialAdapterIdentity.h"
#include "backend/services/SerialPortClaims.h"
#include "backend/services/SyringePumpService.h"

using json = nlohmann::json;
using PumpId = backend::services::SyringePumpService::PumpId;
using Direction = backend::services::SyringePumpService::Direction;
using RunStatus = backend::services::SyringePumpService::RunStatus;

namespace frontend {

namespace {
    static QString getUserConfigDir()
    {
        QString appDir = QCoreApplication::applicationDirPath();
        QString appDirLower = appDir.toLower();

#ifdef _WIN32
        if (appDirLower.contains("program files") ||
            appDirLower.contains("program files (x86)"))
        {
            char appDataPath[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath)))
            {
                QString userConfigDir = QDir(QString::fromStdString(std::string(appDataPath) + "\\MIB_Studio_Qt\\include")).absolutePath();
                QDir().mkpath(userConfigDir);
                return userConfigDir;
            }
        }
#endif
        return QDir(appDir).absoluteFilePath("../include");
    }

    QString runStatusToString(RunStatus status) {
        switch (status) {
            case RunStatus::Stop:     return "Stopped";
            case RunStatus::Forward:  return "Running (Forward)";
            case RunStatus::Backward: return "Running (Backward)";
            case RunStatus::Pause:    return "Paused";
            default:                  return "Unknown";
        }
    }
} // namespace

SyringePumpTab::SyringePumpTab(backend::AppBackend& backend, QWidget* parent)
    : QWidget(parent), ui(new Ui::SyringePumpTab), backend_(backend)
{
    ui->setupUi(this);

    // Connect Sample pump signals
    connect(ui->sampleConnectBtn, &QPushButton::clicked, this, &SyringePumpTab::onConnectSample);
    connect(ui->sampleDisconnectBtn, &QPushButton::clicked, this, &SyringePumpTab::onDisconnectSample);
    connect(ui->sampleStartBtn, &QPushButton::clicked, this, &SyringePumpTab::onStartSample);
    connect(ui->sampleStopBtn, &QPushButton::clicked, this, &SyringePumpTab::onStopSample);
    connect(ui->samplePurgeBtn, &QPushButton::pressed, this, [this]() {
        auto dir = ui->sampleDirectionCombo->currentIndex() == 0 ? Direction::Infuse : Direction::Withdraw;
        backend_.syringePump().purge(PumpId::Sample, dir);
    });
    connect(ui->samplePurgeBtn, &QPushButton::released, this, [this]() {
        backend_.syringePump().stopPurge(PumpId::Sample);
    });

    // Connect Sheath pump signals
    connect(ui->sheathConnectBtn, &QPushButton::clicked, this, &SyringePumpTab::onConnectSheath);
    connect(ui->sheathDisconnectBtn, &QPushButton::clicked, this, &SyringePumpTab::onDisconnectSheath);
    connect(ui->sheathStartBtn, &QPushButton::clicked, this, &SyringePumpTab::onStartSheath);
    connect(ui->sheathStopBtn, &QPushButton::clicked, this, &SyringePumpTab::onStopSheath);
    connect(ui->sheathPurgeBtn, &QPushButton::pressed, this, [this]() {
        auto dir = ui->sheathDirectionCombo->currentIndex() == 0 ? Direction::Infuse : Direction::Withdraw;
        backend_.syringePump().purge(PumpId::Sheath, dir);
    });
    connect(ui->sheathPurgeBtn, &QPushButton::released, this, [this]() {
        backend_.syringePump().stopPurge(PumpId::Sheath);
    });

    // Auto-apply: debounced flow rate / direction changes
    sampleApplyTimer_ = new QTimer(this);
    sampleApplyTimer_->setSingleShot(true);
    sampleApplyTimer_->setInterval(300);
    connect(sampleApplyTimer_, &QTimer::timeout, this, &SyringePumpTab::onApplySample);

    sheathApplyTimer_ = new QTimer(this);
    sheathApplyTimer_->setSingleShot(true);
    sheathApplyTimer_->setInterval(300);
    connect(sheathApplyTimer_, &QTimer::timeout, this, &SyringePumpTab::onApplySheath);

    auto triggerSampleApply = [this]() { if (backend_.syringePump().isConnected(PumpId::Sample)) sampleApplyTimer_->start(); };
    auto triggerSheathApply = [this]() { if (backend_.syringePump().isConnected(PumpId::Sheath)) sheathApplyTimer_->start(); };

    connect(ui->sampleFlowRateSpinBox, &QDoubleSpinBox::editingFinished, this, triggerSampleApply);
    connect(ui->sampleFlowUnitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, triggerSampleApply);
    connect(ui->sampleDirectionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, triggerSampleApply);

    connect(ui->sheathFlowRateSpinBox, &QDoubleSpinBox::editingFinished, this, triggerSheathApply);
    connect(ui->sheathFlowUnitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, triggerSheathApply);
    connect(ui->sheathDirectionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, triggerSheathApply);

    // Load saved config (flow rate, direction)
    loadConfig();

    // Update UI state
    updatePumpUI(0);
    updatePumpUI(1);

    // Status update timer (500ms)
    statusUpdateTimer_ = new QTimer(this);
    statusUpdateTimer_->setInterval(500);
    connect(statusUpdateTimer_, &QTimer::timeout, this, &SyringePumpTab::onUpdateStatus);
    statusUpdateTimer_->start();

    // Save config on app quit
    connect(qApp, &QApplication::aboutToQuit, this, [this]() { saveConfig(); });
}

SyringePumpTab::~SyringePumpTab() {
    delete ui;
}

uint16_t SyringePumpTab::flowUnitFromCombo(int comboIndex) const {
    return (comboIndex == 0) ? 100 : 103;
}

int SyringePumpTab::comboIndexFromFlowUnit(uint16_t unit) const {
    return (unit == 100) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Connection slots — read COM/baud/addr from config.json
// ---------------------------------------------------------------------------
void SyringePumpTab::onConnectSample() { connectPump(0); }

void SyringePumpTab::onDisconnectSample() {
    saveConfig();
    backend_.syringePump().disconnect(PumpId::Sample);
    updatePumpUI(0);
}

void SyringePumpTab::onConnectSheath() { connectPump(1); }

void SyringePumpTab::connectPump(int pumpIndex) {
    const PumpId id = pumpIndex == 0 ? PumpId::Sample : PumpId::Sheath;
    const std::string prefix = pumpIndex == 0 ? "pump_sample_" : "pump_sheath_";
    const QString pumpName = pumpIndex == 0 ? tr("Sample") : tr("Sheath");
    QString path = configPath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Connection Failed"),
            tr("No config found. Open Settings > Syringe Pump Settings to configure."));
        return;
    }

    try {
        QByteArray data = file.readAll();
        file.close();
        json config = json::parse(data.constData(), data.constData() + data.size());

        int comPort = config.value(prefix + "com_port", -1);
        int baudRate = config.value(prefix + "baud_rate", 115200);
        int addr = config.value(prefix + "address", pumpIndex == 0 ? 1 : 2);

        if (comPort < 0) {
            QMessageBox::warning(this, tr("Connection Failed"),
                tr("No COM port configured. Open Settings > Syringe Pump Settings."));
            return;
        }

        // Windows renumbers adapters: check that the configured port still
        // carries the adapter this pump last connected on.
        const auto remembered = backend::services::adapterIdentityFromJson(config.value(prefix + "adapter", json()));
        const auto resolution = backend::services::resolveAdapterPort(
            "COM" + std::to_string(comPort), remembered, backend::services::enumerateSerialPorts());
        if (!resolution.connectable()) {
            SPDLOG_WARN("SyringePumpTab: {} pump not connected: {}", pumpName.toStdString(), resolution.message);
            QMessageBox::warning(this, tr("Connection Failed"), QString::fromStdString(resolution.message));
            return;
        }
        if (resolution.kind == backend::services::PortResolution::Kind::Moved)
            SPDLOG_WARN("SyringePumpTab: {}", resolution.message);

        const auto key = backend::services::portclaims::key(resolution.port);
        const int resolvedCom = key.rfind("com", 0) == 0 ? std::atoi(key.c_str() + 3) : 0;
        bool ok = resolvedCom > 0
                      ? backend_.syringePump().connect(id, resolvedCom, baudRate, static_cast<uint8_t>(addr))
                      : backend_.syringePump().connect(id, resolution.port, baudRate, static_cast<uint8_t>(addr));
        if (!ok) {
            QMessageBox::warning(this, tr("Connection Failed"),
                tr("Failed to connect %1 pump on %2 addr=%3")
                    .arg(pumpName, QString::fromStdString(resolution.port))
                    .arg(addr));
        } else {
            rememberPumpAdapter(pumpIndex, resolution.port);
        }
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Config Error"), QString::fromStdString(e.what()));
    }
    updatePumpUI(pumpIndex);
}

void SyringePumpTab::rememberPumpAdapter(int pumpIndex, const std::string& port) {
    const std::string prefix = pumpIndex == 0 ? "pump_sample_" : "pump_sheath_";
    const auto wanted = backend::services::portclaims::key(port);
    backend::services::AdapterIdentity identity;
    for (const auto& info : backend::services::enumerateSerialPorts())
        if (backend::services::portclaims::key(info.systemName) == wanted) identity = backend::services::identityOf(info);

    QFile file(configPath());
    if (!file.open(QIODevice::ReadWrite | QIODevice::Text)) return;
    try {
        QByteArray data = file.readAll();
        json config;
        if (!data.isEmpty())
            config = json::parse(data.constData(), data.constData() + data.size());
        if (identity.known()) config[prefix + "adapter"] = backend::services::toJson(identity);
        else config.erase(prefix + "adapter"); // no USB identity (onboard port): nothing to check
        // A followed adapter: config.json names its new COM port.
        if (wanted.rfind("com", 0) == 0) {
            const int number = std::atoi(wanted.c_str() + 3);
            if (number > 0) config[prefix + "com_port"] = number;
        }
        file.resize(0);
        QTextStream out(&file);
        out << QString::fromStdString(config.dump(4));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("SyringePumpTab: Failed to remember the pump adapter in config.json: {}", e.what());
    }
}

void SyringePumpTab::onDisconnectSheath() {
    saveConfig();
    backend_.syringePump().disconnect(PumpId::Sheath);
    updatePumpUI(1);
}

// ---------------------------------------------------------------------------
// Control slots
// ---------------------------------------------------------------------------
void SyringePumpTab::onStartSample() {
    backend_.syringePump().start(PumpId::Sample);
}

void SyringePumpTab::onStopSample() {
    backend_.syringePump().stop(PumpId::Sample);
}

void SyringePumpTab::onStartSheath() {
    backend_.syringePump().start(PumpId::Sheath);
}

void SyringePumpTab::onStopSheath() {
    backend_.syringePump().stop(PumpId::Sheath);
}

void SyringePumpTab::onApplySample() {
    double rate = ui->sampleFlowRateSpinBox->value();
    uint16_t unit = flowUnitFromCombo(ui->sampleFlowUnitCombo->currentIndex());
    int dirIdx = ui->sampleDirectionCombo->currentIndex();

    backend_.syringePump().setFlowRate(PumpId::Sample, rate, unit);
    backend_.syringePump().setDirection(PumpId::Sample,
        dirIdx == 0 ? Direction::Infuse : Direction::Withdraw);

    saveConfig();
}

void SyringePumpTab::onApplySheath() {
    double rate = ui->sheathFlowRateSpinBox->value();
    uint16_t unit = flowUnitFromCombo(ui->sheathFlowUnitCombo->currentIndex());
    int dirIdx = ui->sheathDirectionCombo->currentIndex();

    backend_.syringePump().setFlowRate(PumpId::Sheath, rate, unit);
    backend_.syringePump().setDirection(PumpId::Sheath,
        dirIdx == 0 ? Direction::Infuse : Direction::Withdraw);

    saveConfig();
}

// ---------------------------------------------------------------------------
// Status polling
// ---------------------------------------------------------------------------
void SyringePumpTab::onUpdateStatus() {
    auto& pumpService = backend_.syringePump();

    if (pumpService.isConnected(PumpId::Sample)) {
        pumpService.pollStatus(PumpId::Sample);
    }
    if (pumpService.isConnected(PumpId::Sheath)) {
        pumpService.pollStatus(PumpId::Sheath);
    }

    updatePumpUI(0);
    updatePumpUI(1);
}

// ---------------------------------------------------------------------------
// UI state management
// ---------------------------------------------------------------------------
void SyringePumpTab::updatePumpUI(int pumpIndex) {
    auto& pumpService = backend_.syringePump();
    PumpId id = static_cast<PumpId>(pumpIndex);
    auto status = pumpService.getStatus(id);
    bool connected = status.connected;

    if (pumpIndex == 0) {
        ui->sampleConnectBtn->setEnabled(!connected);
        ui->sampleDisconnectBtn->setEnabled(connected);
        ui->sampleFlowRateSpinBox->setEnabled(connected);
        ui->sampleFlowUnitCombo->setEnabled(connected);
        ui->sampleDirectionCombo->setEnabled(connected);
        ui->sampleStartBtn->setEnabled(connected);
        ui->sampleStopBtn->setEnabled(connected);
        ui->samplePurgeBtn->setEnabled(connected);

        if (connected) {
            QString statusText = runStatusToString(status.runStatus);
            if (status.stalled) statusText += " [STALL]";
            ui->sampleStatusLabel->setText(statusText);
            ui->sampleFlowStatusLabel->setText(QString("%1").arg(status.currentFlowRate, 0, 'f', 4));
            ui->sampleVolumeLabel->setText(QString("%1").arg(status.accumulatedVolume, 0, 'f', 4));
        } else {
            ui->sampleStatusLabel->setText("Not connected");
            ui->sampleFlowStatusLabel->setText("--");
            ui->sampleVolumeLabel->setText("--");
        }
    } else {
        ui->sheathConnectBtn->setEnabled(!connected);
        ui->sheathDisconnectBtn->setEnabled(connected);
        ui->sheathFlowRateSpinBox->setEnabled(connected);
        ui->sheathFlowUnitCombo->setEnabled(connected);
        ui->sheathDirectionCombo->setEnabled(connected);
        ui->sheathStartBtn->setEnabled(connected);
        ui->sheathStopBtn->setEnabled(connected);
        ui->sheathPurgeBtn->setEnabled(connected);

        if (connected) {
            QString statusText = runStatusToString(status.runStatus);
            if (status.stalled) statusText += " [STALL]";
            ui->sheathStatusLabel->setText(statusText);
            ui->sheathFlowStatusLabel->setText(QString("%1").arg(status.currentFlowRate, 0, 'f', 4));
            ui->sheathVolumeLabel->setText(QString("%1").arg(status.accumulatedVolume, 0, 'f', 4));
        } else {
            ui->sheathStatusLabel->setText("Not connected");
            ui->sheathFlowStatusLabel->setText("--");
            ui->sheathVolumeLabel->setText("--");
        }
    }
}

// ---------------------------------------------------------------------------
// Config persistence (flow rate, direction only — connection params in settings dialog)
// ---------------------------------------------------------------------------
QString SyringePumpTab::configPath() const {
    QSettings s;
    const QString external = s.value("Config/ExternalAppConfigPath").toString().trimmed();
    if (!external.isEmpty()) return external;
    return QDir(getUserConfigDir()).absoluteFilePath("config.json");
}

void SyringePumpTab::loadConfig() {
    QString path = configPath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    try {
        QByteArray data = file.readAll();
        json config = json::parse(data.constData(), data.constData() + data.size());

        if (config.contains("pump_sample_flow_rate"))
            ui->sampleFlowRateSpinBox->setValue(config["pump_sample_flow_rate"].get<double>());
        if (config.contains("pump_sample_flow_unit"))
            ui->sampleFlowUnitCombo->setCurrentIndex(
                comboIndexFromFlowUnit(config["pump_sample_flow_unit"].get<uint16_t>()));
        if (config.contains("pump_sample_direction"))
            ui->sampleDirectionCombo->setCurrentIndex(config["pump_sample_direction"].get<int>());

        if (config.contains("pump_sheath_flow_rate"))
            ui->sheathFlowRateSpinBox->setValue(config["pump_sheath_flow_rate"].get<double>());
        if (config.contains("pump_sheath_flow_unit"))
            ui->sheathFlowUnitCombo->setCurrentIndex(
                comboIndexFromFlowUnit(config["pump_sheath_flow_unit"].get<uint16_t>()));
        if (config.contains("pump_sheath_direction"))
            ui->sheathDirectionCombo->setCurrentIndex(config["pump_sheath_direction"].get<int>());
    }
    catch (const std::exception& e) {
        SPDLOG_ERROR("SyringePumpTab: Failed to parse config.json: {}", e.what());
    }
}

void SyringePumpTab::saveConfig() {
    QString path = configPath();
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Text)) return;

    try {
        QByteArray data = file.readAll();
        json config;
        if (!data.isEmpty())
            config = json::parse(data.constData(), data.constData() + data.size());

        config["pump_sample_flow_rate"] = ui->sampleFlowRateSpinBox->value();
        config["pump_sample_flow_unit"] = flowUnitFromCombo(ui->sampleFlowUnitCombo->currentIndex());
        config["pump_sample_direction"] = ui->sampleDirectionCombo->currentIndex();

        config["pump_sheath_flow_rate"] = ui->sheathFlowRateSpinBox->value();
        config["pump_sheath_flow_unit"] = flowUnitFromCombo(ui->sheathFlowUnitCombo->currentIndex());
        config["pump_sheath_direction"] = ui->sheathDirectionCombo->currentIndex();

        file.resize(0);
        QTextStream out(&file);
        out << QString::fromStdString(config.dump(4));
    }
    catch (const std::exception& e) {
        SPDLOG_ERROR("SyringePumpTab: Failed to save config.json: {}", e.what());
    }
}

} // namespace frontend
