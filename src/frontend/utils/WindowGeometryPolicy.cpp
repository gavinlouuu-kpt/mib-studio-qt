#include "frontend/utils/WindowGeometryPolicy.h"

#include <QVariant>

#include <algorithm>

namespace frontend::geometry {

int chooseScreen(const QRect& wanted, const QList<QRect>& screens)
{
    if (screens.isEmpty()) return -1;
    int best = 0;
    qint64 bestArea = -1;
    for (int i = 0; i < screens.size(); ++i) {
        const QRect inter = wanted.intersected(screens[i]);
        const qint64 area = inter.isValid() ? static_cast<qint64>(inter.width()) * inter.height() : 0;
        if (area > bestArea) {
            bestArea = area;
            best = i;
        }
    }
    return best;
}

QRect clampToAvailable(const QRect& wanted, const QRect& available, const QSize& minimum)
{
    if (!available.isValid()) return wanted;
    QRect r = wanted;
    // Shrink to fit, but never below the minimum (a tiny screen then simply
    // cannot fit the minimum; we still keep the top-left reachable).
    const int w = std::max(std::min(r.width(), available.width()), std::min(minimum.width(), available.width()));
    const int h = std::max(std::min(r.height(), available.height()), std::min(minimum.height(), available.height()));
    r.setSize(QSize(w, h));
    // Move inside: right/bottom first, then left/top wins (title bar reachable).
    if (r.right() > available.right()) r.moveRight(available.right());
    if (r.bottom() > available.bottom()) r.moveBottom(available.bottom());
    if (r.left() < available.left()) r.moveLeft(available.left());
    if (r.top() < available.top()) r.moveTop(available.top());
    return r;
}

QRect defaultWindowRect(const QRect& available, const QSize& preferred)
{
    const int w = std::min(preferred.width(), available.width());
    const int h = std::min(preferred.height(), available.height());
    QRect r(0, 0, w, h);
    r.moveCenter(available.center());
    return clampToAvailable(r, available, QSize(std::min(kMinWindowWidth, w), std::min(kMinWindowHeight, h)));
}

WindowRestoreDecision resolveWindowGeometry(const std::optional<QRect>& saved, int savedVersion,
                                            const QList<QRect>& screens, const QSize& minimum)
{
    WindowRestoreDecision d;
    if (screens.isEmpty()) {
        d.geometry = saved.value_or(QRect(QPoint(0, 0), kPreferredDefaultWindowSize));
        d.usedSaved = saved.has_value();
        d.screenIndex = -1;
        return d;
    }
    const bool savedUsable = saved.has_value() && savedVersion == kWindowLayoutVersion && saved->isValid() &&
                             saved->width() >= 200 && saved->height() >= 150 && saved->width() <= 16384 &&
                             saved->height() <= 16384;
    if (!savedUsable) {
        d.screenIndex = 0;
        d.geometry = defaultWindowRect(screens[0]);
        return d;
    }
    const int idx = chooseScreen(*saved, screens);
    d.screenIndex = idx;
    const QRect& available = screens[idx];
    // Require a meaningful overlap with a surviving screen; otherwise the
    // monitor is gone and we recover onto the chosen screen's default.
    const QRect inter = saved->intersected(available);
    const qint64 interArea = inter.isValid() ? static_cast<qint64>(inter.width()) * inter.height() : 0;
    const qint64 savedArea = static_cast<qint64>(saved->width()) * saved->height();
    if (interArea * 4 < savedArea) { // less than 25 % visible
        const QRect fallback = defaultWindowRect(available, saved->size());
        d.geometry = fallback;
        d.usedSaved = true;
        d.clamped = true;
        return d;
    }
    d.geometry = clampToAvailable(*saved, available, minimum);
    d.usedSaved = true;
    d.clamped = d.geometry != *saved;
    return d;
}

SidebarFit fitSidebarWidth(int preferred, int contentsWidth, int handleWidth, int workspaceMin, int sidebarMin)
{
    SidebarFit fit;
    const int wanted = std::clamp(preferred, kSidebarMinWidth, kSidebarMaxWidth);
    const int available = contentsWidth - handleWidth - workspaceMin;
    if (available >= wanted) {
        fit.width = wanted;
        return fit;
    }
    fit.compact = true;
    if (available >= sidebarMin) {
        fit.width = available;
        return fit;
    }
    fit.fits = false;
    fit.width = std::max(0, available);
    return fit;
}

int sanitizeSidebarPreferredWidth(const QVariant& stored)
{
    bool ok = false;
    const double v = stored.toDouble(&ok);
    if (!ok || v != v || v <= 0.0 || v > 100000.0) return kSidebarDefaultWidth;
    return std::clamp(static_cast<int>(v), kSidebarMinWidth, kSidebarMaxWidth);
}

} // namespace frontend::geometry
