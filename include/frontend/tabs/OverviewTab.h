#pragma once

#include <QWidget>
#include <QImage>
#include "backend/playback/FrameStore.h"

namespace backend
{
    class AppBackend;
}

class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QLabel;
class QSpinBox;
class QTimer;
class QWidget;

namespace Ui { class OverviewTab; }

namespace frontend
{

    class OverviewTab : public QWidget
    {
        Q_OBJECT
    public:
        explicit OverviewTab(backend::AppBackend &backend, QWidget *parent = nullptr);
        ~OverviewTab();

        enum class FitMode
        {
            FitToWindow,
            Zoom100
        };

        QString currentJsPath() const;
        void refreshCameraMode();

        // Controls whether the ROI overlay is shown on the canvas.
        void setRoiOverlayVisible(bool visible);

        int roiWidth() const { return roiWidth_; }
        int roiHeight() const { return roiHeight_; }
        QPointF roiPosition() const { return roiPosition_; }

    signals:
        void roiChanged(int offsetX, int offsetY, int width, int height);

    private slots:
        void onTick();
        void onReloadJs();
        void onSaveJs();
        void onApplyJs();
        void onBrowseJs();
        void onClearJs();
        void onToggleFit();
        void onToggleRoiOverlay();
        void onRoiPositionChanged(QPointF imagePos);
        void onRoiSizeChanged();

    private:
        QString appDirIncludePath(const QString &fileName) const;
        QString defaultJsPath() const { return appDirIncludePath("overviewConfig.js"); }
        bool loadFileToEditor(const QString &path, QPlainTextEdit *editor, QString *err);
        bool saveEditorToFile(QPlainTextEdit *editor, const QString &path, QString *err);

        Ui::OverviewTab* ui;
        backend::AppBackend &backend_;

        // Frame display
        QWidget *canvas_ = nullptr;
        QTimer *timer_ = nullptr;
        QImage frameImage_;
        FitMode fitMode_{FitMode::FitToWindow};
        backend::playback::Frame scratchFrame_; // reuses vector capacity across ticks

        // ROI overlay state
        bool roiOverlayVisible_ = false;
        QPointF roiPosition_; // Position in image coordinates
        int roiWidth_ = 512;
        int roiHeight_ = 96;
        QSpinBox *roiWidthSpin_ = nullptr;
        QSpinBox *roiHeightSpin_ = nullptr;

        QString loadedCameraKey_;
        QLabel* modeLabel_ = nullptr;
        QPointF savedRoiPosition_;
        int savedRoiWidth_ = 512, savedRoiHeight_ = 96;
        bool saveMindVisionRoi();
        void updateMindVisionBounds();
        // Helper methods
        QString egrabberConfigPath() const;
        void updateEgrabberConfigFromRect(QPointF imagePos);
        void updateEgrabberConfigSize();
        void initializeRoiFromConfig();
    };

} // namespace frontend
