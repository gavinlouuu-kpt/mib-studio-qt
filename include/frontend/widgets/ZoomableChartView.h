#pragma once

#include <QChartView>
#include <QValueAxis>
#include <functional>
#include <vector>

class QWheelEvent;
class QMouseEvent;
class QAction;

namespace frontend {

// QChartView with wheel zoom around the cursor, left/middle-drag pan and
// double-click reset. A press that moves less than
// QApplication::startDragDistance() before release is a click (plotClicked),
// never a pan; past the threshold it is a pan for the rest of the press.
//
// A view may install a press interceptor (setPressInterceptor): a left press
// it claims is never a pan. Without travel it is still a click (plotClicked);
// past the threshold it becomes an intercepted drag reported through
// interceptDragStarted / interceptDragMoved / interceptDragFinished (the view
// moves nothing itself), so an owner can drag its own handles drawn on the
// plot. Without an interceptor, or when it declines, nothing changes.
class ZoomableChartView : public QChartView {
    Q_OBJECT
public:
    explicit ZoomableChartView(QChart* chart, QWidget* parent = nullptr);

    bool isUserZoomed() const { return isUserZoomed_; }
    void resetZoom();
    void setDefaultRange(QValueAxis* axis, double min, double max);
    // The owner restored a user zoom by setting the axis ranges itself.
    void markUserZoomed();

    // Double-click resets the zoom (default). A view that gives double-click
    // its own meaning turns this off and handles plotDoubleClicked itself.
    void setResetOnDoubleClick(bool enabled) { resetOnDoubleClick_ = enabled; }
    bool resetOnDoubleClick() const { return resetOnDoubleClick_; }

    // Drop a pending press or a running pan (e.g. before a dialog takes input).
    void cancelGesture();
    bool isPanning() const { return isPanning_; }

    // "Reset zoom", owned by the view, for context menus.
    QAction* resetZoomAction() const { return resetZoomAction_; }

    // Called on every left press (view coordinates); true claims the press for
    // the owner's drag. Empty = never claims.
    using PressInterceptor = std::function<bool(const QPointF& viewPos)>;
    void setPressInterceptor(PressInterceptor interceptor) { pressInterceptor_ = std::move(interceptor); }
    bool isInterceptDragging() const { return interceptDragging_; }

signals:
    void zoomReset();
    // Release of a press that never crossed the drag threshold, inside the
    // plot area (view coordinates).
    void plotClicked(QPointF viewPos, Qt::MouseButton button);
    void plotDoubleClicked(QPointF viewPos);
    // Pointer movement with no press pending and no pan running.
    void hoverMoved(QPointF viewPos);
    // The pointer left the view (no button held).
    void hoverLeft();
    // A claimed press travelled past the drag threshold; pressPos is where it
    // went down. Moved follows with every pointer move (and once at the start).
    void interceptDragStarted(QPointF pressPos);
    void interceptDragMoved(QPointF pressPos, QPointF viewPos);
    // Release (completed) or lost gesture (focus out, double-click, release
    // elsewhere: completed == false) of an intercepted drag.
    void interceptDragFinished(QPointF pressPos, QPointF viewPos, bool completed);

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
    bool pressIntercepted_ = false;
    bool interceptDragging_ = false;
    PressInterceptor pressInterceptor_;
    QPointF interceptPos_;
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
