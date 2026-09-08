// ui_layout_test (issues #358, #359)
//
// Full-widget layout regression harness (offscreen, isolated INI settings,
// mock camera, hardware/network services disabled). Asserts actual widget
// and window geometry — never PNG dimensions:
//
//  - at 1024x768, 1366x768 and 1920x1080 logical viewports the main window's
//    required size never exceeds the viewport and every canonical tab keeps
//    the primary controls (camera start/stop, experiment start/stop,
//    hardware-panel toggle) inside the window;
//  - long status / review-path strings do not change the required width;
//  - sidebar (hardware panel): splitter is the single geometry owner —
//    drag -> collapse -> expand restores the dragged width, a narrow window
//    clamps instead of resizing the outer window, 50 toggle cycles produce
//    no drift and identical outer geometry, rapid toggles converge, an
//    oversized persisted width is clamped on a 1366 viewport, the reopen
//    control stays visible/focusable, expanded content is reachable;
//  - window geometry restore from a removed/larger monitor lands inside
//    the current desktop; invalid persisted values fall back to defaults.

#include "backend/app/AppBackend.h"
#include "frontend/core/MainWindow.h"
#include "frontend/utils/ApplicationSettings.h"
#include "frontend/utils/ElidingLabel.h"
#include "frontend/utils/SidebarWidget.h"
#include "frontend/utils/StatisticsPanel.h"
#include "frontend/utils/WindowGeometryPolicy.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStyleFactory>
#include <QTabWidget>
#include <QToolButton>
#include <QWidget>

#include <cstdio>
#include <memory>

namespace {

void settle(int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}

bool insideWindow(const QWidget* w, const QWidget* window)
{
    if (!w || !w->isVisible()) return false;
    const QRect r(w->mapTo(window, QPoint(0, 0)), w->size());
    return window->rect().contains(r) && r.width() > 0 && r.height() > 0;
}

struct Viewport { const char* name; QSize size; };

} // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,yolo,syringe_pump,pulse_generator"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    mib::test::Watchdog wd(240);

    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    mib::test::TempDir td("ui_layout");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((td.path() / "settings").string()));
    QString settingsError;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&settingsError), "settings init");
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR")) {
        qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    }

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");

    const Viewport viewports[] = {
        {"1024x768", QSize(1024, 728)},
        {"1366x768", QSize(1366, 728)},
        {"1920x1080", QSize(1920, 1040)},
    };

    // ---- A. viewport matrix ----------------------------------------------------
    for (const auto& vp : viewports) {
        wd.mark(vp.name);
        const QRect available(QPoint(0, 0), vp.size);
        auto window = std::make_unique<MainWindow>(backend);
        window->setAvailableGeometryOverrideForTests(available);
        window->resize(vp.size);
        window->show();
        settle(10);
        // The decorated frame must fit the desktop; the client size is the
        // viewport minus the (platform) frame margin, never less.
        const QSize frameMargin = window->frameGeometry().size() - window->size();
        MIB_EXPECT(available.contains(window->frameGeometry()),
                   std::string(vp.name) + ": decorated window inside the available desktop");
        MIB_EXPECT(window->size() + frameMargin == vp.size,
                   std::string(vp.name) + ": window uses the whole viewport minus the frame (" +
                       std::to_string(window->width()) + "x" + std::to_string(window->height()) + ")");
        const QSize settled = window->size();
        const QSize need = window->minimumSizeHint().expandedTo(window->minimumSize());
        std::fprintf(stderr, "%s: minimumSizeHint=%dx%d sidebar=%d\n", vp.name, need.width(), need.height(),
                     window->isHardwarePanelVisible() ? 1 : 0);
        MIB_EXPECT(need.width() <= vp.size.width() && need.height() <= vp.size.height(),
                   std::string(vp.name) + ": required size fits the viewport");
        auto* tabs = window->mainTabs();
        MIB_REQUIRE(tabs && tabs->count() >= 4, "main tabs");
        auto* startCam = window->findChild<QPushButton*>(QStringLiteral("startCameraBtn"));
        auto* stopCam = window->findChild<QPushButton*>(QStringLiteral("stopCameraBtn"));
        auto* panelBtn = window->findChild<QToolButton*>(QStringLiteral("hardwarePanelBtn"));
        MIB_REQUIRE(startCam && stopCam && panelBtn, "primary controls exist");
        for (int t = 0; t < tabs->count(); ++t) {
            tabs->setCurrentIndex(t);
            settle(6);
            if (auto* exp = window->experimentTabs(); exp && tabs->currentWidget() == exp) {
                for (int e = 0; e < exp->count(); ++e) {
                    exp->setCurrentIndex(e);
                    settle(6);
                    const QSize n2 = window->minimumSizeHint().expandedTo(window->minimumSize());
                    MIB_EXPECT(n2.width() <= vp.size.width() && n2.height() <= vp.size.height(),
                               std::string(vp.name) + ": experiment page " + std::to_string(e) + " fits (" +
                                   std::to_string(n2.width()) + "x" + std::to_string(n2.height()) + ")");
                    auto* startExp = window->findChild<QPushButton*>(QStringLiteral("startExperimentBtn"));
                    MIB_EXPECT(insideWindow(startExp, window.get()), std::string(vp.name) + ": Start Experiment reachable");
                }
            }
            const QSize n = window->minimumSizeHint().expandedTo(window->minimumSize());
            MIB_EXPECT(n.width() <= vp.size.width() && n.height() <= vp.size.height(),
                       std::string(vp.name) + ": tab " + std::to_string(t) + " fits (" + std::to_string(n.width()) +
                           "x" + std::to_string(n.height()) + ")");
            MIB_EXPECT(window->size() == settled, std::string(vp.name) + ": tab " + std::to_string(t) + " did not grow the window");
            MIB_EXPECT(insideWindow(startCam, window.get()) && insideWindow(stopCam, window.get()),
                       std::string(vp.name) + ": camera controls reachable on tab " + std::to_string(t));
            MIB_EXPECT(insideWindow(panelBtn, window.get()), std::string(vp.name) + ": hardware panel toggle reachable");
        }
        // Long dynamic strings must not change the required width.
        const QSize before = window->minimumSizeHint();
        auto* status = window->findChild<frontend::ElidingLabel*>(QStringLiteral("statusLabel"));
        MIB_REQUIRE(status, "status label");
        status->setText(QString(2000, QLatin1Char('W')) + QStringLiteral(" | very long status | ") + QString(500, QLatin1Char('x')));
        auto* reviewPath = window->findChild<frontend::ElidingLabel*>(QStringLiteral("reviewFilePathLabel"));
        if (reviewPath) reviewPath->setText(QStringLiteral("/very/long/") + QString(1500, QLatin1Char('p')) + QStringLiteral("/recording.h5"));
        settle(6);
        MIB_EXPECT(window->minimumSizeHint().width() == before.width(), std::string(vp.name) + ": long status/path do not widen the window");
        MIB_EXPECT(status->isElided() || status->width() > 2000, "status text is elided");
        MIB_EXPECT(status->fullText().size() > 2500 && status->toolTip() == status->fullText(), "full status kept in tooltip");
        window->close();
        settle(4);
    }

    // ---- B. sidebar single-owner semantics (1366x768) -------------------------
    {
        wd.mark("sidebar");
        const QSize vp(1366, 728);
        auto window = std::make_unique<MainWindow>(backend);
        window->setAvailableGeometryOverrideForTests(QRect(QPoint(0, 0), vp));
        window->resize(vp);
        window->show();
        settle(10);
        auto* splitter = window->mainSplitter();
        auto* panelBtn = window->findChild<QToolButton*>(QStringLiteral("hardwarePanelBtn"));
        MIB_REQUIRE(splitter && panelBtn && window->hardwarePanelAction(), "splitter/action present");
        MIB_REQUIRE(window->isHardwarePanelVisible(), "panel visible by default");
        const QRect outer = window->geometry();

        // 1. drag -> collapse -> expand restores the dragged width.
        QList<int> sizes = splitter->sizes();
        const int total = sizes[0] + sizes[1];
        sizes[0] = 420;
        sizes[1] = total - 420;
        splitter->setSizes(sizes);
        emit splitter->splitterMoved(420, 1); // programmatic setSizes does not emit; simulate the drag notification
        settle(4);
        MIB_EXPECT(window->hardwarePanelPreferredWidth() == 420, "dragged width recorded as preference");
        window->setHardwarePanelVisible(false);
        settle(4);
        MIB_EXPECT(!window->isHardwarePanelVisible() && !window->sidebar()->isVisible(), "collapsed hides the panel");
        MIB_EXPECT(splitter->sizes()[1] >= total - 10, "workspace received the space");
        MIB_EXPECT(window->geometry() == outer, "collapse did not move/resize the window");
        MIB_EXPECT(panelBtn->isVisible() && insideWindow(panelBtn, window.get()) && !window->hardwarePanelAction()->isChecked(),
                   "reopen control stays visible and unchecked");
        window->setHardwarePanelVisible(true);
        settle(4);
        MIB_EXPECT(window->isHardwarePanelVisible() && std::abs(splitter->sizes()[0] - 420) <= 1, "expand restores the dragged width");
        MIB_EXPECT(window->geometry() == outer, "expand did not move/resize the window");

        // 2. narrow window: expand clamps rather than resizing the outer window.
        window->resize(900, 728);
        settle(8);
        MIB_EXPECT(window->size() == QSize(900, 728), "window accepted the narrow size");
        const int allowed = frontend::geometry::fitSidebarWidth(420, splitter->contentsRect().width(), splitter->handleWidth()).width;
        MIB_EXPECT(window->isHardwarePanelVisible() && splitter->sizes()[0] <= allowed + 1 && splitter->sizes()[0] < 420,
                   "panel clamped to keep the workspace (" + std::to_string(splitter->sizes()[0]) + " <= " + std::to_string(allowed) + ")");
        MIB_EXPECT(window->hardwarePanelPreferredWidth() == 420, "temporary clamp did not overwrite the preference");
        MIB_EXPECT(window->mainTabs()->width() >= frontend::geometry::kWorkspaceMinWidth, "workspace minimum kept");
        window->resize(vp);
        settle(8);
        MIB_EXPECT(std::abs(splitter->sizes()[0] - 420) <= 1, "preference restored when space returns");

        // 3. 50 cycles: no drift, outer geometry equal.
        const QRect outer2 = window->geometry();
        for (int i = 0; i < 50; ++i) {
            window->setHardwarePanelVisible(false);
            settle(2);
            window->setHardwarePanelVisible(true);
            settle(2);
        }
        MIB_EXPECT(std::abs(splitter->sizes()[0] - 420) <= 1, "no cumulative drift after 50 cycles (" + std::to_string(splitter->sizes()[0]) + ")");
        MIB_EXPECT(window->geometry() == outer2, "outer geometry identical after 50 cycles");
        MIB_EXPECT(window->hardwarePanelPreferredWidth() == 420, "preference stable");

        // 4. rapid toggles converge to the last request.
        for (int i = 0; i < 25; ++i) window->hardwarePanelAction()->trigger();
        settle(6);
        MIB_EXPECT(!window->isHardwarePanelVisible() && !window->hardwarePanelAction()->isChecked(), "25 rapid toggles -> hidden");
        window->hardwarePanelAction()->trigger();
        settle(6);
        MIB_EXPECT(window->isHardwarePanelVisible() && window->hardwarePanelAction()->isChecked(), "one more -> visible");

        // 7. reopen control keyboard accessible.
        MIB_EXPECT(panelBtn->focusPolicy() != Qt::NoFocus && !window->hardwarePanelAction()->shortcut().isEmpty(),
                   "reopen control focusable with a shortcut");
        MIB_EXPECT(window->hardwarePanelAction()->text().contains(QStringLiteral("hardware panel")), "accessible text");

        // 8. content reachable when expanded.
        auto* sidebar = window->sidebar();
        MIB_EXPECT(sidebar->statisticsPanel() && sidebar->statisticsPanel()->isVisibleTo(sidebar), "statistics reachable");
        MIB_EXPECT(sidebar->scrollArea() && sidebar->scrollArea()->widget()->height() > 0, "scrollable content");
        MIB_EXPECT(sidebar->minimumWidth() <= frontend::geometry::kSidebarCompactWidth, "remembered width is not a hard minimum");

        // Tab switching while collapsed does not expand.
        window->setHardwarePanelVisible(false);
        settle(2);
        window->mainTabs()->setCurrentIndex(3);
        window->mainTabs()->setCurrentIndex(1);
        settle(4);
        MIB_EXPECT(!window->isHardwarePanelVisible(), "tab change does not expand the panel");
        window->setHardwarePanelVisible(true);
        window->close();
        settle(4);
        // Persisted with schema version.
        QSettings settings;
        MIB_EXPECT(settings.value(QStringLiteral("Sidebar/LayoutVersion")).toInt() == frontend::geometry::kSidebarLayoutVersion &&
                       settings.value(QStringLiteral("Sidebar/PreferredWidth")).toInt() == 420 &&
                       settings.value(QStringLiteral("Sidebar/Visible")).toBool(),
                   "sidebar preference persisted with version");
    }

    // ---- C. oversized / legacy persisted state on a 1366 viewport --------------
    {
        wd.mark("legacy");
        {
            QSettings settings;
            settings.remove(QStringLiteral("Sidebar"));
            settings.setValue(QStringLiteral("Sidebar/Collapsed"), false);
            settings.setValue(QStringLiteral("Sidebar/ExpandedWidth"), 1500);
            settings.setValue(QStringLiteral("Window/LayoutVersion"), frontend::geometry::kWindowLayoutVersion);
            settings.setValue(QStringLiteral("Window/Rect"), QRect(2600, 300, 2400, 1300)); // removed 4K monitor
            settings.sync();
        }
        const QRect available(0, 0, 1366, 728);
        auto window = std::make_unique<MainWindow>(backend);
        window->setAvailableGeometryOverrideForTests(available);
        window->show();
        settle(10);
        MIB_EXPECT(window->restoredWindowGeometryFromSettings(), "saved geometry considered");
        MIB_EXPECT(available.contains(window->frameGeometry()) || available.contains(window->geometry()),
                   "restored from a removed monitor lands on the current desktop (" +
                       std::to_string(window->geometry().x()) + "," + std::to_string(window->geometry().y()) + " " +
                       std::to_string(window->width()) + "x" + std::to_string(window->height()) + ")");
        MIB_EXPECT(window->hardwarePanelPreferredWidth() == frontend::geometry::kSidebarMaxWidth, "legacy width migrated + capped");
        auto* splitter = window->mainSplitter();
        MIB_EXPECT(window->isHardwarePanelVisible() && splitter->sizes()[0] + frontend::geometry::kWorkspaceMinWidth <= window->width(),
                   "oversized preference clamped on 1366 (" + std::to_string(splitter->sizes()[0]) + ")");
        MIB_EXPECT(window->width() <= 1366, "window within viewport");
        window->close();
        settle(4);
        QSettings settings;
        MIB_EXPECT(settings.value(QStringLiteral("Window/LayoutVersion")).toInt() == frontend::geometry::kWindowLayoutVersion &&
                       available.contains(settings.value(QStringLiteral("Window/Rect")).toRect()),
                   "saved geometry is the fitted one");
        // Garbage persisted values -> defaults, no crash.
        settings.setValue(QStringLiteral("Window/Rect"), QStringLiteral("not-a-rect"));
        settings.setValue(QStringLiteral("Sidebar/PreferredWidth"), QStringLiteral("NaN"));
        settings.sync();
        auto window2 = std::make_unique<MainWindow>(backend);
        window2->setAvailableGeometryOverrideForTests(available);
        window2->show();
        settle(6);
        MIB_EXPECT(!window2->restoredWindowGeometryFromSettings() && available.contains(window2->geometry()),
                   "invalid saved geometry -> default inside the desktop");
        MIB_EXPECT(window2->hardwarePanelPreferredWidth() == frontend::geometry::kSidebarDefaultWidth, "invalid width -> default");
        window2->close();
        settle(4);
    }

    backend.shutdown();
    return mib::test::exitCode();
}
