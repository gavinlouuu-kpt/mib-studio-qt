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

namespace frontend
{

	namespace
	{
		// Get user-writable config directory, falling back to ../include/ for development
		static QString getUserConfigDir()
		{
			QString appDir = QCoreApplication::applicationDirPath();
			QString appDirLower = appDir.toLower();

#ifdef _WIN32
			// Check if installed in Program Files (requires admin to write)
			if (appDirLower.contains("program files") ||
				appDirLower.contains("program files (x86)"))
			{
				// Use user-writable location
				char appDataPath[MAX_PATH];
				if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath)))
				{
					QString userConfigDir = QDir(QString::fromStdString(std::string(appDataPath) + "\\MIB_Studio_Qt\\include")).absolutePath();
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
		static void ensureDefaultConfigExists(const QString &path)
		{
			// Only ensure when path points to app include path
			const QString defaultPath = QDir(getUserConfigDir()).absoluteFilePath("config.json");
			if (QFileInfo(path).absoluteFilePath() != QFileInfo(defaultPath).absoluteFilePath())
			{
				return;
			}
			QFileInfo fi(path);
			QDir dir(fi.absolutePath());
			if (!dir.exists())
			{
				dir.mkpath(".");
			}
			if (!QFile::exists(path))
			{
				QFile res(":/defaults/config.json");
				if (res.open(QIODevice::ReadOnly))
				{
					QFile out(path);
					if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
					{
						const QByteArray data = res.readAll();
						if (out.write(data) != data.size())
						{
							SPDLOG_WARN("NanopositionerTab: failed to write default config.json to {}", path.toStdString());
						}
					}
					else
					{
						SPDLOG_WARN("NanopositionerTab: failed to create {}", path.toStdString());
					}
				}
				else
				{
					SPDLOG_WARN("NanopositionerTab: failed to open resource defaults/config.json");
				}
			}
		}
	}

	NanopositionerTab::NanopositionerTab(backend::AppBackend &backend, QWidget *parent)
		: QWidget(parent), ui(new Ui::NanopositionerTab), backend_(backend)
	{
		ui->setupUi(this);

		// Configure baud rate combo with data values
		ui->baudRateCombo->setItemData(0, 9600);
		ui->baudRateCombo->setItemData(1, 19200);
		ui->baudRateCombo->setItemData(2, 38400);
		ui->baudRateCombo->setItemData(3, 57600);
		ui->baudRateCombo->setItemData(4, 115200);
		ui->baudRateCombo->setCurrentIndex(4); // Default to 115200

		// Connect signals
		connect(ui->connectBtn, &QPushButton::clicked, this, &NanopositionerTab::onConnectNanopositioner);
		connect(ui->disconnectBtn, &QPushButton::clicked, this, &NanopositionerTab::onDisconnectNanopositioner);
		connect(ui->refreshComPortBtn, &QPushButton::clicked, this, [this]() {
            populateComPortList();
            emit discoveryRequested();
        });

        QStringList vendorLines;
        for (const auto& vendor : backend::services::nanopositioner::vendors) {
            vendorLines << QString::fromUtf8(vendor.name) +
                (vendor.protocolAvailable ? tr(": automatic identification") : tr(": protocol support pending"));
        }
        auto* vendorLabel = new QLabel(vendorLines.join("\n"), this);
        vendorLabel->setObjectName("nanopositionerVendorsLabel");
        vendorLabel->setWordWrap(true);
        ui->groupVerticalLayout->insertWidget(0, vendorLabel);
		connect(ui->autofocusEnabledCheck, &QCheckBox::stateChanged, this, &NanopositionerTab::onAutofocusEnabledChanged);
		connect(ui->increaseVoltageBtn, &QPushButton::clicked, this, &NanopositionerTab::onIncreaseVoltage);
		connect(ui->decreaseVoltageBtn, &QPushButton::clicked, this, &NanopositionerTab::onDecreaseVoltage);
		connect(ui->targetRingWidthSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &NanopositionerTab::onTargetRingWidthChanged);

		// Load config first so probe/auto-connect use saved baud and device address
		loadConfig();
		// Populate COM port list (probe uses baud/address from config now)
		populateComPortList();
		updateNanopositionerUI();

		// Status update timer
		statusUpdateTimer_ = new QTimer(this);
		statusUpdateTimer_->setInterval(500);
		connect(statusUpdateTimer_, &QTimer::timeout, this, &NanopositionerTab::onUpdateAutofocusStatus);
		statusUpdateTimer_->start();

		// Set status callback for autofocus service
		backend_.autofocus().setStatusCallback([this](const std::string &message)
											   {
		if (ui->statusLabel) {
			ui->statusLabel->setText(QString::fromStdString(message));
		} });

		// Auto-connect is managed by DeviceInitManager (runs probe in worker, connect on main thread).

		// Persist nanopositioner voltage on app quit so next launch restores position
		connect(qApp, &QApplication::aboutToQuit, this, [this]() { saveConfig(); });
	}

	NanopositionerTab::~NanopositionerTab() {
		delete ui;
	}

    void NanopositionerTab::setDiscoveryRunning(bool running) {
        discoveryRunning_ = running;
        updateNanopositionerUI();
    }

	void NanopositionerTab::populateComPortList()
	{
		const int previouslySelected = ui->comPortCombo->currentData().toInt();
		ui->comPortCombo->clear();
		if (backend_.autofocus().isConnected())
		{
			// Do not probe while connected (SDK uses a single global COM handle). Show current port only.
			int port = backend_.autofocus().getComPort();
			ui->comPortCombo->addItem(QString("COM%1").arg(port), port);
			ui->comPortCombo->setCurrentIndex(0);
			return;
		}

		// List every COM port Windows reports instead of filtering by an active device probe.
		// The CoreMOR serial DLL keeps a single process-global serial handle, and probe reads
		// can be flaky immediately after another program releases the controller. Hiding ports
		// that fail a probe made it impossible to manually select a known-good port.
		std::vector<int> ports = backend::Tools::availableComPortNumbers();
		if (configuredComPort_ > 0 && std::find(ports.begin(), ports.end(), configuredComPort_) == ports.end())
		{
			ports.push_back(configuredComPort_);
			std::sort(ports.begin(), ports.end());
		}

		for (int port : ports)
		{
			ui->comPortCombo->addItem(QString("COM%1").arg(port), port);
		}

		int preferredPort = configuredComPort_ > 0 ? configuredComPort_ : previouslySelected;
		int preferredIndex = ui->comPortCombo->findData(preferredPort);
		if (preferredIndex >= 0)
		{
			ui->comPortCombo->setCurrentIndex(preferredIndex);
		}

		setNanopositionerStatus(ports.empty()
			? tr("No COM ports found. Check USB/power, then click Refresh.")
			: tr("Found %1 COM port(s). Select the nanopositioner port and click Connect.").arg(ports.size()));
	}

	int NanopositionerTab::getBaudRate() const
	{
		return ui->baudRateCombo->currentData().toInt();
	}

	int NanopositionerTab::getConfiguredComPort() const
	{
		return configuredComPort_;
	}

	unsigned char NanopositionerTab::getDeviceAddress() const
	{
		return static_cast<unsigned char>(ui->deviceAddressSpinBox->value());
	}

	void NanopositionerTab::setNanopositionerStatus(const QString &message)
	{
		if (ui->statusLabel)
		{
			ui->statusLabel->setText(message);
		}
	}

	void NanopositionerTab::applyAutoConnectResult(int port)
	{
		// Backend already connected by DeviceInitManager. Update combo to show selected port and save config.
		ui->comPortCombo->clear();
		ui->comPortCombo->addItem(QString("COM%1").arg(port), port);
		ui->comPortCombo->setCurrentIndex(0);
		saveConfig();
		updateNanopositionerUI();
	}

	void NanopositionerTab::updateNanopositionerUI()
	{
		auto &autofocus = backend_.autofocus();
		bool connected = autofocus.isConnected();
		bool enabled = autofocus.isEnabled();

		ui->connectBtn->setEnabled(!connected && !discoveryRunning_ && ui->comPortCombo->count() > 0);
		ui->disconnectBtn->setEnabled(connected);
		ui->comPortCombo->setEnabled(!connected && !discoveryRunning_);
		ui->refreshComPortBtn->setEnabled(!connected && !discoveryRunning_);
		ui->baudRateCombo->setEnabled(!connected && !discoveryRunning_);
		ui->deviceAddressSpinBox->setEnabled(!connected && !discoveryRunning_);
		ui->autofocusEnabledCheck->setEnabled(connected);
		ui->autofocusEnabledCheck->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
		ui->increaseVoltageBtn->setEnabled(connected);
		ui->decreaseVoltageBtn->setEnabled(connected);
		ui->voltageStepSpinBox->setEnabled(connected);

		if (connected)
		{
			double voltage = autofocus.getCurrentVoltage();
			ui->voltageLabel->setText(QString("Voltage: %1 V").arg(voltage, 0, 'f', 2));
		}
		else
		{
			ui->voltageLabel->setText("Voltage: -- V");
		}
	}

	void NanopositionerTab::onConnectNanopositioner()
	{
		if (discoveryRunning_ || ui->comPortCombo->count() == 0)
		{
			return;
		}
		int comPort = ui->comPortCombo->currentData().toInt();
		int baudRate = ui->baudRateCombo->currentData().toInt();
		unsigned char deviceAddress = static_cast<unsigned char>(ui->deviceAddressSpinBox->value());

		// Load config and apply to autofocus service
		loadConfig();

		bool success = backend_.autofocus().connect(comPort, baudRate, deviceAddress);
		if (success)
		{
			saveConfig();
			updateNanopositionerUI();
		}
		else
		{
			QMessageBox::warning(this, tr("Connection Failed"),
								 tr("Failed to connect to nanopositioner on COM%1").arg(comPort));
		}
	}

	void NanopositionerTab::onDisconnectNanopositioner()
	{
		saveConfig();
		backend_.autofocus().disconnect();
		updateNanopositionerUI();
	}

	void NanopositionerTab::onAutofocusEnabledChanged(int state)
	{
		backend_.autofocus().setEnabled(state == Qt::Checked);
	}

	void NanopositionerTab::onIncreaseVoltage()
	{
		// Update manual voltage step from UI
		backend::services::AutofocusService::Config config = backend_.autofocus().getConfig();
		config.manualVoltageStep = ui->voltageStepSpinBox->value();
		backend_.autofocus().setConfig(config);
		backend_.autofocus().increaseVoltage();
	}

	void NanopositionerTab::onDecreaseVoltage()
	{
		// Update manual voltage step from UI
		backend::services::AutofocusService::Config config = backend_.autofocus().getConfig();
		config.manualVoltageStep = ui->voltageStepSpinBox->value();
		backend_.autofocus().setConfig(config);
		backend_.autofocus().decreaseVoltage();
	}

	void NanopositionerTab::onUpdateAutofocusStatus()
	{
		updateNanopositionerUI();
	}

	void NanopositionerTab::onTargetRingWidthChanged(double value)
	{
		backend::services::AutofocusService::Config config = backend_.autofocus().getConfig();
		config.focusSetpoint = value;
		backend_.autofocus().setConfig(config);
		saveConfig();
	}

	QString NanopositionerTab::configPath() const
	{
		QSettings s;
		const QString external = s.value("Config/ExternalAppConfigPath").toString().trimmed();
		if (!external.isEmpty())
		{
			return external;
		}
		// Use centralized helper to get user-writable config directory
		return QDir(getUserConfigDir()).absoluteFilePath("config.json");
	}

	void NanopositionerTab::loadConfig()
	{
		QString path = configPath();
		ensureDefaultConfigExists(path);
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		{
			SPDLOG_WARN("Failed to load config.json from {}", path.toStdString());
			return;
		}

		try
		{
			QByteArray data = file.readAll();
			json config = json::parse(data.constData(), data.constData() + data.size());

			// Load autofocus settings
			if (config.contains("autofocus_com_port"))
			{
				int port = config["autofocus_com_port"].get<int>();
				configuredComPort_ = port;
				int idx = ui->comPortCombo->findData(port);
				if (idx >= 0)
				{
					ui->comPortCombo->setCurrentIndex(idx);
				}
			}
			if (config.contains("autofocus_baud_rate"))
			{
				int baudRate = config["autofocus_baud_rate"].get<int>();
				int index = ui->baudRateCombo->findData(baudRate);
				if (index >= 0)
				{
					ui->baudRateCombo->setCurrentIndex(index);
				}
			}
			if (config.contains("autofocus_device_address"))
			{
				ui->deviceAddressSpinBox->setValue(config["autofocus_device_address"].get<int>());
			}

			// Load autofocus service config
			backend::services::AutofocusService::Config afConfig;
			if (config.contains("autofocus_focus_setpoint"))
			{
				afConfig.focusSetpoint = config["autofocus_focus_setpoint"].get<double>();
				ui->targetRingWidthSpinBox->blockSignals(true);
				ui->targetRingWidthSpinBox->setValue(afConfig.focusSetpoint);
				ui->targetRingWidthSpinBox->blockSignals(false);
			}
			if (config.contains("autofocus_focus_range"))
			{
				afConfig.focusRange = config["autofocus_focus_range"].get<double>();
			}
			if (config.contains("autofocus_voltage_step"))
			{
				afConfig.voltageStep = config["autofocus_voltage_step"].get<double>();
			}
			if (config.contains("autofocus_fine_voltage_step"))
			{
				afConfig.fineVoltageStep = config["autofocus_fine_voltage_step"].get<double>();
			}
			if (config.contains("autofocus_max_voltage"))
			{
				afConfig.maxVoltage = config["autofocus_max_voltage"].get<double>();
			}
			if (config.contains("autofocus_min_voltage"))
			{
				afConfig.minVoltage = config["autofocus_min_voltage"].get<double>();
			}
			if (config.contains("autofocus_initial_voltage"))
			{
				afConfig.initialVoltage = config["autofocus_initial_voltage"].get<double>();
			}
			if (config.contains("autofocus_manual_voltage_step"))
			{
				afConfig.manualVoltageStep = config["autofocus_manual_voltage_step"].get<double>();
				ui->voltageStepSpinBox->setValue(static_cast<int>(afConfig.manualVoltageStep));
			}
			if (config.contains("ring_ratio_stale_ms"))
			{
				afConfig.ringRatioStaleMs = config["ring_ratio_stale_ms"].get<int>();
			}
			if (config.contains("require_new_sample_per_step"))
			{
				afConfig.requireNewSamplePerStep = config["require_new_sample_per_step"].get<bool>();
			}
			if (config.contains("autofocus_min_samples_per_step"))
			{
				afConfig.minSamplesPerStep = config["autofocus_min_samples_per_step"].get<int>();
			}
			if (config.contains("safe_shutdown_voltage"))
			{
				afConfig.safeShutdownVoltage = config["safe_shutdown_voltage"].get<double>();
			}
			if (config.contains("focus_direction"))
			{
				afConfig.focusDirection = config["focus_direction"].get<bool>();
			}

			backend_.autofocus().setConfig(afConfig);
		}
		catch (const std::exception &e)
		{
			SPDLOG_ERROR("Failed to parse config.json: {}", e.what());
		}
	}

	void NanopositionerTab::saveConfig()
	{
		QString path = configPath();
		QFile file(path);
		if (!file.open(QIODevice::ReadWrite | QIODevice::Text))
		{
			SPDLOG_WARN("Failed to save config.json to {}", path.toStdString());
			return;
		}

		try
		{
			QByteArray data = file.readAll();
			json config = json::parse(data.constData(), data.constData() + data.size());

			// Save autofocus settings
			if (ui->comPortCombo->currentIndex() >= 0)
			{
				configuredComPort_ = ui->comPortCombo->currentData().toInt();
				config["autofocus_com_port"] = configuredComPort_;
			}
			config["autofocus_baud_rate"] = ui->baudRateCombo->currentData().toInt();
			config["autofocus_device_address"] = ui->deviceAddressSpinBox->value();
			config["autofocus_focus_setpoint"] = ui->targetRingWidthSpinBox->value();

			// Persist current voltage as initial for next session when connected
			if (backend_.autofocus().isConnected())
			{
				auto cfg = backend_.autofocus().getConfig();
				double v = backend_.autofocus().getCurrentVoltage();
				v = std::clamp(v, cfg.minVoltage, cfg.maxVoltage);
				config["autofocus_initial_voltage"] = v;
			}

			file.resize(0);
			QTextStream out(&file);
			out << QString::fromStdString(config.dump(4));
		}
		catch (const std::exception &e)
		{
			SPDLOG_ERROR("Failed to save config.json: {}", e.what());
		}
	}

} // namespace frontend
