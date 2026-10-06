// End-to-end: the real MainWindow records an experiment on the mock camera
// (the public 512x96 real-cell stream, asset 512x96stream-mock-frames), then
// the Review tab opens that file and the Charts scatter is driven as a user
// would: click a point (cell shows in the docked pane beside the plot), wheel
// zoom, drag pan, Reset zoom, Prev/Next, "Open in window…". A screenshot of
// every state goes to MIB_REVIEW_E2E_OUT (or the temp dir). Requires the real
// frames (python scripts/provision-assets.py --asset 512x96stream-mock-frames
// --count 1000); without them the test is skipped (exit 77). MIB_REVIEW_E2E_KEEP_H5
// copies the recorded run there (input of tools/review_parity/).

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "frontend/core/MainWindow.h"
#include "frontend/tabs/HdfReviewTab.h"
#include "frontend/utils/ApplicationSettings.h"
#include "frontend/widgets/ZoomableChartView.h"

#include "support/assert.h"
#include "support/qt_mouse.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QChart>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QScatterSeries>
#include <QSettings>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTimer>
#include <QValueAxis>

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

constexpr int kMockIntervalMs = 5; // 200 fps
constexpr int kExperimentMs = 8000;

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

std::vector<std::filesystem::path> imageFiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if ((ext == ".tiff" || ext == ".tif" || ext == ".png") && entry.file_size() > 0)
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Per-pixel median over up to `maxSamples` frames (monitoring_kde_e2e recipe).
cv::Mat medianBackground(const std::vector<std::filesystem::path>& files, std::size_t maxSamples) {
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

std::filesystem::path realFrames() {
    if (const char* env = std::getenv("MIB_REVIEW_E2E_FRAMES")) return env;
    if (const char* assets = std::getenv("MIB_ASSETS_DIR"))
        return std::filesystem::path(assets) / "datasets" / "512x96stream-mock-frames";
    return std::filesystem::path("build") / "vendor" / "assets" / "datasets" / "512x96stream-mock-frames";
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

struct Ranges {
    double x0, x1, y0, y1;
};
Ranges ranges(QChart* chart) {
    auto* x = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).first());
    auto* y = qobject_cast<QValueAxis*>(chart->axes(Qt::Vertical).first());
    return {x->min(), x->max(), y->min(), y->max()};
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
#ifdef Q_OS_WIN
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("windows")); // see monitoring_kde_e2e
#endif
    const std::filesystem::path frames = realFrames();
    const auto files = imageFiles(frames);
    if (files.size() < 100) {
        std::fprintf(stderr, "SKIP: real frames not provisioned at %s (python scripts/provision-assets.py "
                             "--asset 512x96stream-mock-frames --count 1000)\n",
                     frames.string().c_str());
        return 77;
    }
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_MOCK_CAMERA_DIR", QByteArray::fromStdString(frames.string()));
    qputenv("MIB_MOCK_CAMERA_INTERVAL_MS", QByteArray::number(kMockIntervalMs));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,yolo"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    mib::test::Watchdog wd(120);
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    stage("QApplication ready");

    mib::test::TempDir td("review_scatter_e2e");
    const cv::Mat background = medianBackground(files, 64);
    MIB_REQUIRE(!background.empty(), "median background from the real frames");

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString((td / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td / "data").string()), "backend init");

    Shots shots{qEnvironmentVariableIsSet("MIB_REVIEW_E2E_OUT") ? qEnvironmentVariable("MIB_REVIEW_E2E_OUT")
                                                                 : QString::fromStdString(td.path().string())};
    QDir().mkpath(shots.dir);

    // ---- the real app, recording a real experiment -------------------------
    wd.mark("window");
    MainWindow window(backend);
    window.setAvailableGeometryOverrideForTests(QRect(0, 0, 1600, 1000));
    window.resize(1400, 900);
    window.show();
    spin(500);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("tabs"));
    MIB_REQUIRE(tabs && tabs->count() >= 4, "main tabs");

    wd.mark("capture");
    auto& processing = backend.processing();
    QMetaObject::invokeMethod(&window, "onStartCapture", Qt::DirectConnection);
    tabs->setCurrentIndex(2);
    if (auto* exp = window.experimentTabs()) exp->setCurrentIndex(1);
    spin(1000);
    {
        // Starting capture re-applies the configuration file; relax it afterwards
        // (every detected object is a valid cell) and process the whole frame.
        auto cfg = processing.getProcessingConfig();
        cfg.enable_border_check = false;
        cfg.enable_area_range_check = false;
        cfg.enable_deformability_range_check = false;
        cfg.enable_area_ratio_check = false;
        cfg.enable_ring_ratio_check = false;
        cfg.require_single_inner_contour = false;
        cfg.auto_background_enabled = false;
        cfg.enable_target_group = false; // the trigger service is disabled
        cfg.empty_frame_pixel_threshold = 1;
        cfg.multi_image_enabled = false;
        processing.setProcessingConfig(cfg);
    }
    processing.setRealtimeBackgroundGray(background);
    processing.setRealtimeRoi(backend::services::ProcessingService::Roi{0, 0, background.cols, background.rows});
    MIB_REQUIRE(spinUntil([&] { return processing.getMonitoringValidAppended() > 20; }, 10000),
                "real cells reach the monitoring ring");

    wd.mark("experiment");
    auto& coordinator = backend.experiment();
    const std::string expPath = (td / "review_e2e_run.h5").string();
    const auto readiness = coordinator.evaluateReadiness(expPath);
    if (!readiness.ready)
        for (const auto& g : readiness.gates)
            std::fprintf(stderr, "  gate %s %s %s\n", g.id.c_str(), backend::app::toString(g.status), g.reason.c_str());
    MIB_REQUIRE(readiness.ready, "experiment readiness");
    backend::app::ExperimentStartRequest request;
    request.outputPath = expPath;
    request.readinessGeneration = readiness.generation;
    request.acknowledgeLatestFrameDrops = true;
    const auto started = coordinator.start(request);
    MIB_REQUIRE(started.started(), "experiment start: " + started.message);
    spin(kExperimentMs / 2);
    shots.take(window, "01-experiment-running");
    spin(kExperimentMs / 2);
    MIB_REQUIRE(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted, "stop accepted");
    MIB_REQUIRE(spinUntil([&] {
                    const auto st = coordinator.status();
                    return st.terminal && st.state == backend::app::ExperimentRunState::Idle;
                }, 30000),
                "experiment finalized");
    const std::string written = coordinator.status().outputPath.empty() ? expPath : coordinator.status().outputPath;
    QMetaObject::invokeMethod(&window, "onStopCapture", Qt::DirectConnection);
    spin(500);
    {
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(written), "experiment file readable");
        std::vector<backend::services::ProcessedFrame> valid;
        reader.readValidMetadata(valid);
        std::fprintf(stderr, "experiment file: %s, %zu valid frames\n", written.c_str(), valid.size());
        MIB_REQUIRE(valid.size() >= 50, "the run recorded a population of real cells");
        reader.closeFile();
    }

    // ---- Review: open the file, Charts tab -----------------------------------
    wd.mark("review");
    tabs->setCurrentIndex(3);
    auto* review = window.findChild<frontend::HdfReviewTab*>();
    MIB_REQUIRE(review, "review tab");
    // Parity sign-off input (tools/review_parity/run.sh): keep the recorded run.
    if (const char* keep = std::getenv("MIB_REVIEW_E2E_KEEP_H5")) {
        std::error_code ec;
        std::filesystem::copy_file(written, keep, std::filesystem::copy_options::overwrite_existing, ec);
        MIB_EXPECT(!ec, "kept the recorded run at MIB_REVIEW_E2E_KEEP_H5: " + ec.message());
    }
    review->loadHdfFileForTests(QString::fromStdString(written));
    auto* frameTypeTabs = review->findChild<QTabWidget*>(QStringLiteral("frameTypeTabs"));
    MIB_REQUIRE(frameTypeTabs, "frame type tabs");
    spin(500);
    shots.take(window, "02-review-valid-frames");
    frameTypeTabs->setCurrentIndex(2);
    spin(800);
    auto* view = review->scatterViewForTests();
    auto* chart = view->chart();
    const auto& pointToFrame = review->scatterPointToFrameForTests();
    MIB_REQUIRE(view && chart && pointToFrame.size() >= 50, "scatter populated");
    std::fprintf(stderr, "scatter: %zu points\n", pointToFrame.size());
    MIB_EXPECT(!review->framePaneFrameForTests(), "pane empty before any click");
    shots.take(window, "03-review-charts");

    // ---- click the point nearest the plot centre -----------------------------
    wd.mark("click");
    const QPointF centre = view->mapFromScene(chart->mapToScene(chart->plotArea().center()));
    int k = 0;
    double best = 1e18;
    for (int i = 0; i < static_cast<int>(pointToFrame.size()); ++i) {
        const QPointF p = review->scatterPointViewPosForTests(i);
        const double d = QLineF(p, centre).length();
        if (d < best) {
            best = d;
            k = i;
        }
    }
    const Ranges home = ranges(chart);
    mouse::move(view, review->scatterPointViewPosForTests(k)); // hover: pointing hand
    mouse::click(view, review->scatterPointViewPosForTests(k));
    spin(600);
    const auto pane = review->framePaneFrameForTests();
    MIB_REQUIRE(pane && pane->second, "pane shows a valid cell after the click");
    MIB_EXPECT(pane->first == pointToFrame[static_cast<size_t>(k)], "pane shows the clicked point's frame");
    MIB_EXPECT(review->scatterHighlightForTests()->isVisible(), "clicked point highlighted");
    auto* paneImage = review->framePaneForTests()->findChild<QLabel*>(QStringLiteral("imageLabel"));
    MIB_REQUIRE(paneImage, "pane image label");
    MIB_EXPECT(!paneImage->pixmap().isNull() && paneImage->pixmap().width() >= 512,
               "the pane shows the real 512x96 cell image");
    MIB_EXPECT(view->cursor().shape() == Qt::PointingHandCursor, "pointing hand over the point");
    shots.take(window, "04-click-point");

    // ---- zoom at the point, click again at the new scale ---------------------
    wd.mark("zoom");
    for (int i = 0; i < 4; ++i) mouse::wheel(view, review->scatterPointViewPosForTests(k), 240);
    spin(400);
    const Ranges zoomed = ranges(chart);
    MIB_EXPECT(zoomed.x1 - zoomed.x0 < 0.5 * (home.x1 - home.x0), "wheel zoomed in");
    mouse::click(view, review->scatterPointViewPosForTests(k));
    spin(300);
    MIB_EXPECT(review->selectedFrameForTests() == pointToFrame[static_cast<size_t>(k)], "click at the zoomed scale");
    shots.take(window, "05-zoomed");

    // ---- drag pans, never selects --------------------------------------------
    wd.mark("pan");
    const int selectedBefore = review->selectedFrameForTests();
    mouse::drag(view, centre, centre + QPointF(160, 60), 8);
    spin(400);
    MIB_EXPECT(review->selectedFrameForTests() == selectedBefore, "drag kept the selection");
    const Ranges panned = ranges(chart);
    MIB_EXPECT(panned.x0 < zoomed.x0 && panned.y0 > zoomed.y0, "drag right/down panned the view");
    shots.take(window, "06-panned");
    view->resetZoomAction()->trigger();
    spin(300);
    const Ranges reset = ranges(chart);
    MIB_EXPECT(std::abs(reset.x0 - home.x0) < 1e-6 && std::abs(reset.y1 - home.y1) < 1e-9, "Reset zoom → data extent");

    // ---- Prev/Next through the valid set --------------------------------------
    wd.mark("prev-next");
    auto* nextBtn = review->framePaneForTests()->findChild<QPushButton*>(QStringLiteral("nextButton"));
    auto* prevBtn = review->framePaneForTests()->findChild<QPushButton*>(QStringLiteral("prevButton"));
    MIB_REQUIRE(nextBtn && prevBtn, "pane prev/next");
    const int from = review->selectedFrameForTests();
    nextBtn->click();
    nextBtn->click();
    spin(300);
    MIB_EXPECT(review->selectedFrameForTests() == from + 2, "two Next steps");
    MIB_EXPECT(review->framePaneFrameForTests() && review->framePaneFrameForTests()->first == review->selectedFrameForTests(),
               "pane follows Next");
    shots.take(window, "07-next-next");
    prevBtn->click();
    spin(300);
    MIB_EXPECT(review->selectedFrameForTests() == from + 1, "Prev steps back");

    // ---- "Open in window…": the modal viewer on the same cell -----------------
    wd.mark("open-in-window");
    auto* openBtn = review->framePaneForTests()->findChild<QPushButton*>(QStringLiteral("closeButton"));
    MIB_REQUIRE(openBtn && openBtn->text().startsWith(QStringLiteral("Open in window")), "Open in window button");
    bool modalSeen = false;
    QTimer::singleShot(900, [&]() {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal) return;
        modalSeen = true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        modal->grab().save(shots.dir + QStringLiteral("/08-open-in-window.png"));
        ++shots.saved;
        modal->close();
    });
    openBtn->click(); // exec() blocks until the timer closes the dialog
    spin(200);
    MIB_EXPECT(modalSeen, "the modal viewer opened from the pane");
    MIB_EXPECT(review->framePaneFrameForTests().has_value(), "pane still shows the cell after the window closed");

    // ---- Export Charts snapshot: full extent, no highlight -------------------
    wd.mark("export");
    for (int i = 0; i < 3; ++i) mouse::wheel(view, review->scatterPointViewPosForTests(k), 240);
    spin(200);
    const Ranges userView = ranges(chart);
    auto snaps = review->renderChartSnapshotsForTests();
    spin(200);
    MIB_EXPECT(snaps.count("scatter_plot.tiff") && !snaps["scatter_plot.tiff"].empty(), "scatter snapshot rendered");
    if (!snaps["scatter_plot.tiff"].empty()) {
        const cv::Mat& bgr = snaps["scatter_plot.tiff"];
        cv::imwrite((shots.dir + QStringLiteral("/09-export-scatter-snapshot.png")).toStdString(), bgr);
        // The snapshot's points keep the on-screen series colour (0x209fdf,
        // BGR in the Mat). Before the BGRA fix in renderChartSnapshots the
        // channels were swapped and the same points were 0xdf9f20 orange;
        // the selection highlight (0xf28e2b) is never in a snapshot.
        int blue = 0, swapped = 0, highlight = 0;
        for (int r = 0; r < bgr.rows; ++r) {
            const auto* row = bgr.ptr<cv::Vec3b>(r);
            for (int c = 0; c < bgr.cols; ++c) {
                const auto near = [&](int b, int g, int rr) {
                    return std::abs(row[c][0] - b) <= 8 && std::abs(row[c][1] - g) <= 8 && std::abs(row[c][2] - rr) <= 8;
                };
                blue += near(0xdf, 0x9f, 0x20);
                swapped += near(0x20, 0x9f, 0xdf);
                highlight += near(0x2b, 0x8e, 0xf2);
            }
        }
        std::fprintf(stderr, "snapshot pixels: blue %d, channel-swapped %d, highlight %d\n", blue, swapped, highlight);
        MIB_EXPECT(blue > 200, "snapshot points keep the on-screen series colour");
        MIB_EXPECT(swapped == 0, "snapshot channels are not swapped (BGRA, not RGBA)");
        MIB_EXPECT(highlight == 0, "snapshot carries no selection highlight");
    }
    const Ranges after = ranges(chart);
    MIB_EXPECT(std::abs(after.x0 - userView.x0) < 1e-6 && std::abs(after.x1 - userView.x1) < 1e-6,
               "the user's zoom survives the export");
    MIB_EXPECT(review->scatterHighlightForTests()->isVisible(), "the highlight survives the export");
    shots.take(window, "10-after-export");

    std::fprintf(stderr, "screenshots: %d in %s\n", shots.saved, qPrintable(shots.dir));
    tabs->setCurrentIndex(0);
    spin(200);
    return mib::test::exitCode();
}
