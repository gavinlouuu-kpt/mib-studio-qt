#include "frontend/tabs/NanopositionerTab.h"
#include "ui_NanopositionerTab.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QTextStream>
#include <QTimer>
#include <QSettings>

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#ifdef _WIN32
#define NOMINMAX // Prevent Windows.h from defining min/max macros
#include <windows.h>
#include <shlobj.h>
#endif

#include "backend/app/AppBackend.h"
#include "backend/app/Tools.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/NanopositionerDiscovery.h"

using json = nlohmann::json;

namespace frontend {

namespace {
// Get user-writable config directory, falling back to ../include/ for development
static QString getUserConfigDir() {
    QString appDir = QCoreApplication::applicationDirPath();
    QString appDirLower = appDir.toLower();

#ifdef _WIN32
    // Check if installed in Program Files (requires admin to write)
    if (appDirLower.contains("program files") || appDirLower.contains("program files (x86)")) {
        // Use user-writable location
        char appDataPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT,
                                       appDataPath))) {
            QString userConfigDir =
                QDir(QString::fromStdString(std::string(appDataPath) + "\\MIB_Studio_Qt\\include"))
                    .absolutePath();
            // Ensure directory exists
            QDir().mkpath(userConfigDir);
            return userConfigDir;
        }
    }
#endif
    // Development: use ../include/ relative to executable
    return QDir(appDir).absoluteFilePath("../include");
}

// Ensure default config exists if path points to default app config location
static void ensureDefaultConfigExists(const QString& path) {
    // Only ensure when path points to app include path
    const QString defaultPath = QDir(getUserConfigDir()).absoluteFilePath("config.json");
    if (QFileInfo(path).absoluteFilePath() != QFileInfo(defaultPath).absoluteFilePath()) {
        return;
    }
    QFileInfo fi(path);
    QDir dir(fi.absolutePath());
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    if (!QFile::exists(path)) {
        QFile res(":/defaults/config.json");
        if (res.open(QIODevice::ReadOnly)) {
            QFile out(path);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                const QByteArray data = res.readAll();
                if (out.write(data) != data.size()) {
                    SPDLOG_WARN("NanopositionerTab: failed to write default config.json to {}",
                                path.toStdString());
                }
            } else {
                SPDLOG_WARN("NanopositionerTab: failed to create {}", path.toStdString());
            }
        } else {
            SPDLOG_WARN("NanopositionerTab: failed to open resource defaults/config.json");
        }
    }
}
} // namespace

NanopositionerTab::NanopositionerTab(backend::AppBackend& backend, QWidget* parent)
    : QWidget(parent), ui(new Ui::NanopositionerTab), backend_(backend) {
    ui->setupUi(this);

    // Configure baud rate combo with data values
    ui->baudRateCombo->setItemData(0, 9600);
    ui->baudRateCombo->setItemData(1, 19200);
    ui->baudRateCombo->setItemData(2, 38400);
    ui->baudRateCombo->setItemData(3, 57600);
    ui->baudRateCombo->setItemData(4, 115200);
    ui->baudRateCombo->setCurrentIndex(4); // Default to 115200
    ui->backendCombo->setItemData(0, static_cast<int>(backend::nanopositioner::BackendKind::Auto));
    ui->backendCombo->setItemData(1, static_cast<int>(backend::nanopositioner::BackendKind::Oeabt));
    ui->backendCombo->setItemData(2,
                                  static_cast<int>(backend::nanopositioner::BackendKind::Coremor));

    // Connect signals
    connect(ui->connectBtn, &QPushButton::clicked, this,
            &NanopositionerTab::onConnectNanopositioner);
    connect(ui->disconnectBtn, &QPushButton::clicked, this,
            &NanopositionerTab::onDisconnectNanopositioner);
    // Refresh never enumerates or probes on the UI thread: the adapter runs a
    // backend discovery job and calls showDiscoveryCandidates() (#419).
    connect(ui->refreshComPortBtn, &QPushButton::clicked, this, [this]() {
        if (discoveryRunning_) return;
        emit discoveryRequested();
    });
    auto* vendorLabel = new QLabel(tr("Automatic identification: OEABT and CoreMorrow / XMT"), this);
    vendorLabel->setObjectName("nanopositionerVendorsLabel");
    vendorLabel->setWordWrap(true);
    ui->groupVerticalLayout->insertWidget(0, vendorLabel);
    connect(ui->backendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                configuredBackend_ = static_cast<backend::nanopositioner::BackendKind>(
                    ui->backendCombo->currentData().toInt());
                populateComPortList();
            });
    connect(ui->autofocusEnabledCheck, &QCheckBox::stateChanged, this,
            &NanopositionerTab::onAutofocusEnabledChanged);
    connect(ui->increaseVoltageBtn, &QPushButton::clicked, this,
            &NanopositionerTab::onIncreaseVoltage);
    connect(ui->decreaseVoltageBtn, &QPushButton::clicked, this,
            &NanopositionerTab::onDecreaseVoltage);
    connect(ui->targetRingWidthSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &NanopositionerTab::onTargetRingWidthChanged);

    // Load config first so probe/auto-connect use saved baud and device address
    loadConfig();
    // The combo is filled by the first discovery job (startup or Refresh).
    populateComPortList();
    setNanopositionerStatus(tr("Click Refresh to search for nanopositioners."));
    updateNanopositionerUI();

    // Status update timer
    statusUpdateTimer_ = new QTimer(this);
    statusUpdateTimer_->setInterval(500);
    connect(statusUpdateTimer_, &QTimer::timeout, this,
            &NanopositionerTab::onUpdateAutofocusStatus);
    statusUpdateTimer_->start();

    // Set status callback for autofocus service
    backend_.autofocus().setStatusCallback([this](const std::string& message) {
        QMetaObject::invokeMethod(
            this,
            [this, message]() {
                if (ui->statusLabel) {
                    ui->statusLabel->setText(QString::fromStdString(message));
                }
            },
            Qt::QueuedConnection);
    });

    // Auto-connect is managed by DeviceInitManager (runs probe in worker, connect on main thread).

    // Persist the latest observed voltage for diagnostics; reconnect remains observe-only.
    connect(qApp, &QApplication::aboutToQuit, this, [this]() { saveConfig(); });
}

NanopositionerTab::~NanopositionerTab() {
    backend_.autofocus().setStatusCallback({});
    delete ui;
}

void NanopositionerTab::setDiscoveryRunning(bool running) {
    discoveryRunning_ = running;
    updateNanopositionerUI();
}

void NanopositionerTab::showDiscoveryCandidates(
    const backend::discovery::DiscoverySnapshot& snapshot) {
    std::vector<backend::nanopositioner::Endpoint> endpoints;
    for (const auto& device : snapshot.candidates) {
        if (device.kind != backend::discovery::DeviceKind::Nanopositioner || !device.nanopositioner) continue;
        endpoints.push_back(*device.nanopositioner);
    }
    endpoints_ = std::move(endpoints);
    populateComPortList();
}

void NanopositionerTab::populateComPortList() {
    const std::string previouslySelected = selectedEndpoint().persistentId;
    ui->comPortCombo->clear();

    const auto filter =
        static_cast<backend::nanopositioner::BackendKind>(ui->backendCombo->currentData().toInt());
    for (std::size_t i = 0; i < endpoints_.size(); ++i) {
        const auto& endpoint = endpoints_[i];
        if (filter != backend::nanopositioner::BackendKind::Auto && endpoint.backend != filter) {
            continue;
        }
        QString label = QString::fromStdString(endpoint.displayName);
        if (endpoint.backend == backend::nanopositioner::BackendKind::Oeabt) {
            label.prepend("OEABT candidate — ");
        }
        ui->comPortCombo->addItem(label, static_cast<int>(i));
    }

    const std::string preferred =
        !configuredEndpointId_.empty() ? configuredEndpointId_ : previouslySelected;
    for (int comboIndex = 0; comboIndex < ui->comPortCombo->count(); ++comboIndex) {
        const int endpointIndex = ui->comPortCombo->itemData(comboIndex).toInt();
        if (endpointIndex >= 0 && static_cast<std::size_t>(endpointIndex) < endpoints_.size() &&
            endpoints_[static_cast<std::size_t>(endpointIndex)].persistentId == preferred) {
            ui->comPortCombo->setCurrentIndex(comboIndex);
            break;
        }
    }

    if (endpoints_.empty() && ui->comPortCombo->count() == 0) {
        return; // nothing discovered yet: keep the caller's status text
    }
    setNanopositionerStatus(
        ui->comPortCombo->count() == 0
            ? tr("No matching serial devices found. Check USB/power, then click Refresh.")
            : tr("Found %1 candidate endpoint(s). Identity is verified only when connecting.")
                  .arg(ui->comPortCombo->count()));
}

int NanopositionerTab::getBaudRate() const {
    return ui->baudRateCombo->currentData().toInt();
}

int NanopositionerTab::getConfiguredComPort() const {
    const auto endpoint = selectedEndpoint();
    return endpoint.coremorPort > 0 ? endpoint.coremorPort : configuredComPort_;
}

unsigned char NanopositionerTab::getDeviceAddress() const {
    return static_cast<unsigned char>(ui->deviceAddressSpinBox->value());
}

backend::nanopositioner::Endpoint NanopositionerTab::selectedEndpoint() const {
    backend::nanopositioner::Endpoint endpoint;
    backend::nanopositioner::BackendKind discoveredBackend =
        backend::nanopositioner::BackendKind::Auto;
    const int comboIndex = ui->comPortCombo->currentIndex();
    if (comboIndex >= 0) {
        const int endpointIndex = ui->comPortCombo->itemData(comboIndex).toInt();
        if (endpointIndex >= 0 && static_cast<std::size_t>(endpointIndex) < endpoints_.size()) {
            endpoint = endpoints_[static_cast<std::size_t>(endpointIndex)];
            discoveredBackend = endpoint.backend;
        }
    }
    if (endpoint.persistentId.empty() && !configuredEndpointId_.empty()) {
        endpoint.persistentId = configuredEndpointId_;
        endpoint.systemPath = configuredEndpointId_;
        endpoint.displayName = configuredEndpointId_;
    }
    const auto selectedBackend =
        static_cast<backend::nanopositioner::BackendKind>(ui->backendCombo->currentData().toInt());
    endpoint.backend = selectedBackend;
    endpoint.coremorBaudRate = getBaudRate();
    endpoint.coremorAddress = getDeviceAddress();
    const bool persistedCoremorEndpoint =
        configuredComPort_ > 0 &&
        endpoint.persistentId == "COM" + std::to_string(configuredComPort_);
    if (endpoint.coremorPort <= 0 && configuredComPort_ > 0 &&
        (selectedBackend == backend::nanopositioner::BackendKind::Coremor ||
         discoveredBackend == backend::nanopositioner::BackendKind::Coremor ||
         persistedCoremorEndpoint)) {
        endpoint.coremorPort = configuredComPort_;
    }
    return endpoint;
}

backend::nanopositioner::Endpoint NanopositionerTab::getConfiguredEndpoint() const {
    return selectedEndpoint();
}

void NanopositionerTab::setNanopositionerStatus(const QString& message) {
    if (ui->statusLabel) {
        ui->statusLabel->setText(message);
    }
}

void NanopositionerTab::applyAutoConnectResult(const backend::nanopositioner::Endpoint& endpoint) {
    configuredEndpointId_ = endpoint.persistentId;
    bool known = false;
    for (const auto& ep : endpoints_) known |= ep.persistentId == endpoint.persistentId;
    if (!known) endpoints_.push_back(endpoint);
    populateComPortList();
    saveConfig();
    updateNanopositionerUI();
}

void NanopositionerTab::updateNanopositionerUI() {
    auto& autofocus = backend_.autofocus();
    bool connected = autofocus.isConnected();
    bool enabled = autofocus.isEnabled();

    ui->connectBtn->setEnabled(!connected && !discoveryRunning_ && ui->comPortCombo->count() > 0);
    ui->disconnectBtn->setEnabled(connected);
    ui->comPortCombo->setEnabled(!connected && !discoveryRunning_);
    ui->refreshComPortBtn->setEnabled(!connected && !discoveryRunning_);
    ui->backendCombo->setEnabled(!connected && !discoveryRunning_);
    const auto endpoint = selectedEndpoint();
    const bool coremorSettings =
        endpoint.backend == backend::nanopositioner::BackendKind::Coremor ||
        (endpoint.backend == backend::nanopositioner::BackendKind::Auto &&
         endpoint.coremorPort > 0 && !endpoint.knownOeabtCandidate);
    ui->baudRateCombo->setEnabled(!connected && !discoveryRunning_ && coremorSettings);
    ui->deviceAddressSpinBox->setEnabled(!connected && !discoveryRunning_ && coremorSettings);
    ui->autofocusEnabledCheck->setEnabled(connected);
    ui->autofocusEnabledCheck->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
    ui->increaseVoltageBtn->setEnabled(connected);
    ui->decreaseVoltageBtn->setEnabled(connected);
    ui->voltageStepSpinBox->setEnabled(connected);

    if (connected) {
        double voltage = autofocus.getCurrentVoltage();
        ui->voltageLabel->setText(QString("Voltage: %1 V").arg(voltage, 0, 'f', 2));
    } else {
        ui->voltageLabel->setText("Voltage: -- V");
    }
}

void NanopositionerTab::onConnectNanopositioner() {
    if (discoveryRunning_) return;
    if (ui->comPortCombo->count() == 0) {
        return;
    }
    const auto endpoint = selectedEndpoint();
    bool success = backend_.autofocus().connect(endpoint);
    if (success) {
        saveConfig();
        updateNanopositionerUI();
    } else {
        QMessageBox::warning(this, tr("Connection Failed"),
                             tr("Failed to identify a nanopositioner on %1")
                                 .arg(QString::fromStdString(endpoint.systemPath)));
    }
}

void NanopositionerTab::onDisconnectNanopositioner() {
    saveConfig();
    backend_.autofocus().disconnect();
    updateNanopositionerUI();
}

void NanopositionerTab::onAutofocusEnabledChanged(int state) {
    backend_.autofocus().setEnabled(state == Qt::Checked);
}

void NanopositionerTab::onIncreaseVoltage() {
    // Update manual voltage step from UI
    backend::services::AutofocusService::Config config = backend_.autofocus().getConfig();
    config.manualVoltageStep = ui->voltageStepSpinBox->value();
    backend_.autofocus().setConfig(config);
    backend_.autofocus().increaseVoltage();
}

void NanopositionerTab::onDecreaseVoltage() {
    // Update manual voltage step from UI
    backend::services::AutofocusService::Config config = backend_.autofocus().getConfig();
    config.manualVoltageStep = ui->voltageStepSpinBox->value();
    backend_.autofocus().setConfig(config);
    backend_.autofocus().decreaseVoltage();
}

void NanopositionerTab::onUpdateAutofocusStatus() {
    updateNanopositionerUI();
}

void NanopositionerTab::onTargetRingWidthChanged(double value) {
    backend::services::AutofocusService::Config config = backend_.autofocus().getConfig();
    config.focusSetpoint = value;
    backend_.autofocus().setConfig(config);
    saveConfig();
}

QString NanopositionerTab::configPath() const {
    QSettings s;
    const QString external = s.value("Config/ExternalAppConfigPath").toString().trimmed();
    if (!external.isEmpty()) {
        return external;
    }
    // Use centralized helper to get user-writable config directory
    return QDir(getUserConfigDir()).absoluteFilePath("config.json");
}

void NanopositionerTab::loadConfig() {
    QString path = configPath();
    ensureDefaultConfigExists(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        SPDLOG_WARN("Failed to load config.json from {}", path.toStdString());
        return;
    }

    try {
        QByteArray data = file.readAll();
        json config = json::parse(data.constData(), data.constData() + data.size());

        // Load autofocus settings
        const std::optional<std::string> backendName =
            config.contains("autofocus_backend") && config["autofocus_backend"].is_string()
                ? std::optional<std::string>(config["autofocus_backend"].get<std::string>())
                : std::nullopt;
        const std::optional<std::string> endpointId =
            config.contains("autofocus_endpoint") && config["autofocus_endpoint"].is_string()
                ? std::optional<std::string>(config["autofocus_endpoint"].get<std::string>())
                : std::nullopt;
        const std::optional<int> legacyPort =
            config.contains("autofocus_com_port")
                ? std::optional<int>(config["autofocus_com_port"].get<int>())
                : std::nullopt;
        const auto selection =
            backend::nanopositioner::resolvePersistedSelection(backendName, endpointId, legacyPort);
        configuredBackend_ = selection.backend;
        configuredEndpointId_ = selection.endpointId;
        configuredComPort_ = selection.legacyComPort;
        const int backendIndex = ui->backendCombo->findData(static_cast<int>(configuredBackend_));
        if (backendIndex >= 0) {
            ui->backendCombo->blockSignals(true);
            ui->backendCombo->setCurrentIndex(backendIndex);
            ui->backendCombo->blockSignals(false);
        }
        if (config.contains("autofocus_baud_rate")) {
            int baudRate = config["autofocus_baud_rate"].get<int>();
            int index = ui->baudRateCombo->findData(baudRate);
            if (index >= 0) {
                ui->baudRateCombo->setCurrentIndex(index);
            }
        }
        if (config.contains("autofocus_device_address")) {
            ui->deviceAddressSpinBox->setValue(config["autofocus_device_address"].get<int>());
        }

        // Load autofocus service config
        backend::services::AutofocusService::Config afConfig;
        if (config.contains("autofocus_focus_setpoint")) {
            afConfig.focusSetpoint = config["autofocus_focus_setpoint"].get<double>();
            ui->targetRingWidthSpinBox->blockSignals(true);
            ui->targetRingWidthSpinBox->setValue(afConfig.focusSetpoint);
            ui->targetRingWidthSpinBox->blockSignals(false);
        }
        if (config.contains("autofocus_focus_range")) {
            afConfig.focusRange = config["autofocus_focus_range"].get<double>();
        }
        if (config.contains("autofocus_voltage_step")) {
            afConfig.voltageStep = config["autofocus_voltage_step"].get<double>();
        }
        if (config.contains("autofocus_fine_voltage_step")) {
            afConfig.fineVoltageStep = config["autofocus_fine_voltage_step"].get<double>();
        }
        if (config.contains("autofocus_max_voltage")) {
            afConfig.maxVoltage = config["autofocus_max_voltage"].get<double>();
        }
        if (config.contains("autofocus_min_voltage")) {
            afConfig.minVoltage = config["autofocus_min_voltage"].get<double>();
        }
        if (config.contains("autofocus_initial_voltage")) {
            afConfig.initialVoltage = config["autofocus_initial_voltage"].get<double>();
        }
        if (config.contains("autofocus_manual_voltage_step")) {
            afConfig.manualVoltageStep = config["autofocus_manual_voltage_step"].get<double>();
            ui->voltageStepSpinBox->setValue(static_cast<int>(afConfig.manualVoltageStep));
        }
        if (config.contains("ring_ratio_stale_ms")) {
            afConfig.ringRatioStaleMs = config["ring_ratio_stale_ms"].get<int>();
        }
        if (config.contains("require_new_sample_per_step")) {
            afConfig.requireNewSamplePerStep = config["require_new_sample_per_step"].get<bool>();
        }
        if (config.contains("autofocus_min_samples_per_step")) {
            afConfig.minSamplesPerStep = config["autofocus_min_samples_per_step"].get<int>();
        }
        if (config.contains("safe_shutdown_voltage")) {
            afConfig.safeShutdownVoltage = config["safe_shutdown_voltage"].get<double>();
        }
        if (config.contains("focus_direction")) {
            afConfig.focusDirection = config["focus_direction"].get<bool>();
        }

        backend_.autofocus().setConfig(afConfig);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to parse config.json: {}", e.what());
    }
}

void NanopositionerTab::saveConfig() {
    QString path = configPath();
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Text)) {
        SPDLOG_WARN("Failed to save config.json to {}", path.toStdString());
        return;
    }

    try {
        QByteArray data = file.readAll();
        json config = json::parse(data.constData(), data.constData() + data.size());

        // Save backend-neutral endpoint selection. Legacy COM settings remain
        // only for CoreMOR compatibility.
        if (ui->comPortCombo->currentIndex() >= 0) {
            const auto endpoint = selectedEndpoint();
            configuredEndpointId_ = endpoint.persistentId;
            configuredBackend_ = endpoint.backend;
            config["autofocus_backend"] =
                backend::nanopositioner::backendKindName(configuredBackend_);
            config["autofocus_endpoint"] = configuredEndpointId_;
            if (endpoint.coremorPort > 0 &&
                configuredBackend_ != backend::nanopositioner::BackendKind::Oeabt) {
                configuredComPort_ = endpoint.coremorPort;
                config["autofocus_com_port"] = configuredComPort_;
            } else {
                config.erase("autofocus_com_port");
            }
        }
        config["autofocus_baud_rate"] = ui->baudRateCombo->currentData().toInt();
        config["autofocus_device_address"] = ui->deviceAddressSpinBox->value();
        config["autofocus_focus_setpoint"] = ui->targetRingWidthSpinBox->value();

        // Persist current voltage as initial for next session when connected
        if (backend_.autofocus().isConnected()) {
            auto cfg = backend_.autofocus().getConfig();
            double v = backend_.autofocus().getCurrentVoltage();
            v = std::clamp(v, cfg.minVoltage, cfg.maxVoltage);
            config["autofocus_initial_voltage"] = v;
        }

        file.resize(0);
        QTextStream out(&file);
        out << QString::fromStdString(config.dump(4));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to save config.json: {}", e.what());
    }
}

} // namespace frontend
