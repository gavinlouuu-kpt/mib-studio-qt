#pragma once

#include <QChartView>
#include <QValueAxis>
#include <vector>

class QWheelEvent;
class QMouseEvent;
class QAction;

namespace frontend {

// QChartView with wheel zoom around the cursor, left/middle-drag pan and
// double-click reset. A press that moves less than
// QApplication::startDragDistance() before release is a click (plotClicked),
// never a pan; past the threshold it is a pan for the rest of the press.
class ZoomableChartView : public QChartView {
    Q_OBJECT
public:
    explicit ZoomableChartView(QChart* chart, QWidget* parent = nullptr);

    bool isUserZoomed() const { return isUserZoomed_; }
    void resetZoom();
    void setDefaultRange(QValueAxis* axis, double min, double max);

    // Double-click resets the zoom (default). A view that gives double-click
    // its own meaning turns this off and handles plotDoubleClicked itself.
    void setResetOnDoubleClick(bool enabled) { resetOnDoubleClick_ = enabled; }
    bool resetOnDoubleClick() const { return resetOnDoubleClick_; }

    // Drop a pending press or a running pan (e.g. before a dialog takes input).
    void cancelGesture();
    bool isPanning() const { return isPanning_; }

    // "Reset zoom", owned by the view, for context menus.
    QAction* resetZoomAction() const { return resetZoomAction_; }

signals:
    void zoomReset();
    // Release of a press that never crossed the drag threshold, inside the
    // plot area (view coordinates).
    void plotClicked(QPointF viewPos, Qt::MouseButton button);
    void plotDoubleClicked(QPointF viewPos);
    // Pointer movement with no press pending and no pan running.
    void hoverMoved(QPointF viewPos);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void panTo(const QPointF& pos);

    bool isUserZoomed_ = false;
    bool isPanning_ = false;
    bool pressPending_ = false;
    bool resetOnDoubleClick_ = true;
    Qt::MouseButton pressButton_ = Qt::NoButton;
    QPointF pressPos_;
    QPointF lastPanPoint_;
    QAction* resetZoomAction_ = nullptr;

    struct AxisDefault {
        QValueAxis* axis = nullptr;
        double min = 0.0;
        double max = 1.0;
    };
    std::vector<AxisDefault> defaultRanges_;
};

} // namespace frontend
