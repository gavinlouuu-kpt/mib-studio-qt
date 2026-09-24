// hdf_review_core_test
//
// Review tab draws the KDE core contours stored in an experiment file:
//  - full-run record solid, live (provisional) record dashed, one series per
//    loop, both on the Deformability-vs-Area scatter, one legend entry each;
//  - a file without records draws none, and opening it removes the previous
//    file's contours (no leakage between files);
//  - an unreadable record is ignored (warning), never fatal.

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "frontend/tabs/HdfReviewTab.h"
#include "frontend/tabs/KdeCoreRecord.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QLineSeries>
#include <QPen>
#include <QSettings>

#include <opencv2/core.hpp>

#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace mon = frontend::monitoring;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;

namespace {

void settle(int rounds = 4) {
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}

ProcessedFrame frame(uint64_t idx, double area, double deform) {
    ProcessedFrame f;
    f.index = idx;
    f.timestampNs = (idx + 1) * 1000ULL;
    f.originalImage = cv::Mat(8, 10, CV_8UC1, cv::Scalar(60));
    f.processedImage = cv::Mat(8, 10, CV_8UC1, cv::Scalar(255));
    f.validation.isValid = true;
    f.validation.objectId = static_cast<int>(idx);
    f.validation.objectCount = 1;
    f.validation.area = area;
    f.validation.deformability = deform;
    return f;
}

void writeExperiment(const std::string& path, const std::string& liveJson,
                     const std::string& analysisJson) {
    Hdf5Service hdf5;
    MIB_REQUIRE(hdf5.openFile(path), "fixture create");
    MIB_REQUIRE(hdf5.initializeDatasets(), "fixture datasets");
    std::vector<ProcessedFrame> valid{frame(0, 500, 0.05), frame(1, 700, 0.06),
                                      frame(2, 900, 0.07)};
    MIB_REQUIRE(hdf5.appendFrames(valid, {}), "fixture frames");
    backend::services::ProcessingConfig cfg;
    backend::services::ProcessingService::Roi roi{0, 0, 10, 8};
    MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 4000, valid.size(), 0, cfg, roi, nullptr, nullptr),
                "fixture info");
    if (!liveJson.empty()) MIB_REQUIRE(hdf5.writeKdeLiveJson(liveJson), "fixture live record");
    if (!analysisJson.empty())
        MIB_REQUIRE(hdf5.writeKdeAnalysisJson(analysisJson), "fixture analysis record");
    hdf5.closeFile();
}

void writePopulation(const std::string& path, const std::string& liveJson) {
    // 400 valid cells in two clusters (areas in pixels; the tab converts with
    // the current pixel-to-micron factor) plus a few invalid rows.
    std::mt19937 rng(11);
    std::normal_distribution<double> ax(700, 60), ay(0.05, 0.01), bx(1300, 70), by(0.12, 0.015);
    std::vector<ProcessedFrame> valid;
    for (uint64_t i = 0; i < 400; ++i)
        valid.push_back(i % 3 == 0 ? frame(i, bx(rng), by(rng)) : frame(i, ax(rng), ay(rng)));
    Hdf5Service hdf5;
    MIB_REQUIRE(hdf5.openFile(path), "population create");
    MIB_REQUIRE(hdf5.initializeDatasets(), "population datasets");
    MIB_REQUIRE(hdf5.appendFrames(valid, {}), "population frames");
    backend::services::ProcessingConfig cfg;
    backend::services::ProcessingService::Roi roi{0, 0, 10, 8};
    MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 4000, valid.size(), 0, cfg, roi, nullptr, nullptr),
                "population info");
    if (!liveJson.empty()) MIB_REQUIRE(hdf5.writeKdeLiveJson(liveJson), "population live record");
    hdf5.closeFile();
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!pred() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
    return pred();
}

std::string readAnalysis(const std::string& path) {
    Hdf5Service reader;
    std::string json;
    if (reader.loadFile(path)) {
        reader.readKdeAnalysisJson(json);
        reader.closeFile();
    }
    return json;
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES",
            QByteArrayLiteral("auto_update,autofocus,trigger,yolo,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL",
            QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL",
            QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR"))
        qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    mib::test::Watchdog wd(120);
    QApplication app(argc, argv);
    mib::test::TempDir td("hdf_review_core");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");

    mon::KdeCoreRecord live;
    live.coreFraction = 0.9;
    live.contours = {{{400, 0.03}, {800, 0.03}, {800, 0.09}, {400, 0.09}, {400, 0.03}}};
    mon::KdeCoreRecord full = live;
    full.provisional = false;
    full.source = "full-run";
    full.contours.push_back({{850, 0.05}, {950, 0.05}, {950, 0.08}, {850, 0.05}});

    const std::string both = (td.path() / "both.h5").string();
    const std::string none = (td.path() / "none.h5").string();
    const std::string broken = (td.path() / "broken.h5").string();
    writeExperiment(both, mon::toJson(live), mon::toJson(full));
    writeExperiment(none, {}, {});
    writeExperiment(broken, "{not json", {});

    wd.mark("review");
    frontend::HdfReviewTab tab(backend);
    tab.resize(1100, 760);
    tab.show();
    settle(4);

    tab.loadHdfFileForTests(QString::fromStdString(both));
    settle(4);
    MIB_EXPECT(tab.hasStoredKdeAnalysis() && tab.hasStoredKdeLive(), "both stored records read");
    const auto& series = tab.storedKdeContourSeriesForTests();
    MIB_EXPECT(series.size() == 3, "one series per stored loop (2 full-run + 1 live)");
    int solid = 0, dashed = 0;
    for (auto* s : series) {
        if (s->pen().style() == Qt::SolidLine) ++solid;
        if (s->pen().style() == Qt::DashLine) ++dashed;
        MIB_EXPECT(s->count() >= 4 && s->chart() != nullptr, "loop drawn on the scatter chart");
    }
    MIB_EXPECT(solid == 2 && dashed == 1, "full-run solid, live dashed");
    MIB_EXPECT(!series.empty() && series.front()->name().contains(QStringLiteral("full run")) &&
                   series.back()->name().contains(QStringLiteral("provisional")),
               "series named after their record");

    tab.loadHdfFileForTests(QString::fromStdString(none));
    settle(4);
    MIB_EXPECT(!tab.hasStoredKdeAnalysis() && !tab.hasStoredKdeLive() &&
                   tab.storedKdeContourSeriesForTests().empty(),
               "a file without records draws no contour and clears the previous file's");

    tab.loadHdfFileForTests(QString::fromStdString(broken));
    settle(4);
    MIB_EXPECT(!tab.hasStoredKdeLive() && tab.storedKdeContourSeriesForTests().empty(),
               "an unreadable record is ignored, not fatal");

    // ---- full-run computation, save, overwrite confirmation --------------------
    wd.mark("full-run");
    const std::string pop = (td.path() / "population.h5").string();
    writePopulation(pop, mon::toJson(live));
    tab.loadHdfFileForTests(QString::fromStdString(pop));
    settle(4);
    MIB_REQUIRE(tab.computeCoreAction() && tab.computeCoreAction()->isEnabled(),
                "compute action enabled for an experiment with cells");
    MIB_EXPECT(!tab.hasStoredKdeAnalysis(), "no full-run record yet");
    tab.computeFullRunCoreForTests();
    MIB_EXPECT(tab.fullRunCoreJobInFlight() && !tab.computeCoreAction()->isEnabled(),
               "computation runs on a worker; action disabled meanwhile");
    MIB_REQUIRE(waitFor([&] { return !tab.fullRunCoreJobInFlight(); }, 30000),
                "computation finishes");
    settle(2);
    MIB_EXPECT(tab.hasStoredKdeAnalysis() &&
                   tab.statusTextForTests().contains(QStringLiteral("saved")),
               "contour computed and saved");
    int solidFullRun = 0;
    for (auto* s : tab.storedKdeContourSeriesForTests())
        solidFullRun += s->pen().style() == Qt::SolidLine ? 1 : 0;
    MIB_EXPECT(solidFullRun >= 1, "full-run contour drawn solid next to the live one");
    const std::string first = readAnalysis(pop);
    const auto firstRec = mon::fromJson(first);
    MIB_REQUIRE(firstRec.has_value(), "saved record parses");
    MIB_EXPECT(!firstRec->provisional && firstRec->source == "full-run" &&
                   firstRec->populationCount == 400 && firstRec->cellCount == 360 &&
                   !firstRec->contours.empty(),
               "saved record is the full-run core of all 400 cells");
    MIB_EXPECT(tab.computeCoreAction()->isEnabled(),
               "review reader reopened after the save (action enabled again)");

    // Existing record + "No": kept.
    tab.setOverwriteAnswerForTests(false);
    tab.computeFullRunCoreForTests();
    MIB_REQUIRE(waitFor([&] { return !tab.fullRunCoreJobInFlight(); }, 30000),
                "second computation finishes");
    settle(2);
    MIB_EXPECT(readAnalysis(pop) == first &&
                   tab.statusTextForTests().contains(QStringLiteral("Kept")),
               "declining the overwrite keeps the stored record");
    // Existing record + "Yes": replaced (new timestamp).
    tab.setOverwriteAnswerForTests(true);
    tab.computeFullRunCoreForTests();
    MIB_REQUIRE(waitFor([&] { return !tab.fullRunCoreJobInFlight(); }, 30000),
                "third computation finishes");
    settle(2);
    const auto replaced = mon::fromJson(readAnalysis(pop));
    MIB_EXPECT(replaced && replaced->computedAtNs > firstRec->computedAtNs &&
                   replaced->cellCount == firstRec->cellCount,
               "confirming replaces the record with an identical estimate at a later time");

    // ---- read-only file: shown, not saved ---------------------------------------
    wd.mark("read-only");
    const std::string ro = (td.path() / "readonly.h5").string();
    writePopulation(ro, {});
    namespace fs = std::filesystem;
    fs::permissions(ro, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove);
    tab.loadHdfFileForTests(QString::fromStdString(ro));
    settle(4);
    tab.computeFullRunCoreForTests();
    MIB_REQUIRE(waitFor([&] { return !tab.fullRunCoreJobInFlight(); }, 30000),
                "read-only computation finishes");
    settle(2);
    MIB_EXPECT(tab.hasStoredKdeAnalysis() &&
                   tab.statusTextForTests().contains(QStringLiteral("not saved")),
               "read-only file: contour shown and the status says it was not saved");
    tab.loadHdfFileForTests(
        QString::fromStdString(none)); // release the read-only file before restoring permissions
    settle(2);
    fs::permissions(ro, fs::perms::owner_write, fs::perm_options::add);
    MIB_EXPECT(readAnalysis(ro).empty(), "nothing was written to the read-only file");

    tab.close();
    settle(2);
    backend.shutdown();
    return mib::test::exitCode();
}
