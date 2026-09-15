// Pure window/sidebar geometry decisions (issues #358, #359).
//
// Everything here works in Qt device-independent coordinates and takes the
// screen list as plain rectangles, so it is unit-testable without a
// QApplication and never multiplies QScreen::availableGeometry() by a DPR.
#pragma once

#include <QList>
#include <QRect>
#include <QSize>

#include <optional>

namespace frontend::geometry {

constexpr int kWindowLayoutVersion = 1;
constexpr int kSidebarLayoutVersion = 1;

// Usable-area contract for the main window (device-independent pixels).
constexpr int kMinWindowWidth = 900;
constexpr int kMinWindowHeight = 560;
constexpr QSize kPreferredDefaultWindowSize{1280, 800};

// Sidebar (hardware panel) contract.
constexpr int kSidebarDefaultWidth = 300;
constexpr int kSidebarMinWidth = 200;      // preferred-width clamp floor
constexpr int kSidebarCompactWidth = 160;  // narrowest usable panel
constexpr int kSidebarMaxWidth = 1000;
constexpr int kWorkspaceMinWidth = 640;    // workspace must keep at least this

// Index of the screen (in `screens`) whose available geometry best matches
// `wanted` (largest intersection area). -1 when `screens` is empty; falls
// back to 0 (the primary) when nothing intersects.
int chooseScreen(const QRect& wanted, const QList<QRect>& screens);

// Clamp a frame rectangle so that it lies inside `available`, shrinking it
// first (never below `minimum`) and then moving it. The title bar edge
// (top) always ends up inside the available area.
QRect clampToAvailable(const QRect& wanted, const QRect& available, const QSize& minimum);

// Default window rectangle for a screen: the preferred size (bounded by the
// available area), centered.
QRect defaultWindowRect(const QRect& available, const QSize& preferred = kPreferredDefaultWindowSize);

struct WindowRestoreDecision {
    QRect geometry;          // frame geometry to apply
    bool usedSaved{false};   // saved geometry was valid and (possibly clamped) reused
    bool clamped{false};     // saved geometry had to be shrunk/moved
    int screenIndex{0};      // screen chosen for the result
};

// Decide the startup geometry from optional persisted state. Invalid,
// out-of-version, empty, absurd or off-screen saved rectangles fall back to
// the default on the best matching (or primary) screen.
WindowRestoreDecision resolveWindowGeometry(const std::optional<QRect>& saved, int savedVersion,
                                            const QList<QRect>& screens,
                                            const QSize& minimum = QSize(kMinWindowWidth, kMinWindowHeight));

// Sidebar width that fits the current splitter (issue #359).
//   preferred: the user's preferred expanded width (any value; clamped)
//   contentsWidth: splitter contents width
//   handleWidth: splitter handle width
// Returns the width to allocate; `fits` is false when not even the compact
// width leaves the declared minimum workspace, in which case `width` is the
// largest width that still leaves the minimum workspace (may be 0).
struct SidebarFit {
    int width{0};
    bool fits{true};
    bool compact{false}; // narrower than the preferred width
};
SidebarFit fitSidebarWidth(int preferred, int contentsWidth, int handleWidth,
                           int workspaceMin = kWorkspaceMinWidth,
                           int sidebarMin = kSidebarCompactWidth);

// Validate a persisted preferred width (legacy or current): non-finite,
// non-positive or absurd values become the default; others are clamped.
int sanitizeSidebarPreferredWidth(const QVariant& stored);

} // namespace frontend::geometry
