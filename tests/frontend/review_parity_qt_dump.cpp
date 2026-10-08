// review_parity_qt_dump — Qt half of the YOFO Review parity sign-off
// (plan 2026-10-01-standalone-review-app, PR 8; tools/review_parity/).
//
// Opens MIB_REVIEW_PARITY_FILE in an offscreen HdfReviewTab and writes what
// the tab shows and exports to MIB_REVIEW_PARITY_OUT/qt/:
//   summary.json  status text, px→µm, scatter point count + axis ranges,
//                 histogram bins / labels / y range
//   metrics.csv   the Export Metrics request the tab builds (experiment files)
//   all/          the Export All request the tab builds, without the chart
//                 snapshots (rendered pixels differ between the shells by design)
//   core.json     the full-run core record the tab saves (experiment files;
//                 computed on a copy of the file)
// The live processing config is set to the file's recorded one first, so the
// histogram range is the same input YOFO Review uses (accepted difference:
// the Qt tab follows the live config). Exits 77 without the two variables.

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/HdfExportService.h"
#include "frontend/tabs/HdfReviewTab.h"
#include "frontend/utils/ApplicationSettings.h"
#include "frontend/widgets/ZoomableChartView.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QBarCategoryAxis>
#include <QBarSeries>
#include <QBarSet>
#include <QChart>
#include <QChartView>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QValueAxis>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>

namespace fs = std::filesystem;
using backend::services::Hdf5Service;

namespace {

void settle(int rounds = 6) {
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!pred() && clock.elapsed() < timeoutMs) settle(1);
    return pred();
}

QJsonObject axisRange(QChart* chart, Qt::Orientation o) {
    QJsonObject r;
    const auto axes = chart->axes(o);
    for (auto* a : axes) {
        if (auto* v = qobject_cast<QValueAxis*>(a)) {
            r["min"] = v->min();
            r["max"] = v->max();
            break;
        }
    }
    return r;
}

// The histogram chart view: the one whose chart carries a QBarSeries.
QJsonObject histogram(frontend::HdfReviewTab& tab) {
    QJsonObject h;
    for (auto* view : tab.findChildren<QChartView*>()) {
        QChart* chart = view->chart();
        if (!chart) continue;
        for (auto* s : chart->series()) {
            auto* bars = qobject_cast<QBarSeries*>(s);
            if (!bars) continue;
            QJsonArray counts;
            for (auto* set : bars->barSets())
                for (int i = 0; i < set->count(); ++i) counts.append(static_cast<int>(set->at(i)));
            QJsonArray labels;
            for (auto* a : chart->axes(Qt::Horizontal))
                if (auto* c = qobject_cast<QBarCategoryAxis*>(a))
                    for (const auto& l : c->categories()) labels.append(l);
            h["counts"] = counts;
            h["labels"] = labels;
            h["y"] = axisRange(chart, Qt::Vertical);
            return h;
        }
    }
    h["error"] = "no QBarSeries histogram (QHistogramSeries build?)";
    return h;
}

backend::recording::HdfExportResult runExport(const backend::recording::HdfExportRequest& request) {
    backend::recording::HdfExportService service;
    backend::recording::HdfExportCancelToken token;
    return service.run(request, token);
}

} // namespace

int main(int argc, char* argv[]) {
    const QByteArray source = qgetenv("MIB_REVIEW_PARITY_FILE");
    const QByteArray outRoot = qgetenv("MIB_REVIEW_PARITY_OUT");
    if (source.isEmpty() || outRoot.isEmpty()) {
        std::printf("SKIP: set MIB_REVIEW_PARITY_FILE and MIB_REVIEW_PARITY_OUT (tools/review_parity/run.sh)\n");
        return 77;
    }
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR"))
        qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    mib::test::Watchdog wd(1800);
    QApplication app(argc, argv);
    mib::test::TempDir td("review_parity_qt");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");

    const fs::path out = fs::path(outRoot.toStdString()) / "qt";
    fs::remove_all(out);
    fs::create_directories(out);

    bool recording = false;
    bool recordedConfig = false;
    {
        Hdf5Service probe;
        MIB_REQUIRE(probe.loadFile(source.toStdString()), "open source");
        recording = probe.isRecordingFile();
        backend::services::ProcessingConfig cfg = backend.processing().getProcessingConfig();
        recordedConfig = probe.readRecordedProcessingConfig(cfg);
        if (recordedConfig) backend.processing().setProcessingConfig(cfg);
        probe.closeFile();
    }
    // The core record is written into the file: work on a copy for experiments.
    std::string reviewPath = source.toStdString();
    if (!recording) {
        // Same file name: export folder / file names derive from it.
        reviewPath = (td.path() / fs::path(source.toStdString()).filename()).string();
        fs::copy_file(source.toStdString(), reviewPath);
    }

    wd.mark("load");
    frontend::HdfReviewTab tab(backend);
    tab.resize(1280, 860);
    tab.show();
    settle();
    tab.loadHdfFileForTests(QString::fromStdString(reviewPath));
    settle(12);

    QJsonObject summary;
    summary["source"] = QString::fromStdString(source.toStdString());
    summary["recording_file"] = recording;
    summary["live_config_from_file"] = recordedConfig;
    summary["status_text"] = tab.statusTextForTests();
    summary["pixel_to_micron"] = tab.pixelToMicronForTests();
    if (!recording) {
        QChart* chart = tab.scatterViewForTests()->chart();
        QJsonObject scatter;
        scatter["count"] = static_cast<int>(tab.scatterPointToFrameForTests().size());
        scatter["x"] = axisRange(chart, Qt::Horizontal);
        scatter["y"] = axisRange(chart, Qt::Vertical);
        summary["scatter"] = scatter;
        summary["histogram"] = histogram(tab);
    }

    wd.mark("exports");
    if (!recording) {
        // onExportMetrics: explicit destination, the file's factor.
        backend::recording::HdfExportRequest m;
        m.sourcePath = reviewPath;
        m.outputRoot = out.string();
        m.format = backend::recording::HdfExportFormat::MetricsCsv;
        m.conversionFactor = tab.pixelToMicronForTests();
        m.explicitDestination = (out / "metrics.csv").string();
        const auto r = runExport(m);
        summary["metrics_export"] = r.status == backend::recording::HdfExportStatus::Completed;
    }
    {
        // onExportAll: format All into <root>/<base>, the file's factor, and the
        // series prompt's default (all series; the request default is the same).
        backend::recording::HdfExportRequest a;
        a.sourcePath = reviewPath;
        a.outputRoot = (out / "all").string();
        a.format = backend::recording::HdfExportFormat::All;
        a.conversionFactor = tab.pixelToMicronForTests();
        fs::create_directories(a.outputRoot);
        const auto r = runExport(a);
        summary["all_export"] = r.status == backend::recording::HdfExportStatus::Completed;
    }

    if (!recording && tab.computeCoreAction() && tab.computeCoreAction()->isEnabled()) {
        wd.mark("core");
        tab.setOverwriteAnswerForTests(true);
        tab.computeFullRunCoreForTests();
        MIB_REQUIRE(waitFor([&] { return !tab.fullRunCoreJobInFlight(); }, 600000), "core computation finishes");
        settle(4);
        summary["core_status_text"] = tab.statusTextForTests();
        Hdf5Service reader; // read-only beside the tab's reader, as hdf_review_core_test does
        std::string json;
        if (reader.loadFile(reviewPath)) {
            reader.readKdeAnalysisJson(json);
            reader.closeFile();
        }
        QFile f(QString::fromStdString((out / "core.json").string()));
        MIB_REQUIRE(f.open(QIODevice::WriteOnly), "write core.json");
        f.write(QByteArray::fromStdString(json));
    }

    QFile f(QString::fromStdString((out / "summary.json").string()));
    MIB_REQUIRE(f.open(QIODevice::WriteOnly), "write summary.json");
    f.write(QJsonDocument(summary).toJson(QJsonDocument::Indented));
    f.close();
    std::printf("qt dump: %s\n", out.string().c_str());

    tab.close();
    settle(2);
    backend.shutdown();
    return mib::test::exitCode();
}
