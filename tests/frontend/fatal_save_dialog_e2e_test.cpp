// End-to-end (issue #828): the real MainWindow runs an experiment on the mock
// camera under the rig's load: 5000 fps of 1216x208 frames (the public 512x96
// real-cell stream, centred on a flat background-coloured border), every
// detected cell accepted and the default 512 MB experiment buffer. Each HDF5
// append is delayed 1.5 s by default, the latency the rig's D: HDD showed for
// a ~530-frame batch (1.7-2.0 s), so the overflow does not depend on the CI
// disk. With _SLOW_MS=0 and _DIR on that HDD the overflow is fully natural
// (2026-11-03: about 16 s in). The buffer fills and every worker refuses frames. Before the fix
// each refusal opened a modal "Save Error" box whose nested event loop opened the next one until
// the GUI thread's stack overflowed. The run must instead fail cleanly with exactly one Save Error
// box, shown only after the run is terminal (file closed).
//
// Knobs (all optional): MIB_FATAL_SAVE_E2E_FPS (5000), _WIDTH/_HEIGHT
// (1216x208), _FRAMES (frames padded, 400), _BUFFER_MB (experiment buffer;
// default: the app's), _SLOW_MS (sleep per HDF5 append, 1500), _DIR (where the
// .h5 goes; default the temp dir).
// Requires the real frames (python scripts/provision-assets.py --asset
// 512x96stream-mock-frames --count 1000); without them the test is skipped (77).

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "frontend/core/MainWindow.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QMessageBox>
#include <QPointer>
#include <QSettings>
#include <QStyleFactory>
#include <QTimer>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

double envNumber(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try {
        return std::stod(v);
    } catch (...) {
        return fallback;
    }
}

void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool spinUntil(const std::function<bool()>& pred, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!pred() && clock.elapsed() < timeoutMs)
        spin(50);
    return pred();
}

std::filesystem::path realFrames() {
    if (const char* env = std::getenv("MIB_REVIEW_E2E_FRAMES")) return env;
    if (const char* assets = std::getenv("MIB_ASSETS_DIR"))
        return std::filesystem::path(assets) / "datasets" / "512x96stream-mock-frames";
    return std::filesystem::path("build") / "vendor" / "assets" / "datasets" /
           "512x96stream-mock-frames";
}

std::vector<std::filesystem::path> frameFiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.is_regular_file() && entry.file_size() > 0 && entry.path().extension() == ".tiff")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

// Per-pixel median over up to `maxSamples` frames (same recipe as the
// Monitoring/Review e2e tests).
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

// Centres `src` on a width x height canvas of `fill`. A flat border, not a
// replicated edge: a cell touching the edge would otherwise smear into a
// streak that the processing would segment.
cv::Mat pad(const cv::Mat& src, int width, int height, uint8_t fill) {
    cv::Mat out(height, width, CV_8UC1, cv::Scalar(fill));
    const int x = (width - src.cols) / 2;
    const int y = (height - src.rows) / 2;
    src.copyTo(out(cv::Rect(x, y, src.cols, src.rows)));
    return out;
}

struct SeenBox {
    QString title;
    QString text;
    bool runTerminal;
};

} // namespace

int main(int argc, char* argv[]) {
    // Offscreen by default; QT_QPA_PLATFORM=windows overrides it on hosts where
    // the offscreen plugin stalls in the QApplication constructor.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    // The watcher fills in the Qt save dialog; a native one is out of its reach.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);

    const auto source = frameFiles(realFrames());
    if (source.size() < 100) {
        std::fprintf(stderr,
                     "SKIP: real frames not provisioned at %s (python scripts/provision-assets.py "
                     "--asset 512x96stream-mock-frames --count 1000)\n",
                     realFrames().string().c_str());
        return 77;
    }
    const double fps = envNumber("MIB_FATAL_SAVE_E2E_FPS", 5000.0);
    const int width = static_cast<int>(envNumber("MIB_FATAL_SAVE_E2E_WIDTH", 1216));
    const int height = static_cast<int>(envNumber("MIB_FATAL_SAVE_E2E_HEIGHT", 208));
    const auto frameCount = static_cast<std::size_t>(envNumber("MIB_FATAL_SAVE_E2E_FRAMES", 400));
    const double bufferMb = envNumber("MIB_FATAL_SAVE_E2E_BUFFER_MB", 0.0);
    const int slowMs = static_cast<int>(envNumber("MIB_FATAL_SAVE_E2E_SLOW_MS", 1500));

    mib::test::Watchdog wd(240);
    mib::test::TempDir td("fatal_save_dialog_e2e");

    // ---- rig-sized frames from the real-cell stream ------------------------------
    const cv::Mat smallBackground = medianBackground(source, 64);
    MIB_REQUIRE(!smallBackground.empty(), "median background from the real frames");
    MIB_REQUIRE(width >= smallBackground.cols && height >= smallBackground.rows,
                "target size holds the frames");
    const auto fill = static_cast<uint8_t>(std::lround(cv::mean(smallBackground)[0]));
    const cv::Mat background = pad(smallBackground, width, height, fill);
    const auto framesDir = td / "frames";
    std::filesystem::create_directories(framesDir);
    for (std::size_t i = 0; i < std::min(frameCount, source.size()); ++i) {
        const cv::Mat gray = cv::imread(source[i].string(), cv::IMREAD_GRAYSCALE);
        MIB_REQUIRE(!gray.empty() && gray.size() == smallBackground.size(),
                    "readable source frame");
        char name[32];
        std::snprintf(name, sizeof(name), "frame.%05zu.tiff", i);
        MIB_REQUIRE(cv::imwrite((framesDir / name).string(), pad(gray, width, height, fill)),
                    "write padded frame");
    }
    std::fprintf(stderr, "frames: %zu of %dx%d (source %dx%d, border %u) at %.0f fps\n",
                 std::min(frameCount, source.size()), width, height, smallBackground.cols,
                 smallBackground.rows, static_cast<unsigned>(fill), fps);

    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_MOCK_CAMERA_DIR", QByteArray::fromStdString(framesDir.string()));
    qputenv("MIB_MOCK_CAMERA_INTERVAL_MS", QByteArray::number(1000.0 / fps, 'f', 4));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL",
            QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL",
            QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));

    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((td / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td / "data").string()), "backend init");

    std::atomic<int> slow{slowMs};
    backend::services::setHdf5PerformanceTraceHook(
        [&](std::string_view name, std::string_view, double, std::string_view) {
            if (name == "hdf5.append_frames" && slow.load() > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(slow.load()));
        });

    std::filesystem::path outDir = td.path();
    if (const char* dir = std::getenv("MIB_FATAL_SAVE_E2E_DIR")) outDir = dir;
    const auto h5Path = outDir / "fatal_save_e2e.h5";
    std::filesystem::remove(h5Path);
    const QString h5 = QString::fromStdString(h5Path.string());
    std::vector<SeenBox> boxes;
    int maxOpenBoxes = 0;
    bool fileDialogAnswered = false;

    {
        wd.mark("window");
        MainWindow window(backend);
        window.setAvailableGeometryOverrideForTests(QRect(0, 0, 1600, 1000));
        window.resize(1400, 900);
        window.show();
        spin(500);

        // Answers the save dialog, and records then closes every message box.
        // The timer also fires inside nested (modal) event loops, so a stack of
        // boxes is visible here as more than one open at once.
        QTimer watcher;
        watcher.setInterval(20);
        QObject::connect(&watcher, &QTimer::timeout, [&]() {
            int open = 0;
            std::vector<QPointer<QMessageBox>> toClose;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (!w->isVisible()) continue;
                if (auto* fd = qobject_cast<QFileDialog*>(w)) {
                    if (!fileDialogAnswered) {
                        fileDialogAnswered = true;
                        fd->selectFile(h5);
                        QMetaObject::invokeMethod(fd, "accept", Qt::QueuedConnection);
                    }
                    continue;
                }
                if (auto* box = qobject_cast<QMessageBox*>(w)) {
                    ++open;
                    if (!box->property("e2eSeen").toBool()) {
                        box->setProperty("e2eSeen", true);
                        const auto st = backend.experiment().status();
                        boxes.push_back({box->windowTitle(), box->text(), st.terminal});
                        std::fprintf(stderr, "box: [%s] terminal=%d %s\n",
                                     qPrintable(box->windowTitle()), st.terminal ? 1 : 0,
                                     qPrintable(box->text().left(120)));
                    }
                    toClose.emplace_back(box);
                    continue;
                }
                if (auto* dlg = qobject_cast<QDialog*>(w);
                    dlg && dlg->isModal() && !dlg->property("e2eSeen").toBool()) {
                    // Any other modal would block the run; report and close it.
                    dlg->setProperty("e2eSeen", true);
                    std::fprintf(stderr, "dialog: %s [%s]\n", dlg->metaObject()->className(),
                                 qPrintable(dlg->windowTitle()));
                    QMetaObject::invokeMethod(dlg, "reject", Qt::QueuedConnection);
                }
            }
            maxOpenBoxes = std::max(maxOpenBoxes, open);
            for (auto& b : toClose)
                if (b) b->done(QMessageBox::Ok);
        });
        watcher.start();

        // ---- capture with every detected cell accepted ---------------------------
        wd.mark("capture");
        auto& processing = backend.processing();
        QMetaObject::invokeMethod(&window, "onStartCapture", Qt::DirectConnection);
        spin(500);
        {
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
        processing.setRealtimeRoi(
            backend::services::ProcessingService::Roi{0, 0, background.cols, background.rows});
        if (bufferMb > 0)
            processing.setMaxBufferedBytes(static_cast<uint64_t>(bufferMb * 1024 * 1024));
        spin(1000);

        // ---- start the experiment (the watcher answers the save dialog) -----------
        wd.mark("start");
        // Readiness settles shortly after capture starts; Start before then is
        // refused with a dialog.
        auto blockers = [&] {
            std::vector<backend::app::ReadinessGate> out;
            for (const auto& g : backend.experiment().evaluateReadiness().gates)
                if (g.blocksStart() && g.id != "storage.output") out.push_back(g);
            return out;
        };
        if (!spinUntil([&] { return blockers().empty(); }, 20000)) {
            for (const auto& g : blockers())
                std::fprintf(stderr, "blocking gate: %s: %s (%s)\n", g.id.c_str(), g.reason.c_str(),
                             g.detail.c_str());
            MIB_REQUIRE(false, "experiment readiness passes on the mock camera");
        }
        QMetaObject::invokeMethod(&window, "onStartExperiment", Qt::QueuedConnection);
        MIB_REQUIRE(
            spinUntil([&] { return backend.experiment().status().startGeneration != 0; }, 20000),
            "experiment started");
        MIB_EXPECT(fileDialogAnswered, "save dialog answered");
        QElapsedTimer runClock;
        runClock.start();

        // ---- overflow: the run fails and finalizes; the GUI stays responsive -----
        wd.mark("overflow");
        const bool terminal =
            spinUntil([&] { return backend.experiment().status().terminal; }, 60000);
        const auto acc = processing.experimentAccountingSnapshot();
        std::fprintf(stderr,
                     "after %.1f s: terminal=%d admitted=%llu processed=%llu persisted=%llu/%llu "
                     "cancelled=%llu (%.0f admitted/s)\n",
                     runClock.elapsed() / 1000.0, terminal ? 1 : 0,
                     (unsigned long long)acc.admitted, (unsigned long long)acc.processed,
                     (unsigned long long)acc.persistenceCommitted,
                     (unsigned long long)acc.persistenceAdmitted,
                     (unsigned long long)acc.persistenceCancelledByPolicy,
                     acc.admitted / std::max(0.001, runClock.elapsed() / 1000.0));
        if (!terminal) {
            // The writer kept up: no overflow to test. Stop the run so the
            // window can close, then fail with the numbers above.
            QMetaObject::invokeMethod(&window, "onStopExperiment", Qt::QueuedConnection);
            spinUntil([&] { return backend.experiment().status().terminal; }, 30000);
        }
        MIB_REQUIRE(terminal,
                    "the experiment buffer overflows and the run reaches a terminal state");
        slow.store(0);
        const auto st = backend.experiment().status();
        std::fprintf(stderr, "terminal: fault=%s message=%s\n", st.faultCode.c_str(),
                     st.faultMessage.c_str());
        MIB_EXPECT(st.faultMessage.find("Experiment buffer capacity exceeded") != std::string::npos,
                   "run failed on buffer capacity");
        spin(1500); // let the terminal status and any deferred dialog land

        wd.mark("stop-capture");
        watcher.stop();
        QMetaObject::invokeMethod(&window, "onStopCapture", Qt::DirectConnection);
        spin(300);
    }
    backend::services::setHdf5PerformanceTraceHook({});
    backend.shutdown();

    int saveErrors = 0;
    for (const auto& b : boxes) {
        if (b.title == QStringLiteral("Save Error")) {
            ++saveErrors;
            MIB_EXPECT(b.runTerminal, "the Save Error box opens only after the run is terminal");
        }
    }
    std::fprintf(stderr, "boxes seen: %zu, Save Error: %d, most open at once: %d\n", boxes.size(),
                 saveErrors, maxOpenBoxes);
    MIB_EXPECT(saveErrors == 1, "exactly one Save Error box for the overflow");
    MIB_EXPECT(maxOpenBoxes <= 1, "message boxes never stack");
    return mib::test::exitCode();
}
