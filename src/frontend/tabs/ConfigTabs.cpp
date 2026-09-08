#include "frontend/tabs/ConfigTabs.h"
#include "frontend/utils/ElidingLabel.h"
#include "frontend/system/ConfigDocumentStore.h"

#include <QMenu>
#include <algorithm>
#include <QAction>
#include <QEvent>
#include <QResizeEvent>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTableView>
#include <QToolButton>
#include <QTimer>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QHeaderView>
#include <QJsonObject>
#include <QJsonArray>
#include <QComboBox>
#include <QCheckBox>
#include <QInputDialog>
#include <QRegularExpression>
#include <QScrollArea>
#include <QGridLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QLineEdit>

#include <spdlog/spdlog.h>
#ifdef _WIN32
#define NOMINMAX  // Prevent Windows.h from defining min/max macros
#include <windows.h>
#include <shlobj.h>
#endif

#include "backend/app/AppBackend.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/SerialBus.h"
#include "backend/processing/ProcessingService.h"
#include "frontend/system/ProfileManager.h"
#include "frontend/models/JsonTableModel.h"
#include "frontend/utils/JsonFlatten.h"

namespace frontend {

namespace {

// Get user-writable config directory, falling back to ../include/ for development
static QString getUserConfigDir() {
    QString appDir = QCoreApplication::applicationDirPath();
    QString appDirLower = appDir.toLower();
    
#ifdef _WIN32
    // Check if installed in Program Files (requires admin to write)
    if (appDirLower.contains("program files") || 
        appDirLower.contains("program files (x86)")) {
        // Use user-writable location
        char appDataPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath))) {
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

} // namespace

namespace {
// Page wrapper: the tab's minimum size is the scroll area's (tiny); wide raw
// forms scroll inside the inspector instead of widening the window (#361).
QScrollArea* wrapPageInScroll(QWidget* page)
{
    auto* scroll = new QScrollArea(page->parentWidget());
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setWidget(page);
    return scroll;
}
} // namespace

ConfigTabs::ConfigTabs(backend::AppBackend& backend, QWidget* parent)
    : QWidget(parent), backend_(backend) {
    auto* layout = new QVBoxLayout(this);
    // Important: let parent QSplitter shrink this widget.
    // Default layout constraints can impose a large minimum size, making splitter dragging feel “stuck”.
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    tabs_ = new QTabWidget(this);

    // ---- Issue #361: bounded primary header (whole inspector) --------------
    // Profile selector, compact state, Save/Reset for config.json, and one
    // native menu for every secondary/profile-management action. Path and
    // notices live in their own wrapping rows so long values never widen the
    // window; every value stays available via tooltip/copy.
    {
        headerWidget_ = new QWidget(this);
        headerWidget_->setObjectName(QStringLiteral("configHeader"));
        auto* headerV = new QVBoxLayout(headerWidget_);
        headerV->setContentsMargins(4, 4, 4, 0);
        headerV->setSpacing(2);
        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        row->addWidget(new QLabel(tr("Profile:"), headerWidget_));
        profileSelect_ = new QComboBox(headerWidget_);
        profileSelect_->setObjectName(QStringLiteral("profileSelect"));
        profileSelect_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        profileSelect_->setMinimumContentsLength(10);
        profileSelect_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        profileSelect_->setMaximumWidth(420);
        profileStatusLabel_ = new frontend::ElidingLabel(headerWidget_);
        profileStatusLabel_->setObjectName(QStringLiteral("profileStateLabel"));
        profileStatusLabel_->setElideMode(Qt::ElideRight);
        profileStatusLabel_->setMinimumVisibleCharacters(6);
        profileStatusLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        jsonReloadBtn_ = new QPushButton(tr("Reset"), headerWidget_);
        jsonReloadBtn_->setObjectName(QStringLiteral("appConfigResetBtn"));
        jsonReloadBtn_->setToolTip(tr("Reload config.json from disk, discarding unsaved edits."));
        jsonSaveBtn_ = new QPushButton(tr("Save"), headerWidget_);
        jsonSaveBtn_->setObjectName(QStringLiteral("appConfigSaveBtn"));
        jsonSaveBtn_->setToolTip(tr("Write the editor content to config.json. Camera scripts are applied "
                                    "separately with Apply to Camera; a saved file is not yet verified on hardware."));
        moreBtn_ = new QToolButton(headerWidget_);
        moreBtn_->setObjectName(QStringLiteral("configMoreBtn"));
        moreBtn_->setText(tr("More…"));
        moreBtn_->setToolTip(tr("Profile management, file selection and view options"));
        moreBtn_->setPopupMode(QToolButton::InstantPopup);
        moreBtn_->setFocusPolicy(Qt::StrongFocus);
        moreMenu_ = new QMenu(moreBtn_);
        saveProfileAct_ = moreMenu_->addAction(tr("Save as profile…"), this, &ConfigTabs::onSaveProfile);
        renameProfileAct_ = moreMenu_->addAction(tr("Rename profile…"), this, &ConfigTabs::onRenameProfile);
        deleteProfileAct_ = moreMenu_->addAction(tr("Delete profile…"), this, &ConfigTabs::onDeleteProfile);
        duplicateAsLocalAct_ = moreMenu_->addAction(tr("Duplicate as local profile…"), this, &ConfigTabs::onDuplicateProfileAsLocal);
        moreMenu_->addSeparator();
        checkUpdatesAct_ = moreMenu_->addAction(tr("Check for profile updates"), this, &ConfigTabs::onCheckProfileUpdates);
        updateSelectedAct_ = moreMenu_->addAction(tr("Update selected profile"), this, &ConfigTabs::onUpdateSelectedProfile);
        showDiffAct_ = moreMenu_->addAction(tr("Show diff against catalog…"), this, &ConfigTabs::onShowProfileDiff);
        moreMenu_->addSeparator();
        browseJsonAct_ = moreMenu_->addAction(tr("Open another config.json…"), this, &ConfigTabs::onBrowseJson);
        clearJsonAct_ = moreMenu_->addAction(tr("Use the default config.json"), this, &ConfigTabs::onClearJson);
        moreMenu_->addSeparator();
        jsonTableAct_ = moreMenu_->addAction(tr("Show config as table"));
        jsonTableAct_->setCheckable(true);
        connect(jsonTableAct_, &QAction::toggled, this, &ConfigTabs::onJsonTableToggled);
        moreBtn_->setMenu(moreMenu_);
        row->addWidget(profileSelect_, 2);
        row->addWidget(profileStatusLabel_, 1);
        row->addWidget(jsonReloadBtn_);
        row->addWidget(jsonSaveBtn_);
        row->addWidget(moreBtn_);
        headerV->addLayout(row);

        jsonPathLabel_ = new frontend::ElidingLabel(headerWidget_);
        jsonPathLabel_->setObjectName(QStringLiteral("appConfigPathLabel"));
        jsonPathLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        headerV->addWidget(jsonPathLabel_);

        jsonNoticeLabel_ = new QLabel(headerWidget_);
        jsonNoticeLabel_->setObjectName(QStringLiteral("appConfigNotices"));
        jsonNoticeLabel_->setWordWrap(true);
        jsonNoticeLabel_->setTextFormat(Qt::PlainText);
        jsonNoticeLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        jsonNoticeLabel_->setStyleSheet(QStringLiteral("color: #b05a00;"));
        jsonNoticeLabel_->setVisible(false);
        headerV->addWidget(jsonNoticeLabel_);
        layout->addWidget(headerWidget_);
    }

    // App JSON config tab (editor / grouped tables)
    {
        auto* page = new QWidget(this);
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(4, 4, 4, 4);
        jsonEdit_ = new QPlainTextEdit(page);
        jsonEdit_->setWordWrapMode(QTextOption::NoWrap);

        // Legacy single table (for backward compatibility)
        jsonModel_ = new JsonTableModel(this);
        jsonTable_ = new QTableView();
        jsonTable_->setModel(jsonModel_);
        jsonTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        jsonTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        jsonTable_->setAlternatingRowColors(true);
		jsonTable_->setSortingEnabled(false);
		jsonTable_->setWordWrap(true);
		jsonTable_->setTextElideMode(Qt::ElideNone);
		jsonTable_->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
		jsonTable_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		jsonTable_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

        jsonStack_ = new QStackedWidget(page);
        
        // New grouped tables layout
        jsonScrollArea_ = new QScrollArea();
        jsonScrollArea_->setWidgetResizable(true);
        jsonScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        jsonScrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        
        jsonGridContainer_ = new QWidget();
        jsonGridLayout_ = new QGridLayout(jsonGridContainer_);
        jsonGridLayout_->setSpacing(10);
        jsonGridLayout_->setContentsMargins(5, 5, 5, 5);
        
        jsonScrollArea_->setWidget(jsonGridContainer_);
        jsonScrollArea_->viewport()->installEventFilter(this);
        jsonRelayoutTimer_ = new QTimer(this);
        jsonRelayoutTimer_->setSingleShot(true);
        jsonRelayoutTimer_->setInterval(100);
        connect(jsonRelayoutTimer_, &QTimer::timeout, this, [this]() {
            if (jsonScrollArea_) relayoutJsonSections(jsonScrollArea_->viewport()->width());
        });

        jsonStack_->addWidget(jsonEdit_);
        jsonStack_->addWidget(jsonScrollArea_);  // Use scroll area instead of single table
        // Legacy table is not added to stack - it's kept for backward compatibility but hidden
        if (jsonTable_) {
            jsonTable_->setParent(nullptr);
            jsonTable_->hide();
        }
        v->addWidget(jsonStack_, 1);

        // Persist toggle choice
        {
            QSettings s;
            const bool showTable = s.value("Preview/ShowTable", true).toBool();
            const QSignalBlocker block(jsonTableAct_);
            jsonTableAct_->setChecked(showTable);
            jsonStack_->setCurrentIndex(showTable ? 1 : 0);
        }

		connect(jsonModel_, &QAbstractItemModel::dataChanged, this,
		        [this](const QModelIndex&, const QModelIndex&, const QVector<int>&) { rebuildJsonFromTable(); });

        // Debounced updates when editing JSON while table is visible
        jsonDebounceTimer_ = new QTimer(this);
        jsonDebounceTimer_->setSingleShot(true);
        jsonDebounceTimer_->setInterval(150);
        connect(jsonDebounceTimer_, &QTimer::timeout, this, &ConfigTabs::onJsonTextChangedDebounced);
        connect(jsonEdit_, &QPlainTextEdit::textChanged, this, [this]() {
            if (jsonStack_ && jsonStack_->currentIndex() == 1) {
                jsonDebounceTimer_->start();
            }
            // Explicit state: dirty is a content comparison (issue #361).
            jsonDoc_.markEdited(jsonEdit_->toPlainText());
            updateJsonNotices();
        });

        page->setLayout(v);
        tabs_->addTab(page, tr("Processing && app config"));
        tabs_->setTabToolTip(tabs_->count() - 1, tr("config.json — processing thresholds, acquisition settings, application options"));
        connect(jsonReloadBtn_, &QPushButton::clicked, this, &ConfigTabs::onReloadJson);
        connect(jsonSaveBtn_, &QPushButton::clicked, this, &ConfigTabs::onSaveJson);
    }

    // Camera JS script tab
    {
        auto* page = new QWidget(this);
        auto* v = new QVBoxLayout(page);
        jsEdit_ = new QPlainTextEdit(page);
        jsEdit_->setWordWrapMode(QTextOption::NoWrap);
        auto* row = new QHBoxLayout();
        jsReloadBtn_ = new QPushButton(tr("Reset"), page);
        jsSaveBtn_ = new QPushButton(tr("Save"), page);
        jsApplyBtn_ = new QPushButton(tr("Apply to Camera"), page);
        jsResetBtn_ = new QPushButton(tr("Reset Camera"), page);
        jsBrowseBtn_ = new QPushButton(tr("Browse..."), page);
        jsClearBtn_ = new QPushButton(tr("Clear"), page);
        jsPathLabel_ = new frontend::ElidingLabel(page);
        jsPathLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        jsUnsavedLabel_ = new QLabel(page);
        jsUnsavedLabel_->setText(tr("Edited – not saved. Save writes the script; Apply to Camera sends it to the device."));
        jsUnsavedLabel_->setVisible(false);
        jsUnsavedLabel_->setStyleSheet("color: #d17a00;");
        profilesIncludeJsCheck_ = new QCheckBox(tr("Profiles include Camera script"), page);
        {
            QSettings s;
            profilesIncludeJsCheck_->setChecked(s.value("Profiles/IncludeJs", true).toBool());
        }
        row->addWidget(jsReloadBtn_);
        row->addWidget(jsSaveBtn_);
        row->addWidget(jsApplyBtn_);
        row->addWidget(jsResetBtn_);
        row->addWidget(jsBrowseBtn_);
        row->addWidget(jsClearBtn_);
        row->addStretch(1);
        row->addWidget(profilesIncludeJsCheck_);
        v->addLayout(row);
        auto* jsInfoRow = new QHBoxLayout();
        jsInfoRow->addWidget(jsPathLabel_, 1);
        jsUnsavedLabel_->setWordWrap(true);
        v->addLayout(jsInfoRow);
        v->addWidget(jsUnsavedLabel_);
        v->addWidget(jsEdit_, 1);
        page->setLayout(v);
        tabs_->addTab(wrapPageInScroll(page), tr("Camera script (EGrabber)"));
        tabs_->setTabToolTip(tabs_->count() - 1, tr("egrabberConfig.js — GenICam camera script"));
        connect(jsReloadBtn_, &QPushButton::clicked, this, &ConfigTabs::onReloadJs);
        connect(jsSaveBtn_, &QPushButton::clicked, this, &ConfigTabs::onSaveJs);
        connect(jsApplyBtn_, &QPushButton::clicked, this, &ConfigTabs::onApplyJs);
        connect(jsResetBtn_, &QPushButton::clicked, this, &ConfigTabs::onResetCamera);
        connect(jsBrowseBtn_, &QPushButton::clicked, this, &ConfigTabs::onBrowseJs);
        connect(jsClearBtn_, &QPushButton::clicked, this, &ConfigTabs::onClearJs);
        connect(jsEdit_, &QPlainTextEdit::textChanged, this, [this]() {
            jsDoc_.markEdited(jsEdit_->toPlainText());
            if (jsUnsavedLabel_) jsUnsavedLabel_->setVisible(jsDoc_.dirty);
            emit documentStateChanged();
        });
        connect(profilesIncludeJsCheck_, &QCheckBox::toggled, this, &ConfigTabs::onIncludeJsToggled);
    }

    // MindVision config tab (acquisition trigger + strobe + pulse generator).
    // "Trigger" here means the camera's acquisition trigger — the sort-output
    // pulse lives in ExperimentMonitoringTab / TriggerService.
    {
        auto* page = new QWidget(this);
        auto* v = new QVBoxLayout(page);
        mvEdit_ = new QPlainTextEdit(page);
        mvEdit_->setWordWrapMode(QTextOption::NoWrap);
        auto* row = new QHBoxLayout();
        mvReloadBtn_ = new QPushButton(tr("Reset"), page);
        mvSaveBtn_ = new QPushButton(tr("Save"), page);
        mvApplyBtn_ = new QPushButton(tr("Apply to Camera"), page);
        mvApplyBtn_->setToolTip(tr("Requires a MindVision camera selected in the Connect tab. "
                                   "Stops capture, applies the config, and rebuilds the capture factory."));
        mvSoftTriggerBtn_ = new QPushButton(tr("Soft Trigger"), page);
        mvSoftTriggerBtn_->setToolTip(tr("Fires one software acquisition trigger. Requires capture "
                                         "running with trigger_mode 1 (software trigger)."));
        mvBrowseBtn_ = new QPushButton(tr("Browse..."), page);
        mvClearBtn_ = new QPushButton(tr("Clear"), page);
        mvPathLabel_ = new frontend::ElidingLabel(page);
        mvPathLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        mvUnsavedLabel_ = new QLabel(page);
        mvUnsavedLabel_->setText(tr("Edited – not saved. Save writes the file; Apply to Camera sends it to the device."));
        mvUnsavedLabel_->setVisible(false);
        mvUnsavedLabel_->setStyleSheet("color: #d17a00;");
        row->addWidget(mvReloadBtn_);
        row->addWidget(mvSaveBtn_);
        row->addWidget(mvApplyBtn_);
        row->addWidget(mvSoftTriggerBtn_);
        row->addWidget(mvBrowseBtn_);
        row->addWidget(mvClearBtn_);
        row->addStretch(1);
        v->addLayout(row);
        {
            auto* mvInfoRow = new QHBoxLayout();
            mvInfoRow->addWidget(mvPathLabel_, 1);
            v->addLayout(mvInfoRow);
            mvUnsavedLabel_->setWordWrap(true);
            v->addWidget(mvUnsavedLabel_);
        }

        // Quick-adjust form for the bench-relevant parameters. Two-way synced
        // with the JSON editor below: widget edits rewrite the JSON keys,
        // editor edits (debounced) repopulate the widgets. Apply/Save always
        // read the editor text, so the form never bypasses the config file.
        auto* mvForm = new QGroupBox(tr("Trigger && strobe parameters"), page);
        // Two labelled rows (trigger / strobe) instead of one 20-widget row so
        // the page's minimum width stays within the inspector budget (#361).
        auto* formGrid = new QGridLayout(mvForm);
        formGrid->setHorizontalSpacing(6);
        int mvFormRow = 0, mvFormCol = 0;
        auto mvFormAdd = [&](QWidget* w) { formGrid->addWidget(w, mvFormRow, mvFormCol++); };
        mvFormAdd(new QLabel(tr("Trigger"), mvForm));
        mvTriggerModeCombo_ = new QComboBox(mvForm);
        mvTriggerModeCombo_->addItem(tr("0: Free run"), 0);
        mvTriggerModeCombo_->addItem(tr("1: Software"), 1);
        mvTriggerModeCombo_->addItem(tr("2: External"), 2);
        mvFormAdd(mvTriggerModeCombo_);
        mvSignalTypeCombo_ = new QComboBox(mvForm);
        mvSignalTypeCombo_->addItem(tr("Rising edge"), 0);
        mvSignalTypeCombo_->addItem(tr("Falling edge"), 1);
        mvSignalTypeCombo_->addItem(tr("High level"), 2);
        mvSignalTypeCombo_->addItem(tr("Low level"), 3);
        mvSignalTypeCombo_->addItem(tr("Double edge"), 4);
        mvSignalTypeCombo_->setToolTip(tr("External trigger signal type (ext_trig_signal_type)"));
        mvFormAdd(mvSignalTypeCombo_);
        mvFormAdd(new QLabel(tr("Exposure (µs)"), mvForm));
        mvExposureSpin_ = new QDoubleSpinBox(mvForm);
        mvExposureSpin_->setRange(0.8, 838860.0); // MV-XGC51 sensor range
        mvExposureSpin_->setDecimals(1);
        mvExposureSpin_->setValue(1.0);
        mvFormAdd(mvExposureSpin_);
        mvFormAdd(new QLabel(tr("Delay (µs)"), mvForm));
        mvTrigDelaySpin_ = new QSpinBox(mvForm);
        mvTrigDelaySpin_->setRange(0, 1000000);
        mvTrigDelaySpin_->setToolTip(tr("Trigger edge to exposure start (acq_trigger_delay_us)"));
        mvFormAdd(mvTrigDelaySpin_);
        mvFormAdd(new QLabel(tr("Jitter (µs)"), mvForm));
        mvJitterSpin_ = new QSpinBox(mvForm);
        mvJitterSpin_->setRange(0, 1000000);
        mvJitterSpin_->setToolTip(tr("Trigger de-glitch filter (ext_trig_jitter_us)"));
        mvFormAdd(mvJitterSpin_);
        mvFormAdd(new QLabel(tr("Count"), mvForm));
        mvTrigCountSpin_ = new QSpinBox(mvForm);
        mvTrigCountSpin_->setRange(1, 1000);
        mvTrigCountSpin_->setToolTip(tr("Frames per trigger (trigger_count)"));
        mvFormAdd(mvTrigCountSpin_);
        mvFormCol++; // spacing column between trigger and strobe groups
        mvFormRow = 1; mvFormCol = 0;
        mvFormAdd(new QLabel(tr("Strobe"), mvForm));
        mvStrobeModeCombo_ = new QComboBox(mvForm);
        mvStrobeModeCombo_->addItem(tr("0: Auto (follows exposure)"), 0);
        mvStrobeModeCombo_->addItem(tr("1: Semi-auto (delay+width)"), 1);
        mvStrobeModeCombo_->addItem(tr("2: Always high"), 2);
        mvStrobeModeCombo_->addItem(tr("3: Always low"), 3);
        mvFormAdd(mvStrobeModeCombo_);
        mvFormAdd(new QLabel(tr("Delay (µs)"), mvForm));
        mvStrobeDelaySpin_ = new QSpinBox(mvForm);
        mvStrobeDelaySpin_->setRange(0, 1000000);
        mvFormAdd(mvStrobeDelaySpin_);
        mvFormAdd(new QLabel(tr("Width (µs)"), mvForm));
        mvStrobeWidthSpin_ = new QSpinBox(mvForm);
        mvStrobeWidthSpin_->setRange(0, 1000000);
        mvFormAdd(mvStrobeWidthSpin_);
        mvStrobePolarityCombo_ = new QComboBox(mvForm);
        mvStrobePolarityCombo_->addItem(tr("Active high"), 1);
        mvStrobePolarityCombo_->addItem(tr("Active low"), 0);
        mvStrobePolarityCombo_->setToolTip(tr("strobe_polarity"));
        mvFormAdd(mvStrobePolarityCombo_);
        formGrid->setColumnStretch(mvFormCol, 1);
        v->addWidget(mvForm);

        v->addWidget(mvEdit_, 1);

        auto* pgGroup = new QGroupBox(tr("Pulse generator (external trigger source, RS485)"), page);
        auto* pgLayout = new QVBoxLayout(pgGroup);
        // Bus row: physical port + serial settings + slave address. RS485 is a
        // multi-drop bus — one adapter can carry several Modbus devices, so
        // the device is (port, bus settings, address), and channel sits below.
        auto* pgBusRow = new QHBoxLayout();
        pgBusRow->addWidget(new QLabel(tr("Port"), pgGroup));
        pgPortCombo_ = new QComboBox(pgGroup);
        pgPortCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        pgPortCombo_->setMinimumContentsLength(12);
        pgPortCombo_->setToolTip(tr("System serial port (e.g. /dev/ttyUSB0 or COM3)."));
        pgBusRow->addWidget(pgPortCombo_, 1);
        pgRefreshPortsBtn_ = new QPushButton(tr("Refresh"), pgGroup);
        pgRefreshPortsBtn_->setToolTip(tr("Re-enumerate serial ports."));
        pgBusRow->addWidget(pgRefreshPortsBtn_);
        pgBusRow->addWidget(new QLabel(tr("Baud"), pgGroup));
        pgBaudCombo_ = new QComboBox(pgGroup);
        for (int baud : {4800, 9600, 14400, 19200, 38400, 56000, 57600, 115200}) {
            pgBaudCombo_->addItem(QString::number(baud), baud);
        }
        pgBaudCombo_->setCurrentText("9600"); // module factory default
        pgBusRow->addWidget(pgBaudCombo_);
        pgDataBitsCombo_ = new QComboBox(pgGroup);
        pgDataBitsCombo_->addItem(QStringLiteral("8"), 8);
        pgDataBitsCombo_->addItem(QStringLiteral("7"), 7);
        pgDataBitsCombo_->setToolTip(tr("Data bits"));
        pgBusRow->addWidget(pgDataBitsCombo_);
        pgParityCombo_ = new QComboBox(pgGroup);
        pgParityCombo_->addItem(QStringLiteral("N"), QChar('N'));
        pgParityCombo_->addItem(QStringLiteral("E"), QChar('E'));
        pgParityCombo_->addItem(QStringLiteral("O"), QChar('O'));
        pgParityCombo_->setToolTip(tr("Parity"));
        pgBusRow->addWidget(pgParityCombo_);
        pgStopBitsCombo_ = new QComboBox(pgGroup);
        pgStopBitsCombo_->addItem(QStringLiteral("1"), 1);
        pgStopBitsCombo_->addItem(QStringLiteral("2"), 2);
        pgStopBitsCombo_->setToolTip(tr("Stop bits"));
        pgBusRow->addWidget(pgStopBitsCombo_);
        pgBusRow->addWidget(new QLabel(tr("Addr"), pgGroup));
        pgAddrSpin_ = new QSpinBox(pgGroup);
        pgAddrSpin_->setRange(1, 255);
        pgAddrSpin_->setValue(1);
        pgAddrSpin_->setToolTip(tr("Modbus slave address of the pulse generator on this bus."));
        pgBusRow->addWidget(pgAddrSpin_);
        pgScanBtn_ = new QPushButton(tr("Scan"), pgGroup);
        pgScanBtn_->setToolTip(tr("Probe addresses 1–16 on the selected port with a read-only "
                                  "register read. Never writes to any device."));
        pgBusRow->addWidget(pgScanBtn_);
        pgConnectBtn_ = new QPushButton(tr("Connect"), pgGroup);
        pgBusRow->addWidget(pgConnectBtn_);
        pgBusRow->addStretch(1);
        pgLayout->addLayout(pgBusRow);

        auto* pgRow = new QHBoxLayout();
        pgRow->addWidget(new QLabel(tr("Ch"), pgGroup));
        pgChannelSpin_ = new QSpinBox(pgGroup);
        pgChannelSpin_->setRange(1, backend::services::PulseGeneratorService::CHANNEL_COUNT);
        pgRow->addWidget(pgChannelSpin_);
        pgRow->addWidget(new QLabel(tr("Freq (Hz)"), pgGroup));
        pgFreqSpin_ = new QDoubleSpinBox(pgGroup);
        pgFreqSpin_->setRange(backend::services::PulseGeneratorService::MIN_FREQUENCY_HZ,
                              backend::services::PulseGeneratorService::MAX_FREQUENCY_HZ);
        pgFreqSpin_->setDecimals(2);
        // 5000 Hz = the bench default trigger rate (5000 fps at 512x96 ROI,
        // 1 us exposure — see resources/defaults/mindvisionConfig.json).
        pgFreqSpin_->setValue(5000.0);
        pgRow->addWidget(pgFreqSpin_);
        pgRow->addWidget(new QLabel(tr("Duty (%)"), pgGroup));
        pgDutySpin_ = new QDoubleSpinBox(pgGroup);
        pgDutySpin_->setRange(0.0, 100.0);
        pgDutySpin_->setDecimals(2);
        pgDutySpin_->setValue(50.0);
        pgRow->addWidget(pgDutySpin_);
        pgApplyBtn_ = new QPushButton(tr("Set"), pgGroup);
        pgStartBtn_ = new QPushButton(tr("Start"), pgGroup);
        pgStartBtn_->setToolTip(tr("Enable the pulse train (writes the configured duty)."));
        pgStopBtn_ = new QPushButton(tr("Stop"), pgGroup);
        pgStopBtn_->setToolTip(tr("Gate the pulse train off (writes duty 0%, line idles low)."));
        pgRow->addWidget(pgApplyBtn_);
        pgRow->addWidget(pgStartBtn_);
        pgRow->addWidget(pgStopBtn_);
        pgRow->addStretch(1);
        pgStatusLabel_ = new QLabel(tr("Disconnected"), pgGroup);
        pgRow->addWidget(pgStatusLabel_);
        pgLayout->addLayout(pgRow);
        v->addWidget(pgGroup);

        page->setLayout(v);
        tabs_->addTab(wrapPageInScroll(page), tr("Camera trigger && strobe (MindVision)"));
        tabs_->setTabToolTip(tabs_->count() - 1, tr("mindvisionConfig.json — acquisition trigger, strobe and pulse generator"));
        connect(mvReloadBtn_, &QPushButton::clicked, this, &ConfigTabs::onReloadMv);
        connect(mvSaveBtn_, &QPushButton::clicked, this, &ConfigTabs::onSaveMv);
        connect(mvApplyBtn_, &QPushButton::clicked, this, &ConfigTabs::onApplyMvConfig);
        connect(mvSoftTriggerBtn_, &QPushButton::clicked, this, &ConfigTabs::onSoftTrigger);
        connect(mvBrowseBtn_, &QPushButton::clicked, this, &ConfigTabs::onBrowseMv);
        connect(mvClearBtn_, &QPushButton::clicked, this, &ConfigTabs::onClearMv);
        mvDebounceTimer_ = new QTimer(this);
        mvDebounceTimer_->setSingleShot(true);
        mvDebounceTimer_->setInterval(150);
        connect(mvDebounceTimer_, &QTimer::timeout, this, &ConfigTabs::onMvTextChangedDebounced);
        connect(mvEdit_, &QPlainTextEdit::textChanged, this, [this]() {
            mvDoc_.markEdited(mvEdit_->toPlainText());
            if (mvUnsavedLabel_) mvUnsavedLabel_->setVisible(mvDoc_.dirty);
            if (!mvSyncGuard_) mvDebounceTimer_->start();
            emit documentStateChanged();
        });
        for (auto* combo : {mvTriggerModeCombo_, mvSignalTypeCombo_, mvStrobeModeCombo_,
                            mvStrobePolarityCombo_}) {
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, &ConfigTabs::onMvFormChanged);
        }
        for (auto* spin : {mvTrigDelaySpin_, mvJitterSpin_, mvTrigCountSpin_,
                           mvStrobeDelaySpin_, mvStrobeWidthSpin_}) {
            connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                    this, &ConfigTabs::onMvFormChanged);
        }
        connect(mvExposureSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &ConfigTabs::onMvFormChanged);
        connect(pgConnectBtn_, &QPushButton::clicked, this, &ConfigTabs::onPulseGenConnectToggle);
        connect(pgApplyBtn_, &QPushButton::clicked, this, &ConfigTabs::onPulseGenApplySettings);
        connect(pgStartBtn_, &QPushButton::clicked, this, &ConfigTabs::onPulseGenStart);
        connect(pgStopBtn_, &QPushButton::clicked, this, &ConfigTabs::onPulseGenStop);
        connect(pgRefreshPortsBtn_, &QPushButton::clicked, this, &ConfigTabs::onPulseGenRefreshPorts);
        connect(pgScanBtn_, &QPushButton::clicked, this, &ConfigTabs::onPulseGenScanToggle);
        restorePulseGenSettings();
        refreshPulseGenUi();
    }

    layout->addWidget(tabs_, 1);

    onReloadJson();
    onReloadJs();
    onReloadMv();

    // Profiles: populate and wire (startup = intentional load of the last profile)
    refreshProfilesList(/*loadSelection=*/true);
    connect(profileSelect_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConfigTabs::onProfileSelectionChanged);
    refreshProfileStatusLabel();
}

// ---- Issue #361: state rendering / compact mode / reflow -------------------

bool ConfigTabs::eventFilter(QObject* watched, QEvent* event)
{
    if (jsonScrollArea_ && watched == jsonScrollArea_->viewport() && event->type() == QEvent::Resize) {
        if (jsonRelayoutTimer_) jsonRelayoutTimer_->start();
    }
    return QWidget::eventFilter(watched, event);
}

int ConfigTabs::columnsForWidth(int availableWidth) const
{
    // Font-aware minimum card width: ~40 average characters.
    const int card = std::max(260, fontMetrics().averageCharWidth() * 40);
    return std::clamp(availableWidth / card, 1, 3);
}

void ConfigTabs::relayoutJsonSections(int availableWidth, bool force)
{
    if (!jsonGridLayout_ || !jsonGridContainer_) return;
    const int cols = columnsForWidth(availableWidth);
    if (!force && cols == jsonColumns_) return;
    jsonColumns_ = cols;
    // Collect the existing group widgets in section order; no model touched.
    QStringList names = jsonSectionTables_.keys();
    names.sort(Qt::CaseInsensitive);
    QList<QWidget*> groups;
    for (const QString& name : names) {
        QString safe = name;
        safe.replace(QRegularExpression(QString("[^A-Za-z0-9_]")), QString("_"));
        if (auto* group = jsonGridContainer_->findChild<QGroupBox*>(QString("group_%1").arg(safe))) groups.push_back(group);
    }
    while (jsonGridLayout_->count() > 0) {
        QLayoutItem* item = jsonGridLayout_->takeAt(0);
        delete item;
    }
    for (int c = 0; c < 3; ++c) jsonGridLayout_->setColumnStretch(c, c < cols ? 1 : 0);
    int row = 0, col = 0;
    for (QWidget* group : groups) {
        jsonGridLayout_->addWidget(group, row, col, 1, 1, Qt::AlignTop);
        group->show();
        if (++col >= cols) { col = 0; ++row; }
    }
    for (int r = 0; r <= row; ++r) jsonGridLayout_->setRowStretch(r, 1);
}

QString ConfigTabs::compactSummary() const
{
    QString profile = profileSelect_ && profileSelect_->currentIndex() > 0 ? profileSelect_->currentText() : tr("No profile");
    return QStringLiteral("%1 · %2").arg(profile, profileStateText());
}

QString ConfigTabs::profileStateText() const
{
    return profileStatusLabel_ ? profileStatusLabel_->fullText() : QString();
}

QString ConfigTabs::noticesText() const
{
    return jsonNoticeLabel_ && jsonNoticeLabel_->isVisibleTo(const_cast<ConfigTabs*>(this)) ? jsonNoticeLabel_->text() : QString();
}

void ConfigTabs::setCompactMode(bool compact)
{
    if (compactMode_ == compact) return;
    compactMode_ = compact;
    if (tabs_) tabs_->setVisible(!compact);
    updateGeometry();
}

QString ConfigTabs::appConfigEditorText() const { return jsonEdit_ ? jsonEdit_->toPlainText() : QString(); }

void ConfigTabs::setAppConfigEditorText(const QString& text)
{
    if (jsonEdit_) jsonEdit_->setPlainText(text); // emits textChanged -> markEdited
}

int ConfigTabs::profileCount() const { return profileSelect_ ? profileSelect_->count() - 1 : 0; }

void ConfigTabs::updateProfileActionState()
{
    const bool selected = profileSelected_;
    if (renameProfileAct_) renameProfileAct_->setEnabled(selected);
    if (deleteProfileAct_) deleteProfileAct_->setEnabled(selected);
    if (duplicateAsLocalAct_) duplicateAsLocalAct_->setEnabled(selected);
    if (showDiffAct_) showDiffAct_->setEnabled(selected && profileHasRemote_);
    if (updateSelectedAct_) updateSelectedAct_->setEnabled(selected && profileUpdateAvailable_);
}

void ConfigTabs::updateJsonNotices()
{
    // Distinct states: Conflict > Edited > Saved > Loaded, plus profile tags.
    if (profileStatusLabel_) {
        QString state = jsonDoc_.conflict ? tr("Conflict") : jsonDoc_.dirty ? tr("Edited (unsaved)")
                        : jsonDoc_.lastSave == ConfigDocumentState::SaveOutcome::Saved ? tr("Saved") : tr("Loaded");
        if (profileIncompatible_) state += tr(" · incompatible");
        if (!profileTags_.isEmpty()) state += QStringLiteral(" · ") + profileTags_;
        profileStatusLabel_->setText(state);
    }
    QStringList lines;
    if (jsonDoc_.conflict) {
        lines << tr("config.json changed elsewhere while you have unsaved edits. Reset discards your edits and loads the file; "
                    "Save overwrites the file with your edits.");
    } else if (jsonDoc_.dirty) {
        lines << tr("Edited – not saved to config.json. Save writes the file; running processing picks up the saved file. "
                    "Camera scripts are applied separately (Apply to Camera).");
    }
    if (jsonDoc_.lastSave == ConfigDocumentState::SaveOutcome::Failed && !jsonDoc_.lastError.isEmpty()) {
        lines << tr("Last save failed: %1").arg(jsonDoc_.lastError);
    }
    if (profileIncompatible_) {
        lines << tr("The selected profile requires a different processing contract than the active core; it cannot be verified on this build.");
    }
    if (profileUpdateAvailable_) {
        lines << tr("A newer version of this profile is available in the catalog (More… › Update selected profile).");
    }
    if (jsonNoticeLabel_) {
        jsonNoticeLabel_->setText(lines.join(QStringLiteral("\n")));
        jsonNoticeLabel_->setVisible(!lines.isEmpty());
    }
    if (jsonSaveBtn_) jsonSaveBtn_->setEnabled(true);
    updateProfileActionState();
    emit documentStateChanged();
}

ConfigTabs::~ConfigTabs() {
    stopPulseGenScan();
}

QString ConfigTabs::appDirIncludePath(const QString& fileName) const {
    // Use centralized helper to get user-writable config directory
    return QDir(getUserConfigDir()).absoluteFilePath(fileName);
}

QString ConfigTabs::currentJsonPath() const {
    QSettings s;
    const QString ext = s.value("Config/ExternalAppConfigPath").toString().trimmed();
    if (!ext.isEmpty()) return ext;
    return defaultJsonPath();
}

QString ConfigTabs::currentJsPath() const {
    QSettings s;
    const QString ext = s.value("Config/ExternalCameraScriptPath").toString().trimmed();
    if (!ext.isEmpty()) return ext;
    return defaultJsPath();
}

void ConfigTabs::clearJsonSyncIndicators()
{
    // State lives in jsonDoc_ (reset by markLoaded/markSaved); just re-render.
    updateJsonNotices();
}

bool ConfigTabs::loadFileToEditor(const QString& path, QPlainTextEdit* editor, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = f.errorString();
        return false;
    }
    QTextStream in(&f);
    const QString content = in.readAll();
    const bool blocked = editor->blockSignals(true);
    editor->setPlainText(content);
    editor->blockSignals(blocked);
    if (editor == jsonEdit_) {
        jsonDoc_.markLoaded(path, content);
        clearJsonSyncIndicators();
    } else if (editor == jsEdit_) {
        jsDoc_.markLoaded(path, content);
        if (jsUnsavedLabel_) jsUnsavedLabel_->setVisible(false);
        emit documentStateChanged();
    } else if (editor == mvEdit_) {
        mvDoc_.markLoaded(path, content);
        if (mvUnsavedLabel_) mvUnsavedLabel_->setVisible(false);
        emit documentStateChanged();
    }
    return true;
}

bool ConfigTabs::saveEditorToFile(QPlainTextEdit* editor, const QString& path, QString* err) {
    ConfigDocumentState* doc = editor == jsonEdit_ ? &jsonDoc_ : editor == jsEdit_ ? &jsDoc_ : editor == mvEdit_ ? &mvDoc_ : nullptr;
    const QString text = editor->toPlainText();
    // Checked write (QSaveFile + verified commit). A file that changed on
    // disk since it was loaded is a conflict: ask before overwriting (never
    // in non-interactive/test mode).
    std::optional<QByteArray> expected;
    if (doc && doc->path == path) expected = doc->loadedFingerprint;
    ConfigWriteResult r = ConfigDocumentStore::writeText(path, text, expected, /*force=*/false);
    if (r.conflict) {
        bool overwrite = false;
        if (!nonInteractive_) {
            overwrite = QMessageBox::question(this, tr("File changed elsewhere"),
                                              tr("%1 changed on disk since it was loaded.\n\nOverwrite it with your edits?")
                                                  .arg(QFileInfo(path).fileName()),
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
        }
        if (!overwrite) {
            if (doc) doc->markSaveFailed(r.error, /*becauseOfConflict=*/true);
            if (err) *err = r.error;
            if (doc == &jsonDoc_) updateJsonNotices();
            return false;
        }
        r = ConfigDocumentStore::writeText(path, text, expected, /*force=*/true);
    }
    if (!r.ok) {
        if (doc) doc->markSaveFailed(r.error);
        if (err) *err = r.error;
        if (doc == &jsonDoc_) updateJsonNotices();
        return false;
    }
    if (doc) {
        doc->path = path;
        doc->markSaved(text);
        if (doc == &jsonDoc_) updateJsonNotices();
        else emit documentStateChanged();
    }
    return true;
}

static bool ensureDefaultsFile(const QString& targetPath, const QString& resourceName, QString* err) {
    QFileInfo fi(targetPath);
    QDir dir(fi.absolutePath());
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            if (err) *err = QObject::tr("Failed to create directory: %1").arg(dir.absolutePath());
            return false;
        }
    }
    if (QFile::exists(targetPath)) {
        return true;
    }
    QFile res(resourceName);
    if (!res.open(QIODevice::ReadOnly)) {
        if (err) *err = QObject::tr("Failed to open resource: %1").arg(resourceName);
        return false;
    }
    QFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QObject::tr("Failed to create: %1").arg(targetPath);
        return false;
    }
    const QByteArray data = res.readAll();
    if (out.write(data) != data.size()) {
        if (err) *err = QObject::tr("Failed to write: %1").arg(targetPath);
        return false;
    }
    return true;
}

void ConfigTabs::onReloadJson() {
    const QString path = currentJsonPath();
    if (path == defaultJsonPath()) {
        QString err;
        if (!ensureDefaultsFile(path, ":/defaults/config.json", &err)) {
            SPDLOG_WARN("ensureDefaultsFile(config.json) failed: {}", err.toStdString());
        }
    }
    QString err;
    if (!loadFileToEditor(path, jsonEdit_, &err)) {
        SPDLOG_WARN("Failed to load config.json from {}: {}", path.toStdString(), err.toStdString());
        if (!nonInteractive_) QMessageBox::warning(this, tr("Reset config.json"), tr("Failed to load: %1").arg(err));
        return;
    }
    jsonPathLabel_->setText(path);
    clearJsonSyncIndicators();
    if (jsonStack_ && jsonStack_->currentIndex() == 1) {
        refreshJsonTableModel();
    }
}

void ConfigTabs::onSaveJson() {
    const QString path = currentJsonPath();
    QString err;
    if (!saveEditorToFile(jsonEdit_, path, &err)) {
        SPDLOG_ERROR("Failed to save config.json to {}: {}", path.toStdString(), err.toStdString());
        if (!nonInteractive_) QMessageBox::warning(this, tr("Save config.json"), tr("Failed to save: %1").arg(err));
        return;
    }
    if (!nonInteractive_) QMessageBox::information(this, tr("Save config.json"), tr("Saved."));
    clearJsonSyncIndicators();
    if (jsonStack_ && jsonStack_->currentIndex() == 1) {
        refreshJsonTableModel();
    }
}

void ConfigTabs::onReloadJs() {
    const QString path = currentJsPath();
    if (path == defaultJsPath()) {
        QString err;
        if (!ensureDefaultsFile(path, ":/defaults/egrabberConfig.js", &err)) {
            SPDLOG_WARN("ensureDefaultsFile(egrabberConfig.js) failed: {}", err.toStdString());
        }
    }
    QString err;
    if (!loadFileToEditor(path, jsEdit_, &err)) {
        SPDLOG_WARN("Failed to load egrabberConfig.js from {}: {}", path.toStdString(), err.toStdString());
        if (!nonInteractive_) QMessageBox::warning(this, tr("Reset egrabberConfig.js"), tr("Failed to load: %1").arg(err));
        return;
    }
    jsPathLabel_->setText(path);
    if (jsUnsavedLabel_) jsUnsavedLabel_->setVisible(false);
}

void ConfigTabs::onSaveJs() {
    const QString path = currentJsPath();
    QString err;
    if (!saveEditorToFile(jsEdit_, path, &err)) {
        SPDLOG_ERROR("Failed to save egrabberConfig.js to {}: {}", path.toStdString(), err.toStdString());
        if (!nonInteractive_) QMessageBox::warning(this, tr("Save egrabberConfig.js"), tr("Failed to save: %1").arg(err));
        return;
    }
    if (!nonInteractive_) QMessageBox::information(this, tr("Save egrabberConfig.js"), tr("Saved."));
    if (jsUnsavedLabel_) jsUnsavedLabel_->setVisible(false);
}

void ConfigTabs::onResetCamera() {
    const auto reply = QMessageBox::question(
        this,
        tr("Reset Camera"),
        tr("This will send GenICam DeviceReset to the selected camera.\n\n"
           "Capture will be stopped and the camera may briefly disconnect.\n"
           "Proceed?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    if (jsResetBtn_) jsResetBtn_->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);

    std::string backendErr;
    const bool ok = backend_.resetSelectedHardwareCamera(&backendErr);

    QApplication::restoreOverrideCursor();
    if (jsResetBtn_) jsResetBtn_->setEnabled(true);

    if (!ok) {
        SPDLOG_ERROR("Reset Camera failed: {}", backendErr);
        if (!nonInteractive_) QMessageBox::warning(this, tr("Reset Camera"),
                             tr("Reset failed: %1").arg(QString::fromStdString(backendErr)));
        return;
    }

    QMessageBox::information(
        this,
        tr("Reset Camera"),
        tr("DeviceReset sent. The camera may briefly disconnect and require Refresh in the Connect tab."));
}

void ConfigTabs::onApplyJs() {
    const QString path = currentJsPath();
    QString err;
    // Always save first to ensure the latest content is applied
    {
        QString saveErr;
        if (!saveEditorToFile(jsEdit_, path, &saveErr)) {
            if (!nonInteractive_) QMessageBox::warning(this, tr("Apply Camera Script"), tr("Failed to save script: %1").arg(saveErr));
            return;
        }
    }

    std::string backendErr;
    if (!backend_.applyCameraScriptFromFile(path.toStdString(), &backendErr)) {
        if (!nonInteractive_) QMessageBox::warning(this,
                             tr("Apply Camera Script"),
                             tr("Failed to apply script: %1").arg(QString::fromStdString(backendErr)));
        return;
    }
    if (!nonInteractive_) QMessageBox::information(this, tr("Apply Camera Script"), tr("Applied to camera. Capture remains stopped."));
}

void ConfigTabs::onBrowseJson() {
    const QString current = currentJsonPath();
    const QString initialDir = QFileInfo(current).absolutePath();
    const QString selected = QFileDialog::getOpenFileName(this,
                                                          tr("Select App config (config.json)"),
                                                          initialDir,
                                                          tr("JSON files (*.json);;All Files (*.*)"));
    if (selected.isEmpty()) return;
    {
        QSettings s;
        s.setValue("Config/ExternalAppConfigPath", selected);
    }
    SPDLOG_INFO("External App config set to {}", selected.toStdString());
	emit appConfigPathChanged(selected);
    QString err;
    if (!loadFileToEditor(selected, jsonEdit_, &err)) {
        SPDLOG_WARN("Failed to load external config.json from {}: {}", selected.toStdString(), err.toStdString());
        if (!nonInteractive_) QMessageBox::warning(this, tr("Reset config.json"), tr("Failed to load: %1").arg(err));
        return;
    }
    jsonPathLabel_->setText(selected);
    clearJsonSyncIndicators();
    if (jsonStack_ && jsonStack_->currentIndex() == 1) {
        refreshJsonTableModel();
    }
}

void ConfigTabs::onClearJson() {
    QSettings s;
    s.remove("Config/ExternalAppConfigPath");
    SPDLOG_INFO("External App config cleared; reverting to default include path");
	emit appConfigPathChanged(currentJsonPath());
    const auto ret = QMessageBox::question(this,
                                           tr("Config Path Cleared"),
                                           tr("External App config path cleared.\nReset from default include path now?\n\nNote: Save to apply any changes."),
                                           QMessageBox::Yes | QMessageBox::No,
                                           QMessageBox::Yes);
    if (ret == QMessageBox::Yes) {
        onReloadJson();
    }
}

void ConfigTabs::onBrowseJs() {
    const QString current = currentJsPath();
    const QString initialDir = QFileInfo(current).absolutePath();
    const QString selected = QFileDialog::getOpenFileName(this,
                                                          tr("Select Camera script (egrabberConfig.js)"),
                                                          initialDir,
                                                          tr("JavaScript files (*.js);;All Files (*.*)"));
    if (selected.isEmpty()) return;
    {
        QSettings s;
        s.setValue("Config/ExternalCameraScriptPath", selected);
    }
    SPDLOG_INFO("External Camera script set to {}", selected.toStdString());
    QString err;
    if (!loadFileToEditor(selected, jsEdit_, &err)) {
        SPDLOG_WARN("Failed to load external egrabberConfig.js from {}: {}", selected.toStdString(), err.toStdString());
        if (!nonInteractive_) QMessageBox::warning(this, tr("Reset egrabberConfig.js"), tr("Failed to load: %1").arg(err));
        return;
    }
    jsPathLabel_->setText(selected);
    if (jsUnsavedLabel_) jsUnsavedLabel_->setVisible(false);
}

void ConfigTabs::onClearJs() {
    QSettings s;
    s.remove("Config/ExternalCameraScriptPath");
    SPDLOG_INFO("External Camera script cleared; reverting to default include path");
    const auto ret = QMessageBox::question(this,
                                           tr("Camera Script Path Cleared"),
                                           tr("External Camera script path cleared.\nReset from default include path now?\n\nNote: Save to apply any changes."),
                                           QMessageBox::Yes | QMessageBox::No,
                                           QMessageBox::Yes);
    if (ret == QMessageBox::Yes) {
        onReloadJs();
    }
}

QString ConfigTabs::currentMvJsonPath() const {
    QSettings s;
    const QString ext = s.value("Config/ExternalMindVisionConfigPath").toString().trimmed();
    if (!ext.isEmpty()) return ext;
    return defaultMvJsonPath();
}

void ConfigTabs::onReloadMv() {
    const QString path = currentMvJsonPath();
    if (path == defaultMvJsonPath()) {
        QString err;
        if (!ensureDefaultsFile(path, ":/defaults/mindvisionConfig.json", &err)) {
            SPDLOG_WARN("ensureDefaultsFile(mindvisionConfig.json) failed: {}", err.toStdString());
        }
    }
    QString err;
    if (!loadFileToEditor(path, mvEdit_, &err)) {
        SPDLOG_WARN("Failed to load MindVision config from {}: {}", path.toStdString(), err.toStdString());
        if (!nonInteractive_) QMessageBox::warning(this, tr("Reset mindvisionConfig.json"), tr("Failed to load: %1").arg(err));
        return;
    }
    mvPathLabel_->setText(path);
    if (mvUnsavedLabel_) mvUnsavedLabel_->setVisible(false);
    syncMvFormFromJson();
}

void ConfigTabs::onSaveMv() {
    const QString path = currentMvJsonPath();
    QString err;
    if (!saveEditorToFile(mvEdit_, path, &err)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Save mindvisionConfig.json"), tr("Failed to save: %1").arg(err));
        return;
    }
    if (mvUnsavedLabel_) mvUnsavedLabel_->setVisible(false);
    SPDLOG_INFO("MindVision config saved to {}", path.toStdString());
}

void ConfigTabs::onApplyMvConfig() {
    if (!backend_.isMindVisionCameraSelected()) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Apply MindVision Config"),
                             tr("Select a MindVision camera in the Connect tab first."));
        return;
    }
    const QString path = currentMvJsonPath();
    // Always save first so the applied file matches the editor
    {
        QString saveErr;
        if (!saveEditorToFile(mvEdit_, path, &saveErr)) {
            if (!nonInteractive_) QMessageBox::warning(this, tr("Apply MindVision Config"),
                                 tr("Failed to save config: %1").arg(saveErr));
            return;
        }
        if (mvUnsavedLabel_) mvUnsavedLabel_->setVisible(false);
    }

    std::string backendErr;
    if (!backend_.applyMindVisionConfigFromFile(path.toStdString(), &backendErr)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Apply MindVision Config"),
                             tr("Failed to apply: %1").arg(QString::fromStdString(backendErr)));
        return;
    }
    if (!nonInteractive_) QMessageBox::information(this, tr("Apply MindVision Config"),
                             tr("Applied to camera. Capture remains stopped."));
}

void ConfigTabs::onSoftTrigger() {
    std::string err;
    if (!backend_.softTriggerCamera(&err)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Soft Trigger"), QString::fromStdString(err));
    }
    // No success dialog — the button is pressed repeatedly during bench tests.
}

void ConfigTabs::onBrowseMv() {
    const QString current = currentMvJsonPath();
    const QString initialDir = QFileInfo(current).absolutePath();
    const QString selected = QFileDialog::getOpenFileName(this,
                                                          tr("Select MindVision config (mindvisionConfig.json)"),
                                                          initialDir,
                                                          tr("JSON files (*.json);;All Files (*.*)"));
    if (selected.isEmpty()) return;
    {
        QSettings s;
        s.setValue("Config/ExternalMindVisionConfigPath", selected);
    }
    SPDLOG_INFO("External MindVision config set to {}", selected.toStdString());
    QString err;
    if (!loadFileToEditor(selected, mvEdit_, &err)) {
        SPDLOG_WARN("Failed to load external mindvisionConfig.json from {}: {}", selected.toStdString(), err.toStdString());
        if (!nonInteractive_) QMessageBox::warning(this, tr("Reset mindvisionConfig.json"), tr("Failed to load: %1").arg(err));
        return;
    }
    mvPathLabel_->setText(selected);
    if (mvUnsavedLabel_) mvUnsavedLabel_->setVisible(false);
    syncMvFormFromJson();
}

void ConfigTabs::onClearMv() {
    QSettings s;
    s.remove("Config/ExternalMindVisionConfigPath");
    SPDLOG_INFO("External MindVision config cleared; reverting to default include path");
    const auto ret = QMessageBox::question(this,
                                           tr("MindVision Config Path Cleared"),
                                           tr("External MindVision config path cleared.\nReset from default include path now?\n\nNote: Save to apply any changes."),
                                           QMessageBox::Yes | QMessageBox::No,
                                           QMessageBox::Yes);
    if (ret == QMessageBox::Yes) {
        onReloadMv();
    }
}

namespace {
// Select the combo entry whose userData matches `value`; falls back to index 0.
void selectComboData(QComboBox* combo, int value) {
    const int idx = combo->findData(value);
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
}
} // namespace

void ConfigTabs::syncMvFormFromJson() {
    QJsonParseError parseErr{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(mvEdit_->toPlainText().toUtf8(), &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        return; // mid-edit invalid JSON: leave the form showing the last good state
    }
    const QJsonObject obj = doc.object();

    mvSyncGuard_ = true;
    selectComboData(mvTriggerModeCombo_, obj.value("trigger_mode").toInt(0));
    selectComboData(mvSignalTypeCombo_, obj.value("ext_trig_signal_type").toInt(0));
    mvExposureSpin_->setValue(obj.value("exposure_time_us").toDouble(3000.0));
    mvTrigDelaySpin_->setValue(obj.value("acq_trigger_delay_us").toInt(0));
    mvJitterSpin_->setValue(obj.value("ext_trig_jitter_us").toInt(0));
    mvTrigCountSpin_->setValue(obj.value("trigger_count").toInt(1));
    selectComboData(mvStrobeModeCombo_, obj.value("strobe_mode").toInt(0));
    mvStrobeDelaySpin_->setValue(obj.value("strobe_delay_us").toInt(0));
    mvStrobeWidthSpin_->setValue(obj.value("strobe_pulse_width_us").toInt(500));
    selectComboData(mvStrobePolarityCombo_, obj.value("strobe_polarity").toInt(1));
    mvSyncGuard_ = false;
}

void ConfigTabs::syncMvJsonFromForm() {
    QJsonParseError parseErr{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(mvEdit_->toPlainText().toUtf8(), &parseErr);
    // Start from the current text so form edits never drop keys the form
    // doesn't cover (ROI, gain, mirrors, ...). Invalid text starts fresh.
    QJsonObject obj = (doc.isObject()) ? doc.object() : QJsonObject{};

    obj["trigger_mode"] = mvTriggerModeCombo_->currentData().toInt();
    obj["ext_trig_signal_type"] = mvSignalTypeCombo_->currentData().toInt();
    obj["exposure_time_us"] = mvExposureSpin_->value();
    obj["acq_trigger_delay_us"] = mvTrigDelaySpin_->value();
    obj["ext_trig_jitter_us"] = mvJitterSpin_->value();
    obj["trigger_count"] = mvTrigCountSpin_->value();
    obj["strobe_mode"] = mvStrobeModeCombo_->currentData().toInt();
    obj["strobe_delay_us"] = mvStrobeDelaySpin_->value();
    obj["strobe_pulse_width_us"] = mvStrobeWidthSpin_->value();
    obj["strobe_polarity"] = mvStrobePolarityCombo_->currentData().toInt();

    mvSyncGuard_ = true;
    mvEdit_->setPlainText(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)));
    mvSyncGuard_ = false;
    if (mvUnsavedLabel_) mvUnsavedLabel_->setVisible(true);
}

void ConfigTabs::onMvFormChanged() {
    if (mvSyncGuard_) return;
    syncMvJsonFromForm();
}

void ConfigTabs::onMvTextChangedDebounced() {
    syncMvFormFromJson();
}

namespace {
// Extra item-data roles on the port combo so USB identity survives for
// persistence and node-rename re-resolution.
constexpr int PortRoleSerialNumber = Qt::UserRole + 1;
constexpr int PortRoleVid = Qt::UserRole + 2;
constexpr int PortRolePid = Qt::UserRole + 3;

backend::services::serialbus::SerialSettings pulseGenSettingsFromUi(
    QComboBox* baud, QComboBox* dataBits, QComboBox* parity, QComboBox* stopBits) {
    backend::services::serialbus::SerialSettings s;
    s.baudRate = baud->currentData().toInt();
    s.dataBits = dataBits->currentData().toInt();
    s.parity = parity->currentData().toChar().toLatin1();
    s.stopBits = stopBits->currentData().toInt();
    return s;
}
} // namespace

void ConfigTabs::refreshPulseGenUi() {
    auto& gen = backend_.pulseGenerator();
    const bool connected = gen.isConnected();
    pgConnectBtn_->setText(connected ? tr("Disconnect") : tr("Connect"));
    pgConnectBtn_->setEnabled(!pgScanRunning_);
    pgPortCombo_->setEnabled(!connected && !pgScanRunning_);
    pgRefreshPortsBtn_->setEnabled(!connected && !pgScanRunning_);
    pgBaudCombo_->setEnabled(!connected && !pgScanRunning_);
    pgDataBitsCombo_->setEnabled(!connected && !pgScanRunning_);
    pgParityCombo_->setEnabled(!connected && !pgScanRunning_);
    pgStopBitsCombo_->setEnabled(!connected && !pgScanRunning_);
    pgAddrSpin_->setEnabled(!connected && !pgScanRunning_);
    pgScanBtn_->setEnabled(!connected);
    pgScanBtn_->setText(pgScanRunning_ ? tr("Cancel scan") : tr("Scan"));
    pgApplyBtn_->setEnabled(connected);
    pgStartBtn_->setEnabled(connected);
    pgStopBtn_->setEnabled(connected);
    if (!connected) {
        const auto lastError = gen.lastError();
        if (pgScanRunning_) {
            pgStatusLabel_->setText(tr("Scanning…"));
        } else if (lastError == backend::services::PulseGeneratorService::LinkError::None) {
            pgStatusLabel_->setText(tr("Disconnected"));
        } else {
            pgStatusLabel_->setText(tr("Disconnected — %1")
                .arg(QString::fromLatin1(
                    backend::services::PulseGeneratorService::toString(lastError))));
        }
        return;
    }
    const auto status = gen.getStatus();
    const int ch = pgChannelSpin_->value() - 1;
    const auto& state = status.channels[static_cast<size_t>(ch)];
    pgStatusLabel_->setText(tr("Verified addr %1 — Ch%2: %3 Hz, %4%% duty, output %5")
                                .arg(gen.getConfig().modbusAddress)
                                .arg(ch + 1)
                                .arg(state.frequencyHz)
                                .arg(state.dutyPercent)
                                .arg(state.outputEnabled ? tr("ON") : tr("off")));
}

void ConfigTabs::refreshPulseGenPorts() {
    const QString previous = pgPortCombo_->currentData().toString();
    pgPortCombo_->clear();
    const auto ports = backend::services::serialbus::availablePorts();
    for (const auto& p : ports) {
        QString label = p.systemName;
        QStringList extra;
        if (!p.description.isEmpty()) extra << p.description;
        if (!p.serialNumber.isEmpty()) extra << tr("S/N %1").arg(p.serialNumber);
        if (p.vendorId != 0) {
            extra << QStringLiteral("%1:%2")
                         .arg(p.vendorId, 4, 16, QLatin1Char('0'))
                         .arg(p.productId, 4, 16, QLatin1Char('0'));
        }
        if (!extra.isEmpty()) label += QStringLiteral(" — ") + extra.join(QStringLiteral(", "));
        pgPortCombo_->addItem(label, p.systemName);
        const int idx = pgPortCombo_->count() - 1;
        pgPortCombo_->setItemData(idx, p.systemLocation, Qt::ToolTipRole);
        pgPortCombo_->setItemData(idx, p.serialNumber, PortRoleSerialNumber);
        pgPortCombo_->setItemData(idx, static_cast<uint>(p.vendorId), PortRoleVid);
        pgPortCombo_->setItemData(idx, static_cast<uint>(p.productId), PortRolePid);
    }
    if (pgPortCombo_->count() == 0) {
        pgPortCombo_->addItem(tr("No serial ports found"), QString());
    } else if (!previous.isEmpty()) {
        const int idx = pgPortCombo_->findData(previous);
        if (idx >= 0) pgPortCombo_->setCurrentIndex(idx);
    }
}

void ConfigTabs::onPulseGenRefreshPorts() {
    refreshPulseGenPorts();
}

void ConfigTabs::savePulseGenSettings() const {
    QSettings s;
    s.beginGroup(QStringLiteral("PulseGenerator"));
    s.setValue(QStringLiteral("PortName"), pgPortCombo_->currentData().toString());
    s.setValue(QStringLiteral("PortSerialNumber"),
               pgPortCombo_->currentData(PortRoleSerialNumber).toString());
    s.setValue(QStringLiteral("PortVid"), pgPortCombo_->currentData(PortRoleVid).toUInt());
    s.setValue(QStringLiteral("PortPid"), pgPortCombo_->currentData(PortRolePid).toUInt());
    s.setValue(QStringLiteral("Baud"), pgBaudCombo_->currentData().toInt());
    s.setValue(QStringLiteral("DataBits"), pgDataBitsCombo_->currentData().toInt());
    s.setValue(QStringLiteral("Parity"), QString(pgParityCombo_->currentData().toChar()));
    s.setValue(QStringLiteral("StopBits"), pgStopBitsCombo_->currentData().toInt());
    s.setValue(QStringLiteral("Address"), pgAddrSpin_->value());
    s.setValue(QStringLiteral("Channel"), pgChannelSpin_->value());
    s.setValue(QStringLiteral("FrequencyHz"), pgFreqSpin_->value());
    s.setValue(QStringLiteral("DutyPercent"), pgDutySpin_->value());
    s.endGroup();
}

void ConfigTabs::restorePulseGenSettings() {
    refreshPulseGenPorts();
    QSettings s;
    s.beginGroup(QStringLiteral("PulseGenerator"));
    auto selectCombo = [](QComboBox* combo, const QVariant& value) {
        const int idx = combo->findData(value);
        if (idx >= 0) combo->setCurrentIndex(idx);
    };
    if (s.contains(QStringLiteral("Baud")))
        selectCombo(pgBaudCombo_, s.value(QStringLiteral("Baud")).toInt());
    if (s.contains(QStringLiteral("DataBits")))
        selectCombo(pgDataBitsCombo_, s.value(QStringLiteral("DataBits")).toInt());
    if (s.contains(QStringLiteral("Parity")))
        selectCombo(pgParityCombo_,
                    QChar(s.value(QStringLiteral("Parity")).toString().isEmpty()
                              ? QChar('N')
                              : s.value(QStringLiteral("Parity")).toString().at(0)));
    if (s.contains(QStringLiteral("StopBits")))
        selectCombo(pgStopBitsCombo_, s.value(QStringLiteral("StopBits")).toInt());
    if (s.contains(QStringLiteral("Address")))
        pgAddrSpin_->setValue(s.value(QStringLiteral("Address")).toInt());
    if (s.contains(QStringLiteral("Channel")))
        pgChannelSpin_->setValue(s.value(QStringLiteral("Channel")).toInt());
    if (s.contains(QStringLiteral("FrequencyHz")))
        pgFreqSpin_->setValue(s.value(QStringLiteral("FrequencyHz")).toDouble());
    if (s.contains(QStringLiteral("DutyPercent")))
        pgDutySpin_->setValue(s.value(QStringLiteral("DutyPercent")).toDouble());

    // Port re-resolution: exact system name first; if the node was renamed
    // (ttyUSB0 -> ttyUSB1), fall back to the stored USB identity — but only
    // when it matches exactly one port, otherwise the operator must choose.
    const QString portName = s.value(QStringLiteral("PortName")).toString();
    const QString serialNumber = s.value(QStringLiteral("PortSerialNumber")).toString();
    const uint vid = s.value(QStringLiteral("PortVid")).toUInt();
    const uint pid = s.value(QStringLiteral("PortPid")).toUInt();
    s.endGroup();

    if (!portName.isEmpty()) {
        const int byName = pgPortCombo_->findData(portName);
        if (byName >= 0) {
            pgPortCombo_->setCurrentIndex(byName);
            return;
        }
    }
    if (!serialNumber.isEmpty() || vid != 0) {
        int match = -1;
        int matches = 0;
        for (int i = 0; i < pgPortCombo_->count(); ++i) {
            const bool serialOk =
                pgPortCombo_->itemData(i, PortRoleSerialNumber).toString() == serialNumber;
            const bool usbOk = pgPortCombo_->itemData(i, PortRoleVid).toUInt() == vid &&
                               pgPortCombo_->itemData(i, PortRolePid).toUInt() == pid;
            if (serialOk && usbOk) {
                match = i;
                ++matches;
            }
        }
        if (matches == 1) {
            pgPortCombo_->setCurrentIndex(match);
            SPDLOG_INFO("ConfigTabs: pulse-generator port re-resolved by USB identity to {}",
                        pgPortCombo_->currentData().toString().toStdString());
        } else if (matches > 1) {
            SPDLOG_WARN("ConfigTabs: {} ports share the stored USB identity — "
                        "operator must pick the pulse-generator port", matches);
        }
    }
}

void ConfigTabs::stopPulseGenScan() {
    pgScanCancel_.store(true);
    if (pgScanThread_.joinable()) {
        pgScanThread_.join();
    }
    pgScanRunning_ = false;
}

void ConfigTabs::onPulseGenScanToggle() {
    using ScanHit = backend::services::PulseGeneratorService::ScanHit;
    if (pgScanRunning_) {
        stopPulseGenScan();
        refreshPulseGenUi();
        return;
    }
    const QString portName = pgPortCombo_->currentData().toString();
    if (portName.isEmpty()) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Pulse Generator"),
                             tr("Select a serial port before scanning."));
        return;
    }
    if (pgScanThread_.joinable()) {
        pgScanThread_.join();
    }
    const auto settings = pulseGenSettingsFromUi(pgBaudCombo_, pgDataBitsCombo_,
                                                 pgParityCombo_, pgStopBitsCombo_);
    pgScanCancel_.store(false);
    pgScanRunning_ = true;
    refreshPulseGenUi();
    // Bounded, read-only, cancelable probe off the GUI thread; results are
    // marshaled back with a queued call.
    pgScanThread_ = std::thread([this, portName, settings]() {
        using LinkError = backend::services::PulseGeneratorService::LinkError;
        LinkError scanError = LinkError::None;
        const auto hits = backend_.pulseGenerator().scanBus(
            portName, settings, 1, 16, pgScanCancel_, 250, &scanError);
        QMetaObject::invokeMethod(this, [this, portName, hits, scanError]() {
            pgScanRunning_ = false;
            refreshPulseGenUi();
            if (pgScanCancel_.load()) {
                return;
            }
            if (scanError != backend::services::PulseGeneratorService::LinkError::None) {
                // The port itself could not be opened — very different advice
                // than a silent bus.
                QMessageBox::warning(
                    this, tr("Pulse Generator"),
                    tr("Could not open %1 for scanning: %2. The port may be held "
                       "by another program, or by MIB with different serial settings.")
                        .arg(portName)
                        .arg(QString::fromLatin1(
                            backend::services::PulseGeneratorService::toString(scanError))));
                return;
            }
            if (hits.empty()) {
                if (!nonInteractive_) QMessageBox::information(this, tr("Pulse Generator"),
                                         tr("No Modbus devices responded on %1 "
                                            "(addresses 1–16).").arg(portName));
                return;
            }
            QStringList lines;
            uint8_t firstGenerator = 0;
            for (const auto& hit : hits) {
                switch (hit.kind) {
                case ScanHit::Kind::PulseGenerator:
                    if (firstGenerator == 0) firstGenerator = hit.address;
                    lines << tr("Address %1 — pulse generator").arg(hit.address);
                    break;
                case ScanHit::Kind::ModbusDevice:
                    lines << tr("Address %1 — Modbus device (not a pulse generator, "
                                "left untouched)").arg(hit.address);
                    break;
                case ScanHit::Kind::Error:
                    lines << tr("Address %1 — corrupt/inconsistent response "
                                "(possible duplicate-address collision)").arg(hit.address);
                    break;
                }
            }
            if (firstGenerator != 0) {
                pgAddrSpin_->setValue(firstGenerator);
            }
            if (!nonInteractive_) QMessageBox::information(this, tr("Pulse Generator scan — %1").arg(portName),
                                     lines.join(QStringLiteral("\n")));
        }, Qt::QueuedConnection);
    });
}

void ConfigTabs::onPulseGenConnectToggle() {
    auto& gen = backend_.pulseGenerator();
    if (gen.isConnected()) {
        gen.disconnect();
    } else {
        const QString portName = pgPortCombo_->currentData().toString();
        if (portName.isEmpty()) {
            if (!nonInteractive_) QMessageBox::warning(this, tr("Pulse Generator"),
                                 tr("No serial port selected. Plug in the RS485 adapter "
                                    "and press Refresh."));
            return;
        }
        const auto settings = pulseGenSettingsFromUi(pgBaudCombo_, pgDataBitsCombo_,
                                                     pgParityCombo_, pgStopBitsCombo_);
        const auto addr = static_cast<uint8_t>(pgAddrSpin_->value());
        if (!gen.connect(portName, settings, addr)) {
            QMessageBox::warning(
                this, tr("Pulse Generator"),
                tr("Failed to connect on %1 (baud %2, addr %3): %4. "
                   "Check wiring, port and Modbus address.")
                    .arg(portName).arg(settings.baudRate).arg(addr)
                    .arg(QString::fromLatin1(
                        backend::services::PulseGeneratorService::toString(gen.lastError()))));
        } else {
            savePulseGenSettings();
        }
    }
    refreshPulseGenUi();
}

void ConfigTabs::onPulseGenApplySettings() {
    auto& gen = backend_.pulseGenerator();
    const int ch = pgChannelSpin_->value() - 1;
    bool ok = gen.setFrequency(ch, pgFreqSpin_->value());
    ok = gen.setDutyCycle(ch, pgDutySpin_->value()) && ok;
    if (!ok) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Pulse Generator"), tr("Failed to write settings to the module."));
    }
    refreshPulseGenUi();
}

void ConfigTabs::onPulseGenStart() {
    auto& gen = backend_.pulseGenerator();
    const int ch = pgChannelSpin_->value() - 1;
    // Push the current spinbox settings before enabling so Start alone is
    // enough after connect.
    if (!gen.setFrequency(ch, pgFreqSpin_->value()) ||
        !gen.setDutyCycle(ch, pgDutySpin_->value()) ||
        !gen.setOutputEnabled(ch, true)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Pulse Generator"), tr("Failed to start the pulse train."));
    }
    refreshPulseGenUi();
}

void ConfigTabs::onPulseGenStop() {
    auto& gen = backend_.pulseGenerator();
    const int ch = pgChannelSpin_->value() - 1;
    if (!gen.setOutputEnabled(ch, false)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Pulse Generator"), tr("Failed to stop the pulse train."));
    }
    refreshPulseGenUi();
}

void ConfigTabs::onJsonTableToggled(bool checked) {
    if (!jsonStack_) return;
    jsonStack_->setCurrentIndex(checked ? 1 : 0);
    QSettings s;
    s.setValue("Preview/ShowTable", checked);
    if (checked) {
        refreshJsonTableModel();
    }
}

void ConfigTabs::onJsonTextChangedDebounced() {
    if (jsonStack_ && jsonStack_->currentIndex() == 1) {
        refreshJsonTableModel();
    }
}

void ConfigTabs::refreshJsonTableModel() {
    if (!jsonModel_ || !jsonGridLayout_) return;
    const QByteArray bytes = jsonEdit_ ? jsonEdit_->toPlainText().toUtf8() : QByteArray();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError) {
        SPDLOG_WARN("JSON parse error at offset {}: {}", err.offset, err.errorString().toStdString());
        jsonModel_->clear();
        // Clear all section tables
        for (auto* model : jsonSectionModels_) {
            if (model) model->clear();
        }
        return;
    }
    
    // Use grouped layout
    const auto grouped = jsonutil::groupJsonBySections(doc);
    
    // Get list of current section names
    QStringList currentSections = jsonSectionTables_.keys();
    QStringList newSections = grouped.keys();
    
    // Remove sections that no longer exist
    for (const QString& sectionName : currentSections) {
        if (!newSections.contains(sectionName)) {
            QTableView* table = jsonSectionTables_.take(sectionName);
            JsonTableModel* model = jsonSectionModels_.take(sectionName);
            if (table && jsonGridLayout_) {
                // Find and remove group box (contains title + table)
                QString safe = sectionName;
                safe.replace(QRegularExpression(QString("[^A-Za-z0-9_]")), QString("_"));
                const QString groupName = QString("group_%1").arg(safe);
                if (auto* group = jsonGridContainer_->findChild<QGroupBox*>(groupName)) {
                    jsonGridLayout_->removeWidget(group);
                    group->deleteLater();
                } else {
                    // Fallback: ensure the table is removed if it was previously added directly
                    jsonGridLayout_->removeWidget(table);
                    table->deleteLater();
                }
            }
            if (model) {
                model->deleteLater();
            }
        }
    }
    
    // Clear grid layout (but keep widgets)
    if (jsonGridLayout_) {
        while (jsonGridLayout_->count() > 0) {
            QLayoutItem* item = jsonGridLayout_->takeAt(0);
            if (item && item->widget()) {
                item->widget()->hide();
            }
            delete item;
        }
    }
    
    // Update or create section tables and add to grid (column count follows
    // the viewport width; a later resize only moves widgets — issue #361)
    const int colsPerRow = jsonScrollArea_ ? columnsForWidth(jsonScrollArea_->viewport()->width()) : 3;
    jsonColumns_ = colsPerRow;
    for (int c = 0; c < 3; ++c) jsonGridLayout_->setColumnStretch(c, c < colsPerRow ? 1 : 0);
    QStringList sortedSections = grouped.keys();
    sortedSections.sort(Qt::CaseInsensitive);
    
    int gridRow = 0;
    int gridCol = 0;
    
    for (const QString& sectionName : sortedSections) {
        const jsonutil::FlattenTable& sectionTable = grouped[sectionName];
        
        QTableView* table = jsonSectionTables_.value(sectionName, nullptr);
        JsonTableModel* model = jsonSectionModels_.value(sectionName, nullptr);

        // Stable object name for reusing widgets between refreshes
        QString safe = sectionName;
        safe.replace(QRegularExpression(QString("[^A-Za-z0-9_]")), QString("_"));
        const QString groupName = QString("group_%1").arg(safe);
        QGroupBox* group = jsonGridContainer_->findChild<QGroupBox*>(groupName);
        
        if (!group) {
            group = new QGroupBox(sectionName, jsonGridContainer_);
            group->setObjectName(groupName);
            group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            auto* gv = new QVBoxLayout(group);
            gv->setContentsMargins(6, 6, 6, 6);
            gv->setSpacing(4);

            // Create new table + model
            model = new JsonTableModel(this);
            table = new QTableView(group);
            table->setModel(model);
            table->setSelectionBehavior(QAbstractItemView::SelectRows);
            table->setSelectionMode(QAbstractItemView::SingleSelection);
            table->setAlternatingRowColors(true);
            table->setSortingEnabled(false);
            table->setWordWrap(true);
            table->setTextElideMode(Qt::ElideNone);
            table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
            table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
            table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            table->setMinimumHeight(140);

            gv->addWidget(table, 1);

            // Connect dataChanged signal
            connect(model, &QAbstractItemModel::dataChanged, this,
                    [this](const QModelIndex&, const QModelIndex&, const QVector<int>&) { rebuildJsonFromTable(); });

            jsonSectionTables_[sectionName] = table;
            jsonSectionModels_[sectionName] = model;
        } else if (!table) {
            // Group exists from a previous refresh; recover the table/model pointers
            table = group->findChild<QTableView*>();
            model = table ? qobject_cast<JsonTableModel*>(table->model()) : nullptr;
            if (table) jsonSectionTables_[sectionName] = table;
            if (model) jsonSectionModels_[sectionName] = model;
        }
        
        // Update model data
        if (model) {
            model->setFromFlatten(sectionTable.columns, sectionTable.rows);
        }

        if (group) group->show();
        if (table) table->show();

        // Add grouped widget to grid (keeps header + body together)
        jsonGridLayout_->addWidget(group, gridRow, gridCol, 1, 1, Qt::AlignTop);
        
        // Resize columns
        if (table) {
            table->resizeColumnsToContents();
            table->resizeRowsToContents();
        }
        
        // Move to next column, wrap to next row if needed
        gridCol++;
        if (gridCol >= colsPerRow) {
            gridCol = 0;
            gridRow++;
        }
    }

    // Make rows expand from the top to use available space nicely
    for (int r = 0; r <= gridRow; ++r) {
        jsonGridLayout_->setRowStretch(r, 1);
    }
    
    // Also update legacy single table for backward compatibility
    const auto flattened = jsonutil::flattenJsonForTable(doc);
    jsonModel_->setFromFlatten(flattened.columns, flattened.rows);
    if (jsonTable_) {
        jsonTable_->resizeColumnsToContents();
		jsonTable_->resizeRowsToContents();
    }
}

void ConfigTabs::onExternalConfigFileChanged(const QString& path) {
    // Only reload if the changed file matches the current JSON path
    if (path != currentJsonPath()) {
        return;
    }

    // Explicit document state decides (issue #361): local edits are retained
    // and the conflict is shown whether or not this widget is visible.
    if (!jsonDoc_.markExternalChange()) {
        updateJsonNotices();
        SPDLOG_WARN("ConfigTabs: config changed externally while editor has unsaved changes (conflict retained)");
        return;
    }

    // Reload the file into the editor
    QString err;
    if (!loadFileToEditor(path, jsonEdit_, &err)) {
        SPDLOG_WARN("ConfigTabs: failed to reload config.json from {}: {}", path.toStdString(), err.toStdString());
        return;
    }

    // Refresh the JSON table if it's visible
    if (jsonStack_ && jsonStack_->currentIndex() == 1) {
        refreshJsonTableModel();
    }

    SPDLOG_DEBUG("ConfigTabs: reloaded config.json from external change");
}

void ConfigTabs::rebuildJsonFromTable() {
	if (!jsonEdit_) return;

	QMap<QString, jsonutil::FlattenTable> sections;

	// Collect data from all section tables
	for (auto it = jsonSectionModels_.constBegin(); it != jsonSectionModels_.constEnd(); ++it) {
		const QString& sectionName = it.key();
		JsonTableModel* model = it.value();
		if (!model) continue;
		
		const auto& cols = model->columns();
		const auto& rows = model->rows();
		sections.insert(sectionName, jsonutil::FlattenTable{cols, rows});
	}

	if (sections.isEmpty() && jsonModel_) {
		sections.insert(QStringLiteral("General"),
		                jsonutil::FlattenTable{jsonModel_->columns(), jsonModel_->rows()});
	}

	const QJsonDocument outDoc = jsonutil::rebuildJsonDocumentFromSections(sections);
	if (outDoc.isNull()) {
		return;
	}

	// Update editor without triggering table refresh loop
	const bool blocked = jsonEdit_->blockSignals(true);
	jsonEdit_->setPlainText(QString::fromUtf8(outDoc.toJson(QJsonDocument::Indented)));
	jsonEdit_->blockSignals(blocked);
	jsonDoc_.markEdited(jsonEdit_->toPlainText());
	updateJsonNotices();
}

// ===== Profiles helpers =====
QString ConfigTabs::profilesBaseDir() const {
    return profileManager_.profilesBaseDir();
}

bool ConfigTabs::ensureProfilesDirExists(QString* err) const {
    QDir dir(profileManager_.profilesBaseDir());
    if (dir.exists()) return true;
    if (!dir.mkpath(".")) {
        if (err) *err = tr("Failed to create profiles dir: %1").arg(dir.absolutePath());
        return false;
    }
    return true;
}

QStringList ConfigTabs::listProfiles() const {
    QStringList result;
    QString err;
    const auto summaries = profileManager_.scanLocalProfiles(
        true, remoteCatalog_ ? &*remoteCatalog_ : nullptr, &err,
        static_cast<int>(backend_.processing().activeProcessingCoreIdentity().contractVersion));
    if (!err.isEmpty()) {
        SPDLOG_WARN("ConfigTabs: failed to scan profiles for list: {}", err.toStdString());
    }
    for (const auto& summary : summaries) {
        result << summary.profileName;
    }
    return result;
}

void ConfigTabs::refreshProfilesList(bool loadSelection) {
    if (!profileSelect_) return;
    // Keep the identity currently selected (never its decorated label).
    const QString current = profileSelect_->currentData().toString();
    const QString last = !current.isEmpty() ? current : QSettings().value("Profiles/LastProfileName").toString();
    QString err;
    const auto summaries = profileManager_.scanLocalProfiles(
        true, remoteCatalog_ ? &*remoteCatalog_ : nullptr, &err,
        static_cast<int>(backend_.processing().activeProcessingCoreIdentity().contractVersion));
    if (!err.isEmpty()) {
        SPDLOG_WARN("ConfigTabs: profile scan warning: {}", err.toStdString());
    }
    const bool blocked = profileSelect_->blockSignals(true);
    profileSelect_->clear();
    profileSelect_->addItem(tr("<no profile>"), "");
    for (const auto& summary : summaries) {
        profileSelect_->addItem(profileLabelForSummary(summary), summary.profileName);
    }
    int idx = profileSelect_->findData(last);
    if (idx < 0) idx = 0;
    profileSelect_->setCurrentIndex(idx);
    profileSelect_->blockSignals(blocked);
    if (loadSelection && idx > 0) {
        onProfileSelectionChanged(idx);
    } else {
        refreshProfileStatusLabel();
    }
}

QString ConfigTabs::sanitizeProfileName(const QString& name) const {
    QString n = name.trimmed();
    // Remove invalid chars for folder names on Windows and *nix
    n.replace(QRegularExpression(QString("[\\\\/:*?\"<>|]")), QString("_"));
    if (n == "." || n == "..") n = "profile";
    if (n.isEmpty()) n = "profile";
    if (n.size() > 64) n = n.left(64);
    return n;
}

bool ConfigTabs::writeTextFile(const QString& path, const QString& content, QString* err) const {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (err) *err = f.errorString();
        return false;
    }
    QTextStream out(&f);
    out << content;
    return true;
}

bool ConfigTabs::readTextFile(const QString& path, QString* out, QString* err) const {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = f.errorString();
        return false;
    }
    QTextStream in(&f);
    *out = in.readAll();
    return true;
}

QString ConfigTabs::profileDirPath(const QString& profileName) const {
    return QDir(profilesBaseDir()).absoluteFilePath(profileName);
}
QString ConfigTabs::profileJsonPath(const QString& profileName) const {
    return QDir(profileDirPath(profileName)).absoluteFilePath("config.json");
}
QString ConfigTabs::profileJsPath(const QString& profileName) const {
    return QDir(profileDirPath(profileName)).absoluteFilePath("egrabberConfig.js");
}

QString ConfigTabs::selectedProfileName() const {
    if (!profileSelect_) {
        return QString();
    }
    return profileSelect_->currentData().toString();
}

std::optional<frontend::ProfileManager::LocalProfile> ConfigTabs::selectedProfileSummary() const {
    const QString name = selectedProfileName();
    if (name.isEmpty()) {
        return std::nullopt;
    }
    QString err;
    const auto summaries = profileManager_.scanLocalProfiles(
        true, remoteCatalog_ ? &*remoteCatalog_ : nullptr, &err,
        static_cast<int>(backend_.processing().activeProcessingCoreIdentity().contractVersion));
    if (!err.isEmpty()) {
        SPDLOG_WARN("ConfigTabs: failed to fetch selected profile summary: {}", err.toStdString());
    }
    for (const auto& summary : summaries) {
        if (summary.profileName == name) {
            return summary;
        }
    }
    return std::nullopt;
}

std::optional<frontend::ProfileManager::CatalogEntry> ConfigTabs::selectedRemoteCatalogEntry() const {
    const auto summary = selectedProfileSummary();
    if (!summary.has_value() || !summary->remoteEntry.has_value()) {
        return std::nullopt;
    }
    return summary->remoteEntry;
}

QString ConfigTabs::profileLabelForSummary(const frontend::ProfileManager::LocalProfile& summary) const {
    QStringList tags;
    if (summary.remoteEntry.has_value()) {
        tags << tr("remote");
    } else {
        tags << tr("local");
    }
    if (summary.updateAvailable) {
        tags << tr("update available");
    }
    if (summary.dirty) {
        tags << tr("dirty");
    }
    if (summary.incompatible) {
        tags << tr("incompatible");
    }
    const QString base = summary.displayName.trimmed().isEmpty() ? summary.profileName : summary.displayName.trimmed();
    return tags.isEmpty() ? base : QStringLiteral("%1 [%2]").arg(base, tags.join(QStringLiteral(", ")));
}

void ConfigTabs::refreshProfileStatusLabel() {
    const auto summary = selectedProfileSummary();
    profileSelected_ = summary.has_value();
    profileHasRemote_ = summary.has_value() && summary->remoteEntry.has_value();
    profileIncompatible_ = summary.has_value() && summary->incompatible;
    profileUpdateAvailable_ = summary.has_value() && summary->updateAvailable;
    if (!summary.has_value()) {
        profileTags_ = tr("no profile");
        if (profileStatusLabel_) profileStatusLabel_->setToolTip(QString());
        updateJsonNotices();
        return;
    }
    QStringList details;
    details << (summary->remoteEntry.has_value() ? tr("remote") : tr("local-only"));
    if (summary->updateAvailable) details << tr("update available");
    if (summary->dirty) details << tr("differs from catalog");
    profileTags_ = details.join(QStringLiteral(", "));
    updateJsonNotices();
    if (profileStatusLabel_) {
        const int requiredContract = summary->remoteEntry.has_value()
            ? summary->remoteEntry->processingContractVersion
            : summary->metadata.processingContractVersion;
        profileStatusLabel_->setToolTip(QStringLiteral(
                                            "Profile: %1\nConfig: %2\nMetadata: %3\n"
                                            "Required processing contract: %4\nActive processing contract: %5\n"
                                            "Editor: %6")
                                            .arg(summary->profileName,
                                                 summary->hasConfig ? summary->configPath : tr("missing"),
                                                 summary->hasMetadata ? summary->metaPath : tr("missing"))
                                            .arg(requiredContract > 0 ? QString::number(requiredContract)
                                                                      : tr("any"))
                                            .arg(backend_.processing()
                                                     .activeProcessingCoreIdentity()
                                                     .contractVersion)
                                            .arg(jsonDoc_.stateLabel()));
    }
}

void ConfigTabs::showDiffDialog(const QString& title, const QVector<frontend::ProfileManager::DiffRow>& rows) {
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(1100, 640);
    auto* layout = new QVBoxLayout(&dialog);
    auto* table = new QTableWidget(&dialog);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels({tr("Path"), tr("Status"), tr("Local value"), tr("Remote value"), tr("Risk"), tr("Source")});
    table->setRowCount(rows.size());
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);

    for (int i = 0; i < rows.size(); ++i) {
        const auto& row = rows.at(i);
        const QString status = frontend::ProfileManager::diffStatusToString(row.status);
        table->setItem(i, 0, new QTableWidgetItem(row.path));
        table->setItem(i, 1, new QTableWidgetItem(status));
        table->setItem(i, 2, new QTableWidgetItem(row.localValue));
        table->setItem(i, 3, new QTableWidgetItem(row.remoteValue));
        table->setItem(i, 4, new QTableWidgetItem(row.risk));
        table->setItem(i, 5, new QTableWidgetItem(row.source));
    }

    layout->addWidget(table, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void ConfigTabs::loadSelectedProfileInternal(const QString& profileName) {
    if (profileName.isEmpty()) return;
    const QString cfgPath = profileJsonPath(profileName);
    if (!QFile::exists(cfgPath)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Load Profile"), tr("Profile missing config.json: %1").arg(profileName));
        return;
    }
    // Set external paths and reload
    {
        QSettings s;
        s.setValue("Config/ExternalAppConfigPath", cfgPath);
        s.setValue("Profiles/LastProfileName", profileName);
    }
    SPDLOG_INFO("Profiles: loading profile '{}' (json: {})", profileName.toStdString(), cfgPath.toStdString());
    emit appConfigPathChanged(cfgPath);
    onReloadJson();
    // JS optional
    const bool includeJs = profilesIncludeJsCheck_ ? profilesIncludeJsCheck_->isChecked() : true;
    const QString jsPath = profileJsPath(profileName);
    if (includeJs && QFile::exists(jsPath)) {
        QSettings().setValue("Config/ExternalCameraScriptPath", jsPath);
        onReloadJs();
    }
    refreshProfileStatusLabel();
}

// ===== Profiles slots =====
void ConfigTabs::onProfileSelectionChanged(int index) {
    if (!profileSelect_) return;
    const QString profileName = profileSelect_->itemData(index).toString();
    if (profileName.isEmpty()) {
        // Switch back to default include path (no active profile)
        QSettings s;
        s.remove("Profiles/LastProfileName");
        s.remove("Config/ExternalAppConfigPath");
        s.remove("Config/ExternalCameraScriptPath");
        SPDLOG_INFO("Profiles: cleared active profile; reverting to default include paths");
        onReloadJson();
        onReloadJs();
        refreshProfileStatusLabel();
        return;
    }
    loadSelectedProfileInternal(profileName);
}

void ConfigTabs::onSaveProfile() {
    QString name = QInputDialog::getText(this, tr("Save Profile"),
                                         tr("Profile name:"), QLineEdit::Normal);
    if (name.isNull()) return; // cancelled
    name = sanitizeProfileName(name);
    if (name.isEmpty()) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Save Profile"), tr("Invalid profile name."));
        return;
    }
    QString err;
    if (!ensureProfilesDirExists(&err)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Save Profile"), err);
        return;
    }
    const QString dir = profileDirPath(name);
    QDir().mkpath(dir);
    const QString cfgPath = profileJsonPath(name);
    const QString jsPath = profileJsPath(name);
    const bool includeJs = profilesIncludeJsCheck_ ? profilesIncludeJsCheck_->isChecked() : true;

    // Prepare JSON (pretty) from editor; validate
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonEdit_ ? jsonEdit_->toPlainText().toUtf8() : QByteArray(), &perr);
    QString jsonToWrite;
    if (perr.error == QJsonParseError::NoError) {
        jsonToWrite = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    } else {
        const auto ret = QMessageBox::question(this, tr("Invalid JSON"),
                                               tr("JSON in editor is invalid.\nError: %1\n\nForce-save raw text to profile anyway?")
                                               .arg(perr.errorString()),
                                               QMessageBox::Yes | QMessageBox::No,
                                               QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
        jsonToWrite = jsonEdit_ ? jsonEdit_->toPlainText() : QString();
    }

    // Confirm overwrite if target exists
    if (QFile::exists(cfgPath)) {
        const auto ret = QMessageBox::question(this, tr("Overwrite"),
                                               tr("Profile already has config.json. Overwrite?"),
                                               QMessageBox::Yes | QMessageBox::No,
                                               QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
    }
    if (!writeTextFile(cfgPath, jsonToWrite, &err)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Save Profile"), tr("Failed to write config.json: %1").arg(err));
        return;
    }

    if (includeJs) {
        const QString jsText = jsEdit_ ? jsEdit_->toPlainText() : QString();
        if (!jsText.isEmpty()) {
            if (QFile::exists(jsPath)) {
                const auto ret2 = QMessageBox::question(this, tr("Overwrite"),
                                                        tr("Profile already has egrabberConfig.js. Overwrite?"),
                                                        QMessageBox::Yes | QMessageBox::No,
                                                        QMessageBox::No);
                if (ret2 != QMessageBox::Yes) return;
            }
            if (!writeTextFile(jsPath, jsText, &err)) {
                if (!nonInteractive_) QMessageBox::warning(this, tr("Save Profile"), tr("Failed to write egrabberConfig.js: %1").arg(err));
                return;
            }
        }
    }

    // Update QSettings, refresh list, select and load
    {
        QSettings s;
        s.setValue("Profiles/LastProfileName", name);
    }
    refreshProfilesList(/*loadSelection=*/false);
    const int idx = profileSelect_ ? profileSelect_->findData(name) : -1;
    if (idx >= 0 && profileSelect_->currentIndex() != idx) {
        profileSelect_->setCurrentIndex(idx); // will trigger load
    } else {
        loadSelectedProfileInternal(name);
    }
    SPDLOG_INFO("Profiles: saved profile '{}' (json={}, js_included={})",
                name.toStdString(), cfgPath.toStdString(), includeJs ? 1 : 0);
    refreshProfileStatusLabel();
}

void ConfigTabs::onDeleteProfile() {
    if (!profileSelect_ || profileSelect_->currentIndex() <= 0) {
        if (!nonInteractive_) QMessageBox::information(this, tr("Delete Profile"), tr("No profile selected."));
        return;
    }
    const QString name = profileSelect_->currentData().toString();
    const auto ret = QMessageBox::question(this, tr("Delete Profile"),
                                           tr("Delete profile '%1'?\nThis cannot be undone.").arg(name),
                                           QMessageBox::Yes | QMessageBox::No,
                                           QMessageBox::No);
    if (ret != QMessageBox::Yes) return;
    // If active, revert to default before delete
    const QString activeJson = currentJsonPath();
    const QString nameJson = profileJsonPath(name);
    if (QFileInfo(activeJson).absoluteFilePath() == QFileInfo(nameJson).absoluteFilePath()) {
        QSettings s;
        s.remove("Config/ExternalAppConfigPath");
        s.remove("Config/ExternalCameraScriptPath");
        s.remove("Profiles/LastProfileName");
        SPDLOG_INFO("Profiles: deleting active profile, reverting to defaults");
        onReloadJson();
        onReloadJs();
    }
    QDir dir(profileDirPath(name));
    bool ok = dir.removeRecursively();
    if (!ok) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Delete Profile"), tr("Failed to delete profile directory."));
        return;
    }
    SPDLOG_INFO("Profiles: deleted profile '{}'", name.toStdString());
    refreshProfilesList(/*loadSelection=*/true);
}

void ConfigTabs::onRenameProfile() {
    if (!profileSelect_ || profileSelect_->currentIndex() <= 0) {
        if (!nonInteractive_) QMessageBox::information(this, tr("Rename Profile"), tr("No profile selected."));
        return;
    }
    const QString oldName = profileSelect_->currentData().toString();
    QString newName = QInputDialog::getText(this, tr("Rename Profile"),
                                            tr("New profile name:"), QLineEdit::Normal,
                                            oldName);
    if (newName.isNull()) return;
    newName = sanitizeProfileName(newName);
    if (newName == oldName) return;
    if (newName.isEmpty()) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Rename Profile"), tr("Invalid profile name."));
        return;
    }
    const QString oldDir = profileDirPath(oldName);
    const QString newDir = profileDirPath(newName);
    if (QFile::exists(newDir)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Rename Profile"), tr("A profile with that name already exists."));
        return;
    }
    QDir base(profilesBaseDir());
    if (!base.rename(oldName, newName)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Rename Profile"), tr("Failed to rename profile directory."));
        return;
    }
    // If active, update settings
    const QString activeJson = currentJsonPath();
    const QString oldJson = profileJsonPath(oldName);
    if (QFileInfo(activeJson).absoluteFilePath() == QFileInfo(oldJson).absoluteFilePath()) {
        QSettings s;
        s.setValue("Config/ExternalAppConfigPath", profileJsonPath(newName));
        const bool includeJs = profilesIncludeJsCheck_ ? profilesIncludeJsCheck_->isChecked() : true;
        if (includeJs && QFile::exists(profileJsPath(newName))) {
            s.setValue("Config/ExternalCameraScriptPath", profileJsPath(newName));
        }
        s.setValue("Profiles/LastProfileName", newName);
        SPDLOG_INFO("Profiles: active profile renamed to '{}'", newName.toStdString());
        onReloadJson();
        onReloadJs();
    }
    refreshProfilesList(/*loadSelection=*/true);
    const int idx = profileSelect_->findData(newName);
    if (idx >= 0) profileSelect_->setCurrentIndex(idx);
    refreshProfileStatusLabel();
}

void ConfigTabs::onCheckProfileUpdates() {
    const QUrl catalogUrl = profileManager_.catalogUrlFromEnvOrDefault(QStringLiteral("stable"));
    QString err;
    const auto catalog = profileManager_.fetchCatalog(catalogUrl, &err);
    if (!catalog.has_value()) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Check Updates"), tr("Failed to fetch catalog:\n%1").arg(err));
        SPDLOG_WARN("ConfigTabs: catalog fetch failed from {}: {}", catalogUrl.toString().toStdString(), err.toStdString());
        return;
    }
    if (catalog->catalogSchemaVersion <= 0) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Check Updates"), tr("Catalog is missing catalog_schema_version."));
        return;
    }

    remoteCatalog_ = catalog;
    refreshProfilesList(/*loadSelection=*/false); // passive: never reloads the edited document

    const int remoteCount = catalog->profiles.size();
    int updateCount = 0;
    const auto refreshed = profileManager_.scanLocalProfiles(
        true, &*remoteCatalog_, nullptr,
        static_cast<int>(backend_.processing().activeProcessingCoreIdentity().contractVersion));
    for (const auto& profile : refreshed) {
        if (profile.updateAvailable) {
            ++updateCount;
        }
    }
    if (!nonInteractive_) QMessageBox::information(this,
                             tr("Check Updates"),
                             tr("Catalog refreshed from %1.\n\nProfiles in catalog: %2\nProfiles with updates: %3")
                                 .arg(catalogUrl.toString())
                                 .arg(remoteCount)
                                 .arg(updateCount));
}

void ConfigTabs::onUpdateSelectedProfile() {
    const auto selected = selectedProfileSummary();
    if (!selected.has_value()) {
        if (!nonInteractive_) QMessageBox::information(this, tr("Update Selected"), tr("No profile selected."));
        return;
    }
    if (!selected->remoteEntry.has_value()) {
        if (!nonInteractive_) QMessageBox::information(this, tr("Update Selected"), tr("The selected profile does not have remote catalog data."));
        return;
    }
    const auto ret = QMessageBox::question(this,
                                           tr("Update Selected"),
                                           tr("Update profile '%1' from the public catalog?\n\nThis will replace config.json and any matching camera script after checksum verification.").arg(selected->profileName),
                                           QMessageBox::Yes | QMessageBox::No,
                                           QMessageBox::Yes);
    if (ret != QMessageBox::Yes) {
        return;
    }

    QString err;
    if (!profileManager_.installRemoteProfile(*selected->remoteEntry, selected->profileName, &err)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Update Selected"), tr("Failed to install profile update:\n%1").arg(err));
        SPDLOG_WARN("ConfigTabs: failed to install remote profile '{}': {}", selected->profileName.toStdString(), err.toStdString());
        return;
    }

    refreshProfilesList(/*loadSelection=*/true);
    const QString cfgPath = profileJsonPath(selected->profileName);
    const bool active = QFileInfo(currentJsonPath()).absoluteFilePath() == QFileInfo(cfgPath).absoluteFilePath();
    if (active) {
        emit appConfigPathChanged(cfgPath);
        onReloadJson();
        const bool includeJs = profilesIncludeJsCheck_ ? profilesIncludeJsCheck_->isChecked() : true;
        if (includeJs && QFile::exists(profileJsPath(selected->profileName))) {
            QSettings().setValue("Config/ExternalCameraScriptPath", profileJsPath(selected->profileName));
            onReloadJs();
        }
    }
    refreshProfileStatusLabel();
    if (!nonInteractive_) QMessageBox::information(this, tr("Update Selected"), tr("Profile update installed successfully."));
}

void ConfigTabs::onShowProfileDiff() {
    const auto selected = selectedProfileSummary();
    if (!selected.has_value()) {
        if (!nonInteractive_) QMessageBox::information(this, tr("Show Diff"), tr("No profile selected."));
        return;
    }
    if (!remoteCatalog_.has_value() || !selected->remoteEntry.has_value()) {
        if (!nonInteractive_) QMessageBox::information(this, tr("Show Diff"), tr("Fetch the public catalog first, then select a remote-managed profile."));
        return;
    }

    QByteArray remoteConfig;
    QString remoteErr;
    if (!profileManager_.downloadUrlBlocking(selected->remoteEntry->configUrl, &remoteConfig, &remoteErr)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Show Diff"), tr("Failed to download remote config:\n%1").arg(remoteErr));
        return;
    }

    QString localErr;
    const auto localBytes = profileManager_.readFileBytes(profileJsonPath(selected->profileName), &localErr);
    if (!localBytes.has_value()) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Show Diff"), tr("Failed to read local config:\n%1").arg(localErr));
        return;
    }

    QString diffErr;
    const auto rows = profileManager_.diffConfigBytes(*localBytes, remoteConfig, &diffErr);
    if (!diffErr.isEmpty()) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Show Diff"), tr("Failed to diff configs:\n%1").arg(diffErr));
        return;
    }
    if (rows.isEmpty()) {
        if (!nonInteractive_) QMessageBox::information(this, tr("Show Diff"), tr("No configuration differences detected."));
        return;
    }
    showDiffDialog(tr("Profile Diff: %1").arg(selected->profileName), rows);
}

void ConfigTabs::onDuplicateProfileAsLocal() {
    const auto selected = selectedProfileSummary();
    if (!selected.has_value()) {
        if (!nonInteractive_) QMessageBox::information(this, tr("Duplicate as Local"), tr("No profile selected."));
        return;
    }
    QString newName = QInputDialog::getText(this,
                                            tr("Duplicate as Local"),
                                            tr("New local profile name:"),
                                            QLineEdit::Normal,
                                            selected->profileName + QStringLiteral("-copy"));
    if (newName.isNull()) {
        return;
    }
    newName = sanitizeProfileName(newName);
    if (newName.isEmpty()) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Duplicate as Local"), tr("Invalid profile name."));
        return;
    }

    QString err;
    if (!profileManager_.duplicateProfileAsLocal(selected->profileName, newName, &err)) {
        if (!nonInteractive_) QMessageBox::warning(this, tr("Duplicate as Local"), tr("Failed to duplicate profile:\n%1").arg(err));
        return;
    }

    refreshProfilesList(/*loadSelection=*/true);
    const int idx = profileSelect_ ? profileSelect_->findData(newName) : -1;
    if (idx >= 0) {
        profileSelect_->setCurrentIndex(idx);
    }
    if (!nonInteractive_) QMessageBox::information(this, tr("Duplicate as Local"), tr("Created local profile '%1'.").arg(newName));
}

void ConfigTabs::onIncludeJsToggled(bool checked) {
    QSettings().setValue("Profiles/IncludeJs", checked);
}

} // namespace frontend
