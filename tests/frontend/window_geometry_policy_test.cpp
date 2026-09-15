// window_geometry_policy_test (issues #358, #359)
//
// Pure geometry decisions, no QApplication: screen choice, clamping to the
// available desktop, default placement, restoration from a removed/larger
// monitor, invalid/legacy persisted values, and sidebar width fitting.

#include "frontend/utils/WindowGeometryPolicy.h"

#include "support/assert.h"

#include <QVariant>

using namespace frontend::geometry;

int main()
{
    const QRect laptop(0, 0, 1366, 728);          // 1366x768 minus a 40 px taskbar
    const QRect desktop(0, 0, 1920, 1040);
    const QRect leftMonitor(-1920, 0, 1920, 1080); // negative coordinates
    const QRect small(0, 0, 1024, 728);

    // ---- screen choice ---------------------------------------------------
    MIB_EXPECT(chooseScreen(QRect(-1500, 100, 800, 600), {desktop, leftMonitor}) == 1, "largest intersection wins");
    MIB_EXPECT(chooseScreen(QRect(5000, 5000, 800, 600), {desktop, leftMonitor}) == 0, "no overlap -> primary");
    MIB_EXPECT(chooseScreen(QRect(0, 0, 10, 10), {}) == -1, "no screens");

    // ---- clamping ----------------------------------------------------------
    {
        const QRect wide(100, 100, 1800, 900);
        const QRect c = clampToAvailable(wide, laptop, QSize(kMinWindowWidth, kMinWindowHeight));
        MIB_EXPECT(laptop.contains(c), "clamped rect inside available");
        MIB_EXPECT(c.width() == 1366 && c.height() == 728, "shrunk to the available size");
        const QRect offRight(1200, 600, 900, 560);
        const QRect c2 = clampToAvailable(offRight, laptop, QSize(kMinWindowWidth, kMinWindowHeight));
        MIB_EXPECT(laptop.contains(c2) && c2.size() == QSize(900, 560), "moved back without shrinking");
        const QRect offTop(-50, -300, 900, 560);
        const QRect c3 = clampToAvailable(offTop, laptop, QSize(kMinWindowWidth, kMinWindowHeight));
        MIB_EXPECT(c3.top() == laptop.top() && c3.left() == laptop.left(), "title bar reachable");
        const QRect tiny = clampToAvailable(QRect(0, 0, 3000, 2000), QRect(0, 0, 640, 480), QSize(900, 560));
        MIB_EXPECT(tiny.size() == QSize(640, 480), "minimum never exceeds the screen");
    }

    // ---- defaults ------------------------------------------------------------
    {
        const QRect d = defaultWindowRect(laptop);
        MIB_EXPECT(laptop.contains(d), "default fits the laptop");
        MIB_EXPECT(d.width() == 1280 && d.height() == 728, "default bounded by available height");
        const QRect d2 = defaultWindowRect(desktop);
        MIB_EXPECT(d2.size() == kPreferredDefaultWindowSize && desktop.contains(d2), "preferred size on a desktop");
        MIB_EXPECT(d2.center() == desktop.center() || (d2.center() - desktop.center()).manhattanLength() <= 2, "centered");
    }

    // ---- restoration -------------------------------------------------------
    {
        // Saved on a large monitor that no longer exists -> recover onto laptop.
        const auto r = resolveWindowGeometry(QRect(2100, 200, 1700, 900), kWindowLayoutVersion, {laptop});
        MIB_EXPECT(r.usedSaved && r.clamped && laptop.contains(r.geometry), "removed monitor recovers onto current desktop");
        MIB_EXPECT(r.geometry.width() <= laptop.width() && r.geometry.height() <= laptop.height(), "fits");
        // Saved partially off-screen -> moved in.
        const auto r2 = resolveWindowGeometry(QRect(900, 400, 1000, 600), kWindowLayoutVersion, {laptop});
        MIB_EXPECT(r2.usedSaved && r2.clamped && laptop.contains(r2.geometry) && r2.geometry.size() == QSize(1000, 600),
                   "partially visible saved rect is moved, not resized");
        // Saved on the left monitor with negative coordinates -> kept there.
        const auto r3 = resolveWindowGeometry(QRect(-1800, 100, 1200, 800), kWindowLayoutVersion, {desktop, leftMonitor});
        MIB_EXPECT(r3.screenIndex == 1 && !r3.clamped && r3.geometry == QRect(-1800, 100, 1200, 800), "negative coordinates are valid");
        // Version mismatch / invalid -> default.
        const auto r4 = resolveWindowGeometry(QRect(0, 0, 1000, 700), kWindowLayoutVersion + 1, {laptop});
        MIB_EXPECT(!r4.usedSaved && r4.geometry == defaultWindowRect(laptop), "old layout version ignored");
        const auto r5 = resolveWindowGeometry(QRect(0, 0, 50, 20), kWindowLayoutVersion, {laptop});
        MIB_EXPECT(!r5.usedSaved, "absurd saved size ignored");
        const auto r6 = resolveWindowGeometry(std::nullopt, kWindowLayoutVersion, {small});
        MIB_EXPECT(!r6.usedSaved && small.contains(r6.geometry) && r6.geometry.width() == 1024, "first launch on 1024x768");
    }

    // ---- sidebar fit ---------------------------------------------------------
    {
        auto f = fitSidebarWidth(300, 1366, 6);
        MIB_EXPECT(f.fits && !f.compact && f.width == 300, "preferred width fits on 1366");
        f = fitSidebarWidth(900, 1366, 6);
        MIB_EXPECT(f.fits && f.compact && f.width == 1366 - 6 - kWorkspaceMinWidth, "oversized preference clamped to keep workspace");
        f = fitSidebarWidth(300, 900, 6);
        MIB_EXPECT(f.fits && f.compact && f.width == 900 - 6 - kWorkspaceMinWidth && f.width >= kSidebarCompactWidth,
                   "narrow window: compact width");
        f = fitSidebarWidth(300, 700, 6);
        MIB_EXPECT(!f.fits, "too narrow for even the compact panel");
        f = fitSidebarWidth(50, 1920, 6);
        MIB_EXPECT(f.width == kSidebarMinWidth, "tiny preference raised to the floor");
        f = fitSidebarWidth(5000, 4000, 6);
        MIB_EXPECT(f.width == kSidebarMaxWidth, "huge preference capped");
    }

    // ---- persisted preference sanitizing -------------------------------------
    MIB_EXPECT(sanitizeSidebarPreferredWidth(QVariant()) == kSidebarDefaultWidth, "missing -> default");
    MIB_EXPECT(sanitizeSidebarPreferredWidth(QVariant(QStringLiteral("abc"))) == kSidebarDefaultWidth, "non-numeric -> default");
    MIB_EXPECT(sanitizeSidebarPreferredWidth(QVariant(-40)) == kSidebarDefaultWidth, "negative -> default");
    MIB_EXPECT(sanitizeSidebarPreferredWidth(QVariant(999999)) == kSidebarDefaultWidth, "absurd -> default");
    MIB_EXPECT(sanitizeSidebarPreferredWidth(QVariant(1500)) == kSidebarMaxWidth, "large -> capped");
    MIB_EXPECT(sanitizeSidebarPreferredWidth(QVariant(120)) == kSidebarMinWidth, "small -> floor");
    MIB_EXPECT(sanitizeSidebarPreferredWidth(QVariant(420)) == 420, "valid kept");

    return mib::test::exitCode();
}
