#pragma once

// Synthesized mouse/wheel input for offscreen widget tests, without QtTest.
// Events go to the widget that receives them in production: for a
// QAbstractScrollArea (QGraphicsView, QChartView) that is its viewport.

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPointF>
#include <QWheelEvent>
#include <QWidget>

namespace mib::test::mouse {

inline QWidget* target(QWidget* w)
{
    if (auto* area = qobject_cast<QAbstractScrollArea*>(w))
        return area->viewport();
    return w;
}

inline void send(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
                 Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QWidget* t = target(w);
    QMouseEvent ev(type, pos, t->mapToGlobal(pos), button, buttons, mods);
    QCoreApplication::sendEvent(t, &ev);
}

inline void press(QWidget* w, QPointF pos, Qt::MouseButton b = Qt::LeftButton)
{
    send(w, QEvent::MouseButtonPress, pos, b, b);
}

inline void move(QWidget* w, QPointF pos, Qt::MouseButtons held = Qt::NoButton)
{
    send(w, QEvent::MouseMove, pos, Qt::NoButton, held);
}

inline void release(QWidget* w, QPointF pos, Qt::MouseButton b = Qt::LeftButton)
{
    send(w, QEvent::MouseButtonRelease, pos, b, Qt::NoButton);
}

inline void click(QWidget* w, QPointF pos, Qt::MouseButton b = Qt::LeftButton)
{
    press(w, pos, b);
    release(w, pos, b);
}

// What the platform delivers for a double-click: press, release, dblclick, release.
inline void doubleClick(QWidget* w, QPointF pos)
{
    click(w, pos);
    send(w, QEvent::MouseButtonDblClick, pos, Qt::LeftButton, Qt::LeftButton);
    release(w, pos);
}

// Press, travel in `steps` moves to `to`, release.
inline void drag(QWidget* w, QPointF from, QPointF to, int steps = 5, Qt::MouseButton b = Qt::LeftButton)
{
    press(w, from, b);
    for (int i = 1; i <= steps; ++i)
        move(w, from + (to - from) * (static_cast<double>(i) / steps), b);
    release(w, to, b);
}

inline void wheel(QWidget* w, QPointF pos, int angleDeltaY, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QWidget* t = target(w);
    QWheelEvent ev(pos, t->mapToGlobal(pos), QPoint(), QPoint(0, angleDeltaY), Qt::NoButton, mods,
                   Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(t, &ev);
}

inline void leave(QWidget* w)
{
    QEvent ev(QEvent::Leave);
    QCoreApplication::sendEvent(w, &ev);
}

} // namespace mib::test::mouse
