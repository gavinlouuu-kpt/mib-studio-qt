// End-to-end: the real MainWindow runs capture on the mock camera (the public
// 512x96 real-cell stream, asset 512x96stream-mock-frames) and the Experiment
// > Monitoring tab shows live cells. The scatter is then driven as a user
// would: click a point (its cell shows in the frame pane over the histogram),
// let the run go on, step to the next cell, open "Open in window…", and close
// the pane. Every state is saved as a screenshot to MIB_MONITORING_E2E_OUT (or
// the temp dir). Requires the real frames (python scripts/provision-assets.py
// --asset 512x96stream-mock-frames --count 1000); without them the test is
// skipped (exit 77).

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "frontend/core/MainWindow.h"
#include "frontend/dialogs/FrameViewerDialog.h"
#include "frontend/tabs/ExperimentMonitoringTab.h"
#include "frontend/utils/ApplicationSettings.h"
#include "frontend/widgets/ZoomableChartView.h"

#include "support/assert.h"
#include "support/qt_mouse.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineF>
#include <QPushButton>
#include <QScatterSeries>
#include <QSettings>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mouse = mib::test::mouse;

namespace {

using Tab = frontend::ExperimentMonitoringTab;

constexpr int kMockIntervalMs = 5; // 200 fps

void stage(const char* what) {
    std::fprintf(stderr, "stage: %s\n", what);
    std::fflush(stderr);
}

void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool spinUntil(const std::function<bool()>& pred, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!pred() && clock.elapsed() < timeoutMs) spin(50);
    return pred();
}

std::size_t imageCount(const std::filesystem::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (entry.is_regular_file() && (ext == ".tiff" || ext == ".tif" || ext == ".png")) ++n;
    }
    return n;
}

std::filesystem::path realFrames() {
    if (const char* env = std::getenv("MIB_REVIEW_E2E_FRAMES")) return env;
    if (const char* assets = std::getenv("MIB_ASSETS_DIR"))
        return std::filesystem::path(assets) / "datasets" / "512x96stream-mock-frames";
    return std::filesystem::path("build") / "vendor" / "assets" / "datasets" / "512x96stream-mock-frames";
}

// Per-pixel median over up to `maxSamples` frames: the stationary background
// the realtime processing subtracts (same recipe as the Review e2e test).
cv::Mat medianBackground(const std::filesystem::path& dir, std::size_t maxSamples) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        if (entry.is_regular_file() && entry.file_size() > 0) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::vector<cv::Mat> samples;
    const std::size_t stride = std::max<std::size_t>(1, files.size() / std::min(maxSamples, files.size()));
    for (std::size_t i = 0; i < files.size() && samples.size() < maxSamples; i += stride) {
        cv::Mat gray = cv::imread(files[i].string(), cv::IMREAD_GRAYSCALE);
        if (!gray.empty() && (samples.empty() || gray.size() == samples.front().size()))
            samples.push_back(std::move(gray));
    }
    if (samples.empty()) return {};
    cv::Mat median(samples.front().rows, samples.front().cols, CV_8UC1);
    std::vector<uint8_t> values(samples.size());
    for (int y = 0; y < median.rows; ++y) {
        for (int x = 0; x < median.cols; ++x) {
            for (std::size_t k = 0; k < samples.size(); ++k) values[k] = samples[k].ptr<uint8_t>(y)[x];
            auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
            std::nth_element(values.begin(), mid, values.end());
            median.ptr<uint8_t>(y)[x] = *mid;
        }
    }
    return median;
}

struct Shots {
    QString dir;
    int saved = 0;
    void take(QWidget& w, const char* name) {
        const QString file = dir + QLatin1Char('/') + QLatin1String(name) + QStringLiteral(".png");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const QPixmap pm = w.grab();
        if (pm.isNull() || !pm.save(file)) {
            std::fprintf(stderr, "could not save %s\n", qPrintable(file));
            return;
        }
        ++saved;
        std::fprintf(stderr, "shot: %s (%dx%d)\n", qPrintable(file), pm.width(), pm.height());
    }
};

// The viewer's image label has a pixmap once a frame is shown.
bool viewerShowsImage(QWidget* viewer) {
    auto* image = viewer ? viewer->findChild<QLabel*>(QStringLiteral("imageLabel")) : nullptr;
    return image && !image->pixmap().isNull();
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    const std::filesystem::path frames = realFrames();
    if (!std::filesystem::is_directory(frames) || imageCount(frames) < 100) {
        std::fprintf(stderr, "SKIP: real frames not provisioned at %s (python scripts/provision-assets.py "
                             "--asset 512x96stream-mock-frames --count 1000)\n",
                     frames.string().c_str());
        return 77;
    }
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_MOCK_CAMERA_DIR", QByteArray::fromStdString(frames.string()));
    qputenv("MIB_MOCK_CAMERA_INTERVAL_MS", QByteArray::number(kMockIntervalMs));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    mib::test::Watchdog wd(180);
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    stage("QApplication ready");

    mib::test::TempDir td("monitoring_scatter_e2e");
    const cv::Mat background = medianBackground(frames, 64);
    MIB_REQUIRE(!background.empty(), "median background from the real frames");

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((td / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td / "data").string()), "backend init");

    Shots shots{qEnvironmentVariableIsSet("MIB_MONITORING_E2E_OUT")
                    ? qEnvironmentVariable("MIB_MONITORING_E2E_OUT")
                    : QString::fromStdString(td.path().string())};
    QDir().mkpath(shots.dir);

    {
        wd.mark("window");
        MainWindow window(backend);
        window.setAvailableGeometryOverrideForTests(QRect(0, 0, 1600, 1000));
        window.resize(1400, 900);
        window.show();
        spin(500);
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("tabs"));
        MIB_REQUIRE(tabs && tabs->count() >= 4, "main tabs");
        auto* monitoring = qobject_cast<Tab*>(window.experimentTabs()->widget(1));
        MIB_REQUIRE(monitoring != nullptr, "Experiment > Monitoring tab");

        // ---- live capture, Monitoring tab visible --------------------------
        wd.mark("capture");
        auto& processing = backend.processing();
        QMetaObject::invokeMethod(&window, "onStartCapture", Qt::DirectConnection);
        tabs->setCurrentIndex(2);
        window.experimentTabs()->setCurrentIndex(1);
        spin(500);
        {
            // Capture re-applies the configuration file; relax the acceptance
            // gates so every detected cell is plotted, as the Review e2e does.
            auto cfg = processing.getProcessingConfig();
            cfg.enable_border_check = false;
            cfg.enable_area_range_check = false;
            cfg.enable_deformability_range_check = false;
            cfg.enable_area_ratio_check = false;
            cfg.enable_ring_ratio_check = false;
            cfg.require_single_inner_contour = false;
            cfg.auto_background_enabled = false;
            cfg.enable_target_group = false;
            cfg.empty_frame_pixel_threshold = 1;
            cfg.multi_image_enabled = false;
            processing.setProcessingConfig(cfg);
        }
        processing.setRealtimeBackgroundGray(background);
        processing.setRealtimeRoi(backend::services::ProcessingService::Roi{0, 0, background.cols, background.rows});
        MIB_REQUIRE(spinUntil([&] { return monitoring->scatterPointFramesForTests().size() >= 30; }, 20000),
                    "live cells appear on the Monitoring scatter");
        spin(600); // let the scatter settle on the latest buffer
        const std::size_t points = monitoring->scatterPointFramesForTests().size();
        std::fprintf(stderr, "live scatter: %zu cells plotted\n", points);
        MIB_EXPECT(!monitoring->selectedFrameIndex(), "nothing selected before a click");
        MIB_EXPECT(!monitoring->framePaneForTests()->isVisible(), "frame pane hidden before a click");
        shots.take(window, "01-monitoring-live");

        // ---- click a live point ----------------------------------------------
        wd.mark("click");
        // Positions are read before the click: the click can change the layout
        // (the pane appears), so reading them afterwards would measure the wrong
        // place. Dense clusters can put several cells on one pixel; the hit rule
        // then takes the lowest frame index, so any of them is a correct answer.
        const std::size_t k = points / 2;
        const QPointF clicked = monitoring->scatterPointViewPosForTests(k);
        const auto& plotted = monitoring->scatterPointFramesForTests();
        std::vector<uint64_t> underPointer;
        for (std::size_t i = 0; i < plotted.size(); ++i)
            if (QLineF(monitoring->scatterPointViewPosForTests(i), clicked).length() < 2.0)
                underPointer.push_back(plotted[i]);
        MIB_REQUIRE(!underPointer.empty(), "a plotted cell under the pointer");
        mouse::click(monitoring->scatterViewForTests(), clicked);
        spin(300);
        const auto selected = monitoring->selectedFrameIndex();
        MIB_REQUIRE(selected.has_value(), "click selects a cell");
        MIB_EXPECT(std::find(underPointer.begin(), underPointer.end(), *selected) != underPointer.end(),
                   "click selects the cell behind the point");
        const uint64_t expectFrame = *selected;
        MIB_EXPECT(monitoring->selectedFrameIndex() == expectFrame, "selection holds after the click");
        MIB_EXPECT(monitoring->framePaneForTests()->isVisible(), "cell pane shown");
        MIB_EXPECT(viewerShowsImage(monitoring->framePaneViewerForTests()), "cell image shown");
        MIB_EXPECT(monitoring->scatterHighlightForTests()->isVisible(), "selected point highlighted");
        std::fprintf(stderr, "clicked point %zu -> frame %llu: \"%s\"\n", k,
                     static_cast<unsigned long long>(expectFrame),
                     qPrintable(monitoring->framePaneTitleForTests()));
        shots.take(window, "02-monitoring-cell-selected");

        // ---- the run goes on; the selected cell stays -------------------------
        wd.mark("live-update");
        spin(1500);
        MIB_EXPECT(monitoring->selectedFrameIndex() == expectFrame, "selection survives live updates");
        MIB_EXPECT(viewerShowsImage(monitoring->framePaneViewerForTests()), "cell image still shown");
        shots.take(window, "03-monitoring-live-update");

        // ---- prev / next walk the live cells ---------------------------------
        wd.mark("step");
        monitoring->stepScatterSelectionForTests(+1);
        spin(200);
        MIB_EXPECT(monitoring->selectedFrameIndex() && *monitoring->selectedFrameIndex() != expectFrame,
                   "next moves to another cell");
        MIB_EXPECT(viewerShowsImage(monitoring->framePaneViewerForTests()), "next cell image shown");

        // ---- Open in window…: a separate, non-modal viewer --------------------
        wd.mark("window-viewer");
        auto* openBtn = monitoring->framePaneViewerForTests()->findChild<QPushButton*>(QStringLiteral("closeButton"));
        MIB_REQUIRE(openBtn != nullptr, "pane button");
        openBtn->click();
        spin(300);
        auto* viewer = monitoring->frameWindowForTests();
        MIB_REQUIRE(viewer != nullptr && viewer->isVisible(), "cell opened in its own window");
        MIB_EXPECT(!viewer->isModal(), "the window is non-modal (the run keeps going)");
        shots.take(*viewer, "04-monitoring-cell-window");
        monitoring->stepScatterSelectionForTests(+1);
        spin(300);
        MIB_EXPECT(viewerShowsImage(viewer), "the window follows the selection");

        // ---- close the pane: selection, highlight and window go -----------------
        wd.mark("close");
        auto* closeBtn = monitoring->framePaneForTests()->findChild<QToolButton*>(QStringLiteral("monitoringFramePaneClose"));
        MIB_REQUIRE(closeBtn != nullptr, "pane close button");
        closeBtn->click();
        spin(300);
        MIB_EXPECT(!monitoring->selectedFrameIndex(), "close clears the selection");
        MIB_EXPECT(!monitoring->framePaneForTests()->isVisible(), "close hides the pane");
        MIB_EXPECT(!monitoring->scatterHighlightForTests()->isVisible(), "close removes the highlight");
        MIB_EXPECT(!monitoring->frameWindowForTests() || !monitoring->frameWindowForTests()->isVisible(),
                   "close hides the cell window");
        shots.take(window, "05-monitoring-closed");

        QMetaObject::invokeMethod(&window, "onStopCapture", Qt::DirectConnection);
        spin(300);
    }

    backend.shutdown();
    std::fprintf(stderr, "screenshots: %d in %s\n", shots.saved, qPrintable(shots.dir));
    return mib::test::exitCode();
}
