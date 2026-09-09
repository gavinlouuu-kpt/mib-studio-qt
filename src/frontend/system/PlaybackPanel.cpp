#include "frontend/system/PlaybackPanel.h"

#include <QPainter>
#include <QTimer>
#include <QSlider>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QCheckBox>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QCursor>
#include <QShortcut>
#include <QKeySequence>
#include <QSize>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QStandardPaths>
#include <limits>
#include <algorithm>
#include <cmath>

#include "backend/app/AppBackend.h"
#include "backend/services/CaptureService.h"
#include "backend/services/CrashReporter.h"
#include "backend/playback/PlaybackService.h"
#include "backend/processing/ProcessingService.h"
#include "backend/playback/FrameStore.h"
#include "backend/app/Tools.h"
#include "frontend/dialogs/BufferSaveDialog.h"

#include <spdlog/spdlog.h>
#include <fmt/format.h>
#ifdef _WIN32
#define NOMINMAX  // Prevent Windows.h from defining min/max macros
#include <windows.h>
#include <shlobj.h>
#endif

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace
{
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

    class ImageCanvas : public QWidget
    {
    public:
        explicit ImageCanvas(QImage *image,
                             QImage *overlay,
                             QRect *imageRoi,
                             QList<PlaybackPanel::ColoredContour> *contours,
                             PlaybackPanel::FitMode *fitMode,
                             QWidget *parent = nullptr)
            : QWidget(parent),
              image_(image),
              overlay_(overlay),
              imageRoi_(imageRoi),
              contours_(contours),
              fitMode_(fitMode) {}

        std::function<void(const QRect &)> onRoiSelected;
        std::function<void()> onRequestBackground;
        std::function<void()> onRequestBackgroundCalibration;
        std::function<bool()> backgroundCalibrationRunning;

    protected:
        void paintEvent(QPaintEvent *) override
        {
            QPainter p(this);
            // Use the active widget palette so the preview surface follows the
            // same application theme as the rest of the tab content.
            p.fillRect(rect(), palette().window());
            if (!image_ || image_->isNull())
                return;

            const int imgW = image_->width();
            const int imgH = image_->height();
            
            double scale;
            QSizeF drawSize;
            QPointF topLeft;
            
            if (fitMode_ && *fitMode_ == PlaybackPanel::FitMode::Zoom100)
            {
                // 100% zoom: 1:1 pixel ratio
                scale = 1.0;
                drawSize = QSizeF(imgW, imgH);
                topLeft = QPointF((width() - drawSize.width()) / 2.0, (height() - drawSize.height()) / 2.0);
            }
            else
            {
                // Fit to window: scale to fit maintaining aspect ratio
                scale = std::min(double(width()) / imgW, double(height()) / imgH);
                drawSize = QSizeF(imgW * scale, imgH * scale);
                topLeft = QPointF((width() - drawSize.width()) / 2.0, (height() - drawSize.height()) / 2.0);
            }

            // Base image
            QImage scaled = image_->scaled(drawSize.toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            p.drawImage(topLeft.toPoint(), scaled);

            // Overlay mask (RGBA) if present
            if (overlay_ && !overlay_->isNull())
            {
                QImage scaledOverlay = overlay_->scaled(drawSize.toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                p.drawImage(topLeft.toPoint(), scaledOverlay);
            }

            // Contours (per-contour classification color)
            if (contours_)
            {
                for (const auto &cc : *contours_)
                {
                    if (cc.polygon.isEmpty())
                        continue;
                    QPen pen(cc.color);
                    pen.setWidth(2);
                    p.setPen(pen);
                    QPolygon scaledPoly;
                    scaledPoly.reserve(cc.polygon.size());
                    for (const QPoint &pt : cc.polygon)
                    {
                        QPointF q = QPointF(pt) * scale + topLeft;
                        scaledPoly << q.toPoint();
                    }
                    p.drawPolyline(scaledPoly);
                }
            }

            // Final ROI
            if (imageRoi_ && !imageRoi_->isNull() && imageRoi_->isValid())
            {
                QPen roiPen(QColor(255, 255, 0));
                roiPen.setWidth(1);
                roiPen.setStyle(Qt::DashLine);
                p.setPen(roiPen);
                QRectF r(imageRoi_->x() * scale + topLeft.x(),
                         imageRoi_->y() * scale + topLeft.y(),
                         imageRoi_->width() * scale,
                         imageRoi_->height() * scale);
                p.drawRect(r);
            }

            // Rubber-band during drag
            if (dragging_)
            {
                QPen rbPen(QColor(0, 180, 255));
                rbPen.setWidth(1);
                rbPen.setStyle(Qt::DashDotLine);
                p.setPen(rbPen);
                QRect r = QRect(dragStartWidgetPos_, dragCurrentWidgetPos_).normalized();
                p.drawRect(r);
            }
        }

        void mousePressEvent(QMouseEvent *e) override
        {
            if (!image_ || image_->isNull())
                return;
            if (e->button() == Qt::LeftButton)
            {
                dragging_ = true;
                dragStartWidgetPos_ = e->pos();
                dragCurrentWidgetPos_ = e->pos();
                update();
            }
        }

        void mouseMoveEvent(QMouseEvent *e) override
        {
            if (!dragging_)
                return;
            dragCurrentWidgetPos_ = e->pos();
            update();
        }

        void mouseReleaseEvent(QMouseEvent *e) override
        {
            if (!image_ || image_->isNull())
                return;
            if (e->button() == Qt::LeftButton && dragging_)
            {
                dragging_ = false;
                dragCurrentWidgetPos_ = e->pos();
                // Map widget rect to image coordinates
                QRect widgetRect = QRect(dragStartWidgetPos_, dragCurrentWidgetPos_).normalized();
                if (widgetRect.width() < 3 || widgetRect.height() < 3)
                {
                    // Too small, ignore
                    update();
                    return;
                }
                const int imgW = image_->width();
                const int imgH = image_->height();
                
                double scale;
                QSizeF drawSize;
                QPointF topLeft;
                
                if (fitMode_ && *fitMode_ == PlaybackPanel::FitMode::Zoom100)
                {
                    scale = 1.0;
                    drawSize = QSizeF(imgW, imgH);
                    topLeft = QPointF((width() - drawSize.width()) / 2.0, (height() - drawSize.height()) / 2.0);
                }
                else
                {
                    scale = std::min(double(width()) / imgW, double(height()) / imgH);
                    drawSize = QSizeF(imgW * scale, imgH * scale);
                    topLeft = QPointF((width() - drawSize.width()) / 2.0, (height() - drawSize.height()) / 2.0);
                }

                auto toImage = [&](const QPoint &wp) -> QPoint
                {
                    QPointF f = (wp - topLeft) / scale;
                    int x = std::clamp(int(std::lround(f.x())), 0, imgW - 1);
                    int y = std::clamp(int(std::lround(f.y())), 0, imgH - 1);
                    return QPoint(x, y);
                };

                QPoint imgP0 = toImage(widgetRect.topLeft());
                QPoint imgP1 = toImage(widgetRect.bottomRight());
                QRect imgRect = QRect(imgP0, imgP1).normalized();
                if (imageRoi_)
                    *imageRoi_ = imgRect;
                if (onRoiSelected)
                    onRoiSelected(imgRect);
                update();
            }
        }

        void contextMenuEvent(QContextMenuEvent *event) override
        {
            QMenu menu(this);
            QAction *setBg = menu.addAction("Set Background");
            const bool calibrating = backgroundCalibrationRunning && backgroundCalibrationRunning();
            QAction *calibrateBg = menu.addAction(calibrating ? "Cancel Background Calibration"
                                                              : "Calibrate Background (bounded)");
            QAction *clearRoi = menu.addAction("Clear ROI");
            QAction *chosen = menu.exec(event->globalPos());
            if (!chosen)
                return;
            if (chosen == setBg)
            {
                if (onRequestBackground)
                    onRequestBackground();
            }
            else if (chosen == calibrateBg)
            {
                if (onRequestBackgroundCalibration)
                    onRequestBackgroundCalibration();
            }
            else if (chosen == clearRoi)
            {
                if (imageRoi_)
                    *imageRoi_ = QRect();
                if (onRoiSelected)
                    onRoiSelected(QRect());
                update();
            }
        }

    private:
        QImage *image_ = nullptr;
        QImage *overlay_ = nullptr;
        QRect *imageRoi_ = nullptr;
        QList<PlaybackPanel::ColoredContour> *contours_ = nullptr;
        PlaybackPanel::FitMode *fitMode_ = nullptr;
        bool dragging_ = false;
        QPoint dragStartWidgetPos_;
        QPoint dragCurrentWidgetPos_;
    };

} // namespace

PlaybackPanel::PlaybackPanel(backend::AppBackend &backend, QWidget *parent)
    : QWidget(parent), backend_(backend)
{
    // UI: image canvas + horizontal slider
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    canvas_ = new ImageCanvas(&frameImage_, &overlayImage_, &imageRoi_, &overlayContours_, &fitMode_, this);
    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setRange(0, 0);
    slider_->setSingleStep(1);
    slider_->setPageStep(8);
    slider_->setFocusPolicy(Qt::StrongFocus);
    slider_->installEventFilter(this);

    layout->addWidget(canvas_, 1);

    // Controls bar
    QWidget *controls = new QWidget(this);
    auto *controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(6, 4, 6, 4);
    controlsLayout->setSpacing(6);
    overlayBtn_ = new QToolButton(controls);
    overlayBtn_->setText("Overlay: Off");
    overlayBtn_->setToolTip("Toggle overlay (Off → Mask → Contours → Both)");
    overlayLegend_ = new QLabel(controls);
    overlayLegend_->setText(
        "<span style='color:#0078FF;'>&#9632;</span> Target "
        "<span style='color:#00FF00;'>&#9632;</span> Valid "
        "<span style='color:#FF0000;'>&#9632;</span> Invalid");
    overlayLegend_->setToolTip("Blue = target group, Green = valid, Red = invalid");
    overlayLegend_->setVisible(false);
    setBgBtn_ = new QToolButton(controls);
    setBgBtn_->setText("Set Background");
    setBgBtn_->setToolTip("Capture current frame as background (when paused)");
    autoBgCheck_ = new QCheckBox(controls);
    autoBgCheck_->setText("Auto");
    autoBgCheck_->setToolTip("Automatically capture background when no movement detected (disabled during experiments)");
    autoBgCheck_->setChecked(false); // Default off
    clearRoiBtn_ = new QToolButton(controls);
    clearRoiBtn_->setText("Clear ROI");
    clearRoiBtn_->setToolTip("Clear the region of interest (ROI)");
    clearRoiBtn_->setEnabled(false);
    saveBufferBtn_ = new QToolButton(controls);
    saveBufferBtn_->setText("Save Buffer");
    saveBufferBtn_->setToolTip("Save buffer frames to disk and manage buffer size");
    recordBtn_ = new QToolButton(controls);
    recordBtn_->setText("Record");
    recordBtn_->setToolTip("Record non-empty frames to HDF5 (images + metadata only, no contour processing)");
    recordBtn_->setStyleSheet(""); // Will be updated by updateRecordingUI
    recordStatusLabel_ = new QLabel(controls);
    recordStatusLabel_->setText("");
    recordStatusLabel_->setStyleSheet("color: gray; padding: 0 4px;");
    fitBtn_ = new QToolButton(controls);
    fitBtn_->setText("Fit: Window");
    fitBtn_->setToolTip("Toggle between fit-to-window and 100% zoom");
    controlsLayout->addWidget(overlayBtn_);
    controlsLayout->addWidget(overlayLegend_);
    controlsLayout->addWidget(setBgBtn_);
    controlsLayout->addWidget(autoBgCheck_);
    controlsLayout->addWidget(clearRoiBtn_);
    controlsLayout->addWidget(saveBufferBtn_);
    controlsLayout->addWidget(recordBtn_);
    controlsLayout->addWidget(recordStatusLabel_);
    controlsLayout->addWidget(fitBtn_);
    controlsLayout->addStretch(1);
    layout->addWidget(controls);

    layout->addWidget(slider_);

    connect(slider_, &QSlider::sliderPressed, this, &PlaybackPanel::onSliderPressed);
    connect(slider_, &QSlider::sliderReleased, this, &PlaybackPanel::onSliderReleased);
    connect(slider_, &QSlider::valueChanged, this, &PlaybackPanel::onSliderValueChanged);
    connect(overlayBtn_, &QToolButton::clicked, this, &PlaybackPanel::onToggleOverlay);
    connect(setBgBtn_, &QToolButton::clicked, this, &PlaybackPanel::onSetBackground);
    connect(autoBgCheck_, &QCheckBox::toggled, this, &PlaybackPanel::onAutoBackgroundToggled);
    connect(clearRoiBtn_, &QToolButton::clicked, this, &PlaybackPanel::onClearRoi);
    connect(saveBufferBtn_, &QToolButton::clicked, this, &PlaybackPanel::onSaveBuffer);
    connect(recordBtn_, &QToolButton::clicked, this, &PlaybackPanel::onToggleRecording);
    connect(fitBtn_, &QToolButton::clicked, this, &PlaybackPanel::onToggleFit);

    // Space shortcut to start/stop capture
    {
        auto *spaceShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
        connect(spaceShortcut, &QShortcut::activated, this, &PlaybackPanel::onToggleCapture);
    }

    // Canvas callbacks
    auto *canvas = static_cast<ImageCanvas *>(canvas_);
    canvas->onRoiSelected = [this](const QRect &r)
    {
        imageRoi_ = r;
        roiActive_ = r.isValid() && !r.isNull();
        SPDLOG_INFO("PlaybackPanel: ROI {}",
                    roiActive_ ? fmt::format("x={}, y={}, w={}, h={}", r.x(), r.y(), r.width(), r.height())
                               : std::string("cleared"));
        // Sync ROI to backend realtime processor (full image when cleared)
        backend::services::ProcessingService::Roi roi{};
        if (roiActive_)
        {
            roi.x = r.x();
            roi.y = r.y();
            roi.w = r.width();
            roi.h = r.height();
        }
        else
        {
            roi.x = 0;
            roi.y = 0;
            roi.w = frameImage_.width();
            roi.h = frameImage_.height();
        }
        backend_.processing().setRealtimeRoi(roi);
        if (overlayMode_ != OverlayMode::Off)
        {
            computeProcessedOverlay();
        }
        canvas_->update();
        
        // Update Clear ROI button state
        if (clearRoiBtn_) {
            clearRoiBtn_->setEnabled(roiActive_);
        }
        
        // Save ROI to config.json
        saveRoiToConfig(r);
    };
    canvas->onRequestBackground = [this]()
    {
        onSetBackground();
    };
    canvas->onRequestBackgroundCalibration = [this]()
    {
        onCalibrateBackground();
    };
    canvas->backgroundCalibrationRunning = [this]()
    {
        return backend_.processing().backgroundCalibrationStatus().state ==
               backend::services::ProcessingService::BackgroundCalibrationState::Running;
    };
    bgCalTimer_ = new QTimer(this);
    bgCalTimer_->setInterval(200);
    connect(bgCalTimer_, &QTimer::timeout, this, &PlaybackPanel::onPollBackgroundCalibration);

    // Timer for periodic refresh (configurable display_fps, default 60 Hz)
    int displayFps = 60;
    {
        // Prefer active config path (external if set), else fall back to resource defaults
        auto readFpsFrom = [&](const QString& cfgPath) -> bool {
            QFile f(cfgPath);
            if (!f.open(QIODevice::ReadOnly)) return false;
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (!doc.isObject()) return false;
            const QJsonObject obj = doc.object();
            if (!obj.contains("display_fps")) return false;
            displayFps = obj.value("display_fps").toInt(displayFps);
            return true;
        };
        QSettings s;
        const QString ext = s.value("Config/ExternalAppConfigPath").toString().trimmed();
        bool loaded = false;
        if (!ext.isEmpty()) {
            loaded = readFpsFrom(ext);
        }
        if (!loaded) {
            // Use centralized helper to get user-writable config directory
            const QString defaultPath = QDir(getUserConfigDir()).absoluteFilePath("config.json");
            loaded = readFpsFrom(defaultPath);
        }
        if (!loaded) {
            readFpsFrom(":/defaults/config.json");
        }
    }
    if (displayFps < 1) displayFps = 1;
    if (displayFps > 240) displayFps = 240;
    const int intervalMs = std::max(1, static_cast<int>(std::lround(1000.0 / static_cast<double>(displayFps))));

    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    timer_->setInterval(intervalMs);
    connect(timer_, &QTimer::timeout, this, &PlaybackPanel::onTick);
    timer_->start();
    SPDLOG_INFO("Playback preview: display_fps={} (~{} ms)", displayFps, intervalMs);

    // Timer for periodic metrics logging (1 second interval)
    metricsTimer_ = new QTimer(this);
    metricsTimer_->setInterval(1000);
    connect(metricsTimer_, &QTimer::timeout, this, &PlaybackPanel::onLogMetrics);
    metricsTimer_->start();

    resetMetrics();
    updateOverlayButtonUi();
}

PlaybackPanel::~PlaybackPanel() = default;

void PlaybackPanel::setRoi(const QRect &roi, bool saveToConfig)
{
    imageRoi_ = roi;
    roiActive_ = roi.isValid() && !roi.isNull();
    SPDLOG_INFO("PlaybackPanel: ROI {}",
                roiActive_ ? fmt::format("x={}, y={}, w={}, h={}", roi.x(), roi.y(), roi.width(), roi.height())
                           : std::string("cleared"));
    // Sync ROI to backend realtime processor (full image when cleared)
    backend::services::ProcessingService::Roi backendRoi{};
    if (roiActive_)
    {
        backendRoi.x = roi.x();
        backendRoi.y = roi.y();
        backendRoi.w = roi.width();
        backendRoi.h = roi.height();
    }
    else
    {
        backendRoi.x = 0;
        backendRoi.y = 0;
        backendRoi.w = frameImage_.width();
        backendRoi.h = frameImage_.height();
    }
    backend_.processing().setRealtimeRoi(backendRoi);
    if (overlayMode_ != OverlayMode::Off)
    {
        computeProcessedOverlay();
    }
    if (canvas_)
        canvas_->update();
    
    // Update Clear ROI button state
    if (clearRoiBtn_) {
        clearRoiBtn_->setEnabled(roiActive_);
    }
    
    // Save ROI to config.json only if requested (default true for user actions)
    if (saveToConfig)
    {
        saveRoiToConfig(roi);
    }
}

QRect PlaybackPanel::getRoi() const
{
    return imageRoi_;
}

QSize PlaybackPanel::getImageDimensions() const
{
    if (frameImage_.isNull())
        return QSize(0, 0);
    return frameImage_.size();
}

void PlaybackPanel::onTick()
{
    // Detect capture start/stop transitions
    const bool running = backend_.capture().isRunning();
    if (running && !prevCaptureRunning_)
    {
        followLive_ = true;
        resetMetrics(); // Reset metrics when capture starts
    }
    else if (!running && prevCaptureRunning_)
    {
        resetMetrics(); // Reset metrics when capture stops
    }
    prevCaptureRunning_ = running;

    // Update recording status display periodically
    if (backend_.isFrameRecording()) {
        updateRecordingUI();
    }

    // Update slider range from available indices
    uint64_t earliest = 0, latest = 0;
    size_t count = 0;
    const bool hasRange = backend_.playback().queryRange(earliest, latest, count);
    if (hasRange)
    {
        const int minVal = static_cast<int>(std::min<uint64_t>(earliest, static_cast<uint64_t>(std::numeric_limits<int>::max())));
        const int maxVal = static_cast<int>(std::min<uint64_t>(latest, static_cast<uint64_t>(std::numeric_limits<int>::max())));
        if (slider_->minimum() != minVal || slider_->maximum() != maxVal)
        {
            slider_->setRange(minVal, maxVal);
        }
    }

    backend::playback::Frame f;
    bool got = false;
    if (scrubbing_)
    {
        const int val = slider_->value();
        pinnedIndex_ = static_cast<uint64_t>(std::max(0, val));
        got = backend_.playback().fetchByIndex(pinnedIndex_, f);
    }
    else if (followLive_)
    {
        got = backend_.playback().fetchLatest(f);
        if (hasRange)
        {
            const int latestInt = static_cast<int>(std::min<uint64_t>(latest, static_cast<uint64_t>(std::numeric_limits<int>::max())));
            if (slider_->value() != latestInt)
            {
                slider_->setValue(latestInt);
            }
            pinnedIndex_ = static_cast<uint64_t>(latestInt);
        }
    }
    else
    {
        // Review mode: display pinned frame, clamp within available window
        if (hasRange)
        {
            if (pinnedIndex_ < earliest)
                pinnedIndex_ = earliest;
            if (pinnedIndex_ > latest)
                pinnedIndex_ = latest;
            const int pinnedInt = static_cast<int>(std::min<uint64_t>(pinnedIndex_, static_cast<uint64_t>(std::numeric_limits<int>::max())));
            if (slider_->value() != pinnedInt)
                slider_->setValue(pinnedInt);
        }
        got = backend_.playback().fetchByIndex(pinnedIndex_, f);
    }

    if (got)
    {
        // Convert Mono8 to QImage; fallback to grayscale if unknown
        if (f.pixelFormat == 0x01080001 /* PFNC Mono8 */ || true)
        {
            const int w = static_cast<int>(f.width);
            const int h = static_cast<int>(f.height);
            const int pitch = static_cast<int>(f.linePitch == 0 ? f.width : f.linePitch);
            QImage img(f.data.data(), w, h, pitch, QImage::Format_Grayscale8);
            frameImage_ = img.copy(); // ensure ownership
        }
        else
        {
            frameImage_ = QImage();
        }
        if (overlayMode_ != OverlayMode::Off)
        {
            computeProcessedOverlay();
        }
        if (canvas_)
            canvas_->update();

        // Track metrics for live playback only (not scrubbing or review mode)
        if (running && followLive_ && !scrubbing_ && hasRange)
        {
            const uint64_t displayTimeUs = backend::Tools::getTimestamp();
            // Use latest index when following live (only track if range is available)
            const uint64_t frameIndex = latest;
            // Only track if this is a new frame (index changed) to avoid duplicate samples
            // Display FPS will be calculated from actual frame updates, not just UI refreshes
            if (!metricsInitialized_ || frameIndex != lastDisplayedIndex_)
            {
                // Frame timestamp may be in nanoseconds, convert to microseconds for consistency
                // Assuming frame timestamps are in nanoseconds (common for Euresys SDK)
                uint64_t frameTimestampUs = f.timestamp;
                // If timestamp is likely in nanoseconds (> 1e12), convert to microseconds
                if (f.timestamp > 1'000'000'000'000ULL)
                {
                    frameTimestampUs = f.timestamp / 1000ULL;
                }
                trackFrameDisplay(frameIndex, frameTimestampUs, displayTimeUs);
            }
        }
    }

    // Enable/disable background button: allowed when scrubbing or not following live
    if (setBgBtn_)
    {
        const bool captureRunning = backend_.capture().isRunning();
        const bool pausedMode = scrubbing_ || !followLive_ || !captureRunning;
        if (setBgBtn_->isEnabled() != pausedMode)
            setBgBtn_->setEnabled(pausedMode);
    }
}

void PlaybackPanel::onSliderPressed()
{
    scrubbing_ = true;
    SPDLOG_INFO("PlaybackPanel: scrubbing started");
    if (setBgBtn_)
        setBgBtn_->setEnabled(true);
    if (slider_)
        slider_->setFocus();
}

void PlaybackPanel::onSliderReleased()
{
    scrubbing_ = false;
    // Stay at user's chosen frame until capture (re)starts
    followLive_ = false;
    SPDLOG_INFO("PlaybackPanel: scrubbing ended, pinned index={}", pinnedIndex_);
    if (setBgBtn_)
        setBgBtn_->setEnabled(true);
}

void PlaybackPanel::onSliderValueChanged(int value)
{
    if (!scrubbing_)
        return; // Only react during scrubbing
    backend::playback::Frame f;
    pinnedIndex_ = static_cast<uint64_t>(std::max(0, value));
    if (backend_.playback().fetchByIndex(pinnedIndex_, f))
    {
        if (f.pixelFormat == 0x01080001 /* PFNC Mono8 */ || true)
        {
            const int w = static_cast<int>(f.width);
            const int h = static_cast<int>(f.height);
            const int pitch = static_cast<int>(f.linePitch == 0 ? f.width : f.linePitch);
            QImage img(f.data.data(), w, h, pitch, QImage::Format_Grayscale8);
            frameImage_ = img.copy();
        }
        else
        {
            frameImage_ = QImage();
        }
        if (overlayMode_ != OverlayMode::Off)
        {
            computeProcessedOverlay();
        }
        if (canvas_)
            canvas_->update();
    }
}

void PlaybackPanel::onToggleOverlay()
{
    switch (overlayMode_)
    {
    case OverlayMode::Off:
        overlayMode_ = OverlayMode::Mask;
        break;
    case OverlayMode::Mask:
        overlayMode_ = OverlayMode::Contours;
        break;
    case OverlayMode::Contours:
        overlayMode_ = OverlayMode::Both;
        break;
    case OverlayMode::Both:
        overlayMode_ = OverlayMode::Off;
        break;
    }
    SPDLOG_INFO("PlaybackPanel: overlay mode changed to {}", static_cast<int>(overlayMode_));
    if (overlayMode_ != OverlayMode::Off)
    {
        computeProcessedOverlay();
    }
    else
    {
        overlayImage_ = QImage();
        overlayContours_.clear();
    }
    updateOverlayButtonUi();
    if (canvas_)
        canvas_->update();
}

void PlaybackPanel::onSetBackground()
{
    const bool pausedMode = scrubbing_ || !followLive_ || !backend_.capture().isRunning();
    if (!pausedMode)
    {
        SPDLOG_INFO("PlaybackPanel: Set Background ignored (not paused)");
        return;
    }
    if (frameImage_.isNull())
        return;
    // Ensure grayscale
    if (frameImage_.format() == QImage::Format_Grayscale8)
        backgroundGray_ = frameImage_.copy();
    else
        backgroundGray_ = frameImage_.convertToFormat(QImage::Format_Grayscale8);
    hasBackground_ = !backgroundGray_.isNull();
    SPDLOG_INFO("PlaybackPanel: background captured ({}x{})",
                backgroundGray_.width(), backgroundGray_.height());
    if (overlayMode_ != OverlayMode::Off)
    {
        computeProcessedOverlay();
        if (canvas_)
            canvas_->update();
    }
    // Also push background to backend realtime processor
    if (!backgroundGray_.isNull())
    {
        QImage gray = backgroundGray_.format() == QImage::Format_Grayscale8 ? backgroundGray_
                                                                            : backgroundGray_.convertToFormat(QImage::Format_Grayscale8);
        cv::Mat bg(gray.height(), gray.width(), CV_8UC1, const_cast<uchar *>(gray.bits()), gray.bytesPerLine());
        backend_.processing().setRealtimeBackgroundGray(bg.clone());
    }
    
    // Update background indicator on button
    updateBackgroundIndicator();
    
    // Emit signal for background image change
    emit backgroundImageSet(backgroundGray_);
}

QImage PlaybackPanel::getBackgroundImage() const
{
    return backgroundGray_.copy();
}

void PlaybackPanel::onCalibrateBackground()
{
    using Proc = backend::services::ProcessingService;
    auto& proc = backend_.processing();
    if (proc.backgroundCalibrationStatus().state == Proc::BackgroundCalibrationState::Running)
    {
        proc.cancelBackgroundCalibration();
        return; // the poll reports the Cancelled result
    }
    if (!backend_.capture().isRunning())
    {
        QMessageBox::information(this, tr("Background Calibration"),
                                 tr("Start Live View first: calibration samples empty frames from the live camera."));
        return;
    }
    Proc::BackgroundCalibrationRequest request; // 10 empty frames, ≤200 attempts, ≤5 s
    std::string error;
    if (!proc.startBackgroundCalibration(request, &error))
    {
        QMessageBox::warning(this, tr("Background Calibration"),
                             tr("Could not start background calibration: %1").arg(QString::fromStdString(error)));
        return;
    }
    SPDLOG_INFO("PlaybackPanel: bounded background calibration started");
    bgCalTimer_->start();
}

void PlaybackPanel::onPollBackgroundCalibration()
{
    using Proc = backend::services::ProcessingService;
    auto& proc = backend_.processing();
    const auto status = proc.backgroundCalibrationStatus();
    if (!status.finished())
        return;
    bgCalTimer_->stop();
    if (status.state == Proc::BackgroundCalibrationState::Succeeded)
    {
        const cv::Mat bg = proc.getRealtimeBackgroundGray();
        if (!bg.empty() && bg.type() == CV_8UC1)
        {
            backgroundGray_ = QImage(bg.data, bg.cols, bg.rows, static_cast<int>(bg.step),
                                     QImage::Format_Grayscale8).copy();
            hasBackground_ = !backgroundGray_.isNull();
            updateBackgroundIndicator();
            emit backgroundImageSet(backgroundGray_);
            if (overlayMode_ != OverlayMode::Off)
            {
                computeProcessedOverlay();
                if (canvas_) canvas_->update();
            }
        }
        SPDLOG_INFO("PlaybackPanel: background calibration succeeded ({} accepted of {} attempted, generation {})",
                    status.accepted, status.attempted, status.publishedBackgroundGeneration);
        return;
    }
    QString what;
    switch (status.state)
    {
    case Proc::BackgroundCalibrationState::FailedInsufficient:
        what = tr("Not enough empty frames: %1 accepted in %2 attempts (%3 contaminated).")
                   .arg(status.accepted).arg(status.attempted).arg(status.rejectedNonEmpty);
        break;
    case Proc::BackgroundCalibrationState::FailedTimeout:
        what = tr("Timed out with %1 empty frames accepted.").arg(status.accepted);
        break;
    case Proc::BackgroundCalibrationState::FailedProcessing:
        what = tr("Processing configuration changed or frames could not be processed.");
        break;
    case Proc::BackgroundCalibrationState::Cancelled:
        what = tr("Cancelled.");
        break;
    default:
        what = QString::fromStdString(status.message);
        break;
    }
    SPDLOG_WARN("PlaybackPanel: background calibration ended without publishing: {}", status.message);
    QMessageBox::information(this, tr("Background Calibration"),
                             tr("%1\n\nThe previous background is unchanged.\n%2")
                                 .arg(what, QString::fromStdString(status.message)));
}

void PlaybackPanel::onBackgroundAutoCaptured(const QImage& background, uint64_t frameIndex) {
    if (background.isNull()) return;
    
    // Ensure grayscale format
    if (background.format() == QImage::Format_Grayscale8) {
        backgroundGray_ = background.copy();
    } else {
        backgroundGray_ = background.convertToFormat(QImage::Format_Grayscale8);
    }
    hasBackground_ = !backgroundGray_.isNull();
    
    SPDLOG_INFO("PlaybackPanel: auto-captured background updated ({}x{}, frame {})",
                backgroundGray_.width(), backgroundGray_.height(), frameIndex);
    
    // Update backend (already set, but ensure sync)
    if (!backgroundGray_.isNull()) {
        QImage gray = backgroundGray_.format() == QImage::Format_Grayscale8 ? backgroundGray_
                                                                          : backgroundGray_.convertToFormat(QImage::Format_Grayscale8);
        cv::Mat bg(gray.height(), gray.width(), CV_8UC1, const_cast<uchar *>(gray.bits()), gray.bytesPerLine());
        backend_.processing().setRealtimeBackgroundGray(bg.clone());
    }
    
    updateBackgroundIndicator();
    emit backgroundImageSet(backgroundGray_);
    
    if (overlayMode_ != OverlayMode::Off) {
        computeProcessedOverlay();
        if (canvas_) canvas_->update();
    }
}

void PlaybackPanel::onAutoBackgroundToggled(bool enabled) {
    backend::services::ProcessingConfig cfg = backend_.processing().getProcessingConfig();
    cfg.auto_background_enabled = enabled;
    backend_.processing().setProcessingConfig(cfg);
    SPDLOG_INFO("PlaybackPanel: auto-background capture {}", enabled ? "enabled" : "disabled");
}

void PlaybackPanel::onClearRoi()
{
    if (!roiActive_) return;
    
    // Clear ROI by setting to empty rect
    QRect emptyRoi;
    setRoi(emptyRoi);
}

void PlaybackPanel::onSaveBuffer()
{
    frontend::BufferSaveDialog dialog(backend_, this);
    dialog.exec();
}

void PlaybackPanel::onToggleRecording()
{
    if (backend_.isFrameRecording()) {
        backend_.stopFrameRecording();
        updateRecordingUI();
        return;
    }

    // Prompt user for HDF5 file path
    QString defaultDir;
    const QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!documentsPath.isEmpty()) {
        defaultDir = QDir(documentsPath).absoluteFilePath("MIB_Studio_Qt/recordings");
    } else {
        defaultDir = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("recordings");
    }
    QDir().mkpath(defaultDir);

    // Generate default filename with timestamp
    const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QString defaultFile = QDir(defaultDir).absoluteFilePath(
        QString("recording_%1.h5").arg(timestamp));

    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("Save Recording As"), defaultFile,
        tr("HDF5 Files (*.h5 *.hdf5)"));

    if (filePath.isEmpty()) return;

    const std::string path = filePath.toStdString();
    if (!backend_.startFrameRecording(path)) {
        QMessageBox::warning(this, tr("Recording Error"),
                             tr("Failed to start frame recording. Check that the camera is running."));
        return;
    }
    updateRecordingUI();
}

void PlaybackPanel::updateRecordingUI()
{
    const bool recording = backend_.isFrameRecording();
    if (recording) {
        recordBtn_->setText("Stop Rec");
        recordBtn_->setStyleSheet("color: red; font-weight: bold;");
        recordBtn_->setToolTip("Stop recording");
        const uint64_t written = backend_.frameRecordingCount();
        const uint64_t filtered = backend_.frameRecordingFiltered();
        recordStatusLabel_->setText(
            QString("Rec: %1 saved, %2 empty skipped").arg(written).arg(filtered));
    } else {
        recordBtn_->setText("Record");
        recordBtn_->setStyleSheet("");
        recordBtn_->setToolTip("Record non-empty frames to HDF5 (images + metadata only, no contour processing)");
        recordStatusLabel_->setText("");
    }
}

void PlaybackPanel::onToggleFit()
{
    if (fitMode_ == FitMode::FitToWindow)
    {
        fitMode_ = FitMode::Zoom100;
        if (fitBtn_)
        {
            fitBtn_->setText("Fit: 100%");
            fitBtn_->setToolTip("Toggle between fit-to-window and 100% zoom");
        }
    }
    else
    {
        fitMode_ = FitMode::FitToWindow;
        if (fitBtn_)
        {
            fitBtn_->setText("Fit: Window");
            fitBtn_->setToolTip("Toggle between fit-to-window and 100% zoom");
        }
    }
    if (canvas_)
        canvas_->update();
}

void PlaybackPanel::onToggleCapture()
{
    // Presentation only: the authoritative command path (CameraController in
    // MainWindow) decides whether a start/stop is allowed right now.
    SPDLOG_INFO("PlaybackPanel: capture toggle requested (Space)");
    emit captureToggleRequested();
}

static cv::Mat qimageToCvGrayClone(const QImage &img)
{
    QImage gray = img.format() == QImage::Format_Grayscale8 ? img : img.convertToFormat(QImage::Format_Grayscale8);
    cv::Mat mat(gray.height(), gray.width(), CV_8UC1,
                const_cast<uchar *>(gray.bits()), gray.bytesPerLine());
    return mat.clone(); // ensure independent buffer
}

static QImage maskToTintedOverlay(const cv::Mat &mask, const QColor &color, int alpha)
{
    const int w = mask.cols;
    const int h = mask.rows;
    QImage overlay(w, h, QImage::Format_RGBA8888);
    overlay.fill(Qt::transparent);
    const uchar r = static_cast<uchar>(color.red());
    const uchar g = static_cast<uchar>(color.green());
    const uchar b = static_cast<uchar>(color.blue());
    const uchar a = static_cast<uchar>(std::clamp(alpha, 0, 255));
    for (int y = 0; y < h; ++y)
    {
        const uchar *mrow = mask.ptr<uchar>(y);
        uchar *orow = overlay.scanLine(y);
        for (int x = 0; x < w; ++x)
        {
            const bool on = mrow[x] > 0;
            if (on)
            {
                orow[4 * x + 0] = r;
                orow[4 * x + 1] = g;
                orow[4 * x + 2] = b;
                orow[4 * x + 3] = a;
            }
            else
            {
                // leave transparent
            }
        }
    }
    return overlay;
}

void PlaybackPanel::computeProcessedOverlay()
{
    const uint64_t t0us = backend::Tools::getTimestamp();
    overlayImage_ = QImage();
    overlayContours_.clear();
    if (overlayMode_ == OverlayMode::Off)
        return;
    if (frameImage_.isNull())
        return;

    // Pull processing config to ensure parity with backend
    backend::services::ProcessingConfig cfg = backend_.processing().getProcessingConfig();
    // ROI
    QRect roi = roiActive_ ? imageRoi_ : QRect(0, 0, frameImage_.width(), frameImage_.height());
    roi = roi.intersected(QRect(0, 0, frameImage_.width(), frameImage_.height()));
    if (!roi.isValid() || roi.isNull())
        return;

    // Current frame gray
    cv::Mat current = qimageToCvGrayClone(frameImage_);

    // Prepare background
    cv::Mat bg;
    const bool canUseBg = hasBackground_ &&
                          !backgroundGray_.isNull() &&
                          backgroundGray_.size() == frameImage_.size();
    if (canUseBg)
    {
        bg = qimageToCvGrayClone(backgroundGray_);
    }

    // Both replay and live-following masks go through the active backend core.
    // This deliberately removes the frontend's duplicate blur/threshold/
    // morphology implementation, which could otherwise disagree after a core
    // version switch.
    auto processed = backend_.processing().computeProcessedFrame(
        current, canUseBg ? bg : cv::Mat(), cfg,
        {roi.x(), roi.y(), roi.width(), roi.height()});
    if (processed.processedImage.empty())
        return;
    cv::Mat mask = processed.processedImage;

    // Determine overlay color from the displayed frame's classification.
    QColor overlayColor(0, 255, 0); // default green
    const bool liveFollowing =
        backend_.capture().isRunning() && followLive_ && !scrubbing_;
    backend::services::FilterResult validation = processed.validation;
    bool haveValidation = true;
    if (liveFollowing) {
        // Following live: the on-screen frame is the latest captured frame,
        // so the live snapshot is authoritative (it also carries cross-frame
        // tracking / target-group ownership a single frame cannot reproduce).
        backend::services::ProcessingService::RealtimeSnapshot snapshot;
        if (backend_.processing().getLatestSnapshot(snapshot)) {
            validation = snapshot.validation;
            haveValidation = true;
        }
    }
    if (haveValidation) {
        if (validation.isTargetGroup) {
            overlayColor = QColor(0, 120, 255);  // Blue
        } else if (validation.isValid) {
            overlayColor = QColor(0, 255, 0);    // Green
        } else {
            overlayColor = QColor(255, 0, 0);    // Red
        }
    }

    // Extract contours with hierarchy so we can isolate nested (inner) contours.
    // Inner contours (hierarchy[i][3] >= 0, i.e. has a parent) are the ones used
    // for metrics calculation in ProcessingService.
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(mask.clone(), contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_SIMPLE);

    // Build overlay image (mask tint) — only for nested contour region
    if (overlayMode_ == OverlayMode::Mask || overlayMode_ == OverlayMode::Both)
    {
        // Create a filtered mask containing only the inner (nested) contour regions
        cv::Mat innerMask = cv::Mat::zeros(mask.rows, mask.cols, CV_8UC1);
        bool hasInner = false;
        for (int i = 0; i < static_cast<int>(contours.size()); ++i) {
            if (hierarchy[i][3] >= 0) { // has parent → inner contour
                cv::drawContours(innerMask, contours, i, cv::Scalar(255), -1);
                hasInner = true;
            }
        }
        if (hasInner) {
            overlayImage_ = maskToTintedOverlay(innerMask, overlayColor, 90);
        } else {
            // Fallback: no nested contour found, show full mask
            overlayImage_ = maskToTintedOverlay(mask, overlayColor, 90);
        }
    }

    // Build contour outlines — draw both outer and inner contours
    if (overlayMode_ == OverlayMode::Contours || overlayMode_ == OverlayMode::Both)
    {
        overlayContours_.clear();
        overlayContours_.reserve(static_cast<int>(contours.size()));
        for (const auto &c : contours)
        {
            QPolygon poly;
            poly.reserve(static_cast<int>(c.size()));
            for (const auto &pt : c)
            {
                poly << QPoint(pt.x, pt.y);
            }
            overlayContours_.append({poly, overlayColor});
        }
    }

    // Record timing
    const uint64_t t1us = backend::Tools::getTimestamp();
    lastOverlayComputeMs_ = static_cast<double>(t1us - t0us) / 1000.0;
}

void PlaybackPanel::setDisplayFps(int fps)
{
    if (fps < 1) fps = 1;
    if (fps > 240) fps = 240;
    const int intervalMs = std::max(1, static_cast<int>(std::lround(1000.0 / static_cast<double>(fps))));
    if (timer_) {
        timer_->setInterval(intervalMs);
        SPDLOG_INFO("Playback preview: display_fps={} (~{} ms) [updated]", fps, intervalMs);
    }
}

void PlaybackPanel::updateOverlayButtonUi()
{
    if (!overlayBtn_)
        return;
    const char *label = nullptr;
    switch (overlayMode_)
    {
    case OverlayMode::Off:
        label = "Overlay: Off";
        break;
    case OverlayMode::Mask:
        label = "Overlay: Mask";
        break;
    case OverlayMode::Contours:
        label = "Overlay: Contours";
        break;
    case OverlayMode::Both:
        label = "Overlay: Both";
        break;
    }
    overlayBtn_->setText(label);
    overlayBtn_->setToolTip("Toggle overlay (Off → Mask → Contours → Both)");
    if (overlayLegend_)
        overlayLegend_->setVisible(overlayMode_ != OverlayMode::Off);
}

bool PlaybackPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == slider_ && event->type() == QEvent::KeyPress)
    {
        auto *ke = static_cast<QKeyEvent *>(event);
        const int key = ke->key();
        const bool shift = (ke->modifiers() & Qt::ShiftModifier);
        const int step = shift ? std::max(1, slider_->pageStep()) : std::max(1, slider_->singleStep());
        const int minVal = slider_->minimum();
        const int maxVal = slider_->maximum();
        int newVal = slider_->value();
        bool handled = false;

        switch (key)
        {
        case Qt::Key_Left:
            newVal = std::clamp(newVal - step, minVal, maxVal);
            handled = true;
            break;
        case Qt::Key_Right:
            newVal = std::clamp(newVal + step, minVal, maxVal);
            handled = true;
            break;
        case Qt::Key_Home:
            newVal = minVal;
            handled = true;
            break;
        case Qt::Key_End:
            newVal = maxVal;
            handled = true;
            break;
        case Qt::Key_Space:
            onToggleCapture();
            return true;
        default:
            break;
        }

        if (handled)
        {
            followLive_ = false;
            if (setBgBtn_)
                setBgBtn_->setEnabled(true);
            if (newVal != slider_->value())
            {
                scrubbing_ = true;
                slider_->setValue(newVal); // triggers onSliderValueChanged
                scrubbing_ = false;
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PlaybackPanel::resetMetrics()
{
    metricsWindow_.clear();
    lastDisplayedIndex_ = 0;
    lastDisplayTimeUs_ = 0;
    metricsInitialized_ = false;
    totalDrops_ = 0;
    framesPresented_ = 0;
    lastDisplayFps_ = 0.0;
}

void PlaybackPanel::trackFrameDisplay(uint64_t frameIndex, uint64_t frameTimestamp, uint64_t displayTime)
{
    const uint64_t windowDurationUs = 1'000'000ULL; // 1 second in microseconds

    // Prune old samples outside the 1-second window
    while (!metricsWindow_.empty() && (displayTime - metricsWindow_.front().displayTimeUs) > windowDurationUs)
    {
        metricsWindow_.pop_front();
    }

    // Detect frame drops by comparing sequential indices
    if (metricsInitialized_)
    {
        if (frameIndex > lastDisplayedIndex_ + 1)
        {
            // Frames were skipped
            const uint64_t drops = frameIndex - lastDisplayedIndex_ - 1;
            totalDrops_ += drops;
        }
    }
    else
    {
        metricsInitialized_ = true;
    }

    // Add new sample to window
    MetricsSample sample;
    sample.displayTimeUs = displayTime;
    sample.frameTimestamp = frameTimestamp;
    sample.frameIndex = frameIndex;
    metricsWindow_.push_back(sample);
    ++framesPresented_;

    lastDisplayedIndex_ = frameIndex;
    lastDisplayTimeUs_ = displayTime;
}

void PlaybackPanel::onLogMetrics()
{
    const bool captureRunning = backend_.capture().isRunning();

    // Only log metrics during live playback when capture is running
    if (!captureRunning || !followLive_ || scrubbing_)
    {
        return;
    }

    if (metricsWindow_.empty())
    {
        return;
    }

    const uint64_t nowUs = backend::Tools::getTimestamp();
    const uint64_t windowDurationUs = 1'000'000ULL; // 1 second

    // Prune samples outside the window
    while (!metricsWindow_.empty() && (nowUs - metricsWindow_.front().displayTimeUs) > windowDurationUs)
    {
        metricsWindow_.pop_front();
    }

    if (metricsWindow_.empty())
    {
        return;
    }

    // Calculate display FPS: number of frames displayed in the window
    const size_t displayCount = metricsWindow_.size();
    const uint64_t windowStartUs = metricsWindow_.front().displayTimeUs;
    const uint64_t windowEndUs = metricsWindow_.back().displayTimeUs;
    const double windowDurationSeconds = (windowEndUs > windowStartUs)
                                             ? static_cast<double>(windowEndUs - windowStartUs) / 1'000'000.0
                                             : 1.0; // Fallback to 1 second if timestamps are equal

    const double displayFps = static_cast<double>(displayCount) / windowDurationSeconds;
    lastDisplayFps_ = displayFps;

    // Calculate average latency
    // Note: Frame timestamps from camera may be in nanoseconds, display time is in microseconds
    // We'll assume frame timestamps are also in microseconds for latency calculation
    // If they're in nanoseconds, we'd need to divide by 1000
    double totalLatencyUs = 0.0;
    size_t validLatencySamples = 0;

    for (const auto &sample : metricsWindow_)
    {
        // Calculate latency: display_time - capture_time
        // Handle potential overflow/wraparound by checking if display time is after capture time
        if (sample.displayTimeUs >= sample.frameTimestamp)
        {
            const double latencyUs = static_cast<double>(sample.displayTimeUs - sample.frameTimestamp);
            totalLatencyUs += latencyUs;
            validLatencySamples++;
        }
    }

    const double avgLatencyMs = (validLatencySamples > 0)
                                    ? (totalLatencyUs / static_cast<double>(validLatencySamples)) / 1000.0
                                    : 0.0;

    // Get drops in the current window (recent drops)
    uint64_t windowDrops = 0;
    if (metricsWindow_.size() > 1)
    {
        auto it = metricsWindow_.begin();
        auto prev = it++;
        for (; it != metricsWindow_.end(); ++it, ++prev)
        {
            if (it->frameIndex > prev->frameIndex + 1)
            {
                windowDrops += (it->frameIndex - prev->frameIndex - 1);
            }
        }
    }

    // Overlay/ROI stats
    const int imgW = frameImage_.width();
    const int imgH = frameImage_.height();
    const int ovW = overlayImage_.isNull() ? 0 : overlayImage_.width();
    const int ovH = overlayImage_.isNull() ? 0 : overlayImage_.height();
    const uint64_t roiArea = roiActive_ ? static_cast<uint64_t>(imageRoi_.width()) * static_cast<uint64_t>(imageRoi_.height())
                                        : static_cast<uint64_t>(std::max(0, imgW)) * static_cast<uint64_t>(std::max(0, imgH));
    SPDLOG_INFO("Playback metrics: display_fps={:.1f}, avg_latency_ms={:.2f}, drops={} (window_drops={}), overlay_ms={:.2f}, roi_area={}, img={}x{}, overlay={}x{}, overlay_mode={}",
                displayFps, avgLatencyMs, totalDrops_, windowDrops, lastOverlayComputeMs_, roiArea, imgW, imgH, ovW, ovH, static_cast<int>(overlayMode_));

    const bool degraded = (displayFps > 0.0 && displayFps < 30.0) ||
                          avgLatencyMs > 250.0 ||
                          windowDrops > 0 ||
                          lastOverlayComputeMs_ > 30.0;
    static uint64_t lastSentryPerfUs = 0;
    if (degraded && nowUs - lastSentryPerfUs >= 60'000'000ULL)
    {
        const std::string data = fmt::format(
            "{{\"display_fps\":{:.3f},\"avg_latency_ms\":{:.3f},\"total_drops\":{},\"window_drops\":{},\"overlay_ms\":{:.3f},\"roi_area\":{},\"image_width\":{},\"image_height\":{},\"overlay_width\":{},\"overlay_height\":{},\"overlay_mode\":{}}}",
            displayFps, avgLatencyMs, totalDrops_, windowDrops, lastOverlayComputeMs_,
            roiArea, imgW, imgH, ovW, ovH, static_cast<int>(overlayMode_));
        backend::services::CrashReporter::capturePerformanceTransaction(
            "playback.degraded", "ui.render", windowDurationSeconds * 1000.0, data);
        lastSentryPerfUs = nowUs;
    }
}

QString PlaybackPanel::getConfigPath() const {
    QSettings s;
    const QString ext = s.value("Config/ExternalAppConfigPath").toString().trimmed();
    if (!ext.isEmpty()) return ext;
    return QDir(getUserConfigDir()).absoluteFilePath("config.json");
}

void PlaybackPanel::saveRoiToConfig(const QRect& roi) {
    const QString configPath = getConfigPath();
    QFile file(configPath);
    
    if (!file.exists()) {
        SPDLOG_DEBUG("PlaybackPanel: config.json does not exist, skipping ROI save");
        return;
    }
    
    if (!file.open(QIODevice::ReadWrite | QIODevice::Text)) {
        SPDLOG_WARN("PlaybackPanel: failed to open config.json for ROI save: {}", file.errorString().toStdString());
        return;
    }
    
    QByteArray data = file.readAll();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    
    if (parseError.error != QJsonParseError::NoError) {
        SPDLOG_WARN("PlaybackPanel: failed to parse config.json for ROI save: {}", parseError.errorString().toStdString());
        file.close();
        return;
    }
    
    if (!doc.isObject()) {
        SPDLOG_WARN("PlaybackPanel: config.json root is not an object, skipping ROI save");
        file.close();
        return;
    }
    
    QJsonObject root = doc.object();
    
    // Only save valid ROI (width > 0 and height > 0)
    if (roi.isValid() && !roi.isNull() && roi.width() > 0 && roi.height() > 0) {
        QJsonObject roiObj;
        roiObj.insert("x", roi.x());
        roiObj.insert("y", roi.y());
        roiObj.insert("w", roi.width());
        roiObj.insert("h", roi.height());
        root.insert("roi", roiObj);
    } else {
        // Remove ROI from config when cleared or invalid
        root.remove("roi");
    }
    doc.setObject(root);
    
    // Write back to file
    file.resize(0);
    file.seek(0);
    QTextStream out(&file);
    out << doc.toJson(QJsonDocument::Indented);
    file.close();
    
    SPDLOG_DEBUG("PlaybackPanel: saved ROI to config.json: x={}, y={}, w={}, h={}", 
                 roi.x(), roi.y(), roi.width(), roi.height());
}

void PlaybackPanel::updateBackgroundIndicator() {
    if (!setBgBtn_) return;
    
    if (hasBackground_ && !backgroundGray_.isNull()) {
        setBgBtn_->setText(QString("Set Background (Set: %1x%2)").arg(backgroundGray_.width()).arg(backgroundGray_.height()));
        setBgBtn_->setToolTip(QString("Background image is set (%1x%2). Click to update.").arg(backgroundGray_.width()).arg(backgroundGray_.height()));
    } else {
        setBgBtn_->setText("Set Background");
        setBgBtn_->setToolTip("Capture current frame as background (when paused)");
    }
}
