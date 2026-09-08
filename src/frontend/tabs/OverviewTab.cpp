#include "frontend/tabs/OverviewTab.h"
#include "ui_OverviewTab.h"

#include <cstring>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPainter>
#include <QTextStream>
#include <QTimer>
#include <QSettings>
#include <QTextOption>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSpinBox>
#include <QLabel>

#include <spdlog/spdlog.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

#include "backend/app/AppBackend.h"
#include "backend/playback/PlaybackService.h"
#include "backend/playback/FrameStore.h"
#include "frontend/utils/ConfigPathManager.h"
#include "frontend/utils/FileIOUtils.h"
#include "frontend/utils/SimpleImageCanvas.h"
#include "frontend/utils/EgrabberConfigParser.h"

namespace frontend
{

    namespace
    {

        // ROI alignment constraints: OffsetX must be multiple of 4, OffsetY must be multiple of 16
        static constexpr int ROI_OFFSET_X_STEP = 16;
        static constexpr int ROI_OFFSET_Y_STEP = 4;

        // Snap a value to the nearest multiple of step, clamping to [0, max]
        static int snapToStep(int value, int step, int max)
        {
            int snapped = (value / step) * step;
            if (snapped > max)
                snapped = (max / step) * step; // Clamp to max aligned value
            if (snapped < 0)
                snapped = 0;
            return snapped;
        }

    } // namespace

    OverviewTab::OverviewTab(backend::AppBackend &backend, QWidget *parent)
        : QWidget(parent), ui(new Ui::OverviewTab), backend_(backend)
    {
        ui->setupUi(this);

        // Create custom SimpleImageCanvas widget and add it to the placeholder
        canvas_ = new SimpleImageCanvas(&frameImage_, &fitMode_, &roiOverlayVisible_, &roiPosition_, &roiWidth_, &roiHeight_, ui->canvasContainer);
        canvas_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        // Replace the placeholder with the actual canvas
        ui->canvasLayout->removeWidget(ui->canvasPlaceholder);
        delete ui->canvasPlaceholder;
        ui->canvasLayout->addWidget(canvas_, 1);

        // Create ROI size spinboxes and insert into controls toolbar
        {
            auto *wLabel = new QLabel(tr("W:"), this);
            roiWidthSpin_ = new QSpinBox(this);
            roiWidthSpin_->setRange(64, 1920);
            roiWidthSpin_->setSingleStep(EgrabberConfigParser::ROI_WIDTH_STEP);
            roiWidthSpin_->setSuffix(tr(" px"));
            roiWidthSpin_->setValue(roiWidth_);

            auto *hLabel = new QLabel(tr("H:"), this);
            roiHeightSpin_ = new QSpinBox(this);
            roiHeightSpin_->setRange(16, 1080);
            roiHeightSpin_->setSingleStep(EgrabberConfigParser::ROI_HEIGHT_STEP);
            roiHeightSpin_->setSuffix(tr(" px"));
            roiHeightSpin_->setValue(roiHeight_);

            // Insert after roiOverlayBtn (index 2) and before the spacer
            ui->controlsLayout->insertWidget(2, wLabel);
            ui->controlsLayout->insertWidget(3, roiWidthSpin_);
            ui->controlsLayout->insertWidget(4, hLabel);
            ui->controlsLayout->insertWidget(5, roiHeightSpin_);
        }

        // Set initial proportions (50/50 ratio)
        ui->splitter->setStretchFactor(0, 1);
        ui->splitter->setStretchFactor(1, 1);
        
        // Set explicit sizes for 50/50 split after widget is shown
        QTimer::singleShot(0, this, [this]() {
            int height = ui->splitter->height();
            if (height > 0) {
                int half = height / 2;
                ui->splitter->setSizes({half, half});
            }
        });

        // Connect button signals
        connect(ui->jsReloadBtn, &QPushButton::clicked, this, &OverviewTab::onReloadJs);
        connect(ui->jsSaveBtn, &QPushButton::clicked, this, &OverviewTab::onSaveJs);
        connect(ui->jsApplyBtn, &QPushButton::clicked, this, &OverviewTab::onApplyJs);
        connect(ui->jsBrowseBtn, &QPushButton::clicked, this, &OverviewTab::onBrowseJs);
        connect(ui->jsClearBtn, &QPushButton::clicked, this, &OverviewTab::onClearJs);
        connect(ui->fitBtn, &QToolButton::clicked, this, &OverviewTab::onToggleFit);
        connect(ui->roiOverlayBtn, &QToolButton::clicked, this, &OverviewTab::onToggleRoiOverlay);
        connect(static_cast<SimpleImageCanvas*>(canvas_), &SimpleImageCanvas::roiPositionChanged,
                this, &OverviewTab::onRoiPositionChanged);
        connect(roiWidthSpin_, &QSpinBox::valueChanged,
                this, &OverviewTab::onRoiSizeChanged);
        connect(roiHeightSpin_, &QSpinBox::valueChanged,
                this, &OverviewTab::onRoiSizeChanged);
        connect(ui->jsEdit, &QPlainTextEdit::textChanged, this, [this]()
                {
        if (ui->jsUnsavedLabel) ui->jsUnsavedLabel->setVisible(true); });

        // Timer for frame display at 50 fps (20ms interval)
        timer_ = new QTimer(this);
        timer_->setTimerType(Qt::PreciseTimer);
        timer_->setInterval(20); // 50 fps = 1000ms / 50 = 20ms
        connect(timer_, &QTimer::timeout, this, &OverviewTab::onTick);
        timer_->start();
        SPDLOG_INFO("Overview tab: display_fps=50 (~20 ms)");

        // Load initial camera script
        onReloadJs();

        // Initialize ROI position from egrabberConfig.js
        initializeRoiFromConfig();
    }

    OverviewTab::~OverviewTab() {
        if (timer_) {
            timer_->stop();
        }
        delete ui;
    }

    QString OverviewTab::appDirIncludePath(const QString &fileName) const
    {
        return ConfigPathManager::getIncludePath(fileName);
    }

    QString OverviewTab::currentJsPath() const
    {
        QSettings s;
        const QString ext = s.value("Config/ExternalOverviewScriptPath").toString().trimmed();
        if (!ext.isEmpty())
            return ext;
        return defaultJsPath();
    }

    bool OverviewTab::loadFileToEditor(const QString &path, QPlainTextEdit *editor, QString *err)
    {
        bool result = FileIOUtils::loadFileToEditor(path, editor, err);
        if (result && editor == ui->jsEdit && ui->jsUnsavedLabel)
            ui->jsUnsavedLabel->setVisible(false);
        return result;
    }

    bool OverviewTab::saveEditorToFile(QPlainTextEdit *editor, const QString &path, QString *err)
    {
        return FileIOUtils::saveEditorToFile(editor, path, err);
    }

    void OverviewTab::setRoiOverlayVisible(bool visible)
    {
        roiOverlayVisible_ = visible;
        if (ui->roiOverlayBtn)
        {
            ui->roiOverlayBtn->setText(roiOverlayVisible_ ? tr("ROI Overlay: On") : tr("ROI Overlay: Off"));
        }
        if (canvas_)
        {
            canvas_->update();
        }
    }

    void OverviewTab::onTick()
    {
        // Fetch into member scratch so the vector capacity is reused across ticks
        bool got = backend_.playback().fetchLatest(scratchFrame_);

        if (got)
        {
            // Convert Mono8 to QImage (all current cameras are Mono8)
            const int w = static_cast<int>(scratchFrame_.width);
            const int h = static_cast<int>(scratchFrame_.height);
            const int pitch = static_cast<int>(scratchFrame_.linePitch == 0 ? scratchFrame_.width : scratchFrame_.linePitch);

            // Reuse the existing QImage allocation when size/format are unchanged
            if (frameImage_.width() == w && frameImage_.height() == h &&
                frameImage_.format() == QImage::Format_Grayscale8 && !frameImage_.isNull())
            {
                // Update pixels in place — no heap allocation
                uchar* dst = frameImage_.bits();
                const uchar* src = scratchFrame_.data.data();
                const int bytePitch = frameImage_.bytesPerLine();
                for (int row = 0; row < h; ++row) {
                    std::memcpy(dst + row * bytePitch, src + row * pitch, static_cast<size_t>(w));
                }
                frameImage_.detach(); // invalidate cacheKey so canvas rescale fires
            }
            else
            {
                QImage img(scratchFrame_.data.data(), w, h, pitch, QImage::Format_Grayscale8);
                frameImage_ = img.copy(); // allocate once on geometry change
            }

            if (canvas_)
                canvas_->update();
        }
    }

    void OverviewTab::onReloadJs()
    {
        const QString path = currentJsPath();
        if (path == defaultJsPath())
        {
            QString err;
            if (!FileIOUtils::ensureDefaultsFile(path, ":/defaults/overviewConfig.js", &err))
            {
                SPDLOG_WARN("ensureDefaultsFile(overviewConfig.js) failed: {}", err.toStdString());
            }
        }
        QString err;
        if (!loadFileToEditor(path, ui->jsEdit, &err))
        {
            SPDLOG_WARN("Failed to load overviewConfig.js from {}: {}", path.toStdString(), err.toStdString());
            QMessageBox::warning(this, tr("Reset overviewConfig.js"), tr("Failed to load: %1").arg(err));
            return;
        }
        ui->jsPathLabel->setText(path);
        if (ui->jsUnsavedLabel)
            ui->jsUnsavedLabel->setVisible(false);
    }

    void OverviewTab::onSaveJs()
    {
        const QString path = currentJsPath();
        QString err;
        if (!saveEditorToFile(ui->jsEdit, path, &err))
        {
            SPDLOG_ERROR("Failed to save overviewConfig.js to {}: {}", path.toStdString(), err.toStdString());
            QMessageBox::warning(this, tr("Save overviewConfig.js"), tr("Failed to save: %1").arg(err));
            return;
        }
        QMessageBox::information(this, tr("Save overviewConfig.js"), tr("Saved."));
        if (ui->jsUnsavedLabel)
            ui->jsUnsavedLabel->setVisible(false);
    }

    void OverviewTab::onApplyJs()
    {
        const QString path = currentJsPath();
        QString err;
        // Always save first to ensure the latest content is applied
        {
            QString saveErr;
            if (!saveEditorToFile(ui->jsEdit, path, &saveErr))
            {
                QMessageBox::warning(this, tr("Apply Camera Script"), tr("Failed to save script: %1").arg(saveErr));
                return;
            }
        }

        std::string backendErr;
        if (!backend_.applyCameraScriptFromFile(path.toStdString(), &backendErr))
        {
            QMessageBox::warning(this,
                                 tr("Apply Camera Script"),
                                 tr("Failed to apply script: %1").arg(QString::fromStdString(backendErr)));
            return;
        }
        QMessageBox::information(this, tr("Apply Camera Script"), tr("Applied to camera. Capture remains stopped."));
    }

    void OverviewTab::onBrowseJs()
    {
        const QString current = currentJsPath();
        const QString initialDir = QFileInfo(current).absolutePath();
        const QString selected = QFileDialog::getOpenFileName(this,
                                                              tr("Select Camera script (overviewConfig.js)"),
                                                              initialDir,
                                                              tr("JavaScript files (*.js);;All Files (*.*)"));
        if (selected.isEmpty())
            return;
        {
            QSettings s;
            s.setValue("Config/ExternalOverviewScriptPath", selected);
        }
        SPDLOG_INFO("External Overview script set to {}", selected.toStdString());
        QString err;
        if (!loadFileToEditor(selected, ui->jsEdit, &err))
        {
            SPDLOG_WARN("Failed to load external overviewConfig.js from {}: {}", selected.toStdString(), err.toStdString());
            QMessageBox::warning(this, tr("Reset overviewConfig.js"), tr("Failed to load: %1").arg(err));
            return;
        }
        ui->jsPathLabel->setText(selected);
        if (ui->jsUnsavedLabel)
            ui->jsUnsavedLabel->setVisible(false);
    }

    void OverviewTab::onClearJs()
    {
        QSettings s;
        s.remove("Config/ExternalOverviewScriptPath");
        SPDLOG_INFO("External Overview script cleared; reverting to default include path");
        const auto ret = QMessageBox::question(this,
                                               tr("Camera Script Path Cleared"),
                                               tr("External Camera script path cleared.\nReset from default include path now?\n\nNote: Save to apply any changes."),
                                               QMessageBox::Yes | QMessageBox::No,
                                               QMessageBox::Yes);
        if (ret == QMessageBox::Yes)
        {
            onReloadJs();
        }
    }

    void OverviewTab::onToggleFit()
    {
        if (fitMode_ == FitMode::FitToWindow)
        {
            fitMode_ = FitMode::Zoom100;
            if (ui->fitBtn)
            {
                ui->fitBtn->setText(tr("Fit: 100%"));
                ui->fitBtn->setToolTip(tr("Toggle between fit-to-window and 100% zoom"));
            }
        }
        else
        {
            fitMode_ = FitMode::FitToWindow;
            if (ui->fitBtn)
            {
                ui->fitBtn->setText(tr("Fit: Window"));
                ui->fitBtn->setToolTip(tr("Toggle between fit-to-window and 100% zoom"));
            }
        }
        if (canvas_)
            canvas_->update();
    }

    void OverviewTab::onToggleRoiOverlay()
    {
        roiOverlayVisible_ = !roiOverlayVisible_;
        if (ui->roiOverlayBtn)
        {
            if (roiOverlayVisible_)
            {
                ui->roiOverlayBtn->setText(tr("ROI Overlay: On"));
            }
            else
            {
                ui->roiOverlayBtn->setText(tr("ROI Overlay: Off"));
            }
        }
        if (canvas_)
            canvas_->update();
    }

    void OverviewTab::onRoiPositionChanged(QPointF imagePos)
    {
        roiPosition_ = imagePos;
        updateEgrabberConfigFromRect(imagePos);
        emit roiChanged(static_cast<int>(roiPosition_.x()), static_cast<int>(roiPosition_.y()), roiWidth_, roiHeight_);
    }

    QString OverviewTab::egrabberConfigPath() const
    {
        QSettings s;
        const QString ext = s.value("Config/ExternalCameraScriptPath").toString().trimmed();
        if (!ext.isEmpty())
            return ext;
        return appDirIncludePath("egrabberConfig.js");
    }

    void OverviewTab::updateEgrabberConfigFromRect(QPointF imagePos)
    {
        const QString path = egrabberConfigPath();

        // Ensure default file exists if using default path
        if (path == appDirIncludePath("egrabberConfig.js"))
        {
            QString err;
            if (!FileIOUtils::ensureDefaultsFile(path, ":/defaults/egrabberConfig.js", &err))
            {
                SPDLOG_WARN("ensureDefaultsFile(egrabberConfig.js) failed: {}", err.toStdString());
            }
        }

        int requestedX = static_cast<int>(std::round(imagePos.x()));
        int requestedY = static_cast<int>(std::round(imagePos.y()));

        QString err;
        if (!EgrabberConfigParser::updateRoiOffsets(path, requestedX, requestedY, &err))
        {
            SPDLOG_ERROR("Failed to update egrabberConfig.js: {}", err.toStdString());
        }
    }

    void OverviewTab::initializeRoiFromConfig()
    {
        const QString path = egrabberConfigPath();
        int offsetX, offsetY;
        if (EgrabberConfigParser::readRoiOffsets(path, offsetX, offsetY))
        {
            roiPosition_ = QPointF(offsetX, offsetY);
            SPDLOG_DEBUG("Initialized ROI position from egrabberConfig.js: OffsetX={}, OffsetY={}", offsetX, offsetY);
        }
        else
        {
            QPoint defaultPos = EgrabberConfigParser::defaultRoiPosition();
            roiPosition_ = QPointF(defaultPos);
        }

        // Read ROI size
        int w, h;
        if (EgrabberConfigParser::readRoiSize(path, w, h))
        {
            roiWidth_ = w;
            roiHeight_ = h;
            SPDLOG_DEBUG("Initialized ROI size from egrabberConfig.js: Width={}, Height={}", w, h);
        }

        // Update spinboxes
        if (roiWidthSpin_)
        {
            roiWidthSpin_->blockSignals(true);
            roiWidthSpin_->setValue(roiWidth_);
            roiWidthSpin_->blockSignals(false);
        }
        if (roiHeightSpin_)
        {
            roiHeightSpin_->blockSignals(true);
            roiHeightSpin_->setValue(roiHeight_);
            roiHeightSpin_->blockSignals(false);
        }
    }

    void OverviewTab::onRoiSizeChanged()
    {
        // Snap to alignment steps (handles typed non-aligned values)
        int w = roiWidthSpin_->value();
        int h = roiHeightSpin_->value();

        w = (w / EgrabberConfigParser::ROI_WIDTH_STEP) * EgrabberConfigParser::ROI_WIDTH_STEP;
        h = (h / EgrabberConfigParser::ROI_HEIGHT_STEP) * EgrabberConfigParser::ROI_HEIGHT_STEP;

        if (w < 64)
            w = 64;
        if (w > 1920)
            w = 1920;
        if (h < 16)
            h = 16;
        if (h > 1080)
            h = 1080;

        // Update spinboxes if snapping changed the value
        if (w != roiWidthSpin_->value())
        {
            roiWidthSpin_->blockSignals(true);
            roiWidthSpin_->setValue(w);
            roiWidthSpin_->blockSignals(false);
        }
        if (h != roiHeightSpin_->value())
        {
            roiHeightSpin_->blockSignals(true);
            roiHeightSpin_->setValue(h);
            roiHeightSpin_->blockSignals(false);
        }

        roiWidth_ = w;
        roiHeight_ = h;

        // Auto-clamp offset if ROI exceeds sensor bounds
        const int maxSensorW = 1920;
        const int maxSensorH = 1080;
        bool offsetChanged = false;

        int ox = static_cast<int>(roiPosition_.x());
        int oy = static_cast<int>(roiPosition_.y());

        if (ox + w > maxSensorW)
        {
            ox = ((maxSensorW - w) / EgrabberConfigParser::ROI_OFFSET_X_STEP) * EgrabberConfigParser::ROI_OFFSET_X_STEP;
            if (ox < 0)
                ox = 0;
            roiPosition_.setX(ox);
            offsetChanged = true;
        }
        if (oy + h > maxSensorH)
        {
            oy = ((maxSensorH - h) / EgrabberConfigParser::ROI_OFFSET_Y_STEP) * EgrabberConfigParser::ROI_OFFSET_Y_STEP;
            if (oy < 0)
                oy = 0;
            roiPosition_.setY(oy);
            offsetChanged = true;
        }

        // Persist size to egrabberConfig.js
        updateEgrabberConfigSize();

        // If offset was clamped, persist that too
        if (offsetChanged)
        {
            updateEgrabberConfigFromRect(roiPosition_);
        }

        if (canvas_)
            canvas_->update();

        emit roiChanged(static_cast<int>(roiPosition_.x()), static_cast<int>(roiPosition_.y()), roiWidth_, roiHeight_);
    }

    void OverviewTab::updateEgrabberConfigSize()
    {
        const QString path = egrabberConfigPath();

        if (path == appDirIncludePath("egrabberConfig.js"))
        {
            QString err;
            if (!FileIOUtils::ensureDefaultsFile(path, ":/defaults/egrabberConfig.js", &err))
            {
                SPDLOG_WARN("ensureDefaultsFile(egrabberConfig.js) failed: {}", err.toStdString());
            }
        }

        QString err;
        if (!EgrabberConfigParser::updateRoiSize(path, roiWidth_, roiHeight_, &err))
        {
            SPDLOG_ERROR("Failed to update ROI size in egrabberConfig.js: {}", err.toStdString());
        }
    }

} // namespace frontend
