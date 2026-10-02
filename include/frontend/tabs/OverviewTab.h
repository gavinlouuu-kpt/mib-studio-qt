#pragma once

#include <QWidget>
#include <QImage>
#include <QPointF>
#include <QString>
#include <QVector>
#include "backend/playback/FrameStore.h"

namespace backend
{
    class AppBackend;
}

class QHideEvent;
class QPlainTextEdit;
class QShowEvent;
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

        // Dot-grid wafer localization overlay (knowledge_map/services/DotGridService.md).
        // The Overview tab is the only view of the pose: decoding runs only while
        // this tab is on screen (DotGridService::setPaused from show/hide events).
        struct DotGridOverlay
        {
            bool active{false};    // localization enabled (Wafer Grid on)
            bool valid{false};     // latest decode succeeded
            QVector<QPointF> dots; // detected dot centroids, image pixels
            QPointF centre;        // image centre marker (the reported wafer position)
            QString text;          // pose summary drawn in the corner
        };
        const DotGridOverlay &dotGridOverlay() const { return dotGridOverlay_; }

        int roiWidth() const { return roiWidth_; }
        int roiHeight() const { return roiHeight_; }
        QPointF roiPosition() const { return roiPosition_; }

    signals:
        void roiChanged(int offsetX, int offsetY, int width, int height);

    protected:
        void showEvent(QShowEvent *event) override;
        void hideEvent(QHideEvent *event) override;

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
        void onToggleDotGrid();

    private:
        void updateDotGridOverlay();
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

        // Dot-grid wafer localization
        QToolButton *dotGridBtn_ = nullptr;
        DotGridOverlay dotGridOverlay_;

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
