#pragma once

#include <QWidget>
#include <QImage>
#include <QRect>
#include <QList>
#include <QPolygon>
#include <cstdint>
#include <memory>
#include <vector>
#include <deque>

namespace backend { class AppBackend; }

class QTimer;
class QSlider;
class QWidget;
class QToolButton;
class QCheckBox;
class QLabel;

class PlaybackPanel : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackPanel(backend::AppBackend& backend, QWidget* parent = nullptr);
    ~PlaybackPanel() override;

    enum class OverlayMode { Off, Mask, Contours, Both };
    enum class FitMode { FitToWindow, Zoom100 };
    struct ColoredContour {
        QPolygon polygon;
        QColor color;
    };

    // ROI management
    void setRoi(const QRect& roi, bool saveToConfig = true);
    QRect getRoi() const;
    QSize getImageDimensions() const;
    
    // Metrics
    double getDisplayFps() const { return lastDisplayFps_; }
		void setDisplayFps(int fps);
    // Issue #370: presentation counters, distinct from acquisition /
    // processing / persistence loss. The preview pulls the latest frame at a
    // capped rate; frames it never showed are "skipped by display" — they
    // were still acquired, processed and (if applicable) recorded.
    uint64_t displayFramesPresented() const { return framesPresented_; }
    uint64_t displayFramesSkipped() const { return totalDrops_; }

    // Background image access
    QImage getBackgroundImage() const;

signals:
    void backgroundImageSet(const QImage& image);
    // Space-bar toggle. The panel never starts/stops capture itself (issue
    // #360): MainWindow routes this through its CameraController so the
    // experiment guard and duplicate-command protection always apply.
    void captureToggleRequested();

public slots:
    void onBackgroundAutoCaptured(const QImage& background, uint64_t frameIndex);

private slots:
    void onTick();
    void onSliderPressed();
    void onSliderReleased();
    void onSliderValueChanged(int value);
    void onToggleOverlay();
    void onSetBackground();
    // Bounded background calibration (issue #369): starts/cancels the
    // backend operation and polls its explicit result.
    void onCalibrateBackground();
    void onPollBackgroundCalibration();
    void onClearRoi();
    void onToggleCapture();
    void onSaveBuffer();
    void onLogMetrics();
    void onToggleFit();
    void onAutoBackgroundToggled(bool enabled);
    void onToggleRecording();
    void updateRecordingUI();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void computeProcessedOverlay();
    void updateOverlayButtonUi();
    void resetMetrics();
    void trackFrameDisplay(uint64_t frameIndex, uint64_t frameTimestamp, uint64_t displayTime);
    void saveRoiToConfig(const QRect& roi);
    QString getConfigPath() const;
    void updateBackgroundIndicator();

    backend::AppBackend& backend_;

    QTimer* timer_ = nullptr;
    QTimer* metricsTimer_ = nullptr;   // Timer for periodic metrics logging
    QTimer* bgCalTimer_ = nullptr;     // Polls the bounded background calibration
    QWidget* canvas_ = nullptr;
    QSlider* slider_ = nullptr;
    QToolButton* overlayBtn_ = nullptr;
    QLabel* overlayLegend_ = nullptr;
    QToolButton* setBgBtn_ = nullptr;
    QCheckBox* autoBgCheck_ = nullptr;
    QToolButton* clearRoiBtn_ = nullptr;
    QToolButton* saveBufferBtn_ = nullptr;
    QToolButton* recordBtn_ = nullptr;
    QLabel* recordStatusLabel_ = nullptr;
    QToolButton* fitBtn_ = nullptr;
    bool scrubbing_ = false;
    bool followLive_ = true;           // auto-follow latest when true
    bool prevCaptureRunning_ = false;  // detect start transitions
    uint64_t pinnedIndex_ = 0;         // selected index when reviewing
    QImage frameImage_;
    // Overlay/ROI state
    OverlayMode overlayMode_ { OverlayMode::Both };
    FitMode fitMode_ { FitMode::FitToWindow };
    QRect imageRoi_;
    bool roiActive_ = false;
    bool hasBackground_ = false;
    QImage backgroundGray_;            // stored as grayscale QImage
    QImage overlayImage_;              // RGBA overlay (mask)
    QList<ColoredContour> overlayContours_;  // image-space contours with classification color

    // Last overlay computation timing (ms)
    double lastOverlayComputeMs_ = 0.0;

    // Metrics tracking
    struct MetricsSample {
        uint64_t displayTimeUs;        // When frame was displayed (microseconds)
        uint64_t frameTimestamp;       // Frame capture timestamp (same units as frame.timestamp)
        uint64_t frameIndex;           // Frame index for drop detection
    };

    std::deque<MetricsSample> metricsWindow_;  // Rolling window of samples (last 1 second)
    uint64_t lastDisplayedIndex_ = 0;          // Last displayed frame index for drop detection
    uint64_t lastDisplayTimeUs_ = 0;           // Last display time for window pruning
    bool metricsInitialized_ = false;           // Track if we've seen first frame
    uint64_t totalDrops_ = 0;                   // Frames the display skipped (presentation only)
    uint64_t framesPresented_ = 0;              // Frames actually rendered (issue #370)
    double lastDisplayFps_ = 0.0;               // Last computed display FPS (1s window)
};


