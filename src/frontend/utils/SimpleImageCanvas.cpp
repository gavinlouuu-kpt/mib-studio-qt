#include "frontend/utils/SimpleImageCanvas.h"
// OverviewTab.h is already included in SimpleImageCanvas.h

#include <QPainter>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <cmath>

namespace frontend
{

    namespace
    {
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
    }

    SimpleImageCanvas::SimpleImageCanvas(QImage *image, OverviewTab::FitMode *fitMode,
                                         bool *roiVisible, QPointF *roiPos,
                                         int *roiWidth, int *roiHeight,
                                         QWidget *parent)
        : QWidget(parent), image_(image), fitMode_(fitMode),
          roiVisible_(roiVisible), roiPos_(roiPos),
          roiWidth_(roiWidth), roiHeight_(roiHeight)
    {
        setMouseTracking(true);
    }

    void SimpleImageCanvas::paintEvent(QPaintEvent *)
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!image_ || image_->isNull())
            return;

        const int imgW = image_->width();
        const int imgH = image_->height();

        double scale;
        QSizeF drawSize;
        QPointF topLeft;

        if (fitMode_ && *fitMode_ == OverviewTab::FitMode::Zoom100)
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

        // Store transformation info for coordinate conversion
        scale_ = scale;
        topLeft_ = topLeft;
        drawSize_ = drawSize;

        // Base image: cache the scaled result; recompute only on content/size change
        const QSize drawSz = drawSize.toSize();
        const qint64 imgKey = image_->cacheKey();
        if (scaledImgCache_.isNull() || imgKey != lastImgCacheKey_ || drawSz != lastDrawSize_) {
            scaledImgCache_ = image_->scaled(drawSz, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            lastImgCacheKey_ = imgKey;
            lastDrawSize_ = drawSz;
        }
        p.drawImage(topLeft.toPoint(), scaledImgCache_);

        // Draw ROI overlay if visible
        if (roiVisible_ && *roiVisible_ && roiPos_)
        {
            const int roiW = roiWidth_ ? *roiWidth_ : 512;
            const int roiH = roiHeight_ ? *roiHeight_ : 96;

            // Convert image coordinates to canvas coordinates
            QPointF canvasPos = imageToCanvas(displayedRoiPosition());
            QRectF roiRect(canvasPos.x(), canvasPos.y(), roiW * scale, roiH * scale);

            // Draw semi-transparent rectangle
            p.setPen(QPen(QColor(255, 0, 0, 200), 2));
            p.setBrush(QBrush(QColor(255, 0, 0, 30)));
            p.drawRect(roiRect);
        }
    }

    void SimpleImageCanvas::mousePressEvent(QMouseEvent *event)
    {
        if (!roiVisible_ || !*roiVisible_ || !roiPos_ || !image_ || image_->isNull())
        {
            QWidget::mousePressEvent(event);
            return;
        }

        if (event->button() == Qt::LeftButton)
        {
            QPointF canvasPos = event->pos();
            QPointF imagePos = canvasToImage(canvasPos);

            // Check if click is within ROI rectangle
            const int roiW = roiWidth_ ? *roiWidth_ : 512;
            const int roiH = roiHeight_ ? *roiHeight_ : 96;
            QRectF roiRect(displayedRoiPosition(), QSizeF(roiW, roiH));

            if (roiRect.contains(imagePos))
            {
                dragging_ = true;
                dragStartCanvasPos_ = canvasPos;
                dragStartRoiPos_ = *roiPos_;
            }
        }
    }

    void SimpleImageCanvas::mouseMoveEvent(QMouseEvent *event)
    {
        if (dragging_ && roiPos_)
        {
            QPointF canvasPos = event->pos();
            QPointF deltaCanvas = canvasPos - dragStartCanvasPos_;
            QPointF deltaImage = QPointF(deltaCanvas.x() / scale_, deltaCanvas.y() / scale_);

            if (flipX_) deltaImage.setX(-deltaImage.x());
            if (flipY_) deltaImage.setY(-deltaImage.y());
            QPointF newRoiPos = dragStartRoiPos_ + deltaImage;

            // Constrain to image bounds
            if (!image_ || image_->isNull())
            {
                QWidget::mouseMoveEvent(event);
                return;
            }

            const int imgW = image_->width();
            const int imgH = image_->height();
            const int roiW = roiWidth_ ? *roiWidth_ : 512;
            const int roiH = roiHeight_ ? *roiHeight_ : 96;

            // Constrain to image bounds first
            newRoiPos.setX(std::max(0.0, std::min(double(imgW - roiW), newRoiPos.x())));
            newRoiPos.setY(std::max(0.0, std::min(double(imgH - roiH), newRoiPos.y())));

            // Snap to the selected provider's alignment constraints
            int maxOffsetX = imgW - roiW;
            int maxOffsetY = imgH - roiH;
            int snappedX =
                snapToStep(static_cast<int>(std::round(newRoiPos.x())), stepX_, maxOffsetX);
            int snappedY =
                snapToStep(static_cast<int>(std::round(newRoiPos.y())), stepY_, maxOffsetY);

            *roiPos_ = QPointF(snappedX, snappedY);
            update();
        }
    }

    void SimpleImageCanvas::mouseReleaseEvent(QMouseEvent *event)
    {
        if (dragging_ && event->button() == Qt::LeftButton)
        {
            dragging_ = false;
            if (roiPos_)
            {
                emit roiPositionChanged(*roiPos_);
            }
        }
        QWidget::mouseReleaseEvent(event);
    }

    QPointF SimpleImageCanvas::displayedRoiPosition() const {
        if (!roiPos_) return {};
        auto pos = *roiPos_;
        if (image_) {
            if (flipX_) pos.setX(image_->width() - (roiWidth_ ? *roiWidth_ : 512) - pos.x());
            if (flipY_) pos.setY(image_->height() - (roiHeight_ ? *roiHeight_ : 96) - pos.y());
        }
        return pos;
    }

    QPointF SimpleImageCanvas::canvasToImage(const QPointF &canvasPos) const
    {
        QPointF relative = canvasPos - topLeft_;
        return QPointF(relative.x() / scale_, relative.y() / scale_);
    }

    QPointF SimpleImageCanvas::imageToCanvas(const QPointF &imagePos) const
    {
        return topLeft_ + QPointF(imagePos.x() * scale_, imagePos.y() * scale_);
    }

} // namespace frontend
