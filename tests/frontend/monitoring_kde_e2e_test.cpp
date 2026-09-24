// monitoring_kde_e2e_test
//
// End-to-end: the real MainWindow on the real mock-camera pipeline (capture
// thread -> FrameStore -> realtime processing -> monitoring rings -> Monitoring
// tab) with the scatter density (KDE) colouring off, on, and off again, to
// show that the periodic estimate does not degrade the other services.
//
// Frames come from the real public stream `gavinlouuu/512x96stream` (asset
// 512x96stream-mock-frames in env/assets.json; provision with
// `python scripts/provision-assets.py --asset 512x96stream-mock-frames
// --count 1000`, or point MIB_KDE_E2E_FRAMES at a folder) with a per-pixel
// median background; when the asset is absent the test writes a synthetic set
// (dark ellipses of varying size and eccentricity on a noisy grey background).
// The shipped data/mock_frames sample yields no valid cells. Acceptance
// criteria are relaxed so every detected object is a valid, plotted cell.
//
// Windows: uses the native platform (the offscreen one deadlocks in the
// QApplication constructor), so the window is briefly visible on a desktop.
//
// Per phase it samples, from the GUI thread, the capture write-index advance,
// processing algo/valid FPS, monitoring-ring appends, the realtime overlay
// lag (write head minus processed snapshot index) and the GUI event-loop
// responsiveness (gaps of a 5 ms precise timer). Gates are ratios between
// the KDE-on phase and the baseline (machine-independent); absolute numbers
// are printed for the record. Screenshots of the tab in both states are
// written to MIB_KDE_E2E_OUT (or the temp dir) for a visual check.

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/recording/Hdf5Service.h"
#include "frontend/tabs/KdeCoreRecord.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "frontend/core/MainWindow.h"
#include "frontend/tabs/ExperimentMonitoringTab.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/stats.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPixmap>
#include <QScatterSeries>
#include <QSettings>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTimer>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr int kFrameW = 512;
constexpr int kFrameH = 96;
constexpr int kFrameCount = 120;
constexpr int kMockIntervalMs = 5; // 200 fps: keeps capture + processing busy
constexpr int kWarmupMs = 4000;
constexpr int kPhaseMs = 8000;
constexpr int kRecoveryMs = 4000;

struct PhaseMetrics {
    std::string name;
    double seconds{0.0};
    uint64_t captured{0};           // FrameStore write-index advance
    uint64_t monitoringAppended{0}; // valid frames appended to the monitoring ring
    uint64_t jobsProcessed{0};
    std::vector<double> algoFps;
    std::vector<double> validFps;
    std::vector<double> lag;
    std::vector<double> guiGapMs;
    uint64_t kdeGenerations{0};
    int scatterPoints{0};
    int lastKdeMs{0};
    std::size_t lastKdePoints{0};
};

double mean(const std::vector<double>& v) {
    return v.empty() ? 0.0 : mib::test::summarize(v).mean;
}

double lateMean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    const std::size_t tail = std::max<std::size_t>(1, v.size() / 3);
    std::vector<double> late(v.end() - static_cast<std::ptrdiff_t>(tail), v.end());
    return mean(late);
}

void writeSyntheticFrames(const std::filesystem::path& dir, cv::Mat& backgroundOut) {
    std::filesystem::create_directories(dir);
    std::mt19937 rng(20260923);
    std::normal_distribution<double> noise(0.0, 2.5);
    auto noisyBackground = [&] {
        cv::Mat bg(kFrameH, kFrameW, CV_8UC1);
        for (int y = 0; y < kFrameH; ++y)
            for (int x = 0; x < kFrameW; ++x)
                bg.at<uchar>(y, x) = static_cast<uchar>(std::clamp(140.0 + noise(rng), 0.0, 255.0));
        return bg;
    };
    backgroundOut = noisyBackground();
    std::uniform_int_distribution<int> cells(1, 3);
    std::uniform_real_distribution<double> radius(8.0, 16.0);
    std::uniform_real_distribution<double> eccentricity(0.0, 0.45);
    std::uniform_real_distribution<double> angle(0.0, 180.0);
    std::uniform_int_distribution<int> cx(40, kFrameW - 40);
    std::uniform_int_distribution<int> cy(28, kFrameH - 28);
    for (int i = 0; i < kFrameCount; ++i) {
        cv::Mat frame = noisyBackground();
        const int n = cells(rng);
        for (int k = 0; k < n; ++k) {
            const double a = radius(rng);
            const double b = a * (1.0 - eccentricity(rng));
            cv::ellipse(frame, cv::Point(cx(rng), cy(rng)),
                        cv::Size(static_cast<int>(a), static_cast<int>(b)), angle(rng), 0.0, 360.0,
                        cv::Scalar(70), -1, cv::LINE_AA);
        }
        char name[64];
        std::snprintf(name, sizeof(name), "frame_%05d.png", i);
        cv::imwrite((dir / name).string(), frame);
    }
}

// Per-pixel median over up to `maxSamples` frames evenly sampled from the
// folder (same recipe as tests/tools/mock_pipeline_timing_run.cpp): a clean
// static background even though some frames contain cells.
cv::Mat medianBackground(const std::vector<std::filesystem::path>& files, std::size_t maxSamples) {
    std::vector<cv::Mat> samples;
    const std::size_t stride =
        std::max<std::size_t>(1, files.size() / std::min(maxSamples, files.size()));
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
            for (std::size_t k = 0; k < samples.size(); ++k)
                values[k] = samples[k].ptr<uint8_t>(y)[x];
            auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
            std::nth_element(values.begin(), mid, values.end());
            median.ptr<uint8_t>(y)[x] = *mid;
        }
    }
    return median;
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

// The real 512x96 stream (asset 512x96stream-mock-frames, env/assets.json)
// when it is provisioned or named through MIB_KDE_E2E_FRAMES; otherwise the
// synthetic set. Returns the folder and the background to use.
std::filesystem::path resolveFrames(const std::filesystem::path& scratch, cv::Mat& background,
                                    std::string& source) {
    std::filesystem::path real;
    if (const char* env = std::getenv("MIB_KDE_E2E_FRAMES"))
        real = env;
    else if (const char* assets = std::getenv("MIB_ASSETS_DIR"))
        real = std::filesystem::path(assets) / "datasets" / "512x96stream-mock-frames";
    else
        real = std::filesystem::path("build") / "vendor" / "assets" / "datasets" /
               "512x96stream-mock-frames";
    const auto files = imageFiles(real);
    if (files.size() >= 100) {
        background = medianBackground(files, 64);
        if (!background.empty()) {
            source =
                "real stream " + real.string() + " (" + std::to_string(files.size()) + " frames)";
            return real;
        }
    }
    const std::filesystem::path synthetic = scratch / "frames";
    writeSyntheticFrames(synthetic, background);
    source = "synthetic frames";
    return synthetic;
}

void stage(const char* what) {
    std::fprintf(stderr, "stage: %s\n", what);
    std::fflush(stderr);
}

void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
#ifdef Q_OS_WIN
    // The Conan Qt 6.7.3 offscreen platform intermittently deadlocks inside
    // the QApplication constructor on Windows (see
    // processing_core_dialog_test); the native platform boots headlessly on
    // runners and merely shows the window on a desktop session.
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("windows"));
#endif
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_MOCK_CAMERA_INTERVAL_MS", QByteArray::number(kMockIntervalMs));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,yolo"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL",
            QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL",
            QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    mib::test::Watchdog wd(90);
    QApplication app(argc, argv);
    stage("QApplication ready");
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    mib::test::TempDir td("monitoring_kde_e2e");
    cv::Mat background;
    std::string frameSource;
    const std::filesystem::path framesDir = resolveFrames(td.path(), background, frameSource);
    qputenv("MIB_MOCK_CAMERA_DIR", QByteArray::fromStdString(framesDir.string()));
    std::printf("frames: %s; background %dx%d\n", frameSource.c_str(), background.cols,
                background.rows);
    stage("frames ready");

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((td / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td / "data").string()), "backend init");
    stage("backend initialized");

    const QString outDir = qEnvironmentVariableIsSet("MIB_KDE_E2E_OUT")
                               ? qEnvironmentVariable("MIB_KDE_E2E_OUT")
                               : QString::fromStdString(td.path().string());
    QDir().mkpath(outDir);

    wd.mark("window");
    MainWindow window(backend);
    window.setAvailableGeometryOverrideForTests(QRect(0, 0, 1480, 1000));
    window.resize(1280, 800);
    window.show();
    spin(500);

    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("tabs"));
    MIB_REQUIRE(tabs && tabs->count() >= 3, "main tabs");
    auto* experimentTabs = window.experimentTabs();
    MIB_REQUIRE(experimentTabs && experimentTabs->count() >= 2, "experiment tabs");
    auto* tab = qobject_cast<frontend::ExperimentMonitoringTab*>(experimentTabs->widget(1));
    MIB_REQUIRE(tab, "monitoring tab");
    tab->setKdeEnabled(false);

    wd.mark("capture");
    auto& processing = backend.processing();
    QMetaObject::invokeMethod(&window, "onStartCapture", Qt::DirectConnection);
    tabs->setCurrentIndex(2);
    experimentTabs->setCurrentIndex(1);
    spin(1000);
    // Starting capture re-applies the configuration file; relax it afterwards.
    // Every detected object is a valid cell: the point of this test is the
    // chart load, not the acceptance criteria.
    {
        auto cfg = processing.getProcessingConfig();
        cfg.enable_border_check = false;
        cfg.enable_area_range_check = false;
        cfg.enable_deformability_range_check = false;
        cfg.enable_area_ratio_check = false;
        cfg.enable_ring_ratio_check = false;
        cfg.require_single_inner_contour = false;
        cfg.auto_background_enabled = false;
        cfg.enable_target_group = true; // exercise both series
        cfg.target_group_area_min = 0;
        cfg.target_group_area_max = 120;
        cfg.target_group_deformability_min = 0.0;
        cfg.target_group_deformability_max = 1.0;
        cfg.empty_frame_pixel_threshold = 1;
        cfg.multi_image_enabled = false;
        processing.setProcessingConfig(cfg);
    }
    processing.setRealtimeBackgroundGray(background);
    // The configured ROI targets a full-size sensor and falls outside these
    // 512x96 frames (the screenshot tour moves it to the origin for the same
    // reason); process the whole frame.
    processing.setRealtimeRoi(
        backend::services::ProcessingService::Roi{0, 0, background.cols, background.rows});

    spin(kWarmupMs);
    {
        const auto cfg = processing.getProcessingConfig();
        const auto roi = processing.getRealtimeRoi();
        std::printf(
            "warm-up: head=%llu algoFps=%.1f validFps=%.1f invalidFps=%.1f monitoringValid=%llu "
            "monitoringInvalid=%llu "
            "roi=%d,%d,%d,%d border=%d area=%d ring=%d inner=%d bg=%dx%d\n",
            static_cast<unsigned long long>(backend.getFrameStore()->latestAvailableIndex()),
            processing.getAlgoFps1s(), processing.getValidFps1s(), processing.getInvalidFps1s(),
            static_cast<unsigned long long>(processing.getMonitoringValidAppended()),
            static_cast<unsigned long long>(processing.getMonitoringInvalidAppended()), roi.x,
            roi.y, roi.w, roi.h, cfg.enable_border_check, cfg.enable_area_range_check,
            cfg.enable_ring_ratio_check, cfg.require_single_inner_contour,
            processing.getRealtimeBackgroundGray().cols,
            processing.getRealtimeBackgroundGray().rows);
        std::fflush(stdout);
    }
    MIB_REQUIRE(backend.getFrameStore() && backend.getFrameStore()->latestAvailableIndex() > 50,
                "mock camera streams");
    MIB_REQUIRE(processing.getMonitoringValidAppended() > 0, "cells reach the monitoring ring");

    // ---- instrumentation ------------------------------------------------------
    PhaseMetrics* current = nullptr;
    QElapsedTimer guiClock;
    guiClock.start();
    qint64 lastTickNs = guiClock.nsecsElapsed();
    QTimer guiTick;
    guiTick.setTimerType(Qt::PreciseTimer);
    guiTick.setInterval(5);
    QObject::connect(&guiTick, &QTimer::timeout, [&] {
        const qint64 now = guiClock.nsecsElapsed();
        if (current) current->guiGapMs.push_back((now - lastTickNs) / 1e6);
        lastTickNs = now;
    });
    QTimer sampler;
    sampler.setInterval(100);
    QObject::connect(&sampler, &QTimer::timeout, [&] {
        if (!current) return;
        current->algoFps.push_back(processing.getAlgoFps1s());
        current->validFps.push_back(processing.getValidFps1s());
        backend::services::ProcessingService::RealtimeSnapshot snap;
        if (processing.getLatestSnapshot(snap)) {
            const uint64_t head = backend.getFrameStore()->latestAvailableIndex();
            current->lag.push_back(static_cast<double>(head > snap.index ? head - snap.index : 0));
        }
    });
    guiTick.start();
    sampler.start();

    auto runPhase = [&](const char* name, int ms) {
        PhaseMetrics m;
        m.name = name;
        const uint64_t head0 = backend.getFrameStore()->latestAvailableIndex();
        const uint64_t app0 = processing.getMonitoringValidAppended();
        const uint64_t jobs0 = processing.stats().jobsProcessed.load();
        const uint64_t gen0 = tab->kdeGeneration();
        QElapsedTimer clock;
        clock.start();
        current = &m;
        lastTickNs = guiClock.nsecsElapsed();
        spin(ms);
        current = nullptr;
        m.seconds = clock.elapsed() / 1000.0;
        m.captured = backend.getFrameStore()->latestAvailableIndex() - head0;
        m.monitoringAppended = processing.getMonitoringValidAppended() - app0;
        m.jobsProcessed = processing.stats().jobsProcessed.load() - jobs0;
        m.kdeGenerations = tab->kdeGeneration() - gen0;
        m.scatterPoints =
            tab->scatterSeriesForTests()->count() + tab->targetGroupSeriesForTests()->count();
        for (const auto* s : tab->kdeLevelSeriesForTests())
            m.scatterPoints += s->count();
        for (const auto* s : tab->kdeTargetLevelSeriesForTests())
            m.scatterPoints += s->count();
        m.lastKdeMs = tab->lastKdeComputeMs();
        m.lastKdePoints = tab->lastKdePointCount();
        return m;
    };
    auto report = [](const PhaseMetrics& m) {
        const auto gaps = mib::test::summarize(m.guiGapMs);
        std::printf("%-10s %5.1fs captured=%4llu (%.0f fps) monitoring+=%4llu jobs=%5llu "
                    "algoFps=%6.1f validFps=%6.1f "
                    "lag(late)=%5.1f gui5ms p50=%5.2f p99=%6.2f max=%7.2f ms points=%4d kde=%llu "
                    "(last %zu pts, %d ms)\n",
                    m.name.c_str(), m.seconds, static_cast<unsigned long long>(m.captured),
                    m.captured / m.seconds, static_cast<unsigned long long>(m.monitoringAppended),
                    static_cast<unsigned long long>(m.jobsProcessed), mean(m.algoFps),
                    mean(m.validFps), lateMean(m.lag), gaps.p50, gaps.p99, gaps.max,
                    m.scatterPoints, static_cast<unsigned long long>(m.kdeGenerations),
                    m.lastKdePoints, m.lastKdeMs);
    };
    auto snapshot = [&](const char* name) {
        const QString file =
            QDir(outDir).filePath(QString::fromLatin1(name) + QStringLiteral(".png"));
        if (!tab->grab().save(file))
            std::fprintf(stderr, "could not save %s\n", qPrintable(file));
        else
            std::printf("screenshot: %s\n", qPrintable(file));
    };

    // ---- phases ---------------------------------------------------------------
    wd.mark("baseline");
    const PhaseMetrics baseline = runPhase("kde-off", kPhaseMs);
    report(baseline);
    snapshot("monitoring-kde-off");

    wd.mark("kde-on");
    tab->setKdeIntervalMs(500); // 4x the default cadence: harsher than any real setting
    tab->setKdeEnabled(true);
    const PhaseMetrics kdeOn = runPhase("kde-on", kPhaseMs);
    report(kdeOn);
    snapshot("monitoring-kde-on");
    std::printf("kde tooltip: %s\n", qPrintable(tab->kdeToggle()->toolTip()));

    // ---- a real experiment while KDE is on: the file keeps the live contour ---
    // Tab -> ExperimentCoordinator::setLiveKdeCoreRecord after every estimate
    // -> Hdf5Service::writeKdeLiveJson during finalization.
    wd.mark("experiment");
    {
        auto& coordinator = backend.experiment();
        const std::string expPath = (td / "kde_run.h5").string();
        {
            // The harness disables the trigger service, so a run with sorting
            // enabled is refused by the trigger.output gate; sorting is not
            // what this step checks.
            auto cfg = processing.getProcessingConfig();
            cfg.enable_target_group = false;
            processing.setProcessingConfig(cfg);
        }
        const auto readiness = coordinator.evaluateReadiness(expPath);
        if (!readiness.ready) {
            for (const auto& g : readiness.gates)
                std::fprintf(stderr, "  gate %s %s %s\n", g.id.c_str(), backend::app::toString(g.status), g.reason.c_str());
        }
        MIB_REQUIRE(readiness.ready, "experiment readiness with the mock camera running");
        backend::app::ExperimentStartRequest request;
        request.outputPath = expPath;
        request.readinessGeneration = readiness.generation;
        request.acknowledgeLatestFrameDrops = true;
        const auto started = coordinator.start(request);
        MIB_REQUIRE(started.started(), "experiment start: " + started.message);
        auto spinUntil = [&](const std::function<bool()>& pred, int timeoutMs) {
            QElapsedTimer clock;
            clock.start();
            while (!pred() && clock.elapsed() < timeoutMs) spin(50);
            return pred();
        };
        const uint64_t gen0 = tab->kdeGeneration();
        MIB_REQUIRE(spinUntil([&] { return tab->kdeGeneration() >= gen0 + 2; }, 10000),
                    "estimates land while the experiment runs");
        MIB_REQUIRE(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted, "experiment stop accepted");
        MIB_REQUIRE(spinUntil([&] {
                        const auto st = coordinator.status();
                        return st.terminal && st.state == backend::app::ExperimentRunState::Idle;
                    }, 20000),
                    "experiment finalized");
        const std::string written = coordinator.status().outputPath;
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(written.empty() ? expPath : written), "experiment file readable");
        std::string json;
        MIB_EXPECT(reader.readKdeLiveJson(json), "finalized file carries the live KDE core record");
        std::string analysis;
        MIB_EXPECT(!reader.readKdeAnalysisJson(analysis), "no full-run record is written automatically");
        reader.closeFile();
        std::string why;
        const auto record = frontend::monitoring::fromJson(json, &why);
        MIB_EXPECT(record.has_value(), "stored record parses: " + why);
        if (record) {
            std::printf("stored live record: fraction %.2f, %llu of %llu cells, %zu loop(s)\n", record->coreFraction,
                        static_cast<unsigned long long>(record->cellCount),
                        static_cast<unsigned long long>(record->populationCount), record->contours.size());
            MIB_EXPECT(record->provisional && record->source == "live-buffer", "stored record is provisional");
            MIB_EXPECT(!record->contours.empty() && record->populationCount >= 200
                           && record->cellCount >= 0.85 * record->coreFraction * record->populationCount,
                       "stored record holds the on-screen core contour of a real population");
        }
    }

    wd.mark("recovery");
    tab->setKdeEnabled(false);
    const PhaseMetrics recovery = runPhase("kde-off-2", kRecoveryMs);
    report(recovery);

    guiTick.stop();
    sampler.stop();

    // ---- gates (ratios against the baseline) ----------------------------------
    MIB_EXPECT(kdeOn.scatterPoints >= 200, "the scatter carries a real population while KDE is on");
    MIB_EXPECT(kdeOn.kdeGenerations >= 6, "estimates keep landing at the 500 ms cadence");
    MIB_EXPECT(kdeOn.lastKdePoints >= 200 && kdeOn.lastKdeMs < 250,
               "estimate covers the buffer and stays ms-scale");
    MIB_EXPECT(kdeOn.captured >= 0.9 * baseline.captured,
               "capture throughput unaffected (>= 90% of baseline)");
    MIB_EXPECT(mean(kdeOn.algoFps) >= 0.85 * mean(baseline.algoFps),
               "processing algo FPS unaffected (>= 85% of baseline)");
    MIB_EXPECT(kdeOn.monitoringAppended >= 0.8 * baseline.monitoringAppended,
               "monitoring ring keeps filling (>= 80% of baseline)");
    MIB_EXPECT(lateMean(kdeOn.lag) <= std::max(2.0 * lateMean(baseline.lag), 3.0),
               "realtime overlay lag stays bounded");
    {
        const auto base = mib::test::summarize(baseline.guiGapMs);
        const auto on = mib::test::summarize(kdeOn.guiGapMs);
        MIB_EXPECT(on.p99 <= std::max(3.0 * base.p99, 50.0),
                   "GUI loop p99 tick gap within 3x baseline (or 50 ms)");
        MIB_EXPECT(on.max <= std::max(3.0 * base.max, 150.0),
                   "GUI loop worst stall within 3x baseline (or 150 ms)");
    }
    MIB_EXPECT(recovery.scatterPoints > 0 && recovery.kdeGenerations == 0,
               "switching off stops the estimates, chart keeps running");

    wd.mark("shutdown");
    QMetaObject::invokeMethod(&window, "onStopCapture", Qt::DirectConnection);
    spin(300);
    window.close();
    spin(200);
    backend.shutdown();
    return mib::test::exitCode();
}
